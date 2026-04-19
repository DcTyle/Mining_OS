#include "qbit_miner/runtime/phase_transport_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace qbit_miner {

namespace {

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double normalize_positive(double value) {
    return clamp_unit(value / (1.0 + std::abs(value)));
}

[[nodiscard]] double wrap_turns(double value) {
    const double wrapped = std::fmod(value, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

[[nodiscard]] double phase_delta_turns(double lhs, double rhs) {
    double delta = wrap_turns(lhs) - wrap_turns(rhs);
    if (delta > 0.5) {
        delta -= 1.0;
    } else if (delta < -0.5) {
        delta += 1.0;
    }
    return delta;
}

[[nodiscard]] double mean_unit(std::initializer_list<double> values) {
    if (values.size() == 0U) {
        return 0.0;
    }
    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return clamp_unit(total / static_cast<double>(values.size()));
}

[[nodiscard]] double vector_norm3(const std::array<double, 3>& vector) {
    return std::sqrt(std::max(0.0, (vector[0] * vector[0]) + (vector[1] * vector[1]) + (vector[2] * vector[2])));
}

[[nodiscard]] std::array<double, 3> normalized_direction(const SubstrateTrace& trace) {
    const double rotation_norm = vector_norm3(trace.rotational_velocity);
    if (rotation_norm > 1.0e-12) {
        return {
            trace.rotational_velocity[0] / rotation_norm,
            trace.rotational_velocity[1] / rotation_norm,
            trace.rotational_velocity[2] / rotation_norm,
        };
    }

    const std::array<double, 3> fallback{
        trace.derived_constants.axis_scale_x - 0.5,
        trace.derived_constants.axis_scale_y - 0.5,
        trace.derived_constants.axis_scale_z - 0.5,
    };
    const double fallback_norm = vector_norm3(fallback);
    if (fallback_norm > 1.0e-12) {
        return {
            fallback[0] / fallback_norm,
            fallback[1] / fallback_norm,
            fallback[2] / fallback_norm,
        };
    }
    return {0.0, 0.0, 1.0};
}

[[nodiscard]] double compute_anchor_evm_norm(const SubstrateTrace& trace) {
    const std::array<double, 9> phase_texture{
        normalize_positive(trace.phase_transport),
        trace.derived_constants.axis_scale_x,
        trace.derived_constants.axis_scale_y,
        trace.derived_constants.axis_scale_z,
        normalize_positive(trace.flux_transport),
        mean_unit({clamp_unit(trace.photonic_identity.coherence), trace.constant_phase_alignment}),
        normalize_positive(vector_norm3(trace.rotational_velocity)),
        normalize_positive(trace.substrate_inertia),
        mean_unit({normalize_positive(trace.observer_factor), normalize_positive(trace.coupling_strength), trace.reverse_causal_flux_coherence}),
    };

    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t index = 0; index < phase_texture.size(); ++index) {
        const double anchor = trace.photonic_identity.spectra_9d[index];
        const double delta = phase_texture[index] - anchor;
        numerator += delta * delta;
        denominator += anchor * anchor;
    }
    return clamp_unit(std::sqrt(numerator / std::max(denominator, 1.0e-9)));
}

[[nodiscard]] std::string classify_transport_mode(double transport_mode_magnitude) {
    if (transport_mode_magnitude >= 0.78) {
        return "activate";
    }
    if (transport_mode_magnitude >= 0.52) {
        return "resonate";
    }
    if (transport_mode_magnitude >= 0.28) {
        return "stabilize";
    }
    return "hold";
}

[[nodiscard]] std::string map_egress_topic(const std::string& transport_mode) {
    return "substrate.phase.dispatch." + transport_mode;
}

}  // namespace

PhaseTransportShadowScheduler::PhaseTransportShadowScheduler(double dispatch_readiness_threshold)
    : dispatch_readiness_threshold_(dispatch_readiness_threshold) {}

double PhaseTransportShadowScheduler::dispatch_readiness_threshold() const noexcept {
    return dispatch_readiness_threshold_;
}

PhaseDispatchArtifact PhaseTransportShadowScheduler::compute_artifact(const SubstrateTrace& trace) const {
    const double phase_transport_norm = normalize_positive(trace.phase_transport);
    const double flux_transport_norm = normalize_positive(trace.flux_transport);
    const double pulse_projection_norm = normalize_positive(trace.encoded_pulse[2]);
    const double coupling_norm = normalize_positive(trace.coupling_strength);
    const double phase_lock_error = clamp_unit(
        std::abs(phase_delta_turns(trace.phase_transport, trace.photonic_identity.field_vector.phase)) / 0.5);
    const double phase_transport_carrier_norm = mean_unit({
        1.0 - phase_lock_error,
        trace.constant_phase_alignment,
        trace.derived_constants.phase_alignment_probability,
        trace.derived_constants.zero_point_proximity,
    });

    PhaseDispatchArtifact artifact;
    artifact.phase_id = trace.photonic_identity.trace_id;
    artifact.fourier_transport_frequency = mean_unit({
        phase_transport_norm,
        pulse_projection_norm,
        trace.derived_constants.field_wavelength_norm,
        trace.derived_constants.path_speed_norm,
    });
    artifact.phase_vector_magnitude = mean_unit({
        phase_transport_carrier_norm,
        flux_transport_norm,
        coupling_norm,
        trace.trajectory_conservation_score,
        trace.derived_constants.temporal_relativity_norm,
    });
    artifact.phase_vector_direction = normalized_direction(trace);
    artifact.phase_lock_error = phase_lock_error;
    artifact.anchor_evm_norm = compute_anchor_evm_norm(trace);
    artifact.temporal_admissibility = mean_unit({
        trace.reverse_causal_flux_coherence,
        trace.trajectory_conservation_score,
        trace.zero_point_overlap_score,
        1.0 - clamp_unit(trace.temporal_dynamics_noise),
    });
    artifact.zero_point_proximity = trace.derived_constants.zero_point_proximity;
    artifact.coherence_alignment = mean_unit({
        clamp_unit(trace.photonic_identity.coherence),
        trace.derived_constants.phase_alignment_probability,
        trace.constant_phase_alignment,
    });
    artifact.anchor_correlation = mean_unit({
        artifact.coherence_alignment,
        1.0 - artifact.anchor_evm_norm,
        1.0 - artifact.phase_lock_error,
    });
    artifact.interference_projection = mean_unit({
        trace.derived_constants.field_interference_norm,
        trace.derived_constants.crosstalk_weight,
        1.0 - trace.derived_constants.zero_point_proximity,
    });
    artifact.sideband_energy_norm = mean_unit({
        normalize_positive(std::fabs(trace.encoded_pulse[0] - trace.encoded_pulse[1])),
        normalize_positive(std::fabs(trace.encoded_pulse[1] - trace.encoded_pulse[2])),
        trace.derived_constants.crosstalk_weight,
        trace.derived_constants.field_interference_norm,
    });
    artifact.group_delay_skew = normalize_positive(
        trace.timing.closed_loop_latency_ms - (trace.derived_constants.time_per_field * 1000.0)
    );
    artifact.dynamic_range_headroom = clamp_unit(1.0 - std::max({
        clamp_unit(trace.photonic_identity.field_vector.amplitude),
        clamp_unit(trace.photonic_identity.field_vector.current),
        clamp_unit(trace.photonic_identity.field_vector.voltage),
        clamp_unit(trace.photonic_identity.field_vector.frequency),
    }));
    artifact.phase_lock_pressure = mean_unit({
        artifact.coherence_alignment,
        artifact.phase_vector_magnitude,
        coupling_norm,
        trace.derived_constants.axis_resonance,
    });
    artifact.relock_pressure = mean_unit({
        artifact.phase_lock_error,
        artifact.anchor_evm_norm,
        1.0 - trace.constant_phase_alignment,
        clamp_unit(trace.temporal_dynamics_noise),
    });
    artifact.transport_readiness = mean_unit({
        artifact.anchor_correlation,
        artifact.temporal_admissibility,
        artifact.zero_point_proximity,
        artifact.phase_vector_magnitude,
        1.0 - artifact.interference_projection,
        1.0 - artifact.relock_pressure,
    });
    artifact.transport_mode = classify_transport_mode(artifact.phase_vector_magnitude);
    artifact.activation_surface = trace.photonic_identity.trace_id + ":" + artifact.transport_mode;
    artifact.egress_topic = map_egress_topic(artifact.transport_mode);
    artifact.dispatch_permitted =
        trace.status == "ready" &&
        artifact.transport_mode != "hold" &&
        artifact.transport_readiness >= dispatch_readiness_threshold_ &&
        artifact.temporal_admissibility >= 0.50 &&
        artifact.sideband_energy_norm <= 0.72 &&
        artifact.anchor_evm_norm <= 0.82;
    return artifact;
}

}  // namespace qbit_miner