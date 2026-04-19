#include "qbit_miner/runtime/substrate_viewport_encoder.hpp"

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

[[nodiscard]] double compute_anchor_evm_norm(const SubstrateTrace& trace, const std::array<double, 9>& texture_map_9d) {
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t index = 0; index < texture_map_9d.size(); ++index) {
        const double anchor = trace.photonic_identity.spectra_9d[index];
        const double delta = texture_map_9d[index] - anchor;
        numerator += delta * delta;
        denominator += anchor * anchor;
    }
    return clamp_unit(std::sqrt(numerator / std::max(denominator, 1.0e-9)));
}

}  // namespace

SubstrateViewportFrame SubstrateViewportEncoder::encode_frame(const SubstrateTrace& trace) const {
    SubstrateViewportFrame frame;
    frame.phase_id = trace.photonic_identity.trace_id;
    frame.viewport_direction = normalized_direction(trace);

    const double spin_norm = normalize_positive(vector_norm3(trace.rotational_velocity));
    const double inertia_norm = normalize_positive(trace.substrate_inertia);
    const double feedback_norm = mean_unit({
        normalize_positive(trace.observer_factor),
        normalize_positive(trace.coupling_strength),
        trace.reverse_causal_flux_coherence,
    });
    frame.texture_map_9d = {
        normalize_positive(trace.phase_transport),
        trace.derived_constants.axis_scale_x,
        trace.derived_constants.axis_scale_y,
        trace.derived_constants.axis_scale_z,
        normalize_positive(trace.flux_transport),
        mean_unit({clamp_unit(trace.photonic_identity.coherence), trace.constant_phase_alignment}),
        spin_norm,
        inertia_norm,
        feedback_norm,
    };

    const double curvature_proxy = mean_unit({
        normalize_positive(trace.coupling_collision_noise),
        trace.derived_constants.field_interference_norm,
        std::fabs(frame.viewport_direction[2]),
    });
    frame.phase_lock_error = normalize_positive(trace.phase_transport - trace.photonic_identity.field_vector.phase);
    frame.anchor_evm_norm = compute_anchor_evm_norm(trace, frame.texture_map_9d);
    frame.anchor_correlation = mean_unit({
        clamp_unit(trace.photonic_identity.coherence),
        trace.constant_phase_alignment,
        trace.derived_constants.phase_alignment_probability,
        1.0 - frame.anchor_evm_norm,
    });
    frame.sideband_energy_norm = mean_unit({
        normalize_positive(std::fabs(trace.encoded_pulse[0] - trace.encoded_pulse[1])),
        normalize_positive(std::fabs(trace.encoded_pulse[1] - trace.encoded_pulse[2])),
        trace.derived_constants.crosstalk_weight,
        trace.derived_constants.field_interference_norm,
    });
    const double field_time_ms = trace.derived_constants.time_per_field * 1000.0;
    frame.group_delay_skew = normalize_positive(trace.timing.closed_loop_latency_ms - field_time_ms);
    const double max_drive = std::max({
        clamp_unit(trace.photonic_identity.field_vector.amplitude),
        clamp_unit(trace.photonic_identity.field_vector.current),
        clamp_unit(trace.photonic_identity.field_vector.voltage),
        clamp_unit(trace.photonic_identity.field_vector.frequency),
    });
    frame.dynamic_range_headroom = clamp_unit(1.0 - max_drive);
    frame.relock_pressure = mean_unit({
        frame.phase_lock_error,
        frame.anchor_evm_norm,
        1.0 - trace.constant_phase_alignment,
        clamp_unit(trace.temporal_dynamics_noise),
    });

    frame.tensor_signature_6d = {
        mean_unit({clamp_unit(trace.photonic_identity.coherence), trace.constant_phase_alignment}),
        curvature_proxy,
        normalize_positive(trace.flux_transport),
        inertia_norm,
        normalize_positive(trace.phase_transport),
        normalize_positive(trace.phase_transport - trace.flux_transport),
    };
    frame.visual_rgba = {
        mean_unit({frame.texture_map_9d[0], frame.texture_map_9d[5]}),
        mean_unit({frame.texture_map_9d[4], trace.derived_constants.zero_point_proximity}),
        mean_unit({spin_norm, trace.reverse_causal_flux_coherence}),
        1.0,
    };
    frame.material_pbr = {
        mean_unit({curvature_proxy, frame.sideband_energy_norm}),
        mean_unit({frame.texture_map_9d[4], frame.texture_map_9d[5]}),
        mean_unit({feedback_norm, 1.0 - frame.anchor_evm_norm}),
        trace.derived_constants.zero_point_proximity,
    };
    frame.audio_channels = {
        mean_unit({normalize_positive(trace.phase_transport), frame.tensor_signature_6d[4]}),
        mean_unit({normalize_positive(trace.flux_transport), curvature_proxy}),
        mean_unit({trace.reverse_causal_flux_coherence, frame.relock_pressure}),
        mean_unit({clamp_unit(trace.photonic_identity.coherence), feedback_norm}),
    };
    return frame;
}

}  // namespace qbit_miner