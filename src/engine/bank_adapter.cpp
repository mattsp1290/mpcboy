#include "bank_adapter.h"

#include <cstring>

namespace mpcboy {

void syncBank(rackboy::sav::RackboyBank& bank, const Content& c)
{
    for (int i = 0; i < kNumTables; ++i)
    {
        bank.tables[i] = c.tables.tables[i];
        bank.tables[i].occupied = true;   // empty tables just do nothing
    }
    for (int i = 0; i < kNumWaves; ++i)
    {
        std::memcpy(bank.waves[i].samples, c.waves.waves[i], kWaveBytes);
        bank.waves[i].occupied = true;
    }
    bank.loaded = true;
}

void initBank(rackboy::sav::RackboyBank& bank, const Content& c)
{
    rackboy::sav::bank_reset(bank);
    syncBank(bank, c);
}

}  // namespace mpcboy
