include(GoogleTest)

add_executable(tests
wal_replay_test.cpp
wal_test.cpp
write_batch_test.cpp
)
target_link_libraries(tests PRIVATE gritdb GTest::gtest_main)
gtest_discover_tests(tests)
