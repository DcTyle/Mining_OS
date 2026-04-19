#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qbit_miner {

struct FieldVector {
    double amplitude = 0.0;
    double voltage = 0.0;
    double current = 0.0;
    double frequency = 0.0;
    double phase = 0.0;
    double flux = 0.0;
    double thermal_noise = 0.0;
    double field_noise = 0.0;
};

struct SpinInertia {
    std::array<double, 3> axis_spin {0.0, 0.0, 0.0};
    std::array<double, 3> axis_orientation {0.0, 0.0, 0.0};
    double momentum_score = 0.0;
    double inertial_mass_proxy = 0.0;
    double relativistic_correlation = 0.0;
    double relative_temporal_coupling = 0.0;
    std::uint32_t temporal_coupling_count = 0;
};

struct PhotonicIdentity {
    std::string trace_id;
    std::string source_identity;
    std::string gpu_device_id;
    std::array<double, 9> spectra_9d {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    FieldVector field_vector;
    SpinInertia spin_inertia;
    double coherence = 0.0;
    double memory = 0.0;
    double nexus = 0.0;
    double observed_latency_ms = 0.0;
};

struct SubstrateRequestContext {
    std::uint64_t tick_index = 0;
    double request_time_ms = 0.0;
    double response_time_ms = 0.0;
    double accounting_time_ms = 0.0;
    double next_feedback_time_ms = 0.0;
    double closed_loop_latency_ms = 0.0;
    double encode_deadline_ms = 0.0;
};

enum class GpuKernelIterationPhase {
    Launch,
    Completion,
};

struct GpuKernelSyncState {
    std::uint64_t kernel_iteration = 0;
    std::string kernel_name;
    GpuKernelIterationPhase kernel_phase = GpuKernelIterationPhase::Launch;
    std::uint64_t telemetry_sequence = 0;
    double telemetry_timestamp_s = 0.0;
    double telemetry_skew_s = 0.0;
    double kernel_launch_time_s = 0.0;
    double kernel_completion_time_s = 0.0;
    bool exact_telemetry_match = false;
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

struct GpuFeedbackFrame {
    PhotonicIdentity photonic_identity;
    SubstrateRequestContext timing;
    GpuKernelSyncState gpu_kernel_sync;
    std::uint32_t encodable_node_count = 0;
    double sent_signal = 0.0;
    double measured_signal = 0.0;
    double integrated_feedback = 0.0;
    double derivative_signal = 0.0;
    double lattice_closure = 0.0;
    double phase_closure = 0.0;
    double recurrence_alignment = 0.0;
    double conservation_alignment = 0.0;
};

}  // namespace qbit_miner
