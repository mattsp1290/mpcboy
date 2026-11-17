#include "table_bank.h"

#include <cstring>

namespace mpcboy {

void clearContent(Content& c)
{
    for (int i = 0; i < kNumTables; ++i) rackboy::sav::table_reset(c.tables.tables[i]);
    std::memset(c.waves.waves, 0, sizeof(c.waves.waves));
}

}  // namespace mpcboy
