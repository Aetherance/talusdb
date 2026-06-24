#include "db/db_iter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "comparator.h"
#include "db/dbformat.h"
#include "gtest/gtest.h"
#include "iterator.h"

namespace db {
namespace {

class VectorInternalIterator : public Iterator {
public:
  VectorInternalIterator(const InternalKeyComparator* comparator,
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
    return Status::OkStatus();
  }

private:
  const InternalKeyComparator* const comparator_;
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t index_;
};

class DBIterTest : public ::testing::Test {
protected:
  DBIterTest() : internal_comparator_(BytewiseComparator()) {}

  void Add(SequenceNumber sequence, ValueType type, const std::string& key,
           const std::string& value = "") {
    std::string internal_key;
    AppendInternalKey(&internal_key, ParsedInternalKey(key, sequence, type));
    entries_.emplace_back(std::move(internal_key), value);
  }

  std::unique_ptr<Iterator> NewIter(SequenceNumber sequence) {
    return std::unique_ptr<Iterator>(
        NewDBIterator(nullptr, BytewiseComparator(),
                      new VectorInternalIterator(&internal_comparator_, entries_), sequence, 1));
  }

  std::vector<std::string> ForwardKeys(Iterator* iter) {
    std::vector<std::string> keys;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      keys.push_back(iter->Key().ToString());
    }
    EXPECT_TRUE(iter->GetStatus().Ok());
    return keys;
  }

  InternalKeyComparator internal_comparator_;
  std::vector<std::pair<std::string, std::string>> entries_;
};

TEST_F(DBIterTest, ForwardIterationReturnsNewestVisibleUserEntries) {
  Add(100, kTypeValue, "alpha", "new-alpha");
  Add(90, kTypeValue, "alpha", "old-alpha");
  Add(105, kTypeDeletion, "bravo");
  Add(100, kTypeValue, "bravo", "visible-before-delete");
  Add(80, kTypeValue, "charlie", "charlie-value");
  Add(120, kTypeValue, "delta", "future-delta");

  std::unique_ptr<Iterator> iter = NewIter(100);

  EXPECT_EQ((std::vector<std::string>{"alpha", "bravo", "charlie"}), ForwardKeys(iter.get()));
}

TEST_F(DBIterTest, DeletionAtSnapshotHidesOlderValues) {
  Add(100, kTypeValue, "alpha", "alpha-value");
  Add(105, kTypeDeletion, "bravo");
  Add(100, kTypeValue, "bravo", "old-bravo");
  Add(90, kTypeValue, "charlie", "charlie-value");

  std::unique_ptr<Iterator> iter = NewIter(110);

  EXPECT_EQ((std::vector<std::string>{"alpha", "charlie"}), ForwardKeys(iter.get()));
}

TEST_F(DBIterTest, SeekSkipsDeletedAndFutureEntries) {
  Add(100, kTypeValue, "alpha", "alpha-value");
  Add(105, kTypeDeletion, "bravo");
  Add(100, kTypeValue, "bravo", "old-bravo");
  Add(120, kTypeValue, "charlie", "future-charlie");
  Add(80, kTypeValue, "delta", "delta-value");

  std::unique_ptr<Iterator> iter = NewIter(110);
  iter->Seek("bravo");

  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("delta", iter->Key().ToString());
  EXPECT_EQ("delta-value", iter->Value().ToString());
}

TEST_F(DBIterTest, ReverseIterationAndDirectionSwitchingUseUserKeys) {
  Add(100, kTypeValue, "alpha", "alpha-value");
  Add(95, kTypeValue, "bravo", "new-bravo");
  Add(90, kTypeValue, "bravo", "old-bravo");
  Add(80, kTypeValue, "charlie", "charlie-value");

  std::unique_ptr<Iterator> iter = NewIter(100);

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("charlie", iter->Key().ToString());
  EXPECT_EQ("charlie-value", iter->Value().ToString());

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("bravo", iter->Key().ToString());
  EXPECT_EQ("new-bravo", iter->Value().ToString());

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("charlie", iter->Key().ToString());
}

}  // namespace
}  // namespace db
