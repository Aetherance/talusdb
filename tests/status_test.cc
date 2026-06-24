#include "status.h"

#include "gtest/gtest.h"

namespace db {
namespace {

TEST(StatusTest, FactoryMethodsExposeCodesMessagesAndStrings) {
  const Status ok = Status::OkStatus();
  EXPECT_TRUE(ok.Ok());
  EXPECT_EQ("OK", ok.ToString());

  const Status not_found = Status::NotFound("missing", "file");
  EXPECT_EQ(Status::Code::kNotFound, not_found.GetCode());
  EXPECT_TRUE(not_found.IsNotFound());
  EXPECT_EQ("missing: file", not_found.Message());
  EXPECT_EQ("NotFound: missing: file", not_found.ToString());

  const Status corruption = Status::Corruption("bad data");
  EXPECT_EQ(Status::Code::kCorruption, corruption.GetCode());
  EXPECT_EQ("Corruption: bad data", corruption.ToString());

  const Status not_supported = Status::NotSupported("append");
  EXPECT_EQ(Status::Code::kNotSupported, not_supported.GetCode());
  EXPECT_EQ("NotSupported: append", not_supported.ToString());

  const Status invalid_argument = Status::InvalidArgument("option", "mismatch");
  EXPECT_EQ(Status::Code::kInvalidArgument, invalid_argument.GetCode());
  EXPECT_EQ("InvalidArgument: option: mismatch", invalid_argument.ToString());

  const Status io_error = Status::IOError("disk", "full");
  EXPECT_EQ(Status::Code::kIOError, io_error.GetCode());
  EXPECT_EQ("IOError: disk: full", io_error.ToString());
}

TEST(StatusTest, EmptyMessageUsesCodeStringOnly) {
  const Status not_found = Status::NotFound(Slice());

  EXPECT_TRUE(not_found.Message().empty());
  EXPECT_EQ("NotFound", not_found.ToString());
}

TEST(StatusTest, EqualityDependsOnCodeAndMessage) {
  const Status first = Status::InvalidArgument("option", "mismatch");
  const Status second = Status::InvalidArgument("option", "mismatch");
  const Status different_code = Status::IOError("option", "mismatch");
  const Status different_message = Status::InvalidArgument("option", "different");

  EXPECT_EQ(first, second);
  EXPECT_NE(first, different_code);
  EXPECT_NE(first, different_message);
}

}  // namespace
}  // namespace db
