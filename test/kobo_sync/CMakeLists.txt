add_executable(KoboSyncTest
  KoboSyncTokenTest.cpp
  KoboFeedParserTest.cpp
  KoboDownloadPlanTest.cpp
  ${REPO_ROOT}/lib/KoboSync/KoboSyncToken.cpp
  ${REPO_ROOT}/lib/KoboSync/KoboSyncTime.cpp
  ${REPO_ROOT}/lib/KoboSync/KoboFeedParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(KoboSyncTest PRIVATE
  ${REPO_ROOT}/lib/KoboSync
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_compile_definitions(KoboSyncTest PRIVATE
  CROSSPOINT_KOBO_SYNC=1
  KOBO_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/calibre_web_sync.json"
)

target_link_libraries(KoboSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KoboSyncTest)
