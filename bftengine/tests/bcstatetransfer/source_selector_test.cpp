// Concord
//
// Copyright (c) 2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").
// You may not use this product except in compliance with the Apache 2.0
// License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the subcomponent's license, as noted in the
// LICENSE file.

#include "gtest/gtest.h"

#include "SourceSelector.hpp"

namespace {

using bftEngine::bcst::impl::SourceSelector;

constexpr auto kDefaultRetransmissionTimeoutMilli = 100;
constexpr auto kDefaultSourceReplicaReplacementTimeoutMilli = 300;

TEST(source_selector_test, no_preferred_on_construction) {
  const auto s =
      SourceSelector{{1, 2, 3}, kDefaultRetransmissionTimeoutMilli, kDefaultSourceReplicaReplacementTimeoutMilli};
  ASSERT_EQ(0, s.numberOfPreferredReplicas());
}

TEST(source_selector_test, preferred_on_construction) {
  const auto other_replicas = std::set<uint16_t>{1, 2, 3};
  const auto s =
      SourceSelector{other_replicas, kDefaultRetransmissionTimeoutMilli, kDefaultSourceReplicaReplacementTimeoutMilli};
  for (const auto& r : other_replicas) {
    ASSERT_FALSE(s.isPreferred(r));
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
