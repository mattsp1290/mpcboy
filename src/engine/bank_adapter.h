// src/engine/bank_adapter.h - builds the RackboyBank view the table interpreter
// reads from the live Content (WP5). Audio thread only; no allocation (the
// RackboyBank is a member of the caller).
#pragma once

#include "rackboy_sav_types.h"
#include "table_bank.h"

namespace mpcboy {

// Tables 0..15 and waves 0..15 become occupied; slots 16+ are left as
// bank_reset made them (unoccupied). `bank` must have been bank_reset once.
void syncBank(rackboy::sav::RackboyBank& bank, const Content& c);

// bank_reset + syncBank.
void initBank(rackboy::sav::RackboyBank& bank, const Content& c);

}  // namespace mpcboy
