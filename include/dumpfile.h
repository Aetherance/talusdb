#pragma once

#include <string>

#include "env.h"
#include "status.h"

namespace db {

// Dump the contents of a LevelDB storage file in a human-readable text format.
Status DumpFile(Env* env, const std::string& fname, WritableFile* dst);

}  // namespace db
