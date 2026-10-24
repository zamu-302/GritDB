include(GoogleTest)

add_executable(tests
  varint_test.cpp
)
target_link_libraries(tests PRIVATE gritdb GTest::gtest_main)
gtest_discover_tests(tests)
