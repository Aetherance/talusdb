#include "table/two_level_iterator.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"

namespace db {
namespace {

using Entry = std::pair<std::string, std::string>;

class FakeIterator : public Iterator {
public:
  FakeIterator() = default;

  explicit FakeIterator(std::vector<Entry> entries)
      : entries_(std::move(entries)), index_(entries_.size()) {}

  void SetError(const Status& s) {
    error_ = s;
  }

  bool Valid() const override {
    return index_ < entries_.size();
  }

  void SeekToFirst() override {
    if (!error_.Ok()) return;
    index_ = 0;
  }

  void SeekToLast() override {
    if (!error_.Ok()) return;
    if (entries_.empty()) {
      index_ = 1;
    } else {
      index_ = entries_.size() - 1;
    }
  }

  void Seek(const Slice& target) override {
    if (!error_.Ok()) return;
    index_ = static_cast<size_t>(
        std::lower_bound(entries_.begin(), entries_.end(), target.ToString(),
                         [](const Entry& e, const std::string& t) { return e.first < t; }) -
        entries_.begin());
  }

  void Next() override {
    assert(Valid());
    ++index_;
  }

  void Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = entries_.size();
    } else {
      --index_;
    }
  }

  Slice Key() const override {
    assert(Valid());
    return entries_[index_].first;
  }

  Slice Value() const override {
    assert(Valid());
    return entries_[index_].second;
  }

  Status GetStatus() const override {
    return error_;
  }

private:
  std::vector<Entry> entries_;
  size_t index_;
  Status error_;
};

struct BlockFunctionContext {
  std::vector<std::vector<Entry>> blocks;
};

Iterator* TestBlockFunction(void* arg, const ReadOptions& options, const Slice& index_value) {
  (void)options;
  auto* ctx = static_cast<BlockFunctionContext*>(arg);
  std::string key = index_value.ToString();
  for (const auto& block : ctx->blocks) {
    if (!block.empty() && block[0].first == key) {
      return new FakeIterator(block);
    }
  }
  return new FakeIterator();
}

Iterator* BuildTwoLevelIterator(BlockFunctionContext* ctx, std::vector<Entry> index_entries) {
  auto* index_iter = new FakeIterator(std::move(index_entries));
  return NewTwoLevelIterator(index_iter, &TestBlockFunction, ctx, ReadOptions());
}

std::vector<Entry> MakeIndexEntries(const std::vector<std::vector<Entry>>& blocks) {
  std::vector<Entry> result;
  for (const auto& block : blocks) {
    if (!block.empty()) {
      result.push_back({block[0].first, block[0].first});
    }
  }
  return result;
}

TEST(TwoLevelIteratorTest, EmptyIndex) {
  BlockFunctionContext ctx;
  ctx.blocks = {};

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, {}));
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->GetStatus().Ok());
}

TEST(TwoLevelIteratorTest, SingleBlock) {
  std::vector<std::vector<Entry>> blocks = {{{"a", "1"}, {"b", "2"}, {"c", "3"}}};
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->SeekToFirst();
  EXPECT_TRUE(iter->Valid());
  EXPECT_EQ("a", iter->Key());
  EXPECT_EQ("1", iter->Value());

  iter->Next();
  EXPECT_TRUE(iter->Valid());
  EXPECT_EQ("b", iter->Key());
  EXPECT_EQ("2", iter->Value());

  iter->Next();
  EXPECT_TRUE(iter->Valid());
  EXPECT_EQ("c", iter->Key());
  EXPECT_EQ("3", iter->Value());

  iter->Next();
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, MultipleBlocksForward) {
  std::vector<std::vector<Entry>> blocks = {
      {{"a", "1"}, {"b", "2"}},
      {{"c", "3"}, {"d", "4"}},
      {{"e", "5"}},
  };
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  std::vector<Entry> expected = {{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}};
  iter->SeekToFirst();
  for (const auto& [k, v] : expected) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << k;
    EXPECT_EQ(k, iter->Key());
    EXPECT_EQ(v, iter->Value());
    iter->Next();
  }
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, MultipleBlocksBackward) {
  std::vector<std::vector<Entry>> blocks = {
      {{"a", "1"}, {"b", "2"}},
      {{"c", "3"}, {"d", "4"}},
      {{"e", "5"}},
  };
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  std::vector<Entry> expected = {{"e", "5"}, {"d", "4"}, {"c", "3"}, {"b", "2"}, {"a", "1"}};
  iter->SeekToLast();
  for (const auto& [k, v] : expected) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << k;
    EXPECT_EQ(k, iter->Key());
    EXPECT_EQ(v, iter->Value());
    iter->Prev();
  }
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, SeekAcrossBlocks) {
  std::vector<std::vector<Entry>> blocks = {
      {{"a", "1"}, {"b", "2"}},
      {{"c", "3"}, {"d", "4"}},
      {{"e", "5"}},
  };
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->Seek("c");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("c", iter->Key());
  EXPECT_EQ("3", iter->Value());

  iter->Seek("a");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a", iter->Key());
  EXPECT_EQ("1", iter->Value());

  iter->Seek("e");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("e", iter->Key());
  EXPECT_EQ("5", iter->Value());
}

TEST(TwoLevelIteratorTest, SeekBeforeFirst) {
  std::vector<std::vector<Entry>> blocks = {{{"b", "2"}, {"c", "3"}}};
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->Seek("a");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("b", iter->Key());
}

TEST(TwoLevelIteratorTest, SeekAfterLast) {
  std::vector<std::vector<Entry>> blocks = {{{"a", "1"}, {"b", "2"}}};
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->Seek("z");
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, SkipEmptyBlocksForward) {
  std::vector<std::vector<Entry>> blocks = {
      {{"a", "1"}}, {}, {{"c", "3"}}, {}, {{"e", "5"}},
  };
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a", iter->Key());
  EXPECT_EQ("1", iter->Value());

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("c", iter->Key());
  EXPECT_EQ("3", iter->Value());

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("e", iter->Key());
  EXPECT_EQ("5", iter->Value());

  iter->Next();
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, SkipEmptyBlocksBackward) {
  std::vector<std::vector<Entry>> blocks = {
      {{"a", "1"}}, {}, {{"c", "3"}}, {}, {{"e", "5"}},
  };
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  std::unique_ptr<Iterator> iter(BuildTwoLevelIterator(&ctx, MakeIndexEntries(blocks)));

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("e", iter->Key());
  EXPECT_EQ("5", iter->Value());

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("c", iter->Key());
  EXPECT_EQ("3", iter->Value());

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a", iter->Key());
  EXPECT_EQ("1", iter->Value());

  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(TwoLevelIteratorTest, IndexErrorPropagates) {
  std::vector<std::vector<Entry>> blocks = {{{"a", "1"}}};
  BlockFunctionContext ctx;
  ctx.blocks = blocks;

  auto* index_iter = new FakeIterator(MakeIndexEntries(blocks));
  index_iter->SetError(Status::IOError("index error"));
  std::unique_ptr<Iterator> iter(
      NewTwoLevelIterator(index_iter, &TestBlockFunction, &ctx, ReadOptions()));

  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_EQ(Status::Code::kIOError, iter->GetStatus().GetCode());
  EXPECT_EQ("index error", iter->GetStatus().Message());
}

Iterator* ErrorBlockFunction(void* arg, const ReadOptions& options, const Slice& index_value) {
  (void)arg;
  (void)options;
  (void)index_value;
  auto* it = new FakeIterator();
  it->SetError(Status::Corruption("data error"));
  return it;
}

TEST(TwoLevelIteratorTest, DataBlockErrorPropagates) {
  BlockFunctionContext ctx;
  ctx.blocks = {{}};

  auto* index_iter = new FakeIterator({{"block1", "block1"}});
  std::unique_ptr<Iterator> iter(
      NewTwoLevelIterator(index_iter, &ErrorBlockFunction, &ctx, ReadOptions()));

  iter->SeekToFirst();
  EXPECT_EQ(Status::Code::kCorruption, iter->GetStatus().GetCode());
  EXPECT_EQ("data error", iter->GetStatus().Message());
}

}  // namespace
}  // namespace db
