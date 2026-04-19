#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "qbit_miner/runtime/phase_transport_scheduler.hpp"
#include "qbit_miner/runtime/substrate_stratum_pow.hpp"
#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct PhaseClampedMiningConfig {
    double min_transport_readiness = 0.64;
    double min_phase_alignment = 0.70;
    double min_zero_point_proximity = 0.58;
    double max_phase_lock_error = 0.24;
    double max_anchor_evm_norm = 0.70;
    double max_sideband_energy_norm = 0.58;
    double max_interference_projection = 0.42;
    double max_relock_pressure = 0.42;
    double max_temporal_noise = 0.40;
    double min_phase_clamp_strength = 0.64;
    double min_target_resonance_norm = 0.72;
    double min_resonance_activation_norm = 0.60;
    double hilbert_wavelength_domain = 1.0;
    double minimum_phase_window_turns = 1.0 / 64.0;
    double maximum_phase_window_turns = 1.0 / 8.0;
    std::size_t max_fourier_fanout_lanes = 8;
    std::size_t fourier_fanout_resonance_budget = 4096;
    std::size_t max_dynamic_fourier_fanout_resonance_budget = 1000000;
    double live_capacity_budget_headroom = 1.25;
    std::size_t max_in_process_validation_candidates_per_worker = 128;
    std::size_t max_dynamic_in_process_validation_candidates_per_worker = 16384;
    std::size_t max_reported_valid_nonces_per_worker = 32;
};

struct PhaseClampedShareActuation {
    std::string trace_id;
    std::string connection_id;
    std::string job_id;
    std::size_t worker_index = 0;
    std::string worker_name;
    std::string request_id;
    std::string submit_payload_json;
    std::string target_network = "bitcoin";
    std::string target_compact_nbits;
    std::string actuation_topic = "substrate.bitcoin.share.refused";
    std::string nonce_hex;
    std::string hash_hex;
    std::string target_hex;
    std::string share_target_hex;
    std::string block_target_hex;
    std::uint32_t nonce_start = 0;
    std::uint32_t nonce_end = 0;
    double target_difficulty = 0.0;
    double block_difficulty = 0.0;
    double expected_hashes_for_share = 0.0;
    double target_network_share_fraction = 0.05;
    double network_hashrate_hs = 0.0;
    double required_hashrate_hs = 0.0;
    double required_share_submissions_per_s = 0.0;
    double target_phase_turns = 0.0;
    double phase_position_turns = 0.0;
    double field_vector_phase_turns = 0.0;
    double phase_transport_turns = 0.0;
    double phase_lock_delta_turns = 0.0;
    double relative_phase_from_zero_point_turns = 0.0;
    double phase_window_turns = 0.0;
    double phase_error_turns = 0.0;
    double phase_clamp_strength = 0.0;
    double hilbert_wavelength_domain = 1.0;
    double flux_phase_transport_norm = 0.0;
    double phi_hat = 0.0;
    double forcing_hat = 0.0;
    double mining_boundary_norm = 0.0;
    double target_boundary_norm = 0.0;
    double driver_kernel_duration_ms = 0.0;
    double driver_execution_density = 0.0;
    double driver_invocation_pressure = 0.0;
    double driver_temporal_alignment = 0.0;
    double driver_resonance = 0.0;
    std::array<double, 3> rf_spin_axis_signed {0.0, 0.0, 0.0};
    std::array<double, 3> rf_spin_orientation_signed {0.0, 0.0, 0.0};
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
    std::array<double, 3> relative_phase_vector_direction {0.0, 0.0, 1.0};
    double relative_phase_vector_magnitude = 0.0;
    double phase_lock_error = 0.0;
    double phase_alignment = 0.0;
    double zero_point_proximity = 0.0;
    double zero_point_line_distance = 0.0;
    double temporal_admissibility = 0.0;
    double interference_projection = 0.0;
    double transport_readiness = 0.0;
    double share_confidence = 0.0;
    double measured_hash_phase_turns = 0.0;
    double measured_nonce_phase_turns = 0.0;
    double collapse_feedback_phase_turns = 0.0;
    double collapse_relock_error_turns = 0.0;
    double observer_collapse_strength = 0.0;
    double phase_flux_conservation = 0.0;
    double nonce_collapse_confidence = 0.0;
    double validation_structure_norm = 0.0;
    double gpu_pulse_phase_turns = 0.0;
    double resonance_field_phase_turns = 0.0;
    double gpu_pulse_propagation_phase_turns = 0.0;
    double selected_coherence_score = 0.0;
    std::int32_t op_phase_bias_q15 = 0;
    std::uint16_t op_band_w_q15 = 0;
    std::size_t fanout_lane_count = 0;
    std::size_t attempted_nonce_count = 0;
    std::size_t valid_nonce_count = 0;
    std::size_t parallel_harmonic_count = 0;
    std::size_t verified_parallel_harmonic_count = 0;
    std::size_t validated_parallel_harmonic_count = 0;
    std::size_t resonance_reinforcement_count = 0;
    std::size_t noise_lane_count = 0;
    std::uint64_t compute_invocation_count = 0;
    std::string phase_program_title;
    std::string phase_program_generated_dir;
    std::size_t phase_program_block_count = 0;
    std::string phase_temporal_sequence;
    std::string canonical_share_id;
    std::vector<std::string> sampled_valid_nonce_hexes;
    SubstrateStratumPowPhaseTrace sha256_phase_trace;
    double block_coherence_norm = 0.0;
    double submit_priority_score = 0.0;
    std::uint32_t queue_quality_class = 0;
    bool driver_timing_valid = false;
    bool offline_pow_checked = false;
    bool offline_pow_valid = false;
    bool block_candidate_valid = false;
    bool share_target_pass = false;
    bool block_target_pass = false;
    bool phase_clamped = false;
    bool measured_nonce_observed = false;
    bool resonant_candidate_available = false;
    bool valid_share_candidate = false;
    bool actuation_permitted = false;
    bool target_resonance_ready = false;
    bool all_parallel_harmonics_verified = false;
    bool phase_program_substrate_native = false;
    bool phase_program_same_pulse_validation = false;
    bool phase_program_pool_format_ready = false;
    bool has_sha256_phase_trace = false;
    std::string gate_reason = "stratum_authority_missing";
};

class PhaseClampedMiningOperatingSystem {
public:
    explicit PhaseClampedMiningOperatingSystem(PhaseClampedMiningConfig config = {});

    [[nodiscard]] const PhaseClampedMiningConfig& config() const noexcept;
    [[nodiscard]] std::vector<PhaseClampedShareActuation> compute_share_actuations(
        const SubstrateTrace& trace,
        const PhaseDispatchArtifact& artifact,
        const SubstrateStratumAuthorityState* authority_state
    ) const;

private:
    PhaseClampedMiningConfig config_;
};

}  // namespace qbit_miner
