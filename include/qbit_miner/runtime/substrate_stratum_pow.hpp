#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qbit_miner {

struct BitcoinWorkProjection {
    double share_difficulty = 0.0;
    double expected_hashes_per_share = 0.0;
    double network_difficulty = 0.0;
    double network_hashrate_hs = 0.0;
    double target_network_share_fraction = 0.0;
    double required_hashrate_hs = 0.0;
    double required_share_submissions_per_s = 0.0;
};

struct SubstrateStratumPowEvaluation {
    std::string header_hex;
    std::string hash_hex;
    std::string target_hex;
    std::string share_target_hex;
    std::string block_target_hex;
    std::string nbits_hex;
    std::string nonce_hex;
    double share_difficulty = 0.0;
    double expected_hashes_for_share = 0.0;
    bool valid_share = false;
    bool valid_block = false;
};

struct SubstrateStratumPhaseFluxMeasurement {
    double carrier_phase_turns = 0.0;
    double target_phase_turns = 0.0;
    double search_epoch_turns = 0.0;
    double phase_pressure = 0.0;
    double flux_transport_norm = 0.0;
    double observer_factor = 0.0;
    double zero_point_proximity = 0.0;
    double temporal_admissibility = 0.0;
    double trajectory_conservation = 0.0;
    double phase_lock_error = 0.0;
    double anchor_evm_norm = 0.0;
    double sideband_energy_norm = 0.0;
    double interference_projection = 0.0;
    double rf_carrier_frequency_norm = 0.0;
    double rf_envelope_amplitude_norm = 0.0;
    double rf_phase_position_turns = 0.0;
    double rf_phase_velocity_turns = 0.0;
    double rf_zero_point_displacement_turns = 0.0;
    double rf_zero_point_distance_norm = 0.0;
    double rf_spin_drive_signed = 0.0;
    double rf_rotation_orientation_signed = 0.0;
    double rf_temporal_coupling_norm = 0.0;
    double rf_resonance_hold_norm = 0.0;
    double rf_sideband_energy_norm = 0.0;
    double rf_energy_transfer_norm = 0.0;
    double rf_particle_stability_norm = 0.0;
    double transfer_drive_norm = 0.0;
    double stability_gate_norm = 0.0;
    double damping_norm = 0.0;
    double spin_alignment_norm = 0.0;
    double transport_drive_norm = 0.0;
    double target_resonance_norm = 0.0;
    double resonance_activation_norm = 0.0;
    double sha256_schedule_phase_turns = 0.0;
    double sha256_round_phase_turns = 0.0;
    double sha256_digest_phase_turns = 0.0;
    double sha256_frequency_bias_norm = 0.0;
    double sha256_harmonic_density_norm = 0.0;
};

struct SubstrateStratumPowCollapseFeedback {
    double measured_hash_phase_turns = 0.0;
    double measured_nonce_phase_turns = 0.0;
    double feedback_phase_turns = 0.0;
    double relock_error_turns = 0.0;
    double observer_collapse_strength = 0.0;
    double phase_flux_conservation = 0.0;
    double nonce_collapse_confidence = 0.0;
};

struct SubstrateStratumPowSearchResult {
    SubstrateStratumPowEvaluation evaluation;
    SubstrateStratumPowCollapseFeedback collapse_feedback;
    std::uint32_t nonce_value = 0;
    std::size_t attempts = 0;
    bool found = false;
};

struct SubstrateStratumPowPhaseTrace {
    SubstrateStratumPowEvaluation evaluation;
    SubstrateStratumPhaseFluxMeasurement initial_measurement;
    SubstrateStratumPhaseFluxMeasurement resonant_measurement;
    SubstrateStratumPhaseFluxMeasurement collapsed_measurement;
    SubstrateStratumPowCollapseFeedback collapse_feedback;
    double nonce_seed_phase_turns = 0.0;
    double header_phase_turns = 0.0;
    double share_target_phase_turns = 0.0;
    double block_target_phase_turns = 0.0;
    bool performed = false;
    std::string temporal_sequence;
};

[[nodiscard]] std::string stratum_bits_to_target_hex(const std::string& nbits_hex);
[[nodiscard]] std::string stratum_difficulty_to_target_hex(double difficulty);
[[nodiscard]] double stratum_nbits_to_difficulty(const std::string& nbits_hex);
[[nodiscard]] double expected_hashes_for_difficulty(double difficulty);
[[nodiscard]] double bitcoin_network_hashrate_from_difficulty(double difficulty, double block_interval_seconds = 600.0);
[[nodiscard]] bool try_parse_stratum_nonce_hex(const std::string& nonce_hex, std::uint32_t& nonce_value);
[[nodiscard]] BitcoinWorkProjection build_bitcoin_work_projection(
    double share_difficulty,
    const std::string& network_nbits_hex,
    double target_network_share_fraction,
    double block_interval_seconds = 600.0
);
[[nodiscard]] SubstrateStratumPowEvaluation evaluate_stratum_pow(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_value,
    double share_difficulty = 0.0
);
[[nodiscard]] std::string build_stratum_header_hex(
    const std::string& prevhash_hex,
    const std::string& coinbase1_hex,
    const std::string& extranonce1_hex,
    std::uint32_t extranonce2_size,
    const std::string& coinbase2_hex,
    const std::vector<std::string>& merkle_branches_hex,
    const std::string& version_hex,
    const std::string& nbits_hex,
    const std::string& ntime_hex,
    const std::string& extranonce2_hex = {}
);
[[nodiscard]] SubstrateStratumPowCollapseFeedback measure_stratum_pow_collapse(
    const SubstrateStratumPowEvaluation& evaluation,
    const SubstrateStratumPhaseFluxMeasurement& measurement
);
[[nodiscard]] SubstrateStratumPhaseFluxMeasurement bias_phase_flux_measurement_with_sha256_frequency(
    const std::string& header_hex,
    const std::string& nbits_hex,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& measurement = {}
);
[[nodiscard]] SubstrateStratumPhaseFluxMeasurement apply_stratum_pow_collapse_feedback(
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    const SubstrateStratumPowCollapseFeedback& feedback,
    bool valid_share
);
[[nodiscard]] SubstrateStratumPowSearchResult find_valid_stratum_nonce(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_start,
    std::size_t max_attempts,
    double share_difficulty = 0.0,
    const SubstrateStratumPhaseFluxMeasurement& measurement = {}
);
[[nodiscard]] SubstrateStratumPowPhaseTrace trace_stratum_pow_phase(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_value,
    double share_difficulty = 0.0,
    const SubstrateStratumPhaseFluxMeasurement& measurement = {}
);

}  // namespace qbit_miner
