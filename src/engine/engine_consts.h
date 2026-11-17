// src/engine/engine_consts.h - named constants shared by the engine and the plugin.
#pragma once

#include <cstdint>

namespace mpcboy {

// APU master registers (the table module names NR1x..NR4x; these three are ours).
constexpr uint16_t kNR50 = 0xFF24;   // master volume / VIN
constexpr uint16_t kNR51 = 0xFF25;   // channel panning
constexpr uint16_t kNR52 = 0xFF26;   // APU power

// DMG video frame rate (the LSDJ table tick rate in Free mode).
constexpr double kDmgFrameHz = 59.73;

}  // namespace mpcboy
