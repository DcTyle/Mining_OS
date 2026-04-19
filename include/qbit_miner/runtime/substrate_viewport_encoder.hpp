#pragma once

#include <array>
#include <string>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct SubstrateViewportFrame {
    std::string phase_id;
    std::array<double, 9> texture_map_9d {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 6> tensor_signature_6d {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> visual_rgba {0.0, 0.0, 0.0, 1.0};
    std::array<double, 4> material_pbr {0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> audio_channels {0.0, 0.0, 0.0, 0.0};
    std::array<double, 3> viewport_direction {0.0, 0.0, 1.0};
    double phase_lock_error = 0.0;
    double relock_pressure = 0.0;
    double anchor_evm_norm = 0.0;
    double anchor_correlation = 0.0;
    double sideband_energy_norm = 0.0;
    double group_delay_skew = 0.0;
    double dynamic_range_headroom = 0.0;
};

class SubstrateViewportEncoder {
public:
    [[nodiscard]] SubstrateViewportFrame encode_frame(const SubstrateTrace& trace) const;
};

}  // namespace qbit_miner