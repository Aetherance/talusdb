#pragma once

namespace db {
class Comparator;
class Iterator;

Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children, int n);
}  // namespace db