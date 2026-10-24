include(GoogleTest)

add_executable(tests
  crc32c.cpp
)
target_link_libraries(tests PRIVATE gritdb GTest::gtest_main)
gtest_discover_tests(tests)
