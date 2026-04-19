#pragma once

#include <string>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

enum class RewardIntervalUnit {
    PerMinute,
    PerHour,
    PerDay,
};

struct RewardProjection {
    double coin = 0.0;
    double value_usd = 0.0;
};

struct MiningTelemetryObservation {
    double graphics_frequency_hz = 0.0;
    double memory_frequency_hz = 0.0;
    double amplitude_norm = 0.0;
    double voltage_v = 0.0;
    double amperage_a = 0.0;
    double power_w = 0.0;
    double temperature_c = 0.0;
    double gpu_util_norm = 0.0;
    double memory_util_norm = 0.0;
    double thermal_interference_norm = 0.0;
    bool live = false;
    std::string provider;
};

struct RigMetrics {
    double hashrate_hs = 0.0;
    double submission_rate_per_s = 0.0;
    double submission_acceptance_rate = 0.0;
    double share_valid_rate = 0.0;
    double difficulty = 0.0;
    double device_power_w = 0.0;
    double device_temperature_c = 0.0;
    double gpu_util_norm = 0.0;
    double memory_util_norm = 0.0;
    double coherence = 0.0;
    RewardProjection reward;
};

struct PoolMetrics {
    double pool_hashrate_hs = 0.0;
    double pool_difficulty = 0.0;
    double pool_effort_norm = 0.0;
    double worker_pool_share_norm = 0.0;
    double submission_rate_per_s = 0.0;
    double acceptance_rate = 0.0;
    RewardProjection reward;
};

struct BlockchainMetrics {
    double network_difficulty = 0.0;
    double network_hashrate_hs = 0.0;
    double block_reward_coin = 0.0;
    double next_halving_progress_norm = 0.0;
    RewardProjection reward;
};

struct MiningMetricsSnapshot {
    RewardIntervalUnit reward_unit = RewardIntervalUnit::PerHour;
    RigMetrics rig;
    PoolMetrics pool;
    BlockchainMetrics blockchain;
    std::string trace_id;
    std::string provider;
    std::string rig_label;
    std::string pool_label;
    std::string network_label;
    std::string reward_unit_label;
    std::string status_line;
    bool live = false;
};

[[nodiscard]] std::string reward_unit_label(RewardIntervalUnit unit);
[[nodiscard]] double reward_unit_scale(RewardIntervalUnit unit);
[[nodiscard]] MiningMetricsSnapshot build_mining_metrics_snapshot(
    const SubstrateTrace& trace,
    const MiningTelemetryObservation& observation,
    RewardIntervalUnit unit
);

}  // namespace qbit_miner