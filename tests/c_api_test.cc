#include <cstring>
#include <string>

#include "c.h"
#include "env.h"
#include "gtest/gtest.h"

namespace db {
namespace {

std::string MakeDBName() {
  Env* env = Env::Default();
  std::string test_dir;
  EXPECT_TRUE(env->GetTestDirectory(&test_dir).Ok());
  return test_dir + "/c-api-test-" + std::to_string(env->NowMicros());
}

void ExpectNoError(char** err) {
  ASSERT_EQ(nullptr, *err) << *err;
}

TEST(CApiTest, BasicDBOperations) {
  const std::string dbname = MakeDBName();
  char* err = nullptr;

  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  leveldb_options_set_compression(options, leveldb_no_compression);
  leveldb_destroy_db(options, dbname.c_str(), &err);
  ExpectNoError(&err);

  leveldb_t* db = leveldb_open(options, dbname.c_str(), &err);
  ExpectNoError(&err);
  ASSERT_NE(nullptr, db);

  leveldb_writeoptions_t* write_options = leveldb_writeoptions_create();
  leveldb_readoptions_t* read_options = leveldb_readoptions_create();

  leveldb_put(db, write_options, "alpha", 5, "one", 3, &err);
  ExpectNoError(&err);

  size_t value_size = 0;
  char* value = leveldb_get(db, read_options, "alpha", 5, &value_size, &err);
  ExpectNoError(&err);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(3u, value_size);
  EXPECT_EQ(0, std::memcmp(value, "one", value_size));
  leveldb_free(value);

  leveldb_writebatch_t* batch = leveldb_writebatch_create();
  leveldb_writebatch_put(batch, "beta", 4, "two", 3);
  leveldb_writebatch_delete(batch, "alpha", 5);
  leveldb_write(db, write_options, batch, &err);
  ExpectNoError(&err);
  leveldb_writebatch_destroy(batch);

  value = leveldb_get(db, read_options, "alpha", 5, &value_size, &err);
  ExpectNoError(&err);
  EXPECT_EQ(nullptr, value);
  EXPECT_EQ(0u, value_size);

  value = leveldb_get(db, read_options, "beta", 4, &value_size, &err);
  ExpectNoError(&err);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(3u, value_size);
  EXPECT_EQ(0, std::memcmp(value, "two", value_size));
  leveldb_free(value);

  leveldb_iterator_t* iter = leveldb_create_iterator(db, read_options);
  leveldb_iter_seek_to_first(iter);
  ASSERT_TRUE(leveldb_iter_valid(iter));
  size_t key_size = 0;
  const char* key = leveldb_iter_key(iter, &key_size);
  EXPECT_EQ(4u, key_size);
  EXPECT_EQ(0, std::memcmp(key, "beta", key_size));
  leveldb_iter_get_error(iter, &err);
  ExpectNoError(&err);
  leveldb_iter_destroy(iter);

  char* property = leveldb_property_value(db, "leveldb.num-files-at-level0");
  ASSERT_NE(nullptr, property);
  leveldb_free(property);

  leveldb_readoptions_destroy(read_options);
  leveldb_writeoptions_destroy(write_options);
  leveldb_close(db);
  leveldb_destroy_db(options, dbname.c_str(), &err);
  ExpectNoError(&err);
  leveldb_options_destroy(options);
}

}  // namespace
}  // namespace db
