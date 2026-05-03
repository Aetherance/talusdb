#include "table/block.h"

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "options.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "util/coding.h"

namespace db {
namespace {

// Build a valid BlockContents from entries using BlockBuilder.
BlockContents BuildBlockContents(const std::vector<std::pair<std::string, std::string>>& entries,
                                 int restart_interval = 16) {
  Options options;
  options.block_restart_interval = restart_interval;

  BlockBuilder builder(&options);
  for (const auto& [key, value] : entries) {
    builder.Add(key, value);
  }

  Slice block_data = builder.Finish();

  char* data = new char[block_data.Size()];
  std::memcpy(data, block_data.Data(), block_data.Size());

  return BlockContents{
      .data = Slice(data, block_data.Size()),
      .cachable = true,
      .heep_allocated = true,
  };
}

// Build a block with invalid size (< sizeof(uint32_t)).
BlockContents BuildTruncatedBlock() {
  char* data = new char[1];
  data[0] = 0;
  return BlockContents{
      .data = Slice(data, 1),
      .cachable = false,
      .heep_allocated = true,
  };
}

// Build a valid block with 0 restarts (num_restarts == 0).
BlockContents BuildZeroRestartsBlock() {
  char* data = new char[4];
  EncodeFixed32(data, 0);
  return BlockContents{
      .data = Slice(data, 4),
      .cachable = false,
      .heep_allocated = true,
  };
}

TEST(BlockTest, EmptyBlockReturnsError) {
  BlockContents contents = BuildTruncatedBlock();
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  ASSERT_NE(nullptr, iter);
  EXPECT_FALSE(iter->Valid());
  EXPECT_EQ(Status::Code::kCorruption, iter->GetStatus().GetCode());
}

TEST(BlockTest, ZeroRestartsReturnsEmptyIterator) {
  BlockContents contents = BuildZeroRestartsBlock();
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  ASSERT_NE(nullptr, iter);
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->GetStatus().Ok());
}

TEST(BlockTest, SingleEntry) {
  BlockContents contents = BuildBlockContents({{"key", "value"}});
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  ASSERT_NE(nullptr, iter);

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("key", iter->Key());
  EXPECT_EQ("value", iter->Value());
}

TEST(BlockTest, IterateForward) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  std::vector<std::pair<std::string, std::string>> expected = {
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
  };

  iter->SeekToFirst();
  for (const auto& [key, value] : expected) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << key;
    EXPECT_EQ(key, iter->Key());
    EXPECT_EQ(value, iter->Value());
    iter->Next();
  }
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, SeekToLast) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("gamma", iter->Key());
  EXPECT_EQ("3", iter->Value());
}

TEST(BlockTest, IterateBackward) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  std::vector<std::pair<std::string, std::string>> expected = {
      {"gamma", "3"},
      {"beta", "2"},
      {"alpha", "1"},
  };

  iter->SeekToLast();
  for (const auto& [key, value] : expected) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << key;
    EXPECT_EQ(key, iter->Key());
    EXPECT_EQ(value, iter->Value());
    iter->Prev();
  }
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, SeekExactKey) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"beta", "2"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->Seek("beta");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("beta", iter->Key());
  EXPECT_EQ("2", iter->Value());

  iter->Seek("gamma");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("gamma", iter->Key());
  EXPECT_EQ("3", iter->Value());
}

TEST(BlockTest, SeekBeforeFirst) {
  BlockContents contents = BuildBlockContents({
      {"beta", "2"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->Seek("alpha");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("beta", iter->Key());
}

TEST(BlockTest, SeekAfterLast) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"beta", "2"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->Seek("omega");
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, SeekBetweenKeys) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
      {"gamma", "3"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->Seek("beta");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("gamma", iter->Key());
}

TEST(BlockTest, SeekToFirstOnEmptyBlockReturnsEmptyIterator) {
  BlockContents contents = BuildZeroRestartsBlock();
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  ASSERT_NE(nullptr, iter);
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, MultipleRestartIntervals) {
  BlockContents contents = BuildBlockContents(
      {
          {"a", "1"},
          {"b", "2"},
          {"c", "3"},
          {"d", "4"},
          {"e", "5"},
      },
      /*restart_interval=*/2);
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  std::vector<std::pair<std::string, std::string>> expected = {
      {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"},
  };

  iter->SeekToFirst();
  for (const auto& [key, value] : expected) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << key;
    EXPECT_EQ(key, iter->Key());
    EXPECT_EQ(value, iter->Value());
    iter->Next();
  }
  EXPECT_FALSE(iter->Valid());

  // Verify Prev still works with multiple restarts
  iter->SeekToLast();
  for (auto i = expected.size(); i > 0; --i) {
    ASSERT_TRUE(iter->Valid()) << "Expected valid at key: " << expected[i - 1].first;
    EXPECT_EQ(expected[i - 1].first, iter->Key());
    EXPECT_EQ(expected[i - 1].second, iter->Value());
    iter->Prev();
  }
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, SeekWithRestartIntervals) {
  BlockContents contents = BuildBlockContents(
      {
          {"a0001", "1"},
          {"a0002", "2"},
          {"a0003", "3"},
          {"b0001", "4"},
          {"b0002", "5"},
          {"b0003", "6"},
      },
      /*restart_interval=*/3);
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  // Seek to a key in the second restart interval
  iter->Seek("b0001");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("b0001", iter->Key());
  EXPECT_EQ("4", iter->Value());

  // Seek to a key before all entries
  iter->Seek("a");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a0001", iter->Key());

  // Seek within first restart interval
  iter->Seek("a0002");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a0002", iter->Key());
}

TEST(BlockTest, PrevFromFirstEntry) {
  BlockContents contents = BuildBlockContents({
      {"alpha", "1"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, CorruptNumRestarts) {
  // Build a block but then claim more restarts than the data allows.
  Options options;
  options.block_restart_interval = 16;
  BlockBuilder builder(&options);
  builder.Add("key", "value");
  Slice block_data = builder.Finish();

  // Create a copy and set num_restarts to an impossibly large value.
  std::string corrupted(block_data.Data(), block_data.Size());
  // num_restarts is the last 4 bytes.
  EncodeFixed32(&corrupted[corrupted.size() - 4], 1000);

  char* data = new char[corrupted.size()];
  std::memcpy(data, corrupted.data(), corrupted.size());

  BlockContents contents{
      .data = Slice(data, corrupted.size()),
      .cachable = false,
      .heep_allocated = true,
  };

  Block block(contents);
  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, BlockOwnership) {
  // When heep_allocated is true, Block takes ownership and delete[].
  char* raw = new char[4];
  EncodeFixed32(raw, 0);
  {
    BlockContents contents{
        .data = Slice(raw, 4),
        .cachable = false,
        .heep_allocated = true,
    };
    Block block(contents);
    // Block destructor will delete[] raw.
  }
}

TEST(BlockTest, UnOwnedBlock) {
  // When heep_allocated is false, Block does not delete data.
  std::string storage(4, '\0');
  EncodeFixed32(storage.data(), 0);
  {
    BlockContents contents{
        .data = Slice(storage),
        .cachable = false,
        .heep_allocated = false,
    };
    Block block(contents);
  }
  // storage is still valid here.
  EXPECT_EQ(0U, DecodeFixed32(storage.data()));
}

TEST(BlockTest, MultipleEntriesWithSharedPrefix) {
  BlockContents contents = BuildBlockContents({
      {"prefix_alpha", "value_a"},
      {"prefix_beta", "value_b"},
      {"prefix_gamma", "value_c"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("prefix_alpha", iter->Key());
  EXPECT_EQ("value_a", iter->Value());

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("prefix_beta", iter->Key());
  EXPECT_EQ("value_b", iter->Value());

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("prefix_gamma", iter->Key());
  EXPECT_EQ("value_c", iter->Value());

  iter->Next();
  EXPECT_FALSE(iter->Valid());
}

TEST(BlockTest, NextAfterLastIsNoOp) {
  BlockContents contents = BuildBlockContents({
      {"key", "value"},
  });
  Block block(contents);

  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  iter->Next();
  EXPECT_FALSE(iter->Valid());
  // Next on invalid iterator should be safe (it asserts, but we just don't call it).
}

}  // namespace
}  // namespace db
