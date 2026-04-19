#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "qbit_miner/substrate/photonic_identity.hpp"

namespace qbit_miner {

struct GpuKernelTelemetrySample {
    std::uint64_t telemetry_sequence = 0;
    double sample_window_start_s = 0.0;
    double sample_window_end_s = 0.0;
    double timestamp_s = 0.0;
    double delta_s = 0.0;
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

struct GpuKernelIterationEvent {
    std::uint64_t kernel_iteration = 0;
    std::string kernel_name;
    GpuKernelIterationPhase kernel_phase = GpuKernelIterationPhase::Completion;
    double launch_timestamp_s = 0.0;
    double completion_timestamp_s = 0.0;
    std::uint64_t telemetry_sequence_hint = 0;
    std::array<std::uint32_t, 3> lattice_extent {0U, 0U, 0U};
    std::array<std::uint32_t, 3> workgroup_count {0U, 0U, 0U};
    std::string driver_provider;
    bool driver_timing_valid = false;
    double gpu_launch_timestamp_s = 0.0;
    double gpu_completion_timestamp_s = 0.0;
    double gpu_execution_time_s = 0.0;
    bool pipeline_statistics_valid = false;
    std::uint64_t compute_invocation_count = 0;
};

struct GpuKernelTelemetryBridgeConfig {
    std::size_t sample_buffer_capacity = 256U;
    double max_sync_skew_s = 0.010;
    bool require_exact_sequence_hint = false;
};

struct GpuKernelTelemetryMatch {
    GpuKernelIterationEvent kernel_event;
    GpuKernelTelemetrySample telemetry_sample;
    double telemetry_skew_s = 0.0;
    bool exact_sequence_match = false;
};

class GpuKernelTelemetryBridge {
public:
    explicit GpuKernelTelemetryBridge(GpuKernelTelemetryBridgeConfig config = {});

    [[nodiscard]] const GpuKernelTelemetryBridgeConfig& config() const noexcept;
    void push_telemetry_sample(const GpuKernelTelemetrySample& sample);
    [[nodiscard]] std::optional<GpuKernelTelemetryMatch> synchronize_iteration(
        const GpuKernelIterationEvent& kernel_event
    ) const;
    [[nodiscard]] GpuFeedbackFrame build_feedback_frame(
        const GpuKernelTelemetryMatch& synchronized_match,
        const std::string& gpu_device_id = "gpu-kernel-bridge"
    ) const;
    [[nodiscard]] std::size_t buffered_sample_count() const noexcept;

private:
    [[nodiscard]] std::optional<GpuKernelTelemetryMatch> match_by_sequence_hint(
        const GpuKernelIterationEvent& kernel_event
    ) const;
    [[nodiscard]] std::optional<GpuKernelTelemetryMatch> match_by_timestamp(
        const GpuKernelIterationEvent& kernel_event
    ) const;

    GpuKernelTelemetryBridgeConfig config_;
    std::deque<GpuKernelTelemetrySample> samples_;
};

}  // namespace qbit_miner
