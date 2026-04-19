#include "qbit_miner/runtime/substrate_controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace qbit_miner {

namespace {

SubstrateTrace make_failure_trace(const GpuFeedbackFrame& frame) {
    SubstrateTrace trace;
    trace.photonic_identity = frame.photonic_identity;
    trace.timing = frame.timing;
    trace.encodable_node_count = frame.encodable_node_count;
    trace.status = "failed";
    return trace;
}

[[nodiscard]] SubstrateStratumPhaseFluxMeasurement build_controller_phase_trace_measurement(
    const PhaseClampedShareActuation& actuation
) {
    SubstrateStratumPhaseFluxMeasurement measurement;
    measurement.carrier_phase_turns = actuation.phase_position_turns;
    measurement.target_phase_turns = actuation.target_phase_turns;
    measurement.search_epoch_turns = actuation.gpu_pulse_phase_turns;
    measurement.phase_pressure = std::clamp(
        (0.42 * actuation.phase_clamp_strength)
        + (0.28 * actuation.share_confidence)
        + (0.18 * actuation.resonance_activation_norm)
        + (0.12 * actuation.target_resonance_norm),
        0.0,
        1.0
    );
    measurement.flux_transport_norm = actuation.flux_phase_transport_norm;
    measurement.observer_factor = std::clamp(
        (0.55 * actuation.observer_collapse_strength)
        + (0.25 * actuation.share_confidence)
        + (0.20 * (1.0 - actuation.phase_lock_error)),
        0.0,
        1.0
    );
    measurement.zero_point_proximity = actuation.zero_point_proximity;
    measurement.temporal_admissibility = actuation.temporal_admissibility;
    measurement.trajectory_conservation = actuation.phase_flux_conservation;
    measurement.phase_lock_error = actuation.phase_lock_error;
    measurement.anchor_evm_norm = std::clamp(
        (0.50 * actuation.target_boundary_norm)
        + (0.50 * (1.0 - actuation.selected_coherence_score)),
        0.0,
        1.0
    );
    measurement.sideband_energy_norm = actuation.rf_sideband_energy_norm;
    measurement.interference_projection = actuation.interference_projection;
    measurement.rf_carrier_frequency_norm = actuation.rf_carrier_frequency_norm;
    measurement.rf_envelope_amplitude_norm = actuation.rf_envelope_amplitude_norm;
    measurement.rf_phase_position_turns = actuation.rf_phase_position_turns;
    measurement.rf_phase_velocity_turns = actuation.rf_phase_velocity_turns;
    measurement.rf_zero_point_displacement_turns = actuation.rf_zero_point_displacement_turns;
    measurement.rf_zero_point_distance_norm = actuation.rf_zero_point_distance_norm;
    measurement.rf_spin_drive_signed = actuation.rf_spin_drive_signed;
    measurement.rf_rotation_orientation_signed = actuation.rf_rotation_orientation_signed;
    measurement.rf_temporal_coupling_norm = actuation.rf_temporal_coupling_norm;
    measurement.rf_resonance_hold_norm = actuation.rf_resonance_hold_norm;
    measurement.rf_sideband_energy_norm = actuation.rf_sideband_energy_norm;
    measurement.rf_energy_transfer_norm = actuation.rf_energy_transfer_norm;
    measurement.rf_particle_stability_norm = actuation.rf_particle_stability_norm;
    measurement.transfer_drive_norm = actuation.transfer_drive_norm;
    measurement.stability_gate_norm = actuation.stability_gate_norm;
    measurement.damping_norm = actuation.damping_norm;
    measurement.spin_alignment_norm = actuation.spin_alignment_norm;
    measurement.transport_drive_norm = actuation.transport_drive_norm;
    measurement.target_resonance_norm = actuation.target_resonance_norm;
    measurement.resonance_activation_norm = actuation.resonance_activation_norm;
    return measurement;
}

void attach_sha256_phase_trace(
    PhaseClampedShareActuation& actuation,
    const SubstrateStratumAuthorityState& authority_state
) {
    if (actuation.has_sha256_phase_trace && actuation.sha256_phase_trace.performed) {
        return;
    }
    if (!authority_state.has_active_job || authority_state.active_job_nbits.empty()) {
        return;
    }

    std::uint32_t nonce_value = 0U;
    if (!try_parse_stratum_nonce_hex(actuation.nonce_hex, nonce_value)) {
        return;
    }

    SubstrateStratumWorkerAssignment assignment;
    if (actuation.worker_index < authority_state.worker_assignments.size()) {
        assignment = authority_state.worker_assignments[actuation.worker_index];
    }
    assignment.worker_index = actuation.worker_index;
    assignment.job_id = actuation.job_id.empty() ? authority_state.active_job_id : actuation.job_id;
    assignment.worker_name = actuation.worker_name;

    const std::string worker_header_hex = resolve_stratum_job_header_hex(authority_state, assignment);
    if (worker_header_hex.empty()) {
        return;
    }

    const std::string target_nbits = actuation.target_compact_nbits.empty()
        ? authority_state.active_job_nbits
        : actuation.target_compact_nbits;
    const SubstrateStratumPowPhaseTrace phase_trace = trace_stratum_pow_phase(
        worker_header_hex,
        target_nbits,
        nonce_value,
        authority_state.difficulty,
        build_controller_phase_trace_measurement(actuation)
    );
    if (!phase_trace.performed) {
        return;
    }

    actuation.sha256_phase_trace = phase_trace;
    actuation.has_sha256_phase_trace = true;
    actuation.hash_hex = phase_trace.evaluation.hash_hex;
    actuation.target_hex = phase_trace.evaluation.target_hex;
    actuation.share_target_hex = phase_trace.evaluation.share_target_hex;
    actuation.block_target_hex = phase_trace.evaluation.block_target_hex;
    actuation.offline_pow_checked = true;
    actuation.offline_pow_valid = phase_trace.evaluation.valid_share;
    actuation.block_candidate_valid = phase_trace.evaluation.valid_block;
    actuation.measured_hash_phase_turns = phase_trace.collapse_feedback.measured_hash_phase_turns;
    actuation.measured_nonce_phase_turns = phase_trace.collapse_feedback.measured_nonce_phase_turns;
    actuation.collapse_feedback_phase_turns = phase_trace.collapse_feedback.feedback_phase_turns;
    actuation.collapse_relock_error_turns = phase_trace.collapse_feedback.relock_error_turns;
    actuation.observer_collapse_strength = phase_trace.collapse_feedback.observer_collapse_strength;
    actuation.phase_flux_conservation = phase_trace.collapse_feedback.phase_flux_conservation;
    actuation.nonce_collapse_confidence = phase_trace.collapse_feedback.nonce_collapse_confidence;
    actuation.target_phase_turns = phase_trace.resonant_measurement.target_phase_turns;
    actuation.phase_position_turns = phase_trace.resonant_measurement.carrier_phase_turns;
    actuation.transfer_drive_norm = phase_trace.resonant_measurement.transfer_drive_norm;
    actuation.stability_gate_norm = phase_trace.resonant_measurement.stability_gate_norm;
    actuation.damping_norm = phase_trace.resonant_measurement.damping_norm;
    actuation.transport_drive_norm = phase_trace.resonant_measurement.transport_drive_norm;
    actuation.target_resonance_norm = phase_trace.resonant_measurement.target_resonance_norm;
    actuation.resonance_activation_norm = phase_trace.resonant_measurement.resonance_activation_norm;
    if (actuation.phase_temporal_sequence.empty()) {
        actuation.phase_temporal_sequence = phase_trace.temporal_sequence;
    } else {
        actuation.phase_temporal_sequence += "|sha256_trace:" + phase_trace.temporal_sequence;
    }
}

}  // namespace

SubstrateController::SubstrateController(FieldDynamicsConfig field_config, SubstrateControllerConfig controller_config)
    : application_(std::move(field_config)),
    config_(controller_config),
    shadow_scheduler_(),
    mining_operating_system_() {}

const SubstrateControllerConfig& SubstrateController::config() const noexcept {
    return config_;
}

QuantumMinerApplication& SubstrateController::application() noexcept {
    return application_;
}

const QuantumMinerApplication& SubstrateController::application() const noexcept {
    return application_;
}

RuntimeBus& SubstrateController::bus() noexcept {
    return application_.bus();
}

const SubstrateCache& SubstrateController::cache() const noexcept {
    return application_.cache();
}

SubstrateTrace SubstrateController::process_feedback(const GpuFeedbackFrame& frame) {
    SubstrateTrace trace = application_.process_feedback(frame);
    publish_trace_topics(trace);
    if (config_.emit_phase_shadow_schedule || config_.emit_phase_clamped_share_actuation) {
        const PhaseDispatchArtifact artifact = build_phase_dispatch_artifact(trace);
        if (config_.emit_phase_shadow_schedule) {
            publish_phase_shadow_schedule(trace, artifact);
        }
        if (config_.emit_phase_clamped_share_actuation) {
            publish_phase_clamped_share_actuations(trace, build_share_actuations(trace, artifact));
        }
    }
    return trace;
}

SubstrateRunSummary SubstrateController::run_replay(const std::vector<GpuFeedbackFrame>& frames, std::size_t runtime_ticks) {
    SubstrateRunSummary summary;
    if (frames.empty()) {
        return summary;
    }

    const std::size_t requested_ticks = runtime_ticks == 0 ? config_.runtime_ticks : runtime_ticks;
    const std::size_t total_ticks = requested_ticks == 0 ? frames.size() : requested_ticks;
    summary.requested_ticks = total_ticks;
    summary.traces.reserve(total_ticks);
    summary.phase_dispatch_artifacts.reserve(total_ticks);
    summary.share_actuations.reserve(total_ticks * kStratumWorkerSlotCount);

    for (std::size_t tick_index = 0; tick_index < total_ticks; ++tick_index) {
        const GpuFeedbackFrame& frame = frames[tick_index % frames.size()];
        try {
            summary.traces.push_back(process_feedback(frame));
            if (config_.emit_phase_shadow_schedule) {
                summary.phase_dispatch_artifacts.push_back(build_phase_dispatch_artifact(summary.traces.back()));
            }
            if (config_.emit_phase_clamped_share_actuation) {
                const PhaseDispatchArtifact artifact = config_.emit_phase_shadow_schedule
                    ? summary.phase_dispatch_artifacts.back()
                    : build_phase_dispatch_artifact(summary.traces.back());
                const auto actuations = build_share_actuations(summary.traces.back(), artifact);
                summary.share_actuations.insert(summary.share_actuations.end(), actuations.begin(), actuations.end());
            }
            ++summary.processed_ticks;
        } catch (const std::exception& error) {
            publish_failure(frame, error.what());
            ++summary.failed_ticks;
        } catch (...) {
            publish_failure(frame, "Unknown controller failure");
            ++summary.failed_ticks;
        }
        sleep_if_needed(tick_index, total_ticks);
    }

    return summary;
}

PhaseDispatchArtifact SubstrateController::build_phase_dispatch_artifact(const SubstrateTrace& trace) const {
    return shadow_scheduler_.compute_artifact(trace);
}

std::vector<PhaseClampedShareActuation> SubstrateController::build_share_actuations(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact
) const {
    const SubstrateFirmwareState firmware_state = application_.firmware_runtime().snapshot();
    if (!firmware_state.has_stratum_authority) {
        return {};
    }
    std::vector<PhaseClampedShareActuation> actuations =
        mining_operating_system_.compute_share_actuations(trace, artifact, &firmware_state.stratum_authority);
    for (auto& actuation : actuations) {
        attach_sha256_phase_trace(actuation, firmware_state.stratum_authority);
    }
    return actuations;
}

void SubstrateController::publish_trace_topics(const SubstrateTrace& trace) {
    if (config_.emit_gui_trace_refresh) {
        bus().publish(RuntimeEvent{
            "gui.trace.refresh",
            "Substrate trace ready for GUI consumers",
            trace,
        });
    }

    if (config_.emit_network_trace_publish) {
        bus().publish(RuntimeEvent{
            "network.trace.publish",
            "Substrate trace ready for network publication",
            trace,
        });
    }
}

void SubstrateController::publish_phase_shadow_schedule(const SubstrateTrace& trace, const PhaseDispatchArtifact& artifact) {
    bus().publish(RuntimeEvent{
        "substrate.phase.shadow_schedule",
        "Phase transport shadow scheduler produced a deterministic dispatch artifact",
        trace,
        artifact,
        true,
    });

    if (!artifact.dispatch_permitted) {
        return;
    }

    bus().publish(RuntimeEvent{
        artifact.egress_topic,
        "Phase transport shadow scheduler authorized a relay-ready egress artifact",
        trace,
        artifact,
        true,
    });
}

void SubstrateController::publish_phase_clamped_share_actuations(
    const SubstrateTrace& trace,
    const std::vector<PhaseClampedShareActuation>& actuations
) {
    for (const auto& actuation : actuations) {
        RuntimeEvent event;
        event.topic = actuation.actuation_topic;
        event.message = actuation.actuation_permitted
            ? "Phase-clamped Bitcoin mining operating system authorized a confined share actuation"
            : (actuation.resonant_candidate_available
                ? "Phase-clamped Bitcoin mining operating system produced a GPU-resonant candidate and kept network send gated"
                : "Phase-clamped Bitcoin mining operating system refused a share outside the target envelope");
        event.trace = trace;
        event.phase_clamped_share_actuation = actuation;
        event.has_phase_clamped_share_actuation = true;
        bus().publish(event);
    }
}

void SubstrateController::publish_failure(const GpuFeedbackFrame& frame, const std::string& message) {
    bus().publish(RuntimeEvent{
        "substrate.trace.failed",
        message,
        make_failure_trace(frame),
    });
}

void SubstrateController::sleep_if_needed(std::size_t tick_index, std::size_t total_ticks) const {
    if (config_.tick_interval_ms == 0 || (tick_index + 1) >= total_ticks) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.tick_interval_ms));
}

}  // namespace qbit_miner
