#include <gtest/gtest.h>
#include <posix_filesystem.h>

#include "filesystem.h"
#include <memory>

using namespace gritdb;
#define ASSERT_OK(expr)                                                        \
  do {                                                                         \
    Status _s = (expr);                                                        \
    ASSERT_TRUE(_s.ok()) << _s.ToString();                                     \
  } while (0)

TEST(PosixFsTest, MkDir) {}
