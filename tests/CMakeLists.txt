include(GoogleTest)

add_executable(lob_tests
  test_order_book.cpp
  test_reference.cpp)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
gtest_discover_tests(lob_tests)
