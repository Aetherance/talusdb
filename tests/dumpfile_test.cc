#include "dumpfile.h"

#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "db/filename.h"
#include "db/log_writer.h"
#include "db/write_batch_internal.h"
#include "env.h"
#include "gtest/gtest.h"
#include "options.h"
#include "write_batch.h"

namespace db {
namespace {

class DumpFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    env_ = Env::Default();
    ASSERT_TRUE(env_->GetTestDirectory(&test_dir_).Ok());
    dbname_ = test_dir_ + "/dumpfile-test-" + std::to_string(env_->NowMicros());
    DestroyDB(dbname_, Options());
    ASSERT_TRUE(env_->CreateDir(dbname_).Ok());
  }

  void TearDown() override {
    std::vector<std::string> children;
    if (env_->GetChildren(dbname_, &children).Ok()) {
      for (const std::string& child : children) {
        env_->RemoveFile(dbname_ + "/" + child);
      }
    }
    env_->RemoveDir(dbname_);
  }

  Status DumpToString(const std::string& fname, std::string* output) {
    const std::string output_file = dbname_ + "/dump-output-" + std::to_string(output_id_++);
    WritableFile* dst = nullptr;
    Status status = env_->NewWritableFile(output_file, &dst);
    if (status.Ok()) {
      status = DumpFile(env_, fname, dst);
    }
    if (dst != nullptr) {
      Status close_status = dst->Close();
      if (status.Ok()) {
        status = close_status;
      }
      delete dst;
    }
    if (status.Ok()) {
      status = ReadFileToString(env_, output_file, output);
    }
    env_->RemoveFile(output_file);
    return status;
  }

  std::string FindTableFile() {
    std::vector<std::string> children;
    EXPECT_TRUE(env_->GetChildren(dbname_, &children).Ok());
    for (const std::string& child : children) {
      uint64_t number = 0;
      FileType type = kLogFile;
      if (ParseFileName(child, &number, &type) && type == kTableFile) {
        return dbname_ + "/" + child;
      }
    }
    return "";
  }

  Env* env_ = nullptr;
  std::string test_dir_;
  std::string dbname_;
  int output_id_ = 0;
};

TEST_F(DumpFileTest, DumpsWriteAheadLogRecords) {
  const std::string log_name = LogFileName(dbname_, 7);
  WritableFile* file = nullptr;
  ASSERT_TRUE(env_->NewWritableFile(log_name, &file).Ok());
  log::Writer writer(file);

  WriteBatch batch;
  WriteBatchInternal::SetSequence(&batch, 100);
  batch.Put("alpha", "one");
  batch.Delete("bravo");
  ASSERT_TRUE(writer.AddRecord(WriteBatchInternal::Contents(&batch)).Ok());
  ASSERT_TRUE(file->Close().Ok());
  delete file;

  std::string dump;
  Status status = DumpToString(log_name, &dump);
  ASSERT_TRUE(status.Ok()) << status.ToString();
  EXPECT_NE(std::string::npos, dump.find("sequence 100"));
  EXPECT_NE(std::string::npos, dump.find("  put 'alpha' 'one'\n"));
  EXPECT_NE(std::string::npos, dump.find("  del 'bravo'\n"));
}

TEST_F(DumpFileTest, DumpsTableInternalKeys) {
  Options options;
  options.env = env_;
  options.create_if_missing = true;
  options.compression = kNoCompression;

  DB* raw_db = nullptr;
  ASSERT_TRUE(DB::Open(options, dbname_, &raw_db).Ok());
  std::unique_ptr<DB> db(raw_db);
  ASSERT_TRUE(db->Put(WriteOptions(), "alpha", "one").Ok());
  ASSERT_TRUE(db->Put(WriteOptions(), "bravo", "two").Ok());
  ASSERT_TRUE(db->Write(WriteOptions(), nullptr).Ok());
  db.reset();

  const std::string table_name = FindTableFile();
  ASSERT_FALSE(table_name.empty());

  std::string dump;
  Status status = DumpToString(table_name, &dump);
  ASSERT_TRUE(status.Ok()) << status.ToString();
  EXPECT_NE(std::string::npos, dump.find("'alpha' @ "));
  EXPECT_NE(std::string::npos, dump.find(" : val => 'one'\n"));
  EXPECT_NE(std::string::npos, dump.find("'bravo' @ "));
  EXPECT_NE(std::string::npos, dump.find(" : val => 'two'\n"));
}

TEST_F(DumpFileTest, RejectsUnknownFileType) {
  WritableFile* dst = nullptr;
  const std::string output_file = dbname_ + "/unknown-output";
  ASSERT_TRUE(env_->NewWritableFile(output_file, &dst).Ok());

  Status status = DumpFile(env_, dbname_ + "/unknown.file", dst);
  EXPECT_EQ(Status::Code::kInvalidArgument, status.GetCode());

  ASSERT_TRUE(dst->Close().Ok());
  delete dst;
}

}  // namespace
}  // namespace db
