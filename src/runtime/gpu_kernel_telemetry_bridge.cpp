#include "qbit_miner/runtime/gpu_kernel_telemetry_bridge.hpp"

#include <algorithm>
#include <cmath>

namespace qbit_miner {

namespace {

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double phase_seed(double timestamp_s) {
    const double wrapped = std::fmod(timestamp_s * 0.17, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

[[nodiscard]] double safe_delta(double newer, double older, double fallback = 0.0) {
    if (newer <= 0.0 || older <= 0.0 || newer < older) {
        return fallback;
    }
    return newer - older;
}

[[nodiscard]] double midpoint(double lhs, double rhs) {
    return (lhs + rhs) * 0.5;
}

[[nodiscard]] double kernel_launch_reference_time_s(const GpuKernelIterationEvent& event) {
    if (event.driver_timing_valid && event.gpu_launch_timestamp_s > 0.0) {
        return event.gpu_launch_timestamp_s;
    }
    return event.launch_timestamp_s;
}

[[nodiscard]] double kernel_completion_reference_time_s(const GpuKernelIterationEvent& event) {
    if (event.driver_timing_valid && event.gpu_completion_timestamp_s > 0.0) {
        return event.gpu_completion_timestamp_s;
    }
    if (event.completion_timestamp_s > 0.0) {
        return event.completion_timestamp_s;
    }
    return kernel_launch_reference_time_s(event);
}

[[nodiscard]] double kernel_execution_time_s(const GpuKernelIterationEvent& event, double fallback = 0.0) {
    if (event.driver_timing_valid && event.gpu_execution_time_s > 0.0) {
        return event.gpu_execution_time_s;
    }
    return safe_delta(kernel_completion_reference_time_s(event), kernel_launch_reference_time_s(event), fallback);
}

}  // namespace

GpuKernelTelemetryBridge::GpuKernelTelemetryBridge(GpuKernelTelemetryBridgeConfig config)
    : config_(config) {}

const GpuKernelTelemetryBridgeConfig& GpuKernelTelemetryBridge::config() const noexcept {
    return config_;
}

void GpuKernelTelemetryBridge::push_telemetry_sample(const GpuKernelTelemetrySample& sample) {
    samples_.push_back(sample);
    while (samples_.size() > config_.sample_buffer_capacity) {
        samples_.pop_front();
    }
}

std::optional<GpuKernelTelemetryMatch> GpuKernelTelemetryBridge::match_by_sequence_hint(
    const GpuKernelIterationEvent& kernel_event
) const {
    if (kernel_event.telemetry_sequence_hint == 0U) {
        return std::nullopt;
    }

    for (const auto& sample : samples_) {
        if (sample.telemetry_sequence != kernel_event.telemetry_sequence_hint) {
            continue;
        }

        const double reference_time = kernel_event.kernel_phase == GpuKernelIterationPhase::Launch
            ? kernel_launch_reference_time_s(kernel_event)
            : kernel_completion_reference_time_s(kernel_event);
        return GpuKernelTelemetryMatch{
            kernel_event,
            sample,
            std::fabs(sample.timestamp_s - reference_time),
            true,
        };
    }
    return std::nullopt;
}

std::optional<GpuKernelTelemetryMatch> GpuKernelTelemetryBridge::match_by_timestamp(
    const GpuKernelIterationEvent& kernel_event
) const {
    if (samples_.empty()) {
        return std::nullopt;
    }

    const double reference_time = kernel_event.kernel_phase == GpuKernelIterationPhase::Launch
        ? kernel_launch_reference_time_s(kernel_event)
        : kernel_completion_reference_time_s(kernel_event);

    const GpuKernelTelemetrySample* best_sample = nullptr;
    double best_skew = config_.max_sync_skew_s;

    for (const auto& sample : samples_) {
        const double sample_start = sample.sample_window_start_s > 0.0
            ? sample.sample_window_start_s
            : (sample.timestamp_s - std::max(sample.delta_s, 0.0));
        const double sample_end = sample.sample_window_end_s > 0.0
            ? sample.sample_window_end_s
            : sample.timestamp_s;

        double skew = 0.0;
        if (reference_time < sample_start) {
            skew = sample_start - reference_time;
        } else if (reference_time > sample_end) {
            skew = reference_time - sample_end;
        }

        if (best_sample == nullptr || skew < best_skew) {
            best_sample = &sample;
            best_skew = skew;
        }
    }

    if (best_sample == nullptr || best_skew > config_.max_sync_skew_s) {
        return std::nullopt;
    }

    return GpuKernelTelemetryMatch{
        kernel_event,
        *best_sample,
        best_skew,
        false,
    };
}

std::optional<GpuKernelTelemetryMatch> GpuKernelTelemetryBridge::synchronize_iteration(
    const GpuKernelIterationEvent& kernel_event
) const {
    if (config_.require_exact_sequence_hint) {
        return match_by_sequence_hint(kernel_event);
    }
    if (const auto exact_match = match_by_sequence_hint(kernel_event); exact_match.has_value()) {
        return exact_match;
    }
    return match_by_timestamp(kernel_event);
}

GpuFeedbackFrame GpuKernelTelemetryBridge::build_feedback_frame(
    const GpuKernelTelemetryMatch& synchronized_match,
    const std::string& gpu_device_id
) const {
    const auto& sample = synchronized_match.telemetry_sample;
    const auto& kernel = synchronized_match.kernel_event;
    const double kernel_launch_time_s = kernel_launch_reference_time_s(kernel);
    const double kernel_completion_time_s = kernel_completion_reference_time_s(kernel);
    const double kernel_duration_s = kernel_execution_time_s(kernel, sample.delta_s);

    GpuFeedbackFrame frame;
    frame.photonic_identity.source_identity = sample.live
        ? "synchronized-gpu-telemetry"
        : "synchronized-gpu-telemetry-fallback";
    frame.photonic_identity.gpu_device_id = gpu_device_id;
    frame.photonic_identity.coherence = clamp_unit(
        (0.40 * sample.gpu_util_norm)
        + (0.24 * sample.amplitude_norm)
        + (0.20 * (1.0 - sample.thermal_interference_norm))
        + (0.16 * (1.0 - clamp_unit(synchronized_match.telemetry_skew_s / std::max(config_.max_sync_skew_s, 1.0e-9))))
    );
    frame.photonic_identity.memory = clamp_unit(sample.memory_util_norm);
    frame.photonic_identity.nexus = clamp_unit(
        (0.34 * sample.gpu_util_norm)
        + (0.26 * sample.memory_util_norm)
        + (0.20 * sample.amplitude_norm)
        + (0.20 * clamp_unit(1.0 - synchronized_match.telemetry_skew_s))
    );
    frame.photonic_identity.observed_latency_ms =
        std::max(0.1, kernel_duration_s * 1000.0);
    frame.photonic_identity.field_vector.amplitude = clamp_unit(sample.amplitude_norm);
    frame.photonic_identity.field_vector.voltage = std::max(0.0, sample.voltage_v);
    frame.photonic_identity.field_vector.current = std::max(0.0, sample.amperage_a);
    frame.photonic_identity.field_vector.frequency = clamp_unit(sample.graphics_frequency_hz / 2.5e9);
    frame.photonic_identity.field_vector.phase = phase_seed(midpoint(kernel_launch_time_s, kernel_completion_time_s));
    frame.photonic_identity.field_vector.flux = clamp_unit(
        (0.36 * sample.gpu_util_norm)
        + (0.22 * sample.memory_util_norm)
        + (0.22 * clamp_unit(sample.power_w / 400.0))
        + (0.20 * clamp_unit(1.0 - synchronized_match.telemetry_skew_s))
    );
    frame.photonic_identity.field_vector.thermal_noise = clamp_unit(sample.thermal_interference_norm);
    frame.photonic_identity.field_vector.field_noise = clamp_unit(
        0.5 * (1.0 - sample.gpu_util_norm) + 0.5 * synchronized_match.telemetry_skew_s / std::max(config_.max_sync_skew_s, 1.0e-9)
    );
    frame.photonic_identity.spin_inertia.axis_spin = {
        clamp_unit(sample.gpu_util_norm) - 0.5,
        clamp_unit(sample.memory_util_norm) - 0.5,
        clamp_unit(sample.amplitude_norm) - 0.5,
    };
    frame.photonic_identity.spin_inertia.axis_orientation = {
        clamp_unit(sample.graphics_frequency_hz / 2.5e9) - 0.5,
        clamp_unit(sample.memory_frequency_hz / 1.5e9) - 0.5,
        clamp_unit(1.0 - sample.thermal_interference_norm) - 0.5,
    };
    frame.photonic_identity.spin_inertia.momentum_score = clamp_unit(sample.gpu_util_norm);
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = clamp_unit(sample.power_w / 400.0);
    frame.photonic_identity.spin_inertia.relativistic_correlation = clamp_unit(sample.memory_util_norm);
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = clamp_unit(1.0 - synchronized_match.telemetry_skew_s);
    frame.photonic_identity.spin_inertia.temporal_coupling_count =
        static_cast<std::uint32_t>(4U + std::lround((sample.gpu_util_norm + sample.memory_util_norm) * 4.0));

    frame.timing.tick_index = kernel.kernel_iteration;
    frame.timing.request_time_ms = kernel_launch_time_s * 1000.0;
    frame.timing.response_time_ms = sample.timestamp_s * 1000.0;
    frame.timing.accounting_time_ms = kernel_completion_time_s * 1000.0;
    frame.timing.next_feedback_time_ms =
        std::max(sample.sample_window_end_s > 0.0 ? sample.sample_window_end_s : sample.timestamp_s, kernel_completion_time_s) * 1000.0;
    frame.timing.closed_loop_latency_ms = std::max(0.1, kernel_duration_s * 1000.0);
    frame.timing.encode_deadline_ms = frame.timing.closed_loop_latency_ms + std::max(1.0, sample.delta_s * 1000.0);

    frame.gpu_kernel_sync.kernel_iteration = kernel.kernel_iteration;
    frame.gpu_kernel_sync.kernel_name = kernel.kernel_name;
    frame.gpu_kernel_sync.kernel_phase = kernel.kernel_phase;
    frame.gpu_kernel_sync.telemetry_sequence = sample.telemetry_sequence;
    frame.gpu_kernel_sync.telemetry_timestamp_s = sample.timestamp_s;
    frame.gpu_kernel_sync.telemetry_skew_s = synchronized_match.telemetry_skew_s;
    frame.gpu_kernel_sync.kernel_launch_time_s = kernel_launch_time_s;
    frame.gpu_kernel_sync.kernel_completion_time_s = kernel_completion_time_s;
    frame.gpu_kernel_sync.exact_telemetry_match = synchronized_match.exact_sequence_match;
    frame.gpu_kernel_sync.lattice_extent = kernel.lattice_extent;
    frame.gpu_kernel_sync.workgroup_count = kernel.workgroup_count;
    frame.gpu_kernel_sync.driver_provider = kernel.driver_provider;
    frame.gpu_kernel_sync.driver_timing_valid = kernel.driver_timing_valid;
    frame.gpu_kernel_sync.gpu_launch_timestamp_s = kernel.gpu_launch_timestamp_s;
    frame.gpu_kernel_sync.gpu_completion_timestamp_s = kernel.gpu_completion_timestamp_s;
    frame.gpu_kernel_sync.gpu_execution_time_s = kernel.gpu_execution_time_s;
    frame.gpu_kernel_sync.pipeline_statistics_valid = kernel.pipeline_statistics_valid;
    frame.gpu_kernel_sync.compute_invocation_count = kernel.compute_invocation_count;

    frame.encodable_node_count = static_cast<std::uint32_t>(8U + std::lround(sample.memory_util_norm * 10.0));
    frame.sent_signal = clamp_unit((0.55 * sample.gpu_util_norm) + (0.45 * sample.amplitude_norm));
    frame.measured_signal = clamp_unit((0.50 * sample.memory_util_norm) + (0.50 * sample.amplitude_norm));
    frame.integrated_feedback = clamp_unit(
        (0.36 * sample.gpu_util_norm)
        + (0.24 * sample.memory_util_norm)
        + (0.20 * clamp_unit(sample.power_w / 400.0))
        + (0.20 * clamp_unit(1.0 - synchronized_match.telemetry_skew_s))
    );
    frame.derivative_signal = clamp_unit(
        std::abs(sample.gpu_util_norm - sample.memory_util_norm)
        + (0.25 * clamp_unit(synchronized_match.telemetry_skew_s / std::max(config_.max_sync_skew_s, 1.0e-9)))
    );
    frame.lattice_closure = clamp_unit((0.55 * frame.photonic_identity.coherence) + (0.45 * frame.photonic_identity.memory));
    frame.phase_closure = clamp_unit((0.60 * frame.photonic_identity.coherence) + (0.40 * frame.photonic_identity.field_vector.phase));
    frame.recurrence_alignment = clamp_unit(
        (0.45 * frame.integrated_feedback)
        + (0.35 * (1.0 - sample.thermal_interference_norm))
        + (0.20 * clamp_unit(1.0 - synchronized_match.telemetry_skew_s / std::max(config_.max_sync_skew_s, 1.0e-9)))
    );
    frame.conservation_alignment = clamp_unit((0.50 * frame.lattice_closure) + (0.50 * frame.phase_closure));

    return frame;
}

std::size_t GpuKernelTelemetryBridge::buffered_sample_count() const noexcept {
    return samples_.size();
}

}  // namespace qbit_miner
