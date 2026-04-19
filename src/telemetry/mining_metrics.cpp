#include "qbit_miner/telemetry/mining_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace qbit_miner {

namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double safe_abs_norm(double value, double scale) {
    if (scale <= 0.0) {
        return 0.0;
    }
    return clamp01(std::abs(value) / scale);
}

}  // namespace

std::string reward_unit_label(RewardIntervalUnit unit) {
    switch (unit) {
    case RewardIntervalUnit::PerMinute:
        return "per minute";
    case RewardIntervalUnit::PerHour:
        return "per hour";
    case RewardIntervalUnit::PerDay:
        return "per day";
    }
    return "per hour";
}

double reward_unit_scale(RewardIntervalUnit unit) {
    switch (unit) {
    case RewardIntervalUnit::PerMinute:
        return 60.0;
    case RewardIntervalUnit::PerHour:
        return 3600.0;
    case RewardIntervalUnit::PerDay:
        return 86400.0;
    }
    return 3600.0;
}

MiningMetricsSnapshot build_mining_metrics_snapshot(
    const SubstrateTrace& trace,
    const MiningTelemetryObservation& observation,
    RewardIntervalUnit unit
) {
    const double coherence = clamp01(trace.photonic_identity.coherence);
    const double coupling = clamp01(trace.coupling_strength);
    const double conservation = clamp01(trace.trajectory_conservation_score);
    const double overlap = clamp01(trace.zero_point_overlap_score);
    const double phase_alignment = clamp01(trace.constant_phase_alignment);
    const double thermal_margin = clamp01(1.0 - observation.thermal_interference_norm);
    const double utilization = clamp01((0.60 * observation.gpu_util_norm) + (0.40 * observation.memory_util_norm));
    const double live_factor = observation.live ? 1.0 : 0.94;
    const double reward_scale = reward_unit_scale(unit);

    const double rig_hashrate_hs = std::max(
        1.0,
        observation.graphics_frequency_hz * 0.18
            * (0.45 + (0.55 * coupling))
            * (0.50 + (0.50 * conservation))
            * (0.45 + (0.55 * utilization))
            * (0.60 + (0.40 * live_factor))
    );

    const double rig_difficulty = std::max(
        128.0,
        256.0
            * (1.0 + (8.0 * (1.0 - overlap))
            + (6.0 * clamp01(trace.derived_constants.zero_point_line_distance))
            + (2.0 * safe_abs_norm(trace.temporal_dynamics_noise, 1.0)))
    );

    const double submission_rate_per_s = std::max(0.01, rig_hashrate_hs / (rig_difficulty * 3500000.0));
    const double acceptance_rate = clamp01(
        (0.42 * conservation)
        + (0.30 * phase_alignment)
        + (0.28 * thermal_margin)
    );
    const double share_valid_rate = clamp01(
        (0.40 * overlap)
        + (0.35 * coherence)
        + (0.25 * clamp01(1.0 - trace.temporal_dynamics_noise))
    );

    const double network_difficulty = 9.2e13
        * (0.92 + (0.12 * phase_alignment) + (0.06 * utilization));
    const double network_hashrate_hs = std::max(1.0, network_difficulty * 4294967296.0 / 600.0);

    const double pool_share_norm = clamp01(0.008 + (0.024 * coupling) + (0.012 * phase_alignment));
    const double pool_hashrate_hs = std::max(rig_hashrate_hs * 1200.0, network_hashrate_hs * pool_share_norm);
    const double pool_difficulty = network_difficulty * std::max(0.001, pool_share_norm);
    const double pool_effort_norm = clamp01((0.55 * acceptance_rate) + (0.45 * share_valid_rate));
    const double worker_pool_share_norm = clamp01(rig_hashrate_hs / std::max(pool_hashrate_hs, 1.0));

    const double block_reward_coin = 3.125;
    const double blocks_per_interval = reward_scale / 600.0;
    const double worker_interval_reward_coin = block_reward_coin
        * blocks_per_interval
        * (rig_hashrate_hs / std::max(network_hashrate_hs, 1.0))
        * acceptance_rate
        * share_valid_rate;
    const double pool_interval_reward_coin = block_reward_coin
        * blocks_per_interval
        * (pool_hashrate_hs / std::max(network_hashrate_hs, 1.0))
        * pool_effort_norm;
    const double network_interval_reward_coin = block_reward_coin * blocks_per_interval;

    const double btc_price_usd = 68000.0 + (9000.0 * clamp01(trace.reverse_causal_flux_coherence));

    MiningMetricsSnapshot snapshot;
    snapshot.reward_unit = unit;
    snapshot.trace_id = trace.photonic_identity.trace_id;
    snapshot.provider = observation.provider;
    snapshot.rig_label = trace.photonic_identity.gpu_device_id.empty() ? "Rig-01" : trace.photonic_identity.gpu_device_id;
    snapshot.pool_label = "Quantum Pool Alpha";
    snapshot.network_label = "Bitcoin";
    snapshot.reward_unit_label = reward_unit_label(unit);
    snapshot.live = observation.live;

    snapshot.rig.hashrate_hs = rig_hashrate_hs;
    snapshot.rig.submission_rate_per_s = submission_rate_per_s;
    snapshot.rig.submission_acceptance_rate = acceptance_rate;
    snapshot.rig.share_valid_rate = share_valid_rate;
    snapshot.rig.difficulty = rig_difficulty;
    snapshot.rig.device_power_w = observation.power_w;
    snapshot.rig.device_temperature_c = observation.temperature_c;
    snapshot.rig.gpu_util_norm = observation.gpu_util_norm;
    snapshot.rig.memory_util_norm = observation.memory_util_norm;
    snapshot.rig.coherence = coherence;
    snapshot.rig.reward.coin = worker_interval_reward_coin;
    snapshot.rig.reward.value_usd = worker_interval_reward_coin * btc_price_usd;

    snapshot.pool.pool_hashrate_hs = pool_hashrate_hs;
    snapshot.pool.pool_difficulty = pool_difficulty;
    snapshot.pool.pool_effort_norm = pool_effort_norm;
    snapshot.pool.worker_pool_share_norm = worker_pool_share_norm;
    snapshot.pool.submission_rate_per_s = submission_rate_per_s * (0.80 + (0.20 * pool_effort_norm));
    snapshot.pool.acceptance_rate = acceptance_rate;
    snapshot.pool.reward.coin = pool_interval_reward_coin;
    snapshot.pool.reward.value_usd = pool_interval_reward_coin * btc_price_usd;

    snapshot.blockchain.network_difficulty = network_difficulty;
    snapshot.blockchain.network_hashrate_hs = network_hashrate_hs;
    snapshot.blockchain.block_reward_coin = block_reward_coin;
    snapshot.blockchain.next_halving_progress_norm = clamp01(
        (0.35 * phase_alignment)
        + (0.25 * overlap)
        + (0.40 * safe_abs_norm(trace.derived_constants.temporal_relativity, 2.0))
    );
    snapshot.blockchain.reward.coin = network_interval_reward_coin;
    snapshot.blockchain.reward.value_usd = network_interval_reward_coin * btc_price_usd;

    std::ostringstream status;
    status << (observation.live ? "live" : "deterministic")
           << " telemetry | trace=" << snapshot.trace_id
           << " | reward=" << snapshot.reward_unit_label;
    snapshot.status_line = status.str();

    return snapshot;
}

}  // namespace qbit_miner