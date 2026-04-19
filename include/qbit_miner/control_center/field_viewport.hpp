#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/runtime/substrate_viewport_encoder.hpp"

namespace qbit_miner {

struct SubstrateStratumAuthorityState;

struct VoxelMaterial {
    std::array<float, 4> base_color_rgba {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> normal_xyz {0.0f, 0.0f, 1.0f};
    float roughness = 0.0f;
    float metallic = 0.0f;
    float emissive = 0.0f;
    float conductivity = 0.0f;
};

struct FieldAudioChannels {
    std::array<float, 4> channels {0.0f, 0.0f, 0.0f, 0.0f};
    float carrier_frequency_hz = 0.0f;
    float phase_progression = 0.0f;
};

struct FieldVoxel {
    std::array<float, 3> position_xyz {0.0f, 0.0f, 0.0f};
    float density = 0.0f;
    float size = 1.0f;
    VoxelMaterial material;
    FieldAudioChannels audio;
};

struct MiningPhaseEncodingState {
    std::array<float, 3> target_direction_xyz {0.0f, 0.0f, 1.0f};
    float share_target_phase_turns = 0.0f;
    float header_phase_turns = 0.0f;
    float nonce_origin_phase_turns = 0.0f;
    float target_sequence_phase_turns = 0.0f;
    float target_resonance_norm = 0.0f;
    float resonance_activation_norm = 0.0f;
    float phase_flux_conservation_norm = 0.0f;
    float nonce_collapse_confidence_norm = 0.0f;
    float observer_collapse_strength_norm = 0.0f;
    float validation_structure_norm = 0.0f;
    float sha256_schedule_phase_turns = 0.0f;
    float sha256_round_phase_turns = 0.0f;
    float sha256_digest_phase_turns = 0.0f;
    float sha256_frequency_bias_norm = 0.0f;
    float sha256_harmonic_density_norm = 0.0f;
    float target_frequency_norm = 0.0f;
    float target_sequence_frequency_norm = 0.0f;
    float target_repeat_flux_norm = 0.0f;
    float reverse_observer_collapse_norm = 0.0f;
    float spider_code_frequency_norm = 0.0f;
    float spider_code_amplitude_norm = 0.0f;
    float spider_code_voltage_norm = 0.0f;
    float spider_code_amperage_norm = 0.0f;
    float spider_projection_coherence_norm = 0.0f;
    float spider_harmonic_gate_norm = 0.0f;
    float spider_noise_sink_norm = 0.0f;
    float frontier_activation_budget_norm = 0.0f;
    float cumulative_activation_budget_norm = 0.0f;
    float pulse_operator_density_norm = 0.0f;
    float nested_fourier_resonance_norm = 0.0f;
    float pool_ingest_vector_norm = 0.0f;
    float pool_submit_vector_norm = 0.0f;
    float phase_pressure_norm = 0.0f;
    float transfer_drive_norm = 0.0f;
    float stability_gate_norm = 0.0f;
    float damping_norm = 0.0f;
    float transport_drive_norm = 0.0f;
    float worker_parallelism_norm = 0.0f;
    float lane_coherence_norm = 0.0f;
    float phase_lock_error_norm = 0.0f;
    float phase_clamp_strength_norm = 0.0f;
    float temporal_admissibility_norm = 0.0f;
    float zero_point_proximity_norm = 0.0f;
    float transport_readiness_norm = 0.0f;
    float share_confidence_norm = 0.0f;
    float validation_rate_norm = 0.0f;
    std::uint32_t target_lane_count = 0;
    std::uint32_t active_worker_count = 0;
    std::uint32_t attempted_nonce_count = 0;
    std::uint32_t valid_nonce_count = 0;
    std::uint32_t fourier_branch_factor = 0;
    std::uint32_t fourier_inner_tier_depth = 0;
    std::uint32_t fourier_frontier_tier_depth = 0;
    std::uint32_t pulse_operator_capacity_bits = 0;
    std::uint32_t phase_program_block_count = 0;
    std::uint64_t fourier_frontier_activation_count = 0;
    std::uint64_t fourier_cumulative_activation_count = 0;
    bool active = false;
    bool submit_path_ready = false;
    bool all_parallel_harmonics_verified = false;
    bool phase_program_substrate_native = false;
    bool phase_program_same_pulse_validation = false;
    bool phase_program_pool_format_ready = false;
    float share_target_pass_norm = 0.0f;
    float block_target_pass_norm = 0.0f;
    float block_coherence_norm = 0.0f;
    float reinforcement_norm = 0.0f;
    float noise_lane_fraction_norm = 0.0f;
    float submit_priority_score_norm = 0.0f;
    std::uint32_t resonance_reinforcement_count = 0;
    std::uint32_t noise_lane_count = 0;
    std::uint32_t queue_quality_class = 0;
    std::uint32_t reserved_queue_state = 0;
};

struct MiningGpuWorkerAuthority {
    std::array<std::uint32_t, 20> header_template_words {};
    std::uint32_t worker_index = 0;
    std::uint32_t nonce_start = 0;
    std::uint32_t nonce_end = 0;
    bool active = false;
};

struct MiningGpuAuthorityField {
    std::array<std::uint32_t, 8> share_target_words {};
    std::array<std::uint32_t, 8> block_target_words {};
    std::array<MiningGpuWorkerAuthority, kStratumWorkerSlotCount> workers {};
    std::string connection_id;
    std::string job_id;
    std::string nbits_hex;
    std::string share_target_hex;
    std::string block_target_hex;
    double share_difficulty = 0.0;
    std::uint32_t active_worker_count = 0;
    bool active = false;
};

struct SavedFieldTextureState {
    std::array<float, 9> carrier_9d {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 9> resume_trajectory_9d {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> pulse_quartet {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 3> phase_direction_xyz {0.0f, 0.0f, 1.0f};
    float phase_magnitude = 0.0f;
    float zero_point_proximity = 0.0f;
    float resonance_energy = 0.0f;
    float anchor_correlation = 0.0f;
    float phase_lock_error = 0.0f;
    float relock_pressure = 0.0f;
    float sideband_energy_norm = 0.0f;
    float dynamic_range_headroom = 0.0f;
    MiningPhaseEncodingState mining_phase_encoding;
    std::string trace_id;
};

struct FieldViewportFrame {
    SavedFieldTextureState saved_state;
    MiningGpuAuthorityField gpu_mining_authority;
    SubstrateViewportFrame authoritative_frame;
    std::vector<FieldVoxel> voxels;
    std::array<float, 4> aggregate_visual_rgba {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> aggregate_material_pbr {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> aggregate_audio {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t extent_x = 18;
    std::uint32_t extent_y = 18;
    std::uint32_t extent_z = 18;
    double time_s = 0.0;
    std::string material_trace_tag;
    std::string authoritative_program_source;
};

struct StereoPcmFrame {
    std::vector<std::int16_t> interleaved_samples;
    std::uint32_t sample_rate_hz = 48000;
};

[[nodiscard]] MiningPhaseEncodingState build_mining_phase_encoding_state(
    const SubstrateStratumAuthorityState* authority_state
);
[[nodiscard]] MiningGpuAuthorityField build_mining_gpu_authority_field(
    const SubstrateStratumAuthorityState* authority_state
);
[[nodiscard]] SavedFieldTextureState build_saved_field_texture_state(
    const SubstrateViewportFrame& viewport_frame,
    const SubstrateStratumAuthorityState* authority_state = nullptr
);
[[nodiscard]] FieldViewportFrame build_field_viewport_frame(
    const SubstrateViewportFrame& viewport_frame,
    double time_s,
    std::uint32_t lattice_extent = 18,
    const SubstrateStratumAuthorityState* authority_state = nullptr
);
[[nodiscard]] StereoPcmFrame synthesize_field_audio(
    const FieldViewportFrame& frame,
    std::size_t frame_count = 4096,
    std::uint32_t sample_rate_hz = 48000
);

}  // namespace qbit_miner
