#include "db/log_writer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "db/log_format.h"
#include "env.h"
#include "gtest/gtest.h"
#include "slice.h"
#include "status.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace db {
namespace log {

// ========== Test infrastructure ==========

class StringDest : public WritableFile {
public:
  std::string contents_;

  Status Close() override {
    return Status::OkStatus();
  }
  Status Flush() override {
    return Status::OkStatus();
  }
  Status Sync() override {
    return Status::OkStatus();
  }
  Status Append(const Slice& data) override {
    contents_.append(data.Data(), data.Size());
    return Status::OkStatus();
  }
};

struct PhysicalRecord {
  RecordType type;
  std::string data;
  uint32_t crc;
  size_t offset;
};

static std::vector<PhysicalRecord> GetPhysicalRecords(const std::string& src) {
  std::vector<PhysicalRecord> records;
  size_t pos = 0;
  while (pos + kHeaderSize <= src.size()) {
    size_t block_offset = pos % kBlockSize;
    if (block_offset > kBlockSize - kHeaderSize) {
      pos += kBlockSize - block_offset;
      continue;
    }
    const char* p = src.data() + pos;
    uint32_t crc = DecodeFixed32(p);
    uint32_t length = (static_cast<uint32_t>(p[4]) & 0xff) | (static_cast<uint32_t>(p[5]) & 0xff)
                                                                 << 8;
    uint8_t type = static_cast<uint8_t>(p[6]);

    if (type == kZeroType && length == 0) {
      pos += kHeaderSize;
      continue;
    }
    if (kHeaderSize + length > src.size() - pos) {
      break;
    }
    records.push_back(
        {static_cast<RecordType>(type), std::string(p + kHeaderSize, length), crc, pos});
    pos += kHeaderSize + length;
  }
  return records;
}

static std::vector<std::string> GetLogicalRecords(const std::string& src) {
  auto phys = GetPhysicalRecords(src);
  std::vector<std::string> logical;
  std::string current;
  for (auto& r : phys) {
    switch (r.type) {
      case kFullType:
        logical.push_back(r.data);
        break;
      case kFirstType:
        current = r.data;
        break;
      case kMiddleType:
        current += r.data;
        break;
      case kLastType:
        current += r.data;
        logical.push_back(current);
        current.clear();
        break;
      default:
        break;
    }
  }
  return logical;
}

static std::string BigString(const std::string& partial, size_t n) {
  std::string result;
  while (result.size() < n) result.append(partial);
  result.resize(n);
  return result;
}

static std::string NumberString(int n) {
  char buf[50];
  std::snprintf(buf, sizeof(buf), "%d.", n);
  return std::string(buf);
}

// ========== Tests ==========

TEST(LogWriterTest, Empty) {
  StringDest dest;
  Writer writer(&dest);
  ASSERT_TRUE(writer.AddRecord(Slice("")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(1, records.size());
  EXPECT_EQ(kFullType, records[0].type);
  EXPECT_EQ("", records[0].data);
}

TEST(LogWriterTest, ReadWrite) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice("foo")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("bar")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("xxxx")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(4, records.size());
  EXPECT_EQ(kFullType, records[0].type);
  EXPECT_EQ("foo", records[0].data);
  EXPECT_EQ(kFullType, records[1].type);
  EXPECT_EQ("bar", records[1].data);
  EXPECT_EQ(kFullType, records[2].type);
  EXPECT_EQ("", records[2].data);
  EXPECT_EQ(kFullType, records[3].type);
  EXPECT_EQ("xxxx", records[3].data);
}

TEST(LogWriterTest, ManyBlocks) {
  StringDest dest;
  Writer writer(&dest);

  const int N = 10000;
  for (int i = 0; i < N; i++) {
    ASSERT_TRUE(writer.AddRecord(Slice(NumberString(i))).Ok());
  }

  auto records = GetLogicalRecords(dest.contents_);
  ASSERT_EQ(N, records.size());
  for (int i = 0; i < N; i++) {
    EXPECT_EQ(NumberString(i), records[i]);
  }
}

TEST(LogWriterTest, Fragmentation) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice("small")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice(BigString("medium", 50000))).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice(BigString("large", 100000))).Ok());

  auto records = GetLogicalRecords(dest.contents_);
  ASSERT_EQ(3, records.size());
  EXPECT_EQ("small", records[0]);
  EXPECT_EQ(BigString("medium", 50000), records[1]);
  EXPECT_EQ(BigString("large", 100000), records[2]);

  auto phys = GetPhysicalRecords(dest.contents_);
  EXPECT_LT(1, phys.size());
  EXPECT_EQ(kFullType, phys[0].type);
}

TEST(LogWriterTest, FragmentationTypes) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice(BigString("x", kBlockSize + kHeaderSize))).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_GE(records.size(), 1);
  EXPECT_EQ(kFirstType, records[0].type);
  EXPECT_EQ(BigString("x", kBlockSize - kHeaderSize), records[0].data);

  if (records.size() >= 2) {
    EXPECT_EQ(kLastType, records[1].type);
    size_t leftover = (kBlockSize + kHeaderSize) - (kBlockSize - kHeaderSize);
    EXPECT_EQ(leftover, records[1].data.size());
  }
}

TEST(LogWriterTest, MarginalTrailer) {
  StringDest dest;
  Writer writer(&dest);

  const int n = kBlockSize - 2 * kHeaderSize;
  ASSERT_TRUE(writer.AddRecord(Slice(BigString("foo", n))).Ok());
  ASSERT_EQ(kBlockSize - kHeaderSize, dest.contents_.size());

  ASSERT_TRUE(writer.AddRecord(Slice("")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("bar")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(3, records.size());
  EXPECT_EQ(BigString("foo", n), records[0].data);
  EXPECT_EQ("", records[1].data);
  EXPECT_EQ("bar", records[2].data);
}

TEST(LogWriterTest, ShortTrailer) {
  StringDest dest;
  Writer writer(&dest);

  const int n = kBlockSize - 2 * kHeaderSize + 4;
  ASSERT_TRUE(writer.AddRecord(Slice(BigString("foo", n))).Ok());

  ASSERT_TRUE(writer.AddRecord(Slice("")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("bar")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(3, records.size());
  EXPECT_EQ(BigString("foo", n), records[0].data);
  EXPECT_EQ("", records[1].data);
  EXPECT_EQ("bar", records[2].data);
}

TEST(LogWriterTest, AlignedEof) {
  StringDest dest;
  Writer writer(&dest);

  const int n = kBlockSize - 2 * kHeaderSize + 4;
  ASSERT_TRUE(writer.AddRecord(Slice(BigString("foo", n))).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(1, records.size());
  EXPECT_EQ(BigString("foo", n), records[0].data);
}

TEST(LogWriterTest, OpenForAppend) {
  StringDest dest;
  {
    Writer writer(&dest);
    ASSERT_TRUE(writer.AddRecord(Slice("hello")).Ok());
  }
  {
    Writer writer(&dest, dest.contents_.size());
    ASSERT_TRUE(writer.AddRecord(Slice("world")).Ok());
  }

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(2, records.size());
  EXPECT_EQ("hello", records[0].data);
  EXPECT_EQ("world", records[1].data);
}

TEST(LogWriterTest, CRCIsCorrect) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice("hello")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(1, records.size());

  uint32_t expected = crc32c::Value("\x01", 1);
  expected = crc32c::Extend(expected, "hello", 5);
  expected = crc32c::Mask(expected);
  EXPECT_EQ(expected, records[0].crc);
}

TEST(LogWriterTest, RecordOffsetsAreSequential) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice("first")).Ok());
  ASSERT_TRUE(writer.AddRecord(Slice("second")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(2, records.size());
  EXPECT_EQ(0, records[0].offset);
  EXPECT_EQ(kHeaderSize + 5, records[1].offset);
}

TEST(LogWriterTest, TotalBytesMatchFragmentSizes) {
  StringDest dest;
  Writer writer(&dest);

  ASSERT_TRUE(writer.AddRecord(Slice("abcdefghij")).Ok());

  auto records = GetPhysicalRecords(dest.contents_);
  ASSERT_EQ(1, records.size());
  EXPECT_EQ(10, records[0].data.size());

  size_t expected_total = kHeaderSize + records[0].data.size();
  EXPECT_EQ(expected_total, dest.contents_.size());
}

}  // namespace log
}  // namespace db
