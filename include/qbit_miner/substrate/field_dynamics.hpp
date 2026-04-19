#pragma once

#include <array>
#include <string>
#include <vector>

#include "qbit_miner/substrate/photonic_identity.hpp"

namespace qbit_miner {

struct FieldDynamicsConfig {
    double numeric_epsilon = 1.0e-9;
    double minimal_gpu_wavelength_step = 1.0 / 1048576.0;
};

struct DerivedTemporalConstants {
    double phase_position_turns = 0.0;
    double phase_alignment = 0.0;
    double zero_point_overlap = 0.0;
    double vector_alignment = 0.0;
    double spin_alignment = 0.0;
    double orientation_alignment = 0.0;
    double axis_scale_x = 0.0;
    double axis_scale_y = 0.0;
    double axis_scale_z = 0.0;
    double axis_resonance = 0.0;
    double vector_energy = 0.0;
    double path_speed = 0.0;
    double path_speed_norm = 0.0;
    double field_wavelength = 0.0;
    double field_wavelength_norm = 0.0;
    double time_per_field = 0.0;
    double zero_point_line_distance = 0.0;
    double zero_point_proximity = 0.0;
    double relative_temporal_position = 0.0;
    double temporal_relativity = 0.0;
    double temporal_relativity_norm = 0.0;
    double phase_alignment_probability = 0.0;
    double resonance_intercept_force = 0.0;
    double entanglement_weight = 0.0;
    double crosstalk_weight = 0.0;
    double normalization_drive = 0.0;
    double rotational_velocity_norm = 0.0;
    double field_interference_norm = 0.0;
    double driver_kernel_duration_ms = 0.0;
    double driver_kernel_duration_norm = 0.0;
    double driver_execution_density = 0.0;
    double driver_invocation_pressure = 0.0;
    double driver_temporal_alignment = 0.0;
    double driver_resonance = 0.0;
    double observer_gain = 0.0;
    double coupling_gain = 0.0;
    double collision_gain = 0.0;
    double rotation_gain = 0.0;
    double coherence_gain = 0.0;
    double inertia_gain = 0.0;
    double phase_gain = 0.0;
    double flux_gain = 0.0;
    double expansion_gain = 0.0;
    double reverse_causal_gain = 0.0;
    double zero_point_gain = 0.0;
    double temporal_noise_gain = 0.0;
    double effective_wavelength_step = 0.0;
};

struct CalibrationSweepStep {
    std::string variable;
    std::string direction;
    double wavelength_step = 0.0;
    double predicted_zero_point_overlap = 0.0;
    double predicted_constant_phase_alignment = 0.0;
};

struct CalibrationPlan {
    double minimal_wavelength_step = 0.0;
    double trajectory_conservation_score = 0.0;
    double zero_point_overlap_score = 0.0;
    double reverse_causal_flux_coherence = 0.0;
    std::vector<CalibrationSweepStep> sweeps;
};

struct SubstrateTrace {
    PhotonicIdentity photonic_identity;
    SubstrateRequestContext timing;
    GpuKernelSyncState gpu_kernel_sync;
    std::uint32_t encodable_node_count = 0;
    DerivedTemporalConstants derived_constants;
    std::array<double, 9> trajectory_9d {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 3> encoded_pulse {0.0, 0.0, 0.0};
    std::array<double, 3> rotational_velocity {0.0, 0.0, 0.0};
    double phase_transport = 0.0;
    double flux_transport = 0.0;
    double observer_factor = 0.0;
    double coupling_strength = 0.0;
    double coupling_collision_noise = 0.0;
    double temporal_dynamics_noise = 0.0;
    double reverse_causal_flux_coherence = 0.0;
    double zero_point_overlap_score = 0.0;
    double constant_phase_alignment = 0.0;
    double trajectory_conservation_score = 0.0;
    double expansion_factor = 0.0;
    double substrate_inertia = 0.0;
    double transit_budget_ms = 0.0;
    CalibrationPlan calibration_plan;
    std::string status;
};

class FieldDynamicsEngine {
public:
    explicit FieldDynamicsEngine(FieldDynamicsConfig config = {});

    [[nodiscard]] const FieldDynamicsConfig& config() const noexcept;
    [[nodiscard]] DerivedTemporalConstants derive_temporal_constants(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_observer_factor(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_flux_transport(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_phase_transport(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_coupling_strength(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] std::array<double, 3> compute_rotational_velocity(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_coupling_collision_noise(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] std::array<double, 9> compute_trajectory_9d(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_expansion_factor(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_reverse_causal_flux_coherence(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_temporal_dynamics_noise(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_constant_phase_alignment(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_zero_point_overlap_score(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_trajectory_conservation_score(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] CalibrationPlan build_calibration_plan(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] double compute_substrate_inertia(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] std::array<double, 3> encode_pulse_vector(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] std::string make_trace_id(const GpuFeedbackFrame& frame) const;
    [[nodiscard]] SubstrateTrace trace_feedback(const GpuFeedbackFrame& frame) const;

private:
    FieldDynamicsConfig config_;
};

}  // namespace qbit_miner
