include(GoogleTest)

add_executable(tests
table_test.cpp
table_flush_test.cpp
)
target_link_libraries(tests PRIVATE gritdb GTest::gtest_main)
gtest_discover_tests(tests)
