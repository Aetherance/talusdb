#include "comparator.h"
#include "iterator.h"
#include "merger.h"

namespace db {
class MergingIterator : public Iterator {
public:
  MergingIterator(const Comparator* comparator, Iterator** children, int n)
      : comparator_(comparator),
        children_(new Iterator*[n]),
        n_(n),
        current_(nullptr),
        direction_(kForward) {
    for (int i = 0; i < n; i++) {
      children_[i] = children[i];
    }
  }

  ~MergingIterator() override {
    for (int i = 0; i < n_; i++) {
      delete children_[i];
    }
    delete[] children_;
  }

  bool Valid() const override {
    return (current_ != nullptr);
  }

  void SeekToFirst() override {
    for (int i = 0; i < n_; i++) {
      children_[i]->SeekToFirst();
    }
    FindSmallest();
    direction_ = kForward;
  }

  void SeekToLast() override {
    for (int i = 0; i < n_; i++) {
      children_[i]->SeekToLast();
    }
    FindLargest();
    direction_ = kReverse;
  }

  void Seek(const Slice& target) override {
    for (int i = 0; i < n_; i++) {
      children_[i]->Seek(target);
    }
    FindSmallest();
    direction_ = kForward;
  }

  void Next() override {
    assert(Valid());
    if (direction_ != kForward) {
      for (int i = 0; i < n_; i++) {
        Iterator* child = children_[i];
        if (child != current_) {
          child->Seek(Key());
          if (child->Valid() && comparator_->Compare(Key(), child->Key()) == 0) {
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }
    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    assert(Valid());
    if (direction_ != kReverse) {
      for (int i = 0; i < n_; i++) {
        Iterator* child = children_[i];
        if (child != current_) {
          child->Seek(Key());
          if (child->Valid()) {
            child->Prev();
          } else {
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }
    current_->Prev();
    FindLargest();
  }

  Slice Key() const override {
    assert(Valid());
    return current_->Key();
  }
  Slice Value() const override {
    assert(Valid());
    return current_->Value();
  }
  Status GetStatus() const override {
    Status status;
    for (int i = 0; i < n_; i++) {
      status = children_[i]->GetStatus();
      if (!status.Ok()) {
        break;
      }
    }
    return status;
  }

private:
  enum Direction { kForward, kReverse };

  void FindSmallest();
  void FindLargest();

  // We might want to use a heap in case there are lots of children.
  // For now we use a simple array since we expect a very small number
  // of children in leveldb.
  const Comparator* comparator_;
  Iterator** children_;
  int n_;
  Iterator* current_;
  Direction direction_;
};

void MergingIterator::FindSmallest() {
  Iterator* smallest = nullptr;
  for (int i = 0; i < n_; i++) {
    Iterator* child = children_[i];
    if (child->Valid()) {
      if (smallest == nullptr) {
        smallest = child;
      } else if (comparator_->Compare(child->Key(), smallest->Key()) < 0) {
        smallest = child;
      }
    }
  }
  current_ = smallest;
}

void MergingIterator::FindLargest() {
  Iterator* largest = nullptr;
  for (int i = n_ - 1; i >= 0; i--) {
    Iterator* child = children_[i];
    if (child->Valid()) {
      if (largest == nullptr) {
        largest = child;
      } else if (comparator_->Compare(child->Key(), largest->Key()) > 0) {
        largest = child;
      }
    }
  }
  current_ = largest;
}

Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children, int n) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyIterator();
  } else if (n == 1) {
    return children[0];
  } else {
    return new MergingIterator(comparator, children, n);
  }
}
}  // namespace db