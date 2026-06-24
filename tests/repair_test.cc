#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "db/filename.h"
#include "db/log_writer.h"
#include "db/write_batch_internal.h"
#include "env.h"
#include "gtest/gtest.h"
#include "write_batch.h"

namespace db {
namespace {

class RepairTest : public ::testing::Test {
protected:
  void SetUp() override {
    env_ = Env::Default();
    std::string test_dir;
    ASSERT_TRUE(env_->GetTestDirectory(&test_dir).Ok());
    dbname_ = test_dir + "/repair-test-" + std::to_string(env_->NowMicros());
    RemoveDBDir();
    ASSERT_TRUE(env_->CreateDir(dbname_).Ok());
  }

  void TearDown() override {
    RemoveDBDir();
  }

  void RemoveDBDir() {
    std::vector<std::string> children;
    if (!env_->GetChildren(dbname_, &children).Ok()) {
      return;
    }
    for (const std::string& child : children) {
      const std::string path = dbname_ + "/" + child;
      if (child == "lost") {
        std::vector<std::string> lost_children;
        if (env_->GetChildren(path, &lost_children).Ok()) {
          for (const std::string& lost_child : lost_children) {
            env_->RemoveFile(path + "/" + lost_child);
          }
        }
        env_->RemoveDir(path);
      } else {
        env_->RemoveFile(path);
      }
    }
    env_->RemoveDir(dbname_);
  }

  void WriteLog(uint64_t number, const WriteBatch& batch) {
    WritableFile* file = nullptr;
    ASSERT_TRUE(env_->NewWritableFile(LogFileName(dbname_, number), &file).Ok());
    log::Writer writer(file);
    ASSERT_TRUE(writer.AddRecord(WriteBatchInternal::Contents(&batch)).Ok());
    ASSERT_TRUE(file->Close().Ok());
    delete file;
  }

  Env* env_ = nullptr;
  std::string dbname_;
};

TEST_F(RepairTest, ConvertsLogToTableAndReopensDatabase) {
  WriteBatch batch;
  WriteBatchInternal::SetSequence(&batch, 10);
  batch.Put("alpha", "one");
  batch.Put("beta", "two");
  batch.Delete("beta");
  WriteLog(5, batch);

  Options options;
  options.env = env_;
  Status status = RepairDB(dbname_, options);
  ASSERT_TRUE(status.Ok()) << status.ToString();

  EXPECT_TRUE(env_->FileExists(CurrentFileName(dbname_)));
  EXPECT_TRUE(env_->FileExists(dbname_ + "/lost/000005.log"));

  options.create_if_missing = false;
  DB* raw_db = nullptr;
  status = DB::Open(options, dbname_, &raw_db);
  ASSERT_TRUE(status.Ok()) << status.ToString();
  std::unique_ptr<DB> db(raw_db);

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions(), "alpha", &value).Ok());
  EXPECT_EQ("one", value);
  EXPECT_TRUE(db->Get(ReadOptions(), "beta", &value).IsNotFound());
}

}  // namespace
}  // namespace db
