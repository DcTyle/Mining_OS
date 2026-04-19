#pragma once

#include <string>

#include "qbit_miner/runtime/substrate_phase_programs.hpp"
#include "qbit_miner/telemetry/mining_metrics.hpp"

namespace qbit_miner {

struct SubstrateControlIngress {
    RewardIntervalUnit reward_unit = RewardIntervalUnit::PerHour;
    bool audio_enabled = true;
    bool paused = false;
    bool compact_layout = false;
    bool always_on_top = false;
    bool viewport_readonly = true;
    double ui_refresh_ms = 100.0;
    double user_input_phase = 0.0;
    std::string ingress_id;
    std::string authoritative_program_source;
};

class SubstrateControlSurface {
public:
    [[nodiscard]] SubstrateControlIngress build_ingress(
        RewardIntervalUnit reward_unit,
        bool audio_enabled,
        bool paused,
        bool compact_layout,
        bool always_on_top,
        double user_input_phase
    ) const;
};

}  // namespace qbit_miner