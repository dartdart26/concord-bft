project (cre LANGUAGES CXX)

add_library(cre
        src/client_reconfiguration_engine.cpp
        src/poll_based_state_client.cpp
        )

target_include_directories(cre PUBLIC include)

target_link_libraries(cre PUBLIC
        bftclient_new
        cmf_messages
        util)

if (BUILD_TESTING)
    add_subdirectory(test)
endif()