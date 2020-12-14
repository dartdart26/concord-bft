// Concord
//
// Copyright (c) 2020 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the
// "License").  You may not use this product except in compliance with the
// Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#include "categorization/shared_kv_category.h"

#include "assertUtils.hpp"
#include "categorization/column_families.h"
#include "categorization/details.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace std::literals;

namespace concord::kvbc::categorization::detail {

// root_hash = h((h(k1) || h(v1)) || (h(k2) || h(v2)) || ... || (h(k3) || h(v3)))
void updateCategoryHash(const std::string &category_id,
                        const Hash &key_hash,
                        const Hash &value_hash,
                        std::map<std::string, Hasher> &category_hashers) {
  auto [it, inserted] = category_hashers.emplace(category_id, Hasher{});
  if (inserted) {
    it->second.init();
  }
  it->second.update(key_hash.data(), key_hash.size());
  it->second.update(value_hash.data(), value_hash.size());
}

void finishCategoryHashes(std::map<std::string, Hasher> &category_hashers, SharedKeyValueUpdatesInfo &update_info) {
  auto category_hashes = std::map<std::string, Hash>{};
  for (auto &[category_id, hasher] : category_hashers) {
    category_hashes[category_id] = hasher.finish();
  }
  update_info.category_root_hashes = std::move(category_hashes);
}

bool hasVersion(const std::string &category_id, BlockId block_id, const KeyVersionsPerCategory &key_versions) {
  auto cat_it = key_versions.data.find(category_id);
  if (cat_it == key_versions.data.end()) {
    return false;
  }
  const auto &versions = cat_it->second;
  ConcordAssert(!versions.empty());
  auto ver_it = std::lower_bound(versions.begin(), versions.cend(), block_id);
  if (ver_it != versions.cend() && *ver_it == block_id) {
    return true;
  }
  return false;
}

std::optional<BlockId> lastVersionUntil(const std::string &category_id,
                                        BlockId block_id,
                                        const KeyVersionsPerCategory &key_versions) {
  auto cat_it = key_versions.data.find(category_id);
  if (cat_it == key_versions.data.end()) {
    return std::nullopt;
  }
  const auto &versions = cat_it->second;
  ConcordAssert(!versions.empty());
  auto ver_it = std::upper_bound(versions.begin(), versions.cend(), block_id);
  if (ver_it == versions.cbegin()) {
    return std::nullopt;
  }
  return *(ver_it - 1);
}

SharedKeyValueCategory::SharedKeyValueCategory(const std::shared_ptr<storage::rocksdb::NativeClient> &db) : db_{db} {
  createColumnFamilyIfNotExisting(SHARED_KV_DATA_CF, *db_);
  createColumnFamilyIfNotExisting(SHARED_KV_KEY_VERSIONS_CF, *db_);
}

SharedKeyValueUpdatesInfo SharedKeyValueCategory::add(BlockId block_id,
                                                      SharedKeyValueUpdatesData &&update,
                                                      storage::rocksdb::NativeWriteBatch &batch) {
  if (update.kv.empty()) {
    throw std::invalid_argument{"Empty shared key-value updates"};
  }

  auto update_info = SharedKeyValueUpdatesInfo{};
  auto category_hashers = std::map<std::string, Hasher>{};

  for (auto it = update.kv.begin(); it != update.kv.end();) {
    // Save the iterator as extract() invalidates it.
    auto extracted_it = it;
    ++it;
    auto node = update.kv.extract(extracted_it);
    auto &key = node.key();
    auto &shared_value = node.mapped();
    if (shared_value.category_ids.empty()) {
      throw std::invalid_argument{"Empty category ID set for a shared key-value update"};
    }

    const auto versioned_key = versionedKey(key, block_id);
    auto &key_data = update_info.keys.emplace(std::move(key), SharedKeyData{}).first->second;
    auto key_versions = versionsForKey(versioned_key.key_hash.value);
    const auto value_hash = hash(shared_value.value);

    for (auto &&category_id : shared_value.category_ids) {
      if (update.calculate_root_hash) {
        updateCategoryHash(category_id, versioned_key.key_hash.value, value_hash, category_hashers);
      }

      // The value is persisted into the data column family.
      batch.put(SHARED_KV_DATA_CF, serialize(versioned_key), shared_value.value);

      // Append the block ID to the key versions (per category).
      {
        auto &versions_for_cat = key_versions.data[category_id];
        if (!versions_for_cat.empty()) {
          ConcordAssertGT(block_id, versions_for_cat.back());
        }
        versions_for_cat.push_back(block_id);
      }

      // Accumulate category IDs for keys in the update info.
      key_data.categories.push_back(std::move(category_id));
    }

    // Persist the key versions.
    batch.put(SHARED_KV_KEY_VERSIONS_CF, versioned_key.key_hash.value, serialize(key_versions));
  }

  // Finish hash calculation per category.
  if (update.calculate_root_hash) {
    finishCategoryHashes(category_hashers, update_info);
  }
  return update_info;
}

std::optional<Value> SharedKeyValueCategory::get(const std::string &category_id,
                                                 const std::string &key,
                                                 BlockId block_id) const {
  const auto key_hash = hash(key);
  const auto key_versions = versionsForKey(key_hash);
  if (!hasVersion(category_id, block_id, key_versions)) {
    return std::nullopt;
  }
  return getValue(key_hash, block_id);
}

std::optional<Value> SharedKeyValueCategory::getUntilBlock(const std::string &category_id,
                                                           const std::string &key,
                                                           BlockId block_id) const {
  const auto key_hash = hash(key);
  const auto key_versions = versionsForKey(key_hash);
  const auto actual_block_id = lastVersionUntil(category_id, block_id, key_versions);
  if (!actual_block_id) {
    return std::nullopt;
  }
  return getValue(key_hash, *actual_block_id);
}

std::optional<Value> SharedKeyValueCategory::getLatest(const std::string &category_id, const std::string &key) const {
  const auto key_hash = hash(key);
  const auto key_versions = versionsForKey(key_hash);
  auto cat_it = key_versions.data.find(category_id);
  if (cat_it == key_versions.data.cend()) {
    return std::nullopt;
  }
  const auto &versions = cat_it->second;
  ConcordAssert(!versions.empty());
  return getValue(key_hash, versions.back());
}

std::optional<KeyValueProof> SharedKeyValueCategory::getProof(const std::string &category_id,
                                                              const std::string &key,
                                                              BlockId block_id,
                                                              const SharedKeyValueUpdatesInfo &updates_info) const {
  auto value = get(category_id, key, block_id);
  if (!value) {
    return std::nullopt;
  }

  auto proof = KeyValueProof{};
  proof.key = key;
  proof.value = std::move(*value);

  auto i = std::size_t{0};
  for (const auto &[update_key, update_key_data] : updates_info.keys) {
    if (update_key == key) {
      proof.key_value_index = i;
      continue;
    }

    if (std::find(update_key_data.categories.cbegin(), update_key_data.categories.cend(), category_id) !=
        update_key_data.categories.cend()) {
      const auto update_key_hash = hash(update_key);
      const auto update_value = getValue(update_key_hash, block_id);
      const auto update_value_hash = hash(update_value.data);

      auto hasher = Hasher{};
      hasher.init();
      hasher.update(update_key_hash.data(), update_key_hash.size());
      hasher.update(update_value_hash.data(), update_value_hash.size());
      proof.ordered_complement_kv_hashes.push_back(hasher.finish());

      ++i;
    }
  }
  return proof;
}

KeyVersionsPerCategory SharedKeyValueCategory::versionsForKey(const Hash &key_hash) const {
  auto key_versions = KeyVersionsPerCategory{};
  const auto key_versions_db_value = db_->get(SHARED_KV_KEY_VERSIONS_CF, key_hash);
  if (key_versions_db_value) {
    deserialize(*key_versions_db_value, key_versions);
  }
  return key_versions;
}

Value SharedKeyValueCategory::getValue(const Hash &key_hash, BlockId block_id) const {
  auto value = db_->get(SHARED_KV_DATA_CF, serialize(versionedKey(key_hash, block_id)));
  ConcordAssert(value.has_value());
  return Value{std::move(*value), block_id, std::nullopt};
}

}  // namespace concord::kvbc::categorization::detail
