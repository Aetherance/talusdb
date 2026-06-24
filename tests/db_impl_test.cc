#include "db/db_impl.h"

#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "db/dbformat.h"
#include "env.h"
#include "gtest/gtest.h"
#include "write_batch.h"

namespace db {
namespace {

std::string MakeDBName(const std::string& name) {
  Env* env = Env::Default();
  std::string root;
  EXPECT_TRUE(env->GetTestDirectory(&root).Ok());
  return root + "/db-impl-test-" + std::to_string(env->NowMicros()) + "-" + name;
}

class DBImplTest : public ::testing::Test {
protected:
  void SetUp() override {
    options_.env = Env::Default();
    options_.create_if_missing = true;
    dbname_ = MakeDBName(::testing::UnitTest::GetInstance()->current_test_info()->name());
    DestroyDB(dbname_, options_);
  }

  void TearDown() override {
    db_.reset();
    DestroyDB(dbname_, options_);
  }

  void Open() {
    DB* db = nullptr;
    Status status = DB::Open(options_, dbname_, &db);
    ASSERT_TRUE(status.Ok()) << status.ToString();
    db_.reset(db);
  }

  void Reopen() {
    db_.reset();
    options_.create_if_missing = false;
    Open();
  }

  Status Get(const std::string& key, std::string* value) {
    return db_->Get(ReadOptions(), key, value);
  }

  DBImpl* impl() {
    return static_cast<DBImpl*>(db_.get());
  }

  int NumFilesAtLevel(int level) {
    std::string value;
    const std::string property = "leveldb.num-files-at-level" + std::to_string(level);
    if (!db_->GetProperty(property, &value)) {
      ADD_FAILURE() << "missing property " << property;
      return -1;
    }
    return std::stoi(value);
  }

  Options options_;
  std::string dbname_;
  std::unique_ptr<DB> db_;
};

TEST_F(DBImplTest, PutGetDeleteAndReopen) {
  Open();

  ASSERT_TRUE(db_->Put(WriteOptions(), "alpha", "one").Ok());
  std::string value;
  ASSERT_TRUE(Get("alpha", &value).Ok());
  EXPECT_EQ("one", value);

  ASSERT_TRUE(db_->Delete(WriteOptions(), "alpha").Ok());
  EXPECT_TRUE(Get("alpha", &value).IsNotFound());

  ASSERT_TRUE(db_->Put(WriteOptions(), "beta", "two").Ok());
  Reopen();

  EXPECT_TRUE(Get("alpha", &value).IsNotFound());
  ASSERT_TRUE(Get("beta", &value).Ok());
  EXPECT_EQ("two", value);
}

TEST_F(DBImplTest, SnapshotsReadStableSequence) {
  Open();

  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "v1").Ok());
  const Snapshot* snapshot = db_->GetSnapshot();
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "v2").Ok());

  ReadOptions snapshot_read;
  snapshot_read.snapshot = snapshot;
  std::string value;
  ASSERT_TRUE(db_->Get(snapshot_read, "key", &value).Ok());
  EXPECT_EQ("v1", value);

  ASSERT_TRUE(Get("key", &value).Ok());
  EXPECT_EQ("v2", value);
  db_->ReleaseSnapshot(snapshot);
}

TEST_F(DBImplTest, WriteBatchPersistsAtomically) {
  Open();

  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  ASSERT_TRUE(db_->Write(WriteOptions(), &batch).Ok());

  Reopen();

  std::string value;
  EXPECT_TRUE(Get("a", &value).IsNotFound());
  ASSERT_TRUE(Get("b", &value).Ok());
  EXPECT_EQ("2", value);
}

TEST_F(DBImplTest, ForcedMemTableFlushBuildsRecoverableTable) {
  Open();

  const std::string large_value(70 * 1024, 'x');
  ASSERT_TRUE(db_->Put(WriteOptions(), "large", large_value).Ok());
  ASSERT_TRUE(db_->Write(WriteOptions(), nullptr).Ok());

  std::vector<std::string> children;
  ASSERT_TRUE(options_.env->GetChildren(dbname_, &children).Ok());
  bool saw_table = false;
  for (const std::string& child : children) {
    if (child.size() > 4 && child.substr(child.size() - 4) == ".sst") {
      saw_table = true;
      break;
    }
  }
  EXPECT_TRUE(saw_table);

  Reopen();

  std::string value;
  ASSERT_TRUE(Get("large", &value).Ok());
  EXPECT_EQ(large_value, value);
}

TEST_F(DBImplTest, ManualCompactionMovesFilesAndPreservesValues) {
  Open();

  std::vector<std::string> keys;
  std::vector<std::string> values;
  for (int i = 0; i < 6; i++) {
    keys.push_back("key-" + std::to_string(i));
    values.push_back(std::string(1024, static_cast<char>('a' + i)));
    ASSERT_TRUE(db_->Put(WriteOptions(), keys.back(), values.back()).Ok());
    Status status = impl()->TEST_CompactMemTable();
    ASSERT_TRUE(status.Ok()) << status.ToString();
  }

  int source_level = -1;
  for (int level = 0; level + 1 < config::kNumLevels; level++) {
    if (NumFilesAtLevel(level) > 0) {
      source_level = level;
      break;
    }
  }
  ASSERT_GE(source_level, 0);
  ASSERT_GT(NumFilesAtLevel(source_level), 0);

  impl()->TEST_CompactRange(source_level, nullptr, nullptr);

  EXPECT_EQ(0, NumFilesAtLevel(source_level));
  for (size_t i = 0; i < keys.size(); i++) {
    std::string value;
    ASSERT_TRUE(Get(keys[i], &value).Ok()) << keys[i];
    EXPECT_EQ(values[i], value);
  }
}

}  // namespace
}  // namespace db
