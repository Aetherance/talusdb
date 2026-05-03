#include <cstddef>
#include <cstdint>

#include "comparator.h"
#include "iterator.h"
namespace db {
struct BlockContents;
class Comparator;

// +-------------------------+
// | shared (varint)         |
// +-------------------------+
// | non_shared (varint)     |
// +-------------------------+
// | value_length (varint)   |
// +-------------------------+
// | key_delta               |
// | (non_shared )           |
// +-------------------------+
// | value                   |
// | (value_length )         |
// +-------------------------+

class Block {
public:
  explicit Block(const BlockContents& contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block();

  size_t size() const {
    return size_;
  }

  Iterator* NewIterator(const Comparator* comparator);

private:
  class Iter;

  uint32_t NumRestarts() const;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_;
  bool owned_;
};
}  // namespace db