#include "qbit_miner/substrate/trace_serialization.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace qbit_miner {

namespace {

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

template <std::size_t N>
void append_array(std::ostringstream& out, const std::array<double, N>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << values[index];
    }
    out << ']';
}

template <std::size_t N>
void append_u32_array(std::ostringstream& out, const std::array<std::uint32_t, N>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << values[index];
    }
    out << ']';
}

std::string kernel_phase_label(GpuKernelIterationPhase phase) {
    switch (phase) {
    case GpuKernelIterationPhase::Launch:
        return "launch";
    case GpuKernelIterationPhase::Completion:
        return "completion";
    }
    return "launch";
}

void append_field_vector(std::ostringstream& out, const FieldVector& field_vector) {
    out << "\"field_vector\": {"
        << "\"amplitude\": " << field_vector.amplitude << ", "
        << "\"voltage\": " << field_vector.voltage << ", "
        << "\"current\": " << field_vector.current << ", "
        << "\"frequency\": " << field_vector.frequency << ", "
        << "\"phase\": " << field_vector.phase << ", "
        << "\"flux\": " << field_vector.flux << ", "
        << "\"thermal_noise\": " << field_vector.thermal_noise << ", "
        << "\"field_noise\": " << field_vector.field_noise
        << '}';
}

void append_spin_inertia(std::ostringstream& out, const SpinInertia& spin_inertia) {
    out << "\"spin_inertia\": {"
        << "\"axis_spin\": ";
    append_array(out, spin_inertia.axis_spin);
    out << ", \"axis_orientation\": ";
    append_array(out, spin_inertia.axis_orientation);
    out << ", \"momentum_score\": " << spin_inertia.momentum_score
        << ", \"inertial_mass_proxy\": " << spin_inertia.inertial_mass_proxy
        << ", \"relativistic_correlation\": " << spin_inertia.relativistic_correlation
        << ", \"relative_temporal_coupling\": " << spin_inertia.relative_temporal_coupling
        << ", \"temporal_coupling_count\": " << spin_inertia.temporal_coupling_count
        << '}';
}

void append_identity(std::ostringstream& out, const PhotonicIdentity& identity) {
    out << "\"trace_id\": \"" << escape_json(identity.trace_id) << "\", "
        << "\"source_identity\": \"" << escape_json(identity.source_identity) << "\", "
        << "\"gpu_device_id\": \"" << escape_json(identity.gpu_device_id) << "\", "
        << "\"spectra_9d\": ";
    append_array(out, identity.spectra_9d);
    out << ", ";
    append_field_vector(out, identity.field_vector);
    out << ", ";
    append_spin_inertia(out, identity.spin_inertia);
    out << ", \"coherence\": " << identity.coherence
        << ", \"memory\": " << identity.memory
        << ", \"nexus\": " << identity.nexus
        << ", \"observed_latency_ms\": " << identity.observed_latency_ms;
}

void append_gpu_kernel_sync(std::ostringstream& out, const GpuKernelSyncState& sync) {
    out << "\"gpu_kernel_sync\": {"
        << "\"kernel_iteration\": " << sync.kernel_iteration
        << ", \"kernel_name\": \"" << escape_json(sync.kernel_name) << "\""
        << ", \"kernel_phase\": \"" << kernel_phase_label(sync.kernel_phase) << "\""
        << ", \"telemetry_sequence\": " << sync.telemetry_sequence
        << ", \"telemetry_timestamp_s\": " << sync.telemetry_timestamp_s
        << ", \"telemetry_skew_s\": " << sync.telemetry_skew_s
        << ", \"kernel_launch_time_s\": " << sync.kernel_launch_time_s
        << ", \"kernel_completion_time_s\": " << sync.kernel_completion_time_s
        << ", \"exact_telemetry_match\": " << (sync.exact_telemetry_match ? "true" : "false")
        << ", \"driver_provider\": \"" << escape_json(sync.driver_provider) << "\""
        << ", \"driver_timing_valid\": " << (sync.driver_timing_valid ? "true" : "false")
        << ", \"gpu_launch_timestamp_s\": " << sync.gpu_launch_timestamp_s
        << ", \"gpu_completion_timestamp_s\": " << sync.gpu_completion_timestamp_s
        << ", \"gpu_execution_time_s\": " << sync.gpu_execution_time_s
        << ", \"pipeline_statistics_valid\": " << (sync.pipeline_statistics_valid ? "true" : "false")
        << ", \"compute_invocation_count\": " << sync.compute_invocation_count
        << ", \"lattice_extent\": ";
    append_u32_array(out, sync.lattice_extent);
    out << ", \"workgroup_count\": ";
    append_u32_array(out, sync.workgroup_count);
    out << '}';
}

void append_timing(std::ostringstream& out, const SubstrateRequestContext& timing) {
    out << "\"request_time_ms\": " << timing.request_time_ms
        << ", \"response_time_ms\": " << timing.response_time_ms
        << ", \"accounting_time_ms\": " << timing.accounting_time_ms
        << ", \"next_feedback_time_ms\": " << timing.next_feedback_time_ms
        << ", \"closed_loop_latency_ms\": " << timing.closed_loop_latency_ms
        << ", \"encode_deadline_ms\": " << timing.encode_deadline_ms;
}

void append_derived_constants(std::ostringstream& out, const DerivedTemporalConstants& derived) {
    out << "\"derived_constants\": {"
        << "\"phase_position_turns\": " << derived.phase_position_turns
        << ", \"phase_alignment\": " << derived.phase_alignment
        << ", \"zero_point_overlap\": " << derived.zero_point_overlap
        << ", \"vector_alignment\": " << derived.vector_alignment
        << ", \"spin_alignment\": " << derived.spin_alignment
        << ", \"orientation_alignment\": " << derived.orientation_alignment
        << ", \"axis_scale_x\": " << derived.axis_scale_x
        << ", \"axis_scale_y\": " << derived.axis_scale_y
        << ", \"axis_scale_z\": " << derived.axis_scale_z
        << ", \"axis_resonance\": " << derived.axis_resonance
        << ", \"vector_energy\": " << derived.vector_energy
        << ", \"path_speed\": " << derived.path_speed
        << ", \"path_speed_norm\": " << derived.path_speed_norm
        << ", \"field_wavelength\": " << derived.field_wavelength
        << ", \"field_wavelength_norm\": " << derived.field_wavelength_norm
        << ", \"time_per_field\": " << derived.time_per_field
        << ", \"zero_point_line_distance\": " << derived.zero_point_line_distance
        << ", \"zero_point_proximity\": " << derived.zero_point_proximity
        << ", \"relative_temporal_position\": " << derived.relative_temporal_position
        << ", \"temporal_relativity\": " << derived.temporal_relativity
        << ", \"temporal_relativity_norm\": " << derived.temporal_relativity_norm
        << ", \"phase_alignment_probability\": " << derived.phase_alignment_probability
        << ", \"resonance_intercept_force\": " << derived.resonance_intercept_force
        << ", \"entanglement_weight\": " << derived.entanglement_weight
        << ", \"crosstalk_weight\": " << derived.crosstalk_weight
        << ", \"normalization_drive\": " << derived.normalization_drive
        << ", \"rotational_velocity_norm\": " << derived.rotational_velocity_norm
        << ", \"field_interference_norm\": " << derived.field_interference_norm
        << ", \"driver_kernel_duration_ms\": " << derived.driver_kernel_duration_ms
        << ", \"driver_kernel_duration_norm\": " << derived.driver_kernel_duration_norm
        << ", \"driver_execution_density\": " << derived.driver_execution_density
        << ", \"driver_invocation_pressure\": " << derived.driver_invocation_pressure
        << ", \"driver_temporal_alignment\": " << derived.driver_temporal_alignment
        << ", \"driver_resonance\": " << derived.driver_resonance
        << ", \"observer_gain\": " << derived.observer_gain
        << ", \"coupling_gain\": " << derived.coupling_gain
        << ", \"collision_gain\": " << derived.collision_gain
        << ", \"rotation_gain\": " << derived.rotation_gain
        << ", \"coherence_gain\": " << derived.coherence_gain
        << ", \"inertia_gain\": " << derived.inertia_gain
        << ", \"phase_gain\": " << derived.phase_gain
        << ", \"flux_gain\": " << derived.flux_gain
        << ", \"expansion_gain\": " << derived.expansion_gain
        << ", \"reverse_causal_gain\": " << derived.reverse_causal_gain
        << ", \"zero_point_gain\": " << derived.zero_point_gain
        << ", \"temporal_noise_gain\": " << derived.temporal_noise_gain
        << ", \"effective_wavelength_step\": " << derived.effective_wavelength_step
        << '}';
}

void append_calibration_plan(std::ostringstream& out, const CalibrationPlan& plan) {
    out << "\"calibration_plan\": {"
        << "\"minimal_wavelength_step\": " << plan.minimal_wavelength_step
        << ", \"trajectory_conservation_score\": " << plan.trajectory_conservation_score
        << ", \"zero_point_overlap_score\": " << plan.zero_point_overlap_score
        << ", \"reverse_causal_flux_coherence\": " << plan.reverse_causal_flux_coherence
        << ", \"sweeps\": [";
    for (std::size_t index = 0; index < plan.sweeps.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        const auto& step = plan.sweeps[index];
        out << '{'
            << "\"variable\": \"" << escape_json(step.variable) << "\", "
            << "\"direction\": \"" << escape_json(step.direction) << "\", "
            << "\"wavelength_step\": " << step.wavelength_step << ", "
            << "\"predicted_zero_point_overlap\": " << step.predicted_zero_point_overlap << ", "
            << "\"predicted_constant_phase_alignment\": " << step.predicted_constant_phase_alignment
            << '}';
    }
    out << "]}";
}

void append_calibration_plan_only(std::ostringstream& out, const SubstrateTrace& trace) {
    out << '{'
        << "\"trace_id\": \"" << escape_json(trace.photonic_identity.trace_id) << "\", "
        << "\"source_identity\": \"" << escape_json(trace.photonic_identity.source_identity) << "\", "
        << "\"gpu_device_id\": \"" << escape_json(trace.photonic_identity.gpu_device_id) << "\", "
        << "\"trajectory_9d\": ";
    append_array(out, trace.trajectory_9d);
    out << ", \"encodable_node_count\": " << trace.encodable_node_count << ", ";
    append_timing(out, trace.timing);
    out << ", ";
    append_calibration_plan(out, trace.calibration_plan);
    out << '}';
}

}  // namespace

std::string serialize_feedback_frame_json(const GpuFeedbackFrame& frame) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{';
    append_identity(out, frame.photonic_identity);
    out << ", ";
    append_gpu_kernel_sync(out, frame.gpu_kernel_sync);
    out << ", ";
    append_timing(out, frame.timing);
    out << ", \"encodable_node_count\": " << frame.encodable_node_count
        << ", \"sent_signal\": " << frame.sent_signal
        << ", \"measured_signal\": " << frame.measured_signal
        << ", \"integrated_feedback\": " << frame.integrated_feedback
        << ", \"derivative_signal\": " << frame.derivative_signal
        << ", \"lattice_closure\": " << frame.lattice_closure
        << ", \"phase_closure\": " << frame.phase_closure
        << ", \"recurrence_alignment\": " << frame.recurrence_alignment
        << ", \"conservation_alignment\": " << frame.conservation_alignment
        << '}';
    return out.str();
}

std::string serialize_trace_json(const SubstrateTrace& trace) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{';
    append_identity(out, trace.photonic_identity);
    out << ", ";
    append_gpu_kernel_sync(out, trace.gpu_kernel_sync);
    out << ", ";
    append_timing(out, trace.timing);
    out << ", \"encodable_node_count\": " << trace.encodable_node_count
        << ", ";
    append_derived_constants(out, trace.derived_constants);
    out << ", \"trajectory_9d\": ";
    append_array(out, trace.trajectory_9d);
    out << ", \"encoded_pulse\": ";
    append_array(out, trace.encoded_pulse);
    out << ", \"rotational_velocity\": ";
    append_array(out, trace.rotational_velocity);
    out << ", \"phase_transport\": " << trace.phase_transport
        << ", \"flux_transport\": " << trace.flux_transport
        << ", \"observer_factor\": " << trace.observer_factor
        << ", \"coupling_strength\": " << trace.coupling_strength
        << ", \"coupling_collision_noise\": " << trace.coupling_collision_noise
        << ", \"temporal_dynamics_noise\": " << trace.temporal_dynamics_noise
        << ", \"reverse_causal_flux_coherence\": " << trace.reverse_causal_flux_coherence
        << ", \"zero_point_overlap_score\": " << trace.zero_point_overlap_score
        << ", \"constant_phase_alignment\": " << trace.constant_phase_alignment
        << ", \"trajectory_conservation_score\": " << trace.trajectory_conservation_score
        << ", \"expansion_factor\": " << trace.expansion_factor
        << ", \"substrate_inertia\": " << trace.substrate_inertia
        << ", \"transit_budget_ms\": " << trace.transit_budget_ms
        << ", \"status\": \"" << escape_json(trace.status) << "\", ";
    append_calibration_plan(out, trace.calibration_plan);
    out << '}';
    return out.str();
}

std::string serialize_calibration_plan_json(const SubstrateTrace& trace) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    append_calibration_plan_only(out, trace);
    return out.str();
}

std::string serialize_calibration_sweep_json(const SubstrateTrace& trace, const CalibrationSweepStep& step, std::size_t step_index) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{'
        << "\"trace_id\": \"" << escape_json(trace.photonic_identity.trace_id) << "\", "
        << "\"source_identity\": \"" << escape_json(trace.photonic_identity.source_identity) << "\", "
        << "\"gpu_device_id\": \"" << escape_json(trace.photonic_identity.gpu_device_id) << "\", "
        << "\"sweep_index\": " << step_index << ", "
        << "\"variable\": \"" << escape_json(step.variable) << "\", "
        << "\"direction\": \"" << escape_json(step.direction) << "\", "
        << "\"wavelength_step\": " << step.wavelength_step << ", "
        << "\"predicted_zero_point_overlap\": " << step.predicted_zero_point_overlap << ", "
        << "\"predicted_constant_phase_alignment\": " << step.predicted_constant_phase_alignment << ", "
        << "\"trajectory_conservation_score\": " << trace.trajectory_conservation_score << ", "
        << "\"reverse_causal_flux_coherence\": " << trace.reverse_causal_flux_coherence << ", "
        << "\"temporal_dynamics_noise\": " << trace.temporal_dynamics_noise
        << '}';
    return out.str();
}

std::string serialize_calibration_manifest_json(const std::vector<SubstrateTrace>& traces) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{'
        << "\"export_format\": \"quantum_miner.calibration_bundle.v1\", "
        << "\"trace_count\": " << traces.size() << ", ";

    std::size_t sweep_count = 0;
    for (const auto& trace : traces) {
        sweep_count += trace.calibration_plan.sweeps.size();
    }

    out << "\"sweep_file_count\": " << sweep_count << ", \"trace_ids\": [";
    for (std::size_t index = 0; index < traces.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << "\"" << escape_json(traces[index].photonic_identity.trace_id) << "\"";
    }
    out << "], \"source_identities\": [";
    for (std::size_t index = 0; index < traces.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << "\"" << escape_json(traces[index].photonic_identity.source_identity) << "\"";
    }
    out << "]}";
    return out.str();
}

}  // namespace qbit_miner
