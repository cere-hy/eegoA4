add_executable(CssLoadGuardTest CssLoadGuardTest.cpp)

target_include_directories(CssLoadGuardTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(CssLoadGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssLoadGuardTest)
