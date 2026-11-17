// src/engine/factory_content.h - default tables 1-8 and waves 1-8 (WP5).
// Original content; the values may be tuned by ear (WP10). Slots 9-16 stay empty.
#pragma once

#include "table_bank.h"

namespace mpcboy {

// Clears `c`, then fills tables 0..7 and waves 0..7.
void loadFactoryContent(Content& c);

// On-screen names, at most 8 characters. Slots 8..15 return "".
const char* factoryTableName(int slot);
const char* factoryWaveName(int slot);

}  // namespace mpcboy
