#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <array>
#include <string>
#include <vector>

#include "qbit_miner/control_center/field_viewport.hpp"
#include "qbit_miner/runtime/gpu_kernel_telemetry_bridge.hpp"

namespace qbit_miner {

struct MiningValidationSnapshot {
    float same_pulse_validation_norm = 0.0f;
    float candidate_surface_norm = 0.0f;
    float validation_structure_norm = 0.0f;
    float pool_ingest_vector_norm = 0.0f;
    float pool_submit_vector_norm = 0.0f;
    float target_sequence_phase_turns = 0.0f;
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
    float target_phase_alignment_norm = 0.0f;
    float header_phase_alignment_norm = 0.0f;
    float nonce_phase_alignment_norm = 0.0f;
    float sha_phase_alignment_norm = 0.0f;
    float validation_certainty_norm = 0.0f;
    float accepted_lane_fraction = 0.0f;
    float selected_lane_phase_turns = 0.0f;
    std::uint32_t selected_lane_index = 0;
    std::uint32_t activation_tick = 0;
    std::uint32_t active_worker_count = 0;
    std::uint32_t fourier_branch_factor = 0;
    std::uint32_t fourier_inner_tier_depth = 0;
    std::uint32_t fourier_frontier_tier_depth = 0;
    std::uint32_t pulse_operator_capacity_bits = 0;
    std::uint32_t attempted_nonce_count = 0;
    std::uint32_t valid_nonce_count = 0;
    std::uint32_t validation_flags = 0;
    float share_target_pass_norm = 0.0f;
    float block_target_pass_norm = 0.0f;
    float block_coherence_norm = 0.0f;
    float reinforcement_norm = 0.0f;
    float noise_lane_fraction_norm = 0.0f;
    float submit_priority_score_norm = 0.0f;
    std::uint32_t resonance_reinforcement_count = 0;
    std::uint32_t noise_lane_count = 0;
    std::uint32_t queue_quality_class = 0;
    std::uint32_t selected_worker_index = 0;
    std::uint32_t selected_nonce_value = 0;
    std::array<std::uint32_t, 8> selected_hash_words {};
};

class SubstrateComputeRuntime {
public:
    SubstrateComputeRuntime();
    ~SubstrateComputeRuntime();

    SubstrateComputeRuntime(const SubstrateComputeRuntime&) = delete;
    SubstrateComputeRuntime& operator=(const SubstrateComputeRuntime&) = delete;

    [[nodiscard]] bool initialize();
    [[nodiscard]] bool is_available() const noexcept;
    [[nodiscard]] bool update(const FieldViewportFrame& frame, std::uint32_t preview_width, std::uint32_t preview_height);
    void set_kernel_iteration_observer(std::function<void(const GpuKernelIterationEvent&)> observer);
    void set_mining_validation_observer(std::function<void(const MiningValidationSnapshot&)> observer);

    [[nodiscard]] std::uint32_t preview_width() const noexcept;
    [[nodiscard]] std::uint32_t preview_height() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& preview_rgba() const;
    [[nodiscard]] const StereoPcmFrame& last_audio_frame() const;
    [[nodiscard]] const std::wstring& device_label() const;
    [[nodiscard]] std::optional<GpuKernelIterationEvent> last_kernel_iteration() const;
    [[nodiscard]] std::optional<MiningValidationSnapshot> last_mining_validation() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qbit_miner
