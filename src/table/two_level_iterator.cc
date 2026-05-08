#include "two_level_iterator.h"

#include <cassert>
#include <string>

#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"

namespace db {

namespace {

typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                   const ReadOptions& options);

  ~TwoLevelIterator() override;

  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  bool Valid() const override {
    return data_iter_ != nullptr && data_iter_->Valid();
  }
  Slice Key() const override {
    assert(Valid());
    return data_iter_->Key();
  }
  Slice Value() const override {
    assert(Valid());
    return data_iter_->Value();
  }
  Status GetStatus() const override {
    if (!index_iter_->GetStatus().Ok()) {
      return index_iter_->GetStatus();
    } else if (data_iter_ != nullptr && !data_iter_->GetStatus().Ok()) {
      return data_iter_->GetStatus();
    } else {
      return status_;
    }
  }

private:
  void SaveError(const Status& s) {
    if (status_.Ok() && !s.Ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataBlock();

  BlockFunction block_function_;
  void* arg_;
  const ReadOptions options_;
  Status status_;
  Iterator* index_iter_;
  Iterator* data_iter_;
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() {
  delete index_iter_;
  delete data_iter_;
}

void TwoLevelIterator::Seek(const Slice& target) {
  index_iter_->Seek(target);
  InitDataBlock();
  if (data_iter_ != nullptr) data_iter_->Seek(target);
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
  index_iter_->SeekToFirst();
  InitDataBlock();
  if (data_iter_ != nullptr) data_iter_->SeekToFirst();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  index_iter_->SeekToLast();
  InitDataBlock();
  if (data_iter_ != nullptr) data_iter_->SeekToLast();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_->Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_->Prev();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (data_iter_ == nullptr || !data_iter_->Valid()) {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_->Next();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (data_iter_ == nullptr || !data_iter_->Valid()) {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_->Prev();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
  if (data_iter_ != nullptr) SaveError(data_iter_->GetStatus());
  delete data_iter_;
  data_iter_ = data_iter;
}

void TwoLevelIterator::InitDataBlock() {
  if (!index_iter_->Valid()) {
    SetDataIterator(nullptr);
  } else {
    Slice handle = index_iter_->Value();
    if (data_iter_ != nullptr && handle.ToString() == data_block_handle_) {
    } else {
      Iterator* iter = (*block_function_)(arg_, options_, handle);
      data_block_handle_ = handle.ToString();
      SetDataIterator(iter);
    }
  }
}

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace db
