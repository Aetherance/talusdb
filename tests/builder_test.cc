#include "db/builder.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "env.h"
#include "gtest/gtest.h"
#include "iterator.h"
#include "options.h"

namespace db {
namespace {

class VectorIterator : public Iterator {
public:
  VectorIterator(const InternalKeyComparator* comparator,
                 std::vector<std::pair<std::string, std::string>> entries)
      : comparator_(comparator), entries_(std::move(entries)), index_(entries_.size()) {
    std::sort(entries_.begin(), entries_.end(), [this](const auto& lhs, const auto& rhs) {
      return comparator_->Compare(lhs.first, rhs.first) < 0;
    });
  }

  bool Valid() const override {
    return index_ < entries_.size();
  }

  void SeekToFirst() override {
    index_ = 0;
  }

  void SeekToLast() override {
    index_ = entries_.empty() ? entries_.size() : entries_.size() - 1;
  }

  void Seek(const Slice& target) override {
    auto iter = std::lower_bound(entries_.begin(), entries_.end(), target,
                                 [this](const auto& entry, const Slice& key) {
                                   return comparator_->Compare(entry.first, key) < 0;
                                 });
    index_ = static_cast<size_t>(iter - entries_.begin());
  }

  void Next() override {
    ASSERT_TRUE(Valid());
    index_++;
  }

  void Prev() override {
    ASSERT_TRUE(Valid());
    if (index_ == 0) {
      index_ = entries_.size();
    } else {
      index_--;
    }
  }

  Slice Key() const override {
    EXPECT_TRUE(Valid());
    return entries_[index_].first;
  }

  Slice Value() const override {
    EXPECT_TRUE(Valid());
    return entries_[index_].second;
  }

  Status GetStatus() const override {
    return status_;
  }

  void SetStatus(const Status& status) {
    status_ = status;
  }

private:
  const InternalKeyComparator* const comparator_;
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t index_;
  Status status_;
};

class BuilderTest : public ::testing::Test {
protected:
  BuilderTest() : env_(Env::Default()), internal_comparator_(BytewiseComparator()) {
    options_.env = env_;
    options_.comparator = &internal_comparator_;
    options_.compression = kNoCompression;
  }

  void SetUp() override {
    std::string test_dir;
    ASSERT_TRUE(env_->GetTestDirectory(&test_dir).Ok());
    dbname_ = test_dir + "/builder_test";
    env_->RemoveDir(dbname_);
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

  void Add(SequenceNumber sequence, ValueType type, const std::string& key,
           const std::string& value = "") {
    std::string internal_key;
    AppendInternalKey(&internal_key, ParsedInternalKey(key, sequence, type));
    entries_.emplace_back(std::move(internal_key), value);
  }

  std::unique_ptr<VectorIterator> NewInput() {
    return std::make_unique<VectorIterator>(&internal_comparator_, entries_);
  }

  Env* env_;
  InternalKeyComparator internal_comparator_;
  Options options_;
  std::string dbname_;
  std::vector<std::pair<std::string, std::string>> entries_;
};

TEST_F(BuilderTest, BuildsTableAndFillsMetadata) {
  Add(100, kTypeValue, "alpha", "one");
  Add(90, kTypeDeletion, "bravo");
  Add(80, kTypeValue, "charlie", "three");
  std::unique_ptr<VectorIterator> input = NewInput();
  TableCache cache(dbname_, options_, 10);
  FileMetaData meta;
  meta.number = 7;

  Status status = BuildTable(dbname_, env_, options_, &cache, input.get(), &meta);

  ASSERT_TRUE(status.Ok()) << status.ToString();
  EXPECT_GT(meta.file_size, 0);
  EXPECT_EQ("alpha", meta.smallest.user_key().ToString());
  EXPECT_EQ("charlie", meta.largest.user_key().ToString());
  EXPECT_TRUE(env_->FileExists(TableFileName(dbname_, meta.number)));

  std::unique_ptr<Iterator> table_iter(
      cache.NewIterator(ReadOptions(), meta.number, meta.file_size));
  table_iter->SeekToFirst();
  ASSERT_TRUE(table_iter->Valid());
  EXPECT_EQ("alpha", ExtractUserKey(table_iter->Key()).ToString());
  EXPECT_EQ("one", table_iter->Value().ToString());
  table_iter->Next();
  ASSERT_TRUE(table_iter->Valid());
  EXPECT_EQ("bravo", ExtractUserKey(table_iter->Key()).ToString());
  table_iter->Next();
  ASSERT_TRUE(table_iter->Valid());
  EXPECT_EQ("charlie", ExtractUserKey(table_iter->Key()).ToString());
  table_iter->Next();
  EXPECT_FALSE(table_iter->Valid());
  EXPECT_TRUE(table_iter->GetStatus().Ok());
}

TEST_F(BuilderTest, EmptyInputProducesNoTableFile) {
  std::unique_ptr<VectorIterator> input = NewInput();
  TableCache cache(dbname_, options_, 10);
  FileMetaData meta;
  meta.number = 8;

  Status status = BuildTable(dbname_, env_, options_, &cache, input.get(), &meta);

  ASSERT_TRUE(status.Ok()) << status.ToString();
  EXPECT_EQ(0, meta.file_size);
  EXPECT_FALSE(env_->FileExists(TableFileName(dbname_, meta.number)));
}

TEST_F(BuilderTest, InputIteratorErrorRemovesOutputFile) {
  Add(100, kTypeValue, "alpha", "one");
  std::unique_ptr<VectorIterator> input = NewInput();
  input->SetStatus(Status::Corruption("input failed"));
  TableCache cache(dbname_, options_, 10);
  FileMetaData meta;
  meta.number = 9;

  Status status = BuildTable(dbname_, env_, options_, &cache, input.get(), &meta);

  EXPECT_EQ(Status::Code::kCorruption, status.GetCode());
  EXPECT_FALSE(env_->FileExists(TableFileName(dbname_, meta.number)));
}

}  // namespace
}  // namespace db
