#include "qbit_miner/runtime/phase_clamped_mining_os.hpp"

#include "qbit_miner/runtime/substrate_phase_program_metadata.hpp"
#include "qbit_miner/runtime/substrate_stratum_pow.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace qbit_miner {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double clamp_signed(double value, double limit = 1.0) {
    return std::clamp(value, -std::abs(limit), std::abs(limit));
}

[[nodiscard]] double normalize_positive(double value) {
    return clamp_unit(value / (1.0 + std::abs(value)));
}

[[nodiscard]] double wrap_turns(double turns) {
    double wrapped = std::fmod(turns, 1.0);
    if (wrapped < 0.0) {
        wrapped += 1.0;
    }
    return wrapped;
}

[[nodiscard]] double signed_phase_component_turns(double value) {
    return wrap_turns(0.5 + (0.25 * clamp_signed(value)));
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

[[nodiscard]] double spider_map_vector_delta_norm(const SubstrateTrace& trace) {
    if (trace.trajectory_9d.empty() || trace.photonic_identity.spectra_9d.empty()) {
        return 0.0;
    }

    const std::size_t axis_count = std::min(
        trace.trajectory_9d.size(),
        trace.photonic_identity.spectra_9d.size());
    if (axis_count == 0U) {
        return 0.0;
    }

    double accumulated_delta = 0.0;
    for (std::size_t axis_index = 0; axis_index < axis_count; ++axis_index) {
        accumulated_delta += std::abs(
            clamp_signed(trace.photonic_identity.spectra_9d[axis_index], 4.0)
            - clamp_signed(trace.trajectory_9d[axis_index], 4.0));
    }
    return clamp_unit(accumulated_delta / (static_cast<double>(axis_count) * 2.0));
}

[[nodiscard]] double phase_peak_proximity(double phase_turns) {
    const double positive_peak_distance = std::abs(phase_delta_turns(phase_turns, 0.25));
    const double negative_peak_distance = std::abs(phase_delta_turns(phase_turns, 0.75));
    return clamp_unit(1.0 - (std::min(positive_peak_distance, negative_peak_distance) / 0.25));
}

[[nodiscard]] double orientation_shear_norm(const std::array<double, 3>& orientation_axis) {
    return clamp_unit(
        (std::abs(orientation_axis[0] - orientation_axis[1])
            + std::abs(orientation_axis[1] - orientation_axis[2])
            + std::abs(orientation_axis[0] - orientation_axis[2]))
        / 3.0);
}

[[nodiscard]] std::string format_nonce_hex(std::uint32_t nonce_value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(8) << nonce_value;
    return stream.str();
}

[[nodiscard]] double uint32_phase_turns(std::uint32_t value) {
    constexpr double kUint32TurnCount = 4294967296.0;
    return wrap_turns(static_cast<double>(value) / kUint32TurnCount);
}

[[nodiscard]] int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

[[nodiscard]] std::string normalize_hex(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (normalized.empty()) {
        normalized = "0";
    }
    if ((normalized.size() % 2U) != 0U) {
        normalized.insert(normalized.begin(), '0');
    }
    return normalized;
}

[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(const std::string& value) {
    const std::string normalized = normalize_hex(value);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(normalized.size() / 2U);
    for (std::size_t index = 0; index + 1U < normalized.size(); index += 2U) {
        const int high = hex_value(normalized[index]);
        const int low = hex_value(normalized[index + 1U]);
        if (high < 0 || low < 0) {
            continue;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4U) | low));
    }
    return bytes;
}

[[nodiscard]] double bytes_to_phase_turns(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t count
) {
    if (bytes.empty() || count == 0U) {
        return 0.0;
    }

    std::uint64_t accumulator = 0U;
    const std::size_t limit = std::min(bytes.size(), offset + count);
    for (std::size_t index = offset; index < limit; ++index) {
        accumulator = (accumulator << 8U) ^ static_cast<std::uint64_t>(bytes[index]);
    }
    const double normalized = static_cast<double>(accumulator & 0xffffffffULL) / 4294967296.0;
    return wrap_turns(normalized);
}

[[nodiscard]] double share_target_phase_turns(const std::string& target_hex) {
    return bytes_to_phase_turns(hex_to_bytes(target_hex), 0U, 16U);
}

[[nodiscard]] double centered_unit_bias(double value) {
    return clamp_signed(clamp_unit(value) - 0.5, 0.5);
}

[[nodiscard]] double phase_approach_turns(double anchor_turns, double candidate_turns, double weight) {
    return wrap_turns(anchor_turns + (phase_delta_turns(candidate_turns, anchor_turns) * weight));
}

struct EncodedTargetHarmonic {
    double phase_turns = 0.0;
    double coherence = 0.0;
};

[[nodiscard]] EncodedTargetHarmonic encode_spider_graph_target_harmonic(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact
) {
    constexpr double kSpiderSpokeCount = 9.0;

    double harmonic_real = 0.0;
    double harmonic_imag = 0.0;
    double magnitude_total = 0.0;

    for (std::size_t spoke_index = 0; spoke_index < trace.photonic_identity.spectra_9d.size(); ++spoke_index) {
        const double anchor_value = clamp_signed(trace.photonic_identity.spectra_9d[spoke_index]);
        const double trajectory_value = clamp_signed(trace.trajectory_9d[spoke_index]);
        const double spoke_weight = (0.64 * anchor_value) + (0.36 * trajectory_value);
        const double spoke_magnitude = std::abs(spoke_weight);
        if (spoke_magnitude <= 1.0e-12) {
            continue;
        }

        double spoke_turns = static_cast<double>(spoke_index) / kSpiderSpokeCount;
        if (spoke_weight < 0.0) {
            spoke_turns = wrap_turns(spoke_turns + 0.5);
        }

        harmonic_real += spoke_magnitude * std::cos(kTwoPi * spoke_turns);
        harmonic_imag += spoke_magnitude * std::sin(kTwoPi * spoke_turns);
        magnitude_total += spoke_magnitude;
    }

    EncodedTargetHarmonic harmonic;
    if (magnitude_total <= 1.0e-12) {
        harmonic.phase_turns = wrap_turns(trace.photonic_identity.field_vector.phase);
        harmonic.coherence = 0.0;
        return harmonic;
    }

    harmonic.phase_turns = wrap_turns(std::atan2(harmonic_imag, harmonic_real) / kTwoPi);
    harmonic.coherence = clamp_unit(std::sqrt(
        (harmonic_real * harmonic_real) + (harmonic_imag * harmonic_imag)) / magnitude_total);

    harmonic.phase_turns = wrap_turns(
        harmonic.phase_turns
        + (0.10 * centered_unit_bias(trace.photonic_identity.field_vector.frequency))
        + (0.08 * centered_unit_bias(artifact.phase_lock_pressure))
        + (0.06 * centered_unit_bias(trace.derived_constants.phase_alignment_probability)));
    return harmonic;
}

[[nodiscard]] double phase_window_turns(
    const PhaseClampedMiningConfig& config,
    double target_coherence,
    double phase_alignment_probability
) {
    const double restraint_norm = std::clamp(
        (0.55 * clamp_unit(target_coherence))
            + (0.25 * clamp_unit(phase_alignment_probability)),
        0.0,
        0.80);
    const double weighted_window =
        config.maximum_phase_window_turns * (1.0 - (0.35 * restraint_norm));
    return std::clamp(
        weighted_window,
        config.minimum_phase_window_turns,
        config.maximum_phase_window_turns);
}

[[nodiscard]] double target_phase_turns(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    const SubstrateStratumAuthorityState& authority_state
) {
    const EncodedTargetHarmonic harmonic = encode_spider_graph_target_harmonic(trace, artifact);
    const double pool_target_turns = share_target_phase_turns(authority_state.active_share_target_hex);
    const bool has_pool_target_turns = !authority_state.active_share_target_hex.empty();
    const double target_sequence_weight = has_pool_target_turns
        ? std::clamp(0.28 + (0.34 * harmonic.coherence), 0.28, 0.62)
        : 0.0;
    const double position_tether_weight = std::clamp(
        0.34 + (0.26 * harmonic.coherence),
        0.34,
        0.72);
    const double field_tether_weight = std::clamp(
        0.24 + (0.20 * clamp_unit(trace.derived_constants.phase_alignment_probability)),
        0.24,
        0.56);
    double target_turns = harmonic.phase_turns;
    if (has_pool_target_turns) {
        target_turns = phase_approach_turns(target_turns, pool_target_turns, target_sequence_weight);
    }
    target_turns = phase_approach_turns(target_turns, trace.derived_constants.phase_position_turns, position_tether_weight);
    target_turns = phase_approach_turns(target_turns, wrap_turns(trace.photonic_identity.field_vector.phase), field_tether_weight);
    return wrap_turns(
        target_turns
        + (0.05 * centered_unit_bias(trace.zero_point_overlap_score) * harmonic.coherence)
        + (0.03 * centered_unit_bias(artifact.zero_point_proximity))
        + (0.02 * centered_unit_bias(trace.derived_constants.phase_alignment_probability)));
}

struct TargetRelativeTemporalAlignment {
    double zero_point_relative_turns = 0.0;
    double reverse_flux_phase_turns = 0.0;
    double relative_sequence_turns = 0.0;
    double alignment_norm = 0.0;
};

struct MiningResonanceProgramBinding {
    const SubstratePhaseProgramMetadata* metadata = nullptr;
    bool candidate_nonce_surface_ready = false;
    bool validation_structure_ready = false;
    bool same_pulse_validation_ready = false;
    bool pool_ingest_ready = false;
    bool pool_submit_ready = false;
    bool difficulty_hashrate_ratio_ready = false;
    bool deterministic_replay_ready = false;
    bool substrate_native_ready = false;
};

[[nodiscard]] MiningResonanceProgramBinding bind_mining_resonance_program() {
    const SubstratePhaseProgramMetadata& metadata = mining_resonance_program_metadata();

    MiningResonanceProgramBinding binding;
    binding.metadata = &metadata;
    binding.candidate_nonce_surface_ready =
        metadata.has_block("harmonic_sha_field", "transport")
        && metadata.block_has_rule_token("harmonic_sha_field", "candidate_nonce_surface");
    binding.validation_structure_ready =
        metadata.block_has_rule_token("harmonic_sha_field", "validation_structure_vector");
    binding.same_pulse_validation_ready =
        metadata.block_has_rule_token("harmonic_sha_field", "same_pulse_validation");
    binding.pool_ingest_ready = metadata.block_has_rule_token("harmonic_sha_field", "pool_ingest_vector");
    binding.pool_submit_ready = metadata.block_has_rule_token("harmonic_sha_field", "pool_submit_vector");
    binding.difficulty_hashrate_ratio_ready =
        metadata.block_has_rule_token("harmonic_sha_field", "difficulty_hashrate_ratio")
        || metadata.block_has_rule_token("mining_submit_readiness", "difficulty_hashrate_ratio")
        || metadata.block_has_rule_token("mining_candidate_topology", "difficulty_hashrate_ratio");
    binding.deterministic_replay_ready =
        metadata.has_block("mining_resonance_buffer", "commit")
        && metadata.block_has_rule_token("mining_resonance_buffer", "replay: deterministic");
    binding.substrate_native_ready =
        !metadata.empty()
        && metadata.has_block("mining_os_resonance_field", "carrier")
        && metadata.has_block("harmonic_sha_field", "transport")
        && metadata.has_block("temporal_phase_trajectory_accounting", "scheduler")
        && metadata.has_block("mining_candidate_topology", "association")
        && metadata.has_block("mining_submit_readiness", "gate")
        && binding.candidate_nonce_surface_ready
        && binding.validation_structure_ready
        && binding.same_pulse_validation_ready
        && binding.pool_ingest_ready
        && binding.pool_submit_ready
        && binding.difficulty_hashrate_ratio_ready
        && binding.deterministic_replay_ready;
    return binding;
}

[[nodiscard]] TargetRelativeTemporalAlignment measure_target_relative_temporal_alignment(
    const SubstrateTrace& trace,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    double target_phase_turns,
    const SubstrateStratumPowCollapseFeedback* feedback = nullptr
) {
    TargetRelativeTemporalAlignment alignment;

    const double field_vector_phase_turns = wrap_turns(trace.photonic_identity.field_vector.phase);
    alignment.zero_point_relative_turns = phase_delta_turns(trace.derived_constants.phase_position_turns, 0.0);
    const double zero_point_phase_turns = wrap_turns(alignment.zero_point_relative_turns);
    const double feedback_phase_turns = feedback == nullptr
        ? measurement.carrier_phase_turns
        : feedback->feedback_phase_turns;
    const double measured_hash_phase_turns = feedback == nullptr
        ? measurement.sha256_digest_phase_turns
        : feedback->measured_hash_phase_turns;

    const double temporal_energy_dispersion_norm = mean_unit({
        measurement.rf_temporal_coupling_norm,
        measurement.temporal_admissibility,
        measurement.rf_energy_transfer_norm,
        measurement.transport_drive_norm,
        1.0 - measurement.damping_norm,
        feedback == nullptr ? measurement.target_resonance_norm : feedback->phase_flux_conservation,
        feedback == nullptr ? measurement.resonance_activation_norm : feedback->nonce_collapse_confidence,
    });
    const double reverse_flux_phase_term_turns = clamp_signed(
        (0.34 * phase_delta_turns(measurement.search_epoch_turns, field_vector_phase_turns))
        + (0.28 * phase_delta_turns(measurement.carrier_phase_turns, zero_point_phase_turns))
        + (0.22 * measurement.rf_phase_velocity_turns)
        + (0.16 * measurement.rf_zero_point_displacement_turns)
        + (0.10 * phase_delta_turns(feedback_phase_turns, measurement.carrier_phase_turns))
        + (0.08 * phase_delta_turns(measured_hash_phase_turns, target_phase_turns)),
        0.5) * temporal_energy_dispersion_norm * trace.reverse_causal_flux_coherence;

    alignment.reverse_flux_phase_turns = wrap_turns(
        field_vector_phase_turns
        + (0.50 * phase_delta_turns(zero_point_phase_turns, field_vector_phase_turns))
        + reverse_flux_phase_term_turns);
    alignment.relative_sequence_turns = wrap_turns(
        (0.32 * measurement.carrier_phase_turns)
        + (0.24 * field_vector_phase_turns)
        + (0.14 * zero_point_phase_turns)
        + (0.12 * alignment.reverse_flux_phase_turns)
        + (0.10 * measurement.rf_phase_position_turns)
        + (0.04 * measurement.rf_phase_velocity_turns)
        + (0.04 * measurement.search_epoch_turns)
        + (0.08 * feedback_phase_turns)
        + (0.04 * measured_hash_phase_turns)
        - (0.04 * measurement.damping_norm));

    const double sequence_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(alignment.relative_sequence_turns, target_phase_turns)) * 2.0);
    const double field_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(field_vector_phase_turns, target_phase_turns)) * 2.0);
    const double reverse_flux_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(alignment.reverse_flux_phase_turns, target_phase_turns)) * 2.0);
    const double feedback_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback_phase_turns, target_phase_turns)) * 2.0);

    alignment.alignment_norm = clamp_unit(
        (0.46 * sequence_alignment)
        + (0.18 * reverse_flux_alignment)
        + (0.16 * field_alignment)
        + (0.20 * feedback_alignment));
    return alignment;
}

[[nodiscard]] std::string first_gate_failure(
    const PhaseClampedMiningConfig& config,
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    const MiningResonanceProgramBinding& program_binding,
    bool authority_ready
) {
    if (!program_binding.substrate_native_ready) {
        return "phase_program_binding";
    }
    if (!authority_ready) {
        return "stratum_authority_missing";
    }
    if (trace.status != "ready") {
        return "trace_not_ready";
    }
    if (artifact.transport_mode == "hold") {
        return "transport_hold_mode";
    }
    if (artifact.transport_readiness < config.min_transport_readiness) {
        return "transport_readiness";
    }
    if (trace.derived_constants.zero_point_proximity < config.min_zero_point_proximity) {
        return "zero_point_proximity";
    }
    if (artifact.phase_lock_error > config.max_phase_lock_error) {
        return "phase_lock_error";
    }
    if (artifact.anchor_evm_norm > config.max_anchor_evm_norm) {
        return "anchor_evm";
    }
    if (artifact.sideband_energy_norm > config.max_sideband_energy_norm) {
        return "sideband_energy";
    }
    if (artifact.interference_projection > config.max_interference_projection) {
        return "interference_projection";
    }
    if (artifact.relock_pressure > config.max_relock_pressure) {
        return "relock_pressure";
    }
    if (trace.temporal_dynamics_noise > config.max_temporal_noise) {
        return "temporal_noise";
    }
    return {};
}

[[nodiscard]] std::string actuation_topic(bool resonant_candidate_available, bool actuation_permitted) {
    if (actuation_permitted) {
        return "substrate.bitcoin.share.actuation";
    }
    if (resonant_candidate_available) {
        return "substrate.bitcoin.share.candidate";
    }
    return "substrate.bitcoin.share.refused";
}

[[nodiscard]] std::string format_phase_turns(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

[[nodiscard]] std::string build_phase_temporal_sequence(
    const SubstrateTrace& trace,
    double harmonic_phase_turns,
    double target_phase_turns,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    const SubstrateStratumPowCollapseFeedback& feedback
) {
    const double field_vector_phase_turns = wrap_turns(trace.photonic_identity.field_vector.phase);
    const double phase_transport_turns = wrap_turns(trace.phase_transport);
    const double phase_lock_delta_turns = phase_delta_turns(phase_transport_turns, field_vector_phase_turns);
    const TargetRelativeTemporalAlignment temporal_alignment =
        measure_target_relative_temporal_alignment(trace, measurement, target_phase_turns);

    std::ostringstream stream;
    stream
        << "driver:" << format_phase_turns(trace.derived_constants.driver_temporal_alignment)
        << "|position:" << format_phase_turns(trace.derived_constants.phase_position_turns)
        << "|harmonic:" << format_phase_turns(harmonic_phase_turns)
        << "|target:" << format_phase_turns(target_phase_turns)
        << "|field:" << format_phase_turns(field_vector_phase_turns)
        << "|zero:" << format_phase_turns(temporal_alignment.zero_point_relative_turns)
        << "|reverse:" << format_phase_turns(temporal_alignment.reverse_flux_phase_turns)
        << "|sequence:" << format_phase_turns(temporal_alignment.relative_sequence_turns)
        << "|transport:" << format_phase_turns(phase_transport_turns)
        << "|rf:" << format_phase_turns(measurement.rf_phase_position_turns)
        << "|hash:" << format_phase_turns(feedback.measured_hash_phase_turns)
        << "|feedback:" << format_phase_turns(feedback.feedback_phase_turns)
        << "|delta:" << format_phase_turns(phase_lock_delta_turns);
    return stream.str();
}

struct FourierFanoutParameters {
    double flux_phase_transport_norm = 0.0;
    double phi_hat = 0.0;
    double forcing_hat = 0.0;
    double mining_boundary_norm = 0.0;
    double target_boundary_norm = 0.0;
    std::int32_t op_phase_bias_q15 = 0;
    std::uint16_t op_band_w_q15 = 0;
    std::size_t lane_count = 0;
};

struct FourierFanoutResonanceOutcome {
    std::size_t attempted_nonce_count = 0;
    std::size_t valid_nonce_count = 0;
    std::uint32_t primary_nonce_value = 0U;
    std::string primary_nonce_hex;
    std::string primary_hash_hex;
    bool measured_nonce_observed = false;
    double primary_validation_structure_norm = 0.0;
    SubstrateStratumPowCollapseFeedback primary_collapse_feedback;
    struct CandidateSample {
        std::uint32_t nonce_value = 0U;
        std::string nonce_hex;
        std::size_t lane_index = 0U;
        double lane_turns = 0.0;
        double gpu_pulse_phase_turns = 0.0;
        double resonance_field_phase_turns = 0.0;
        double gpu_pulse_propagation_phase_turns = 0.0;
        double validation_structure_norm = 0.0;
        SubstrateStratumPowCollapseFeedback collapse_feedback;
    };
    std::vector<CandidateSample> harmonic_lane_samples;
    std::vector<CandidateSample> ranked_candidate_samples;
    std::vector<std::string> sampled_valid_nonce_hexes;
};

struct ResonantCandidateValidation {
    bool validation_attempted = false;
    bool valid_share = false;
    std::uint32_t nonce_value = 0U;
    std::string nonce_hex;
    SubstrateStratumPowEvaluation evaluation;
    SubstrateStratumPowPhaseTrace phase_trace;
    SubstrateStratumPowCollapseFeedback collapse_feedback;
    bool measured_nonce_observed = false;
    double coherence_score = 0.0;
    double validation_structure_norm = 0.0;
    double gpu_pulse_phase_turns = 0.0;
    double resonance_field_phase_turns = 0.0;
    double gpu_pulse_propagation_phase_turns = 0.0;
    std::size_t parallel_harmonic_count = 0U;
    std::size_t verified_parallel_harmonic_count = 0U;
    std::size_t validated_parallel_harmonic_count = 0U;
    bool all_parallel_harmonics_verified = false;
};

[[nodiscard]] std::size_t count_reinforced_harmonic_lanes(
    const FourierFanoutResonanceOutcome& outcome,
    const std::string& selected_nonce_hex,
    std::size_t fallback_reinforcement_count
) {
    if (selected_nonce_hex.empty()) {
        return fallback_reinforcement_count;
    }

    const auto count_matching_samples = [&selected_nonce_hex](
                                          const std::vector<FourierFanoutResonanceOutcome::CandidateSample>& samples) {
        return static_cast<std::size_t>(std::count_if(
            samples.begin(),
            samples.end(),
            [&selected_nonce_hex](const FourierFanoutResonanceOutcome::CandidateSample& sample) {
                return sample.nonce_hex == selected_nonce_hex;
            }));
    };

    const std::size_t reinforced_count = std::max(
        count_matching_samples(outcome.harmonic_lane_samples),
        count_matching_samples(outcome.ranked_candidate_samples));
    return std::max<std::size_t>(reinforced_count, fallback_reinforcement_count);
}

[[nodiscard]] double measure_block_target_coherence_norm(
    const ResonantCandidateValidation& validated_candidate,
    const SubstrateStratumPowCollapseFeedback& collapse_feedback,
    double target_resonance_norm,
    double phase_alignment,
    double validation_structure_norm
) {
    if (!validated_candidate.validation_attempted || !validated_candidate.phase_trace.performed) {
        return 0.0;
    }

    if (validated_candidate.evaluation.valid_block) {
        return 1.0;
    }

    const double block_digest_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        validated_candidate.phase_trace.resonant_measurement.sha256_digest_phase_turns,
        validated_candidate.phase_trace.block_target_phase_turns)) * 2.0);
    const double block_hash_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        collapse_feedback.measured_hash_phase_turns,
        validated_candidate.phase_trace.block_target_phase_turns)) * 2.0);
    const double share_block_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        validated_candidate.phase_trace.share_target_phase_turns,
        validated_candidate.phase_trace.block_target_phase_turns)) * 2.0);
    return mean_unit({
        block_digest_alignment,
        block_hash_alignment,
        share_block_alignment,
        target_resonance_norm,
        phase_alignment,
        validation_structure_norm,
        collapse_feedback.phase_flux_conservation,
        collapse_feedback.nonce_collapse_confidence,
    });
}

[[nodiscard]] std::uint32_t classify_submit_quality_class(
    bool share_target_pass,
    bool block_target_pass,
    double block_coherence_norm
) {
    if (!share_target_pass) {
        return 0U;
    }
    if (block_target_pass) {
        return 2U;
    }
    return block_coherence_norm >= 0.85 ? 1U : 0U;
}

[[nodiscard]] double compute_submit_priority_score(
    std::uint32_t queue_quality_class,
    double block_coherence_norm,
    std::size_t reinforcement_count,
    double share_confidence,
    double target_resonance_norm,
    bool block_target_pass
) {
    if (block_target_pass || queue_quality_class >= 2U) {
        return 1.0;
    }

    const double quality_weight = queue_quality_class == 1U ? 0.58 : 0.34;
    const double reinforcement_norm = clamp_unit(
        static_cast<double>(std::max<std::size_t>(reinforcement_count, 1U)) / 8.0);
    return clamp_unit(
        quality_weight
        + (0.18 * block_coherence_norm)
        + (0.12 * reinforcement_norm)
        + (0.08 * share_confidence)
        + (0.08 * target_resonance_norm));
}

[[nodiscard]] std::string build_canonical_share_id(
    const std::string& job_id,
    std::size_t worker_index,
    const std::string& nonce_hex,
    const std::string& hash_hex
) {
    std::ostringstream stream;
    stream << job_id << ':' << worker_index << ':' << nonce_hex;
    if (!hash_hex.empty()) {
        stream << ':' << hash_hex;
    }
    return stream.str();
}

struct CandidateValidationAttempt {
    std::size_t candidate_index = 0U;
    FourierFanoutResonanceOutcome::CandidateSample sample;
    SubstrateStratumPowPhaseTrace phase_trace;
    bool valid_share = false;
    SubstrateStratumPowCollapseFeedback validated_collapse_feedback;
    double coherence_score = 0.0;
    double validation_structure_norm = 0.0;
};

struct RfActivationProfile {
    std::array<double, 3> spin_axis_signed {0.0, 0.0, 0.0};
    std::array<double, 3> spin_orientation_signed {0.0, 0.0, 0.0};
    std::size_t dominant_spin_axis_index = 2U;
    double carrier_frequency_norm = 0.0;
    double envelope_amplitude_norm = 0.0;
    double phase_position_turns = 0.0;
    double phase_velocity_turns = 0.0;
    double zero_point_displacement_turns = 0.0;
    double zero_point_distance_norm = 0.0;
    double spin_drive_signed = 0.0;
    double rotation_orientation_signed = 0.0;
    double temporal_coupling_norm = 0.0;
    double resonance_hold_norm = 0.0;
    double sideband_energy_norm = 0.0;
    double energy_transfer_norm = 0.0;
    double particle_stability_norm = 0.0;
    double transfer_drive_norm = 0.0;
    double stability_gate_norm = 0.0;
    double damping_norm = 0.0;
    double spin_alignment_norm = 0.0;
    double transport_drive_norm = 0.0;
};

[[nodiscard]] double instantiate_resonance_activation_norm(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    double target_boundary_norm,
    const RfActivationProfile& rf_profile,
    double target_phase_turns,
    double phase_clamp_strength
) {
    const double spider_delta_norm = spider_map_vector_delta_norm(trace);
    const double target_lock_norm = 1.0 - clamp_unit(std::abs(
        phase_delta_turns(rf_profile.phase_position_turns, target_phase_turns)) * 2.0);
    const double temporal_conservation_norm = mean_unit({
        trace.trajectory_conservation_score,
        trace.constant_phase_alignment,
        artifact.temporal_admissibility,
        trace.derived_constants.driver_temporal_alignment,
        rf_profile.temporal_coupling_norm,
        rf_profile.resonance_hold_norm,
        rf_profile.particle_stability_norm,
    });
    return clamp_unit(
        (0.26 * spider_delta_norm)
        + (0.18 * temporal_conservation_norm)
        + (0.16 * rf_profile.transfer_drive_norm)
        + (0.14 * rf_profile.stability_gate_norm)
        + (0.12 * target_boundary_norm)
        + (0.08 * phase_clamp_strength)
        + (0.08 * target_lock_norm)
        + (0.06 * trace.derived_constants.axis_resonance)
        - (0.08 * rf_profile.damping_norm));
}

[[nodiscard]] RfActivationProfile derive_rf_activation_profile(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    double phase_clamp_strength
) {
    RfActivationProfile profile;
    const auto& spin = trace.photonic_identity.spin_inertia;
    profile.spin_axis_signed = {
        clamp_signed(spin.axis_spin[0]),
        clamp_signed(spin.axis_spin[1]),
        clamp_signed(spin.axis_spin[2]),
    };
    profile.spin_orientation_signed = {
        clamp_signed(spin.axis_orientation[0]),
        clamp_signed(spin.axis_orientation[1]),
        clamp_signed(spin.axis_orientation[2]),
    };

    const std::array<double, 3> axis_scale{
        trace.derived_constants.axis_scale_x,
        trace.derived_constants.axis_scale_y,
        trace.derived_constants.axis_scale_z,
    };
    double dominant_spin_value = std::abs(profile.spin_axis_signed[0]);
    for (std::size_t index = 1; index < profile.spin_axis_signed.size(); ++index) {
        const double candidate = std::abs(profile.spin_axis_signed[index]);
        if (candidate > dominant_spin_value) {
            dominant_spin_value = candidate;
            profile.dominant_spin_axis_index = index;
        }
    }

    const double dominant_axis_scale = axis_scale[profile.dominant_spin_axis_index];
    profile.carrier_frequency_norm = mean_unit({
        clamp_unit(trace.photonic_identity.field_vector.frequency),
        artifact.fourier_transport_frequency,
        trace.derived_constants.path_speed_norm,
        trace.derived_constants.driver_temporal_alignment,
    });
    profile.phase_position_turns = trace.derived_constants.phase_position_turns;
    profile.phase_velocity_turns = phase_delta_turns(
        wrap_turns(trace.phase_transport),
        profile.phase_position_turns);
    profile.zero_point_displacement_turns = phase_delta_turns(profile.phase_position_turns, 0.0);
    profile.zero_point_distance_norm = clamp_unit(std::abs(profile.zero_point_displacement_turns) * 4.0);
    profile.spin_drive_signed = profile.spin_axis_signed[profile.dominant_spin_axis_index];
    profile.rotation_orientation_signed =
        profile.spin_orientation_signed[profile.dominant_spin_axis_index];

    const double directional_amplitude_norm = mean_unit({
        dominant_axis_scale,
        trace.derived_constants.vector_energy,
        trace.derived_constants.axis_resonance,
        1.0 - trace.derived_constants.zero_point_line_distance,
    });
    const double zero_point_crossover_norm = mean_unit({
        trace.derived_constants.zero_point_proximity,
        trace.zero_point_overlap_score,
        1.0 - profile.zero_point_distance_norm,
    });
    const double intercept_inertia_norm = mean_unit({
        trace.derived_constants.resonance_intercept_force,
        normalize_positive(trace.substrate_inertia),
        artifact.phase_vector_magnitude,
    });

    profile.envelope_amplitude_norm = mean_unit({
        clamp_unit(trace.photonic_identity.field_vector.amplitude),
        directional_amplitude_norm,
        trace.derived_constants.axis_resonance,
    });
    profile.temporal_coupling_norm = mean_unit({
        artifact.temporal_admissibility,
        trace.derived_constants.temporal_relativity_norm,
        intercept_inertia_norm,
        clamp_unit(std::max(0.0, spin.relative_temporal_coupling)),
        trace.derived_constants.driver_temporal_alignment,
    });
    profile.resonance_hold_norm = mean_unit({
        trace.constant_phase_alignment,
        trace.trajectory_conservation_score,
        trace.derived_constants.axis_resonance,
        trace.derived_constants.driver_resonance,
        phase_clamp_strength,
    });
    profile.sideband_energy_norm = mean_unit({
        artifact.sideband_energy_norm,
        artifact.interference_projection,
        trace.derived_constants.field_interference_norm,
        1.0 - profile.resonance_hold_norm,
    });
    profile.energy_transfer_norm = mean_unit({
        zero_point_crossover_norm,
        std::max(phase_peak_proximity(profile.phase_position_turns), trace.derived_constants.zero_point_proximity),
        std::abs(profile.spin_drive_signed),
        profile.temporal_coupling_norm,
        1.0 - profile.zero_point_distance_norm,
    });
    profile.particle_stability_norm = mean_unit({
        profile.resonance_hold_norm,
        trace.trajectory_conservation_score,
        1.0 - profile.sideband_energy_norm,
        trace.derived_constants.driver_resonance,
    });
    profile.spin_alignment_norm = mean_unit({
        std::abs(profile.spin_orientation_signed[0]),
        std::abs(profile.spin_orientation_signed[1]),
        std::abs(profile.spin_orientation_signed[2]),
        1.0 - orientation_shear_norm(profile.spin_orientation_signed),
    });

    const double direct_transfer_drive = clamp_unit(
        zero_point_crossover_norm
        * (1.0 - profile.zero_point_distance_norm)
        * profile.temporal_coupling_norm
        * std::abs(profile.spin_drive_signed)
        * 6.0);
    profile.transfer_drive_norm = clamp_unit(
        (0.50 * direct_transfer_drive)
        + (0.30 * profile.energy_transfer_norm)
        + (0.20 * std::max(phase_peak_proximity(profile.phase_position_turns), trace.derived_constants.zero_point_proximity)));
    profile.stability_gate_norm = clamp_unit(
        trace.trajectory_conservation_score
        * profile.particle_stability_norm
        * profile.resonance_hold_norm
        * trace.constant_phase_alignment);
    profile.damping_norm = clamp_unit(
        (0.38 * profile.sideband_energy_norm)
        + (0.32 * clamp_unit(trace.temporal_dynamics_noise))
        + (0.30 * artifact.interference_projection));
    profile.transport_drive_norm = clamp_unit(
        (0.36 * phase_clamp_strength)
        + (0.22 * artifact.anchor_correlation)
        + (0.16 * artifact.coherence_alignment)
        + (0.14 * profile.temporal_coupling_norm)
        + (0.12 * trace.constant_phase_alignment)
        + (0.18 * profile.transfer_drive_norm)
        + (0.12 * profile.stability_gate_norm)
        - (0.12 * profile.damping_norm));
    return profile;
}

[[nodiscard]] FourierFanoutParameters build_fourier_fanout_parameters(
    const PhaseClampedMiningConfig& config,
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    const SubstrateStratumAuthorityState& authority_state,
    const RfActivationProfile& rf_profile,
    double target_phase_turns,
    double phase_clamp_strength
) {
    FourierFanoutParameters parameters;

    const double phase_transport_norm = normalize_positive(trace.phase_transport);
    const double flux_transport_norm = normalize_positive(trace.flux_transport);
    parameters.flux_phase_transport_norm = mean_unit({
        phase_transport_norm,
        flux_transport_norm,
        normalize_positive(trace.encoded_pulse[2]),
        trace.derived_constants.phase_alignment_probability,
        artifact.phase_vector_magnitude,
        trace.derived_constants.driver_resonance,
        rf_profile.transport_drive_norm,
        rf_profile.transfer_drive_norm,
        rf_profile.stability_gate_norm,
    });
    parameters.phi_hat = wrap_turns(
        rf_profile.phase_position_turns
        + (0.375 * phase_transport_norm)
        + (0.25 * flux_transport_norm)
        + (0.125 * target_phase_turns)
        + (0.09375 * rf_profile.phase_velocity_turns)
        + (0.078125 * rf_profile.zero_point_displacement_turns)
        + (0.09375 * trace.derived_constants.zero_point_proximity)
        + (0.0625 * trace.derived_constants.driver_temporal_alignment));
    parameters.forcing_hat = mean_unit({
        artifact.transport_readiness,
        artifact.phase_vector_magnitude,
        trace.constant_phase_alignment,
        trace.reverse_causal_flux_coherence,
        1.0 - artifact.interference_projection,
        trace.derived_constants.driver_temporal_alignment,
        trace.derived_constants.driver_invocation_pressure,
        rf_profile.transfer_drive_norm,
        rf_profile.stability_gate_norm,
        1.0 - rf_profile.damping_norm,
    });
    parameters.mining_boundary_norm = mean_unit({
        phase_clamp_strength,
        1.0 - artifact.phase_lock_error,
        trace.constant_phase_alignment,
        trace.derived_constants.zero_point_proximity,
        1.0 - artifact.anchor_evm_norm,
        1.0 - artifact.sideband_energy_norm,
        trace.derived_constants.driver_execution_density,
        rf_profile.stability_gate_norm,
        rf_profile.particle_stability_norm,
        1.0 - rf_profile.damping_norm,
    });
    parameters.target_boundary_norm = mean_unit({
        parameters.forcing_hat,
        1.0 - clamp_unit(std::abs(phase_delta_turns(parameters.phi_hat, target_phase_turns)) * 2.0),
        trace.derived_constants.phase_alignment_probability,
        trace.derived_constants.driver_resonance,
        rf_profile.transfer_drive_norm,
        rf_profile.resonance_hold_norm,
        trace.zero_point_overlap_score,
    });

    const double phase_bias_norm = std::clamp(
        phase_delta_turns(target_phase_turns, trace.derived_constants.phase_position_turns)
            / std::max(config.maximum_phase_window_turns, 1.0e-9),
        -1.0,
        1.0);
    parameters.op_phase_bias_q15 = static_cast<std::int32_t>(std::lround(phase_bias_norm * 32767.0));

    const double band_weight_norm = mean_unit({
        parameters.forcing_hat,
        1.0 - artifact.sideband_energy_norm,
        1.0 - artifact.interference_projection,
        trace.reverse_causal_flux_coherence,
        trace.derived_constants.driver_resonance,
        rf_profile.stability_gate_norm,
        1.0 - rf_profile.damping_norm,
    });
    parameters.op_band_w_q15 = static_cast<std::uint16_t>(std::lround(band_weight_norm * 65535.0));

    const std::size_t max_lanes = std::max<std::size_t>(config.max_fourier_fanout_lanes, 4U);
    parameters.lane_count = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::lround(
            4.0 + mean_unit({
                parameters.forcing_hat,
                trace.derived_constants.driver_execution_density,
                trace.derived_constants.driver_invocation_pressure,
                rf_profile.transport_drive_norm,
                rf_profile.particle_stability_norm,
            }) * static_cast<double>(max_lanes - 4U))),
        4U,
        max_lanes);

    return parameters;
}

[[nodiscard]] SubstrateStratumPhaseFluxMeasurement build_phase_flux_measurement(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    const FourierFanoutParameters& parameters,
    const SubstrateStratumAuthorityState& authority_state,
    const RfActivationProfile& rf_profile,
    double target_phase_turns,
    double phase_clamp_strength
) {
    SubstrateStratumPhaseFluxMeasurement measurement;
    measurement.carrier_phase_turns = parameters.phi_hat;
    measurement.target_phase_turns = target_phase_turns;
    measurement.search_epoch_turns = wrap_turns(
        (static_cast<double>(trace.timing.tick_index & 0xffffULL) / 65536.0)
        + (0.125 * parameters.flux_phase_transport_norm)
        + (0.125 * rf_profile.temporal_coupling_norm)
        + (0.0625 * phase_clamp_strength));
    measurement.phase_pressure = mean_unit({
        phase_clamp_strength,
        parameters.forcing_hat,
        parameters.mining_boundary_norm,
        parameters.target_boundary_norm,
        trace.derived_constants.driver_resonance,
        rf_profile.transfer_drive_norm,
        rf_profile.stability_gate_norm,
        1.0 - rf_profile.damping_norm,
    });
    measurement.flux_transport_norm = parameters.flux_phase_transport_norm;
    measurement.observer_factor = mean_unit({
        normalize_positive(trace.observer_factor),
        trace.derived_constants.observer_gain,
        artifact.anchor_correlation,
        1.0 - artifact.anchor_evm_norm,
        trace.derived_constants.driver_temporal_alignment,
        trace.derived_constants.driver_invocation_pressure,
        rf_profile.spin_alignment_norm,
        rf_profile.temporal_coupling_norm,
    });
    measurement.zero_point_proximity = trace.derived_constants.zero_point_proximity;
    measurement.temporal_admissibility = mean_unit({
        artifact.temporal_admissibility,
        trace.derived_constants.driver_temporal_alignment,
        1.0 - trace.derived_constants.driver_kernel_duration_norm,
        rf_profile.temporal_coupling_norm,
        rf_profile.resonance_hold_norm,
    });
    measurement.trajectory_conservation = mean_unit({
        trace.trajectory_conservation_score,
        trace.derived_constants.driver_execution_density,
        trace.derived_constants.driver_resonance,
        rf_profile.particle_stability_norm,
        rf_profile.stability_gate_norm,
    });
    measurement.phase_lock_error = artifact.phase_lock_error;
    measurement.anchor_evm_norm = artifact.anchor_evm_norm;
    measurement.sideband_energy_norm = artifact.sideband_energy_norm;
    measurement.interference_projection = artifact.interference_projection;
    measurement.rf_carrier_frequency_norm = rf_profile.carrier_frequency_norm;
    measurement.rf_envelope_amplitude_norm = rf_profile.envelope_amplitude_norm;
    measurement.rf_phase_position_turns = rf_profile.phase_position_turns;
    measurement.rf_phase_velocity_turns = rf_profile.phase_velocity_turns;
    measurement.rf_zero_point_displacement_turns = rf_profile.zero_point_displacement_turns;
    measurement.rf_zero_point_distance_norm = rf_profile.zero_point_distance_norm;
    measurement.rf_spin_drive_signed = rf_profile.spin_drive_signed;
    measurement.rf_rotation_orientation_signed = rf_profile.rotation_orientation_signed;
    measurement.rf_temporal_coupling_norm = rf_profile.temporal_coupling_norm;
    measurement.rf_resonance_hold_norm = rf_profile.resonance_hold_norm;
    measurement.rf_sideband_energy_norm = rf_profile.sideband_energy_norm;
    measurement.rf_energy_transfer_norm = rf_profile.energy_transfer_norm;
    measurement.rf_particle_stability_norm = rf_profile.particle_stability_norm;
    measurement.transfer_drive_norm = rf_profile.transfer_drive_norm;
    measurement.stability_gate_norm = rf_profile.stability_gate_norm;
    measurement.damping_norm = rf_profile.damping_norm;
    measurement.spin_alignment_norm = rf_profile.spin_alignment_norm;
    measurement.transport_drive_norm = rf_profile.transport_drive_norm;
    measurement.target_resonance_norm = mean_unit({
        parameters.target_boundary_norm,
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            measurement.carrier_phase_turns,
            target_phase_turns
        )) * 2.0),
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            measurement.rf_phase_position_turns,
            target_phase_turns
        )) * 2.0),
        measurement.zero_point_proximity,
        measurement.transfer_drive_norm,
        measurement.stability_gate_norm,
        1.0 - measurement.damping_norm,
    });
    measurement.resonance_activation_norm = instantiate_resonance_activation_norm(
        trace,
        artifact,
        parameters.target_boundary_norm,
        rf_profile,
        target_phase_turns,
        phase_clamp_strength);
    if (authority_state.last_measured_nonce_observed) {
        measurement.search_epoch_turns = wrap_turns(
            (0.50 * measurement.search_epoch_turns)
            + (0.25 * authority_state.last_collapse_feedback_phase_turns)
            + (0.25 * authority_state.last_measured_nonce_phase_turns));
        measurement.carrier_phase_turns = wrap_turns(
            (0.50 * measurement.carrier_phase_turns)
            + (0.30 * authority_state.last_collapse_feedback_phase_turns)
            + (0.20 * authority_state.last_measured_nonce_phase_turns)
        );
        measurement.target_phase_turns = wrap_turns(
            (0.80 * measurement.target_phase_turns)
            + (0.20 * authority_state.last_measured_hash_phase_turns)
        );
        measurement.phase_pressure = mean_unit({
            measurement.phase_pressure,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.flux_transport_norm = mean_unit({
            measurement.flux_transport_norm,
            authority_state.last_phase_flux_conservation,
            authority_state.last_observer_collapse_strength,
        });
        measurement.observer_factor = mean_unit({
            measurement.observer_factor,
            authority_state.last_observer_collapse_strength,
            1.0 - clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
        });
        measurement.zero_point_proximity = mean_unit({
            measurement.zero_point_proximity,
            authority_state.last_nonce_collapse_confidence,
            authority_state.last_phase_flux_conservation,
        });
        measurement.temporal_admissibility = mean_unit({
            measurement.temporal_admissibility,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.trajectory_conservation = mean_unit({
            measurement.trajectory_conservation,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.phase_lock_error = mean_unit({
            measurement.phase_lock_error,
            clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
        });
        measurement.rf_phase_position_turns = wrap_turns(
            (0.75 * measurement.rf_phase_position_turns)
            + (0.25 * authority_state.last_measured_nonce_phase_turns)
        );
        measurement.rf_phase_velocity_turns = phase_delta_turns(
            authority_state.last_collapse_feedback_phase_turns,
            measurement.rf_phase_position_turns);
        measurement.rf_zero_point_displacement_turns =
            phase_delta_turns(measurement.rf_phase_position_turns, 0.0);
        measurement.rf_zero_point_distance_norm = clamp_unit(
            std::abs(measurement.rf_zero_point_displacement_turns) * 4.0);
        measurement.transfer_drive_norm = mean_unit({
            measurement.transfer_drive_norm,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.stability_gate_norm = mean_unit({
            measurement.stability_gate_norm,
            authority_state.last_phase_flux_conservation,
            1.0 - clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
        });
        measurement.damping_norm = mean_unit({
            measurement.damping_norm,
            1.0 - authority_state.last_observer_collapse_strength,
            1.0 - authority_state.last_nonce_collapse_confidence,
        });
        measurement.rf_temporal_coupling_norm = mean_unit({
            measurement.rf_temporal_coupling_norm,
            measurement.temporal_admissibility,
            authority_state.last_phase_flux_conservation,
        });
        measurement.rf_resonance_hold_norm = mean_unit({
            measurement.rf_resonance_hold_norm,
            measurement.stability_gate_norm,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.rf_sideband_energy_norm = mean_unit({
            measurement.rf_sideband_energy_norm,
            measurement.sideband_energy_norm,
            measurement.damping_norm,
        });
        measurement.rf_energy_transfer_norm = mean_unit({
            measurement.rf_energy_transfer_norm,
            measurement.transfer_drive_norm,
            authority_state.last_phase_flux_conservation,
        });
        measurement.rf_particle_stability_norm = mean_unit({
            measurement.rf_particle_stability_norm,
            measurement.stability_gate_norm,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.spin_alignment_norm = mean_unit({
            measurement.spin_alignment_norm,
            std::abs(measurement.rf_spin_drive_signed),
            std::abs(measurement.rf_rotation_orientation_signed),
            1.0 - measurement.interference_projection,
        });
        measurement.transport_drive_norm = mean_unit({
            measurement.transport_drive_norm,
            measurement.transfer_drive_norm,
            measurement.stability_gate_norm,
            1.0 - measurement.damping_norm,
        });
        measurement.target_resonance_norm = mean_unit({
            measurement.target_resonance_norm,
            authority_state.last_target_resonance_norm,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
        });
        measurement.resonance_activation_norm = mean_unit({
            measurement.resonance_activation_norm,
            authority_state.last_target_resonance_norm,
            authority_state.last_phase_flux_conservation,
            authority_state.last_nonce_collapse_confidence,
            authority_state.last_observer_collapse_strength,
        });
    }
    return measurement;
}

[[nodiscard]] double fourier_lane_turns(
    const FourierFanoutParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    std::size_t lane_index,
    double target_phase_turns
) {
    const double lane_fraction = parameters.lane_count == 0U
        ? 0.0
        : (static_cast<double>(lane_index) / static_cast<double>(parameters.lane_count));
    const double phase_bias_turns = (static_cast<double>(parameters.op_phase_bias_q15) / 32767.0) * 0.125;
    const double band_weight_turns = (static_cast<double>(parameters.op_band_w_q15) / 65535.0) * 0.0625;
    const double carrier_phase_turns = measurement.carrier_phase_turns == 0.0
        ? parameters.phi_hat
        : measurement.carrier_phase_turns;
    const double exploration_turns = wrap_turns(measurement.search_epoch_turns + lane_fraction);
    const double collapse_phase_turns = wrap_turns(
        carrier_phase_turns
        + (0.125 * clamp_unit(measurement.phase_pressure))
        + (0.0625 * clamp_unit(measurement.observer_factor))
        + (0.0625 * clamp_unit(measurement.target_resonance_norm))
        + (0.046875 * clamp_unit(measurement.resonance_activation_norm))
        + (0.0625 * measurement.rf_phase_velocity_turns)
        + (0.0625 * measurement.rf_zero_point_displacement_turns * clamp_unit(measurement.transfer_drive_norm))
        + (0.03125 * measurement.rf_spin_drive_signed * clamp_unit(measurement.spin_alignment_norm))
        + (0.0625 * measurement.sha256_round_phase_turns)
        + (0.03125 * measurement.sha256_schedule_phase_turns)
        + (0.0625 * exploration_turns)
        - (0.03125 * clamp_unit(measurement.damping_norm))
    );

    return wrap_turns(
        collapse_phase_turns
        + lane_fraction
        + phase_bias_turns
        + (0.125 * exploration_turns)
        + (band_weight_turns * std::sin(kTwoPi * (target_phase_turns + lane_fraction + exploration_turns)))
        + (0.09375 * std::sin(kTwoPi * (parameters.flux_phase_transport_norm + lane_fraction + exploration_turns)))
        + (0.0625 * std::sin(kTwoPi * (
            measurement.transport_drive_norm
            + measurement.resonance_activation_norm
            + measurement.rf_carrier_frequency_norm
            + exploration_turns
            + lane_fraction
        )))
        + (0.09375 * std::sin(kTwoPi * (
            measurement.sha256_schedule_phase_turns
            + measurement.sha256_round_phase_turns
            + exploration_turns
            + lane_fraction
        )))
        + (0.0625 * std::cos(kTwoPi * (
            measurement.sha256_digest_phase_turns
            + measurement.sha256_frequency_bias_norm
            + exploration_turns
            + lane_fraction
        )))
        + (0.0625 * std::cos(kTwoPi * (
            parameters.forcing_hat
            + target_phase_turns
            + lane_fraction
            + exploration_turns
            + clamp_unit(measurement.temporal_admissibility)
            + (0.5 * measurement.rf_temporal_coupling_norm)
        ))));
}

[[nodiscard]] double fourier_lane_center_fraction(
    double lane_turns,
    const FourierFanoutParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    double target_phase_turns
) {
    const double exploration_turns = wrap_turns(measurement.search_epoch_turns + lane_turns);
    return clamp_unit(
        0.5
        + (0.25 * std::sin(kTwoPi * (lane_turns + exploration_turns)))
        + (0.15 * std::sin((2.0 * kTwoPi * lane_turns) + (kTwoPi * (parameters.flux_phase_transport_norm + exploration_turns))))
        + (0.10 * std::sin(kTwoPi * (
            measurement.transfer_drive_norm
            + measurement.resonance_activation_norm
            + measurement.rf_energy_transfer_norm
            + exploration_turns
            + measurement.rf_phase_position_turns
        )))
        + (0.10 * std::sin(kTwoPi * (
            measurement.sha256_schedule_phase_turns
            + measurement.sha256_digest_phase_turns
            + exploration_turns
        )))
        + (0.10 * std::cos(kTwoPi * (
            target_phase_turns
            + parameters.forcing_hat
            + exploration_turns
            + clamp_unit(measurement.observer_factor)
            + (0.5 * clamp_unit(measurement.target_resonance_norm))
        )))
        - (0.10 * clamp_unit(measurement.damping_norm)));
}

[[nodiscard]] std::uint64_t collapse_guided_local_offset(
    std::uint64_t local_window,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    std::uint64_t local_index
) {
    if (local_window <= 1ULL) {
        return 0ULL;
    }

    const double exploration_turns = wrap_turns(
        measurement.search_epoch_turns
        + (static_cast<double>(local_index & 0xffffULL) / 65536.0));
    const double collapse_phase_turns = wrap_turns(
        measurement.carrier_phase_turns
        + (0.5 * clamp_unit(measurement.phase_pressure))
        + (0.25 * clamp_unit(measurement.observer_factor))
        + (0.125 * clamp_unit(measurement.zero_point_proximity))
        + (0.125 * clamp_unit(measurement.transfer_drive_norm))
        + (0.0625 * clamp_unit(measurement.stability_gate_norm))
        + (0.0625 * clamp_unit(measurement.target_resonance_norm))
        + (0.046875 * clamp_unit(measurement.resonance_activation_norm))
        + (0.0625 * measurement.rf_zero_point_displacement_turns)
        + (0.0625 * measurement.sha256_schedule_phase_turns)
        + (0.03125 * measurement.sha256_round_phase_turns)
        + (0.0625 * exploration_turns)
        - (0.0625 * clamp_unit(measurement.damping_norm))
    );
    const std::uint64_t collapse_center = static_cast<std::uint64_t>(std::llround(
        collapse_phase_turns * static_cast<double>(local_window - 1ULL)));
    std::uint64_t stride = 1ULL + (static_cast<std::uint64_t>(std::llround(
        (0.5
            + clamp_unit(measurement.temporal_admissibility)
            + clamp_unit(measurement.observer_factor)
            + clamp_unit(measurement.stability_gate_norm)
            + clamp_unit(measurement.transport_drive_norm)
            + clamp_unit(measurement.target_resonance_norm)
            + clamp_unit(measurement.resonance_activation_norm)
            + clamp_unit(measurement.sha256_frequency_bias_norm))
        * static_cast<double>(local_window - 1ULL))) % local_window);
    if ((stride % 2ULL) == 0ULL) {
        ++stride;
    }
    if (stride == 0ULL) {
        stride = 1ULL;
    }
    return (collapse_center + (local_index * stride)) % local_window;
}

[[nodiscard]] SubstrateStratumPowCollapseFeedback synthesize_resonant_sha_collapse_feedback(
    const FourierFanoutParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    std::uint32_t nonce_value,
    double lane_turns,
    double target_phase_turns
) {
    SubstrateStratumPowCollapseFeedback feedback;

    const double nonce_phase_turns = uint32_phase_turns(nonce_value);
    const double exploration_turns = wrap_turns(measurement.search_epoch_turns + lane_turns);
    const double sha_interference_turns = wrap_turns(
        (0.34 * measurement.sha256_schedule_phase_turns)
        + (0.28 * measurement.sha256_round_phase_turns)
        + (0.24 * measurement.sha256_digest_phase_turns)
        + (0.08 * measurement.sha256_frequency_bias_norm)
        + (0.06 * measurement.sha256_harmonic_density_norm)
    );
    const double harmonic_projection_turns = wrap_turns(
        lane_turns
        + (0.25 * parameters.flux_phase_transport_norm)
        + (0.125 * parameters.forcing_hat)
        + (0.125 * measurement.rf_phase_velocity_turns)
        + (0.0625 * measurement.rf_zero_point_displacement_turns)
        + (0.0625 * exploration_turns)
    );

    feedback.measured_nonce_phase_turns = nonce_phase_turns;
    feedback.measured_hash_phase_turns = wrap_turns(
        (0.24 * sha_interference_turns)
        + (0.18 * target_phase_turns)
        + (0.14 * harmonic_projection_turns)
        + (0.12 * measurement.carrier_phase_turns)
        + (0.10 * measurement.rf_phase_position_turns)
        + (0.10 * nonce_phase_turns)
        + (0.08 * measurement.transfer_drive_norm)
        + (0.08 * measurement.stability_gate_norm)
        + (0.06 * std::sin(kTwoPi * (
            nonce_phase_turns
            + measurement.sha256_digest_phase_turns
            + harmonic_projection_turns)))
        + (0.05 * std::cos(kTwoPi * (
            measurement.sha256_round_phase_turns
            + measurement.rf_temporal_coupling_norm
            + exploration_turns)))
        + (0.05 * std::sin(kTwoPi * (
            target_phase_turns
            + parameters.phi_hat
            + measurement.target_resonance_norm)))
        - (0.04 * measurement.damping_norm)
    );

    const double target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_hash_phase_turns, target_phase_turns)) * 2.0);
    const double carrier_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_nonce_phase_turns, measurement.rf_phase_position_turns)) * 2.0);
    const double schedule_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_nonce_phase_turns, measurement.sha256_schedule_phase_turns)) * 2.0);
    const double round_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_nonce_phase_turns, measurement.sha256_round_phase_turns)) * 2.0);
    const double digest_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_hash_phase_turns, measurement.sha256_digest_phase_turns)) * 2.0);
    const double zero_point_alignment = mean_unit({
        1.0 - measurement.rf_zero_point_distance_norm,
        phase_peak_proximity(feedback.measured_nonce_phase_turns),
        measurement.zero_point_proximity,
    });
    const double harmonic_hold_norm = mean_unit({
        target_alignment,
        carrier_alignment,
        schedule_alignment,
        round_alignment,
        digest_alignment,
        measurement.transfer_drive_norm,
        measurement.stability_gate_norm,
        measurement.target_resonance_norm,
        1.0 - measurement.damping_norm,
    });

    feedback.feedback_phase_turns = wrap_turns(
        measurement.carrier_phase_turns
        + (0.375 * phase_delta_turns(feedback.measured_hash_phase_turns, target_phase_turns))
        + (0.1875 * phase_delta_turns(feedback.measured_nonce_phase_turns, measurement.rf_phase_position_turns))
        + (0.125 * phase_delta_turns(harmonic_projection_turns, measurement.sha256_round_phase_turns))
        + (0.0625 * measurement.rf_phase_velocity_turns)
        + (0.0625 * measurement.rf_zero_point_displacement_turns * measurement.transfer_drive_norm)
        + (0.03125 * measurement.sha256_frequency_bias_norm)
        + (0.03125 * measurement.transport_drive_norm)
        - (0.03125 * measurement.damping_norm)
    );
    feedback.relock_error_turns = std::abs(
        phase_delta_turns(feedback.feedback_phase_turns, target_phase_turns));
    feedback.phase_flux_conservation = mean_unit({
        harmonic_hold_norm,
        target_alignment,
        digest_alignment,
        measurement.temporal_admissibility,
        measurement.trajectory_conservation,
        measurement.rf_resonance_hold_norm,
        measurement.rf_particle_stability_norm,
        measurement.target_resonance_norm,
        1.0 - measurement.interference_projection,
        1.0 - measurement.damping_norm,
    });
    feedback.observer_collapse_strength = mean_unit({
        feedback.phase_flux_conservation,
        measurement.observer_factor,
        measurement.rf_temporal_coupling_norm,
        measurement.spin_alignment_norm,
        measurement.transfer_drive_norm,
        zero_point_alignment,
    });
    feedback.nonce_collapse_confidence = mean_unit({
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
        target_alignment,
        schedule_alignment,
        digest_alignment,
        measurement.target_resonance_norm,
        measurement.transfer_drive_norm,
        measurement.stability_gate_norm,
        1.0 - measurement.damping_norm,
    });
    return feedback;
}

[[nodiscard]] double validation_structure_presence_norm(
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    const SubstrateStratumPowCollapseFeedback& feedback,
    double target_phase_turns
) {
    const double target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_hash_phase_turns, target_phase_turns)) * 2.0);
    const double schedule_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_nonce_phase_turns,
            measurement.sha256_schedule_phase_turns
        )) * 2.0);
    const double round_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_nonce_phase_turns,
            measurement.sha256_round_phase_turns
        )) * 2.0);
    const double digest_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_hash_phase_turns,
            measurement.sha256_digest_phase_turns
        )) * 2.0);
    return std::max(0.0, std::min({
        target_alignment,
        schedule_alignment,
        round_alignment,
        digest_alignment,
    }));
}

[[nodiscard]] double deterministic_resonance_budget_norm(const SubstrateTrace& trace) {
    if (!trace.gpu_kernel_sync.driver_timing_valid || trace.gpu_kernel_sync.compute_invocation_count == 0U) {
        return 0.0;
    }

    return mean_unit({
        trace.derived_constants.driver_execution_density,
        trace.derived_constants.driver_invocation_pressure,
        trace.derived_constants.driver_temporal_alignment,
        trace.derived_constants.driver_resonance,
        trace.derived_constants.phase_alignment_probability,
        trace.trajectory_conservation_score,
        trace.constant_phase_alignment,
        trace.zero_point_overlap_score,
        trace.reverse_causal_flux_coherence,
        normalize_positive(static_cast<double>(trace.gpu_kernel_sync.compute_invocation_count)),
        1.0 - trace.derived_constants.driver_kernel_duration_norm,
    });
}

[[nodiscard]] bool collapse_feedback_is_better(
    const SubstrateStratumPowCollapseFeedback& candidate,
    const SubstrateStratumPowCollapseFeedback& incumbent,
    std::uint32_t candidate_nonce_value,
    std::uint32_t incumbent_nonce_value
) {
    constexpr double kEpsilon = 1.0e-12;

    if (candidate.nonce_collapse_confidence > incumbent.nonce_collapse_confidence + kEpsilon) {
        return true;
    }
    if (candidate.nonce_collapse_confidence + kEpsilon < incumbent.nonce_collapse_confidence) {
        return false;
    }
    if (candidate.phase_flux_conservation > incumbent.phase_flux_conservation + kEpsilon) {
        return true;
    }
    if (candidate.phase_flux_conservation + kEpsilon < incumbent.phase_flux_conservation) {
        return false;
    }
    if (candidate.observer_collapse_strength > incumbent.observer_collapse_strength + kEpsilon) {
        return true;
    }
    if (candidate.observer_collapse_strength + kEpsilon < incumbent.observer_collapse_strength) {
        return false;
    }
    if (candidate.relock_error_turns + kEpsilon < incumbent.relock_error_turns) {
        return true;
    }
    if (candidate.relock_error_turns > incumbent.relock_error_turns + kEpsilon) {
        return false;
    }
    return candidate_nonce_value < incumbent_nonce_value;
}

[[nodiscard]] std::size_t effective_fanout_resonance_budget(
    const PhaseClampedMiningConfig& config,
    const SubstrateTrace& trace,
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
) {
    const std::uint64_t worker_window_size =
        static_cast<std::uint64_t>(assignment.nonce_end) - static_cast<std::uint64_t>(assignment.nonce_start) + 1ULL;
    if (worker_window_size == 0ULL) {
        return 0U;
    }

    const std::uint64_t base_budget = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(config.fourier_fanout_resonance_budget),
        1ULL,
        worker_window_size);

    const std::size_t active_worker_count = std::max<std::size_t>(1U, authority_state.active_worker_count);
    const std::uint64_t dynamic_cap = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(
            std::max<std::size_t>(
                config.fourier_fanout_resonance_budget,
                std::max<std::size_t>(1U, config.max_dynamic_fourier_fanout_resonance_budget / active_worker_count))),
        base_budget,
        worker_window_size);

    const double budget_norm = deterministic_resonance_budget_norm(trace);
    if (budget_norm <= 1.0e-12) {
        return static_cast<std::size_t>(base_budget);
    }

    const std::uint64_t scaled_budget = std::clamp<std::uint64_t>(
        base_budget + static_cast<std::uint64_t>(std::llround(
            static_cast<double>(dynamic_cap - base_budget) * budget_norm)),
        base_budget,
        dynamic_cap);
    return static_cast<std::size_t>(scaled_budget);
}

[[nodiscard]] std::size_t effective_in_process_validation_candidate_limit(
    const PhaseClampedMiningConfig& config,
    std::size_t resonance_budget
) {
    const std::size_t base_limit = std::max<std::size_t>(
        config.max_in_process_validation_candidates_per_worker,
        config.max_reported_valid_nonces_per_worker);
    const std::size_t validation_stride = std::max<std::size_t>(
        1U,
        config.max_in_process_validation_candidates_per_worker);
    const std::size_t scaled_limit = std::max<std::size_t>(
        base_limit,
        std::max<std::size_t>(1U, resonance_budget / validation_stride));
    return std::max<std::size_t>(
        1U,
        std::min<std::size_t>(
            resonance_budget,
            std::min<std::size_t>(
                config.max_dynamic_in_process_validation_candidates_per_worker,
                scaled_limit)));
}

[[nodiscard]] double candidate_coherence_score(
    const SubstrateStratumPowCollapseFeedback& feedback
) {
    return mean_unit({
        feedback.nonce_collapse_confidence,
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
    });
}

[[nodiscard]] bool candidate_sample_is_more_coherent(
    const FourierFanoutResonanceOutcome::CandidateSample& candidate,
    const FourierFanoutResonanceOutcome::CandidateSample& incumbent
) {
    constexpr double kEpsilon = 1.0e-12;
    if (candidate.validation_structure_norm > incumbent.validation_structure_norm + kEpsilon) {
        return true;
    }
    if (candidate.validation_structure_norm + kEpsilon < incumbent.validation_structure_norm) {
        return false;
    }
    const double candidate_score = candidate_coherence_score(candidate.collapse_feedback);
    const double incumbent_score = candidate_coherence_score(incumbent.collapse_feedback);
    if (candidate_score > incumbent_score + kEpsilon) {
        return true;
    }
    if (candidate_score + kEpsilon < incumbent_score) {
        return false;
    }
    return collapse_feedback_is_better(
        candidate.collapse_feedback,
        incumbent.collapse_feedback,
        candidate.nonce_value,
        incumbent.nonce_value);
}

[[nodiscard]] bool validated_candidate_should_submit_before(
    const CandidateValidationAttempt& candidate,
    const CandidateValidationAttempt& incumbent
) {
    constexpr double kEpsilon = 1.0e-12;
    if (candidate.valid_share != incumbent.valid_share) {
        return candidate.valid_share;
    }
    if (candidate.validation_structure_norm > incumbent.validation_structure_norm + kEpsilon) {
        return true;
    }
    if (candidate.validation_structure_norm + kEpsilon < incumbent.validation_structure_norm) {
        return false;
    }
    if (candidate.valid_share) {
        if (candidate.coherence_score > incumbent.coherence_score + kEpsilon) {
            return true;
        }
        if (candidate.coherence_score + kEpsilon < incumbent.coherence_score) {
            return false;
        }
    }
    return candidate.candidate_index < incumbent.candidate_index;
}

[[nodiscard]] double gpu_pulse_phase_turns(const SubstrateTrace& trace) {
    return wrap_turns(
        (0.44 * trace.derived_constants.phase_position_turns)
        + (0.16 * signed_phase_component_turns(trace.encoded_pulse[0]))
        + (0.12 * signed_phase_component_turns(trace.encoded_pulse[1]))
        + (0.10 * signed_phase_component_turns(trace.encoded_pulse[2]))
        + (0.06 * signed_phase_component_turns(trace.rotational_velocity[0]))
        + (0.04 * signed_phase_component_turns(trace.rotational_velocity[1]))
        + (0.04 * signed_phase_component_turns(trace.rotational_velocity[2]))
        + (0.04 * trace.phase_transport));
}

[[nodiscard]] double resonance_field_phase_turns(
    const SubstrateTrace& trace,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    double lane_turns,
    double target_phase_turns,
    double gpu_pulse_phase_turns_value
) {
    const TargetRelativeTemporalAlignment temporal_alignment =
        measure_target_relative_temporal_alignment(trace, measurement, target_phase_turns);
    return wrap_turns(
        (0.34 * gpu_pulse_phase_turns_value)
        + (0.18 * measurement.carrier_phase_turns)
        + (0.12 * measurement.rf_phase_position_turns)
        + (0.10 * measurement.sha256_digest_phase_turns)
        + (0.10 * lane_turns)
        + (0.08 * target_phase_turns)
        + (0.08 * temporal_alignment.alignment_norm)
        + (0.05 * measurement.transfer_drive_norm)
        + (0.05 * measurement.stability_gate_norm)
        + (0.05 * measurement.resonance_activation_norm)
        - (0.04 * measurement.damping_norm));
}

[[nodiscard]] double gpu_pulse_propagation_phase_turns(
    const SubstrateTrace& trace,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    double lane_turns,
    double target_phase_turns,
    double gpu_pulse_phase_turns_value,
    double resonance_field_phase_turns_value
) {
    return wrap_turns(
        resonance_field_phase_turns_value
        + (0.18 * phase_delta_turns(target_phase_turns, measurement.target_phase_turns))
        + (0.12 * phase_delta_turns(measurement.rf_phase_position_turns, gpu_pulse_phase_turns_value))
        + (0.10 * measurement.rf_phase_velocity_turns)
        + (0.08 * measurement.rf_temporal_coupling_norm)
        + (0.06 * measurement.transport_drive_norm)
        + (0.05 * measurement.resonance_activation_norm)
        + (0.06 * trace.reverse_causal_flux_coherence)
        + (0.05 * lane_turns)
        + (0.04 * measurement.sha256_harmonic_density_norm)
        - (0.05 * measurement.damping_norm));
}

void insert_ranked_candidate_sample(
    FourierFanoutResonanceOutcome& outcome,
    FourierFanoutResonanceOutcome::CandidateSample sample,
    std::size_t sample_limit
) {
    if (sample_limit == 0U) {
        return;
    }

    auto position = outcome.ranked_candidate_samples.begin();
    while (position != outcome.ranked_candidate_samples.end()
        && !candidate_sample_is_more_coherent(sample, *position)) {
        ++position;
    }

    if (position == outcome.ranked_candidate_samples.end()
        && outcome.ranked_candidate_samples.size() >= sample_limit) {
        return;
    }

    outcome.ranked_candidate_samples.insert(position, std::move(sample));
    if (outcome.ranked_candidate_samples.size() > sample_limit) {
        outcome.ranked_candidate_samples.pop_back();
    }
}

void upsert_harmonic_lane_sample(
    FourierFanoutResonanceOutcome& outcome,
    FourierFanoutResonanceOutcome::CandidateSample sample,
    std::size_t lane_count
) {
    if (lane_count == 0U) {
        return;
    }

    if (outcome.harmonic_lane_samples.size() < lane_count) {
        outcome.harmonic_lane_samples.resize(lane_count);
    }

    auto& lane_sample = outcome.harmonic_lane_samples[sample.lane_index];
    if (lane_sample.nonce_hex.empty() || candidate_sample_is_more_coherent(sample, lane_sample)) {
        lane_sample = std::move(sample);
    }
}

[[nodiscard]] std::vector<CandidateValidationAttempt> validate_candidate_batch(
    const std::string& worker_header_hex,
    const std::string& nbits_hex,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& frozen_measurement,
    const std::vector<FourierFanoutResonanceOutcome::CandidateSample>& candidates,
    std::size_t begin_index,
    std::size_t end_index
) {
    std::vector<CandidateValidationAttempt> attempts;
    attempts.reserve(end_index > begin_index ? (end_index - begin_index) : 0U);
    for (std::size_t index = begin_index; index < end_index; ++index) {
        const auto& candidate = candidates[index];
        const SubstrateStratumPowPhaseTrace phase_trace = trace_stratum_pow_phase(
            worker_header_hex,
            nbits_hex,
            candidate.nonce_value,
            share_difficulty,
            frozen_measurement
        );
        if (!phase_trace.performed) {
            continue;
        }
        const double validation_structure_norm = validation_structure_presence_norm(
            frozen_measurement,
            phase_trace.collapse_feedback,
            frozen_measurement.target_phase_turns);

        attempts.push_back(CandidateValidationAttempt{
            index,
            candidate,
            phase_trace,
            phase_trace.evaluation.valid_share,
            phase_trace.collapse_feedback,
            candidate_coherence_score(phase_trace.collapse_feedback),
            validation_structure_norm,
        });
    }
    return attempts;
}

[[nodiscard]] ResonantCandidateValidation validate_resonant_candidate_set(
    const PhaseClampedMiningConfig& config,
    const std::string& worker_header_hex,
    const std::string& nbits_hex,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& frozen_measurement,
    const FourierFanoutResonanceOutcome& outcome,
    std::size_t resonance_budget,
    std::size_t validation_parallelism_hint
) {
    ResonantCandidateValidation validation;
    const std::size_t candidate_limit = effective_in_process_validation_candidate_limit(config, resonance_budget);
    if (worker_header_hex.empty() || nbits_hex.empty() || candidate_limit == 0U) {
        return validation;
    }

    std::vector<FourierFanoutResonanceOutcome::CandidateSample> candidates;
    candidates.reserve(
        outcome.harmonic_lane_samples.size()
        + 1U
        + outcome.ranked_candidate_samples.size());
    for (const auto& lane_sample : outcome.harmonic_lane_samples) {
        if (lane_sample.nonce_hex.empty()) {
            continue;
        }
        if (candidates.size() >= candidate_limit) {
            break;
        }
        candidates.push_back(lane_sample);
    }
    if (!outcome.primary_nonce_hex.empty()) {
        candidates.push_back(FourierFanoutResonanceOutcome::CandidateSample{
            outcome.primary_nonce_value,
            outcome.primary_nonce_hex,
            0U,
            0.0,
            0.0,
            0.0,
            0.0,
            outcome.primary_validation_structure_norm,
            outcome.primary_collapse_feedback,
        });
    }
    for (const auto& sample : outcome.ranked_candidate_samples) {
        if (candidates.size() >= candidate_limit) {
            break;
        }
        candidates.push_back(sample);
    }

    std::vector<FourierFanoutResonanceOutcome::CandidateSample> deduplicated_candidates;
    deduplicated_candidates.reserve(candidates.size());
    std::unordered_set<std::uint32_t> visited_nonces;
    visited_nonces.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (!visited_nonces.insert(candidate.nonce_value).second) {
            continue;
        }
        deduplicated_candidates.push_back(candidate);
    }
    if (deduplicated_candidates.empty()) {
        return validation;
    }

    const std::size_t hardware_parallelism = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::thread::hardware_concurrency()));
    const std::size_t parallelism = std::max<std::size_t>(
        1U,
        std::min<std::size_t>({
            deduplicated_candidates.size(),
            std::max<std::size_t>(1U, validation_parallelism_hint),
            hardware_parallelism,
        }));

    std::vector<CandidateValidationAttempt> validation_attempts;
    validation_attempts.reserve(deduplicated_candidates.size());
    if (parallelism <= 1U || deduplicated_candidates.size() <= 1U) {
        validation_attempts = validate_candidate_batch(
            worker_header_hex,
            nbits_hex,
            share_difficulty,
            frozen_measurement,
            deduplicated_candidates,
            0U,
            deduplicated_candidates.size());
    } else {
        const std::size_t chunk_size =
            (deduplicated_candidates.size() + parallelism - 1U) / parallelism;
        std::vector<std::future<std::vector<CandidateValidationAttempt>>> futures;
        futures.reserve(parallelism);
        for (std::size_t chunk_index = 0; chunk_index < parallelism; ++chunk_index) {
            const std::size_t begin_index = chunk_index * chunk_size;
            if (begin_index >= deduplicated_candidates.size()) {
                break;
            }
            const std::size_t end_index = std::min<std::size_t>(
                deduplicated_candidates.size(),
                begin_index + chunk_size);
            futures.push_back(std::async(
                std::launch::async,
                validate_candidate_batch,
                worker_header_hex,
                nbits_hex,
                share_difficulty,
                std::cref(frozen_measurement),
                std::cref(deduplicated_candidates),
                begin_index,
                end_index));
        }

        for (auto& future : futures) {
            auto batch_attempts = future.get();
            validation_attempts.insert(
                validation_attempts.end(),
                std::make_move_iterator(batch_attempts.begin()),
                std::make_move_iterator(batch_attempts.end()));
        }
    }

    const CandidateValidationAttempt* first_attempted = nullptr;
    const CandidateValidationAttempt* selected_valid_candidate = nullptr;
    std::unordered_set<std::size_t> verified_parallel_harmonics;
    std::unordered_set<std::size_t> validated_parallel_harmonics;
    for (const auto& attempt : validation_attempts) {
        if (first_attempted == nullptr || attempt.candidate_index < first_attempted->candidate_index) {
            first_attempted = &attempt;
        }
        verified_parallel_harmonics.insert(attempt.sample.lane_index);
        if (!attempt.valid_share) {
            continue;
        }
        validated_parallel_harmonics.insert(attempt.sample.lane_index);
        if (selected_valid_candidate == nullptr
            || validated_candidate_should_submit_before(attempt, *selected_valid_candidate)) {
            selected_valid_candidate = &attempt;
        }
    }

    validation.parallel_harmonic_count = std::count_if(
        outcome.harmonic_lane_samples.begin(),
        outcome.harmonic_lane_samples.end(),
        [](const FourierFanoutResonanceOutcome::CandidateSample& sample) {
            return !sample.nonce_hex.empty();
        });
    validation.verified_parallel_harmonic_count = verified_parallel_harmonics.size();
    validation.validated_parallel_harmonic_count = validated_parallel_harmonics.size();
    validation.all_parallel_harmonics_verified =
        validation.parallel_harmonic_count > 0U
        && validation.verified_parallel_harmonic_count >= validation.parallel_harmonic_count;

    if (selected_valid_candidate != nullptr) {
        validation.validation_attempted = true;
        validation.valid_share = true;
        validation.nonce_value = selected_valid_candidate->sample.nonce_value;
        validation.nonce_hex = selected_valid_candidate->sample.nonce_hex;
        validation.phase_trace = selected_valid_candidate->phase_trace;
        validation.evaluation = selected_valid_candidate->phase_trace.evaluation;
        validation.collapse_feedback = selected_valid_candidate->validated_collapse_feedback;
        validation.measured_nonce_observed = true;
        validation.coherence_score = selected_valid_candidate->coherence_score;
        validation.validation_structure_norm = selected_valid_candidate->validation_structure_norm;
        validation.gpu_pulse_phase_turns = selected_valid_candidate->sample.gpu_pulse_phase_turns;
        validation.resonance_field_phase_turns = selected_valid_candidate->sample.resonance_field_phase_turns;
        validation.gpu_pulse_propagation_phase_turns = selected_valid_candidate->sample.gpu_pulse_propagation_phase_turns;
        return validation;
    }

    if (first_attempted != nullptr) {
        validation.validation_attempted = true;
        validation.valid_share = false;
        validation.nonce_value = first_attempted->sample.nonce_value;
        validation.nonce_hex = first_attempted->sample.nonce_hex;
        validation.phase_trace = first_attempted->phase_trace;
        validation.evaluation = first_attempted->phase_trace.evaluation;
        validation.collapse_feedback = first_attempted->validated_collapse_feedback;
        validation.measured_nonce_observed = true;
        validation.coherence_score = first_attempted->coherence_score;
        validation.validation_structure_norm = first_attempted->validation_structure_norm;
        validation.gpu_pulse_phase_turns = first_attempted->sample.gpu_pulse_phase_turns;
        validation.resonance_field_phase_turns = first_attempted->sample.resonance_field_phase_turns;
        validation.gpu_pulse_propagation_phase_turns = first_attempted->sample.gpu_pulse_propagation_phase_turns;
    }

    return validation;
}

[[nodiscard]] FourierFanoutResonanceOutcome collect_resonant_nonce_fanout(
    const PhaseClampedMiningConfig& config,
    const SubstrateTrace& trace,
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment,
    const FourierFanoutParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    double target_phase_turns,
    std::size_t resonance_budget
) {
    FourierFanoutResonanceOutcome outcome;
    const std::string worker_header_hex = resolve_stratum_job_header_hex(authority_state, assignment);
    if (worker_header_hex.empty() || authority_state.active_job_nbits.empty()) {
        return outcome;
    }

    const std::uint64_t window_size =
        static_cast<std::uint64_t>(assignment.nonce_end) - static_cast<std::uint64_t>(assignment.nonce_start) + 1ULL;
    if (window_size == 0ULL || resonance_budget == 0U) {
        return outcome;
    }

    const std::size_t lane_count = std::max<std::size_t>(1U, parameters.lane_count);
    outcome.harmonic_lane_samples.resize(lane_count);
    const std::uint64_t local_window = std::max<std::uint64_t>(
        1ULL,
        std::min<std::uint64_t>(
            window_size,
            static_cast<std::uint64_t>(
                std::max<std::size_t>(1U, resonance_budget / lane_count))));

    std::unordered_set<std::uint32_t> visited_nonces;
    visited_nonces.reserve(resonance_budget);
    SubstrateStratumPhaseFluxMeasurement rolling_measurement = measurement;
    const double gpu_pulse_phase_turns_value = gpu_pulse_phase_turns(trace);

    for (std::size_t lane_index = 0; lane_index < lane_count; ++lane_index) {
        if (outcome.attempted_nonce_count >= resonance_budget) {
            break;
        }

        const double lane_turns = fourier_lane_turns(parameters, rolling_measurement, lane_index, target_phase_turns);
        const std::uint64_t center_offset = window_size == 1ULL
            ? 0ULL
            : static_cast<std::uint64_t>(std::llround(
                fourier_lane_center_fraction(lane_turns, parameters, rolling_measurement, target_phase_turns)
                * static_cast<double>(window_size - 1ULL)));
        const std::uint64_t half_window = local_window / 2ULL;
        std::uint64_t resonance_window_begin_offset = center_offset > half_window ? (center_offset - half_window) : 0ULL;
        if ((resonance_window_begin_offset + local_window) > window_size) {
            resonance_window_begin_offset = window_size - local_window;
        }

        for (std::uint64_t local_index = 0; local_index < local_window; ++local_index) {
            if (outcome.attempted_nonce_count >= resonance_budget) {
                break;
            }

            const std::uint32_t resonant_nonce = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(assignment.nonce_start)
                + resonance_window_begin_offset
                + collapse_guided_local_offset(local_window, rolling_measurement, local_index));
            if (!visited_nonces.insert(resonant_nonce).second) {
                continue;
            }

            const SubstrateStratumPowCollapseFeedback collapse_feedback =
                synthesize_resonant_sha_collapse_feedback(
                    parameters,
                    rolling_measurement,
                    resonant_nonce,
                    lane_turns,
                    target_phase_turns);
            const double resonance_field_phase_turns_value = resonance_field_phase_turns(
                trace,
                rolling_measurement,
                lane_turns,
                target_phase_turns,
                gpu_pulse_phase_turns_value);
            const double gpu_pulse_propagation_phase_turns_value = gpu_pulse_propagation_phase_turns(
                trace,
                rolling_measurement,
                lane_turns,
                target_phase_turns,
                gpu_pulse_phase_turns_value,
                resonance_field_phase_turns_value);
            const double validation_structure_norm = validation_structure_presence_norm(
                rolling_measurement,
                collapse_feedback,
                target_phase_turns);
            const double candidate_activation_norm = mean_unit({
                rolling_measurement.resonance_activation_norm,
                collapse_feedback.phase_flux_conservation,
                collapse_feedback.observer_collapse_strength,
                collapse_feedback.nonce_collapse_confidence,
            });
            const double validation_structure_floor = std::min({
                rolling_measurement.target_resonance_norm,
                candidate_activation_norm,
                1.0 - clamp_unit(collapse_feedback.relock_error_turns * 2.0),
            });
            const bool coherent_candidate =
                validation_structure_norm + 1.0e-12 >= validation_structure_floor
                && collapse_feedback.nonce_collapse_confidence + 1.0e-12 >= validation_structure_floor
                && collapse_feedback.phase_flux_conservation + 1.0e-12 >= rolling_measurement.target_resonance_norm
                && candidate_activation_norm + 1.0e-12 >= rolling_measurement.resonance_activation_norm
                && collapse_feedback.relock_error_turns <= config.max_phase_lock_error;
            rolling_measurement = apply_stratum_pow_collapse_feedback(
                rolling_measurement,
                collapse_feedback,
                coherent_candidate
            );
            ++outcome.attempted_nonce_count;

            const bool primary_candidate_is_better = outcome.primary_nonce_hex.empty()
                || validation_structure_norm > outcome.primary_validation_structure_norm + 1.0e-12
                || (std::abs(validation_structure_norm - outcome.primary_validation_structure_norm) <= 1.0e-12
                    && collapse_feedback_is_better(
                        collapse_feedback,
                        outcome.primary_collapse_feedback,
                        resonant_nonce,
                        outcome.primary_nonce_value));
            if (primary_candidate_is_better) {
                outcome.primary_nonce_value = resonant_nonce;
                outcome.primary_nonce_hex = format_nonce_hex(resonant_nonce);
                outcome.primary_hash_hex.clear();
                outcome.measured_nonce_observed = true;
                outcome.primary_validation_structure_norm = validation_structure_norm;
                outcome.primary_collapse_feedback = collapse_feedback;
            }

            if (!coherent_candidate) {
                continue;
            }

            ++outcome.valid_nonce_count;
            const FourierFanoutResonanceOutcome::CandidateSample lane_sample{
                resonant_nonce,
                format_nonce_hex(resonant_nonce),
                lane_index,
                lane_turns,
                gpu_pulse_phase_turns_value,
                resonance_field_phase_turns_value,
                gpu_pulse_propagation_phase_turns_value,
                validation_structure_norm,
                collapse_feedback,
            };
            upsert_harmonic_lane_sample(outcome, lane_sample, lane_count);
            insert_ranked_candidate_sample(
                outcome,
                lane_sample,
                effective_in_process_validation_candidate_limit(config, resonance_budget));
            if (outcome.sampled_valid_nonce_hexes.size() < config.max_reported_valid_nonces_per_worker) {
                outcome.sampled_valid_nonce_hexes.push_back(format_nonce_hex(resonant_nonce));
            }
        }
    }

    return outcome;
}

}  // namespace

PhaseClampedMiningOperatingSystem::PhaseClampedMiningOperatingSystem(PhaseClampedMiningConfig config)
    : config_(config) {
    (void)mining_resonance_program_metadata();
}

const PhaseClampedMiningConfig& PhaseClampedMiningOperatingSystem::config() const noexcept {
    return config_;
}

std::vector<PhaseClampedShareActuation> PhaseClampedMiningOperatingSystem::compute_share_actuations(
    const SubstrateTrace& trace,
    const PhaseDispatchArtifact& artifact,
    const SubstrateStratumAuthorityState* authority_state
) const {
    if (authority_state == nullptr || !authority_state->has_active_job) {
        return {};
    }

    const bool authority_ready =
        authority_state->network_authority_granted
        && authority_state->has_difficulty
        && authority_state->active_worker_count > 0U;
    const MiningResonanceProgramBinding mining_program_binding = bind_mining_resonance_program();
    const EncodedTargetHarmonic target_harmonic = encode_spider_graph_target_harmonic(trace, artifact);
    const double target_phase = target_phase_turns(trace, artifact, *authority_state);
    const double target_window_turns = phase_window_turns(
        config_,
        target_harmonic.coherence,
        trace.derived_constants.phase_alignment_probability);
    const double phase_position_turns = trace.derived_constants.phase_position_turns;
    const double phase_error_turns = std::abs(phase_delta_turns(phase_position_turns, target_phase));
    const double phase_clamp_strength = clamp_unit(1.0 - (phase_error_turns / std::max(target_window_turns, 1.0e-9)));
    const bool phase_clamped = phase_error_turns <= target_window_turns;
    const RfActivationProfile rf_profile = derive_rf_activation_profile(
        trace,
        artifact,
        phase_clamp_strength);
    const FourierFanoutParameters fanout_parameters = build_fourier_fanout_parameters(
        config_,
        trace,
        artifact,
        *authority_state,
        rf_profile,
        target_phase,
        phase_clamp_strength);
    const SubstrateStratumPhaseFluxMeasurement measurement = build_phase_flux_measurement(
        trace,
        artifact,
        fanout_parameters,
        *authority_state,
        rf_profile,
        target_phase,
        phase_clamp_strength
    );
    const std::string gate_failure = first_gate_failure(
        config_,
        trace,
        artifact,
        mining_program_binding,
        authority_ready);

    std::vector<PhaseClampedShareActuation> actuations;
    actuations.reserve(authority_state->active_worker_count);

    for (const auto& assignment : authority_state->worker_assignments) {
        if (!assignment.active) {
            continue;
        }

        const std::string worker_header_hex = resolve_stratum_job_header_hex(*authority_state, assignment);
        const SubstrateStratumPhaseFluxMeasurement worker_measurement =
            bias_phase_flux_measurement_with_sha256_frequency(
                worker_header_hex,
                authority_state->active_job_nbits,
                authority_state->difficulty,
                measurement);
        const double base_target_resonance_norm = mean_unit({
            fanout_parameters.target_boundary_norm,
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                worker_measurement.carrier_phase_turns,
                target_phase
            )) * 2.0),
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                worker_measurement.rf_phase_position_turns,
                target_phase
            )) * 2.0),
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                worker_measurement.sha256_digest_phase_turns,
                target_phase
            )) * 2.0),
            worker_measurement.transfer_drive_norm,
            worker_measurement.stability_gate_norm,
            worker_measurement.sha256_frequency_bias_norm,
            1.0 - worker_measurement.damping_norm,
        });
        const SubstrateStratumSubmitPreviewPayload preview =
            build_stratum_submit_preview_payload(*authority_state, assignment);
        const std::size_t resonance_budget = mining_program_binding.substrate_native_ready
            ? effective_fanout_resonance_budget(
                config_,
                trace,
                *authority_state,
                assignment)
            : config_.fourier_fanout_resonance_budget;
        const FourierFanoutResonanceOutcome resonance_outcome = collect_resonant_nonce_fanout(
            config_,
            trace,
            *authority_state,
            assignment,
            fanout_parameters,
            worker_measurement,
            target_phase,
            resonance_budget);
        const ResonantCandidateValidation validated_candidate = validate_resonant_candidate_set(
            config_,
            worker_header_hex,
            authority_state->active_job_nbits,
            authority_state->difficulty,
            worker_measurement,
            resonance_outcome,
            resonance_budget,
            fanout_parameters.lane_count);

        SubstrateStratumSubmitPreviewPayload submit_payload = preview;
        const std::string selected_nonce_hex = !validated_candidate.nonce_hex.empty()
            ? validated_candidate.nonce_hex
            : resonance_outcome.primary_nonce_hex;
        if (!selected_nonce_hex.empty()) {
            const std::string original_nonce = submit_payload.nonce;
            submit_payload.nonce = selected_nonce_hex;
            const std::size_t nonce_position = submit_payload.payload_json.rfind(original_nonce);
            if (nonce_position != std::string::npos) {
                submit_payload.payload_json.replace(nonce_position, original_nonce.size(), submit_payload.nonce);
            }
        }

        const SubstrateStratumPowCollapseFeedback& selected_collapse_feedback =
            validated_candidate.validation_attempted
                ? validated_candidate.collapse_feedback
                : resonance_outcome.primary_collapse_feedback;
        const TargetRelativeTemporalAlignment target_relative_alignment =
            measure_target_relative_temporal_alignment(
                trace,
                worker_measurement,
                target_phase,
                &selected_collapse_feedback);
        const double phase_alignment = target_relative_alignment.alignment_norm;
        const double validation_structure_norm = validation_structure_presence_norm(
            worker_measurement,
            selected_collapse_feedback,
            target_phase);
        const double candidate_phase_error_turns = std::abs(phase_delta_turns(
            target_relative_alignment.relative_sequence_turns,
            target_phase));
        const double candidate_phase_clamp_strength = clamp_unit(
            std::max(
                1.0 - (candidate_phase_error_turns / std::max(target_window_turns, 1.0e-9)),
                validation_structure_norm));
        const bool candidate_phase_clamped =
            candidate_phase_error_turns <= target_window_turns
            || validation_structure_norm + 1.0e-12 >= worker_measurement.target_resonance_norm;
        const double target_resonance_norm = mean_unit({
            base_target_resonance_norm,
            phase_alignment,
            validation_structure_norm,
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                selected_collapse_feedback.measured_hash_phase_turns,
                target_phase
            )) * 2.0),
            1.0 - clamp_unit(selected_collapse_feedback.relock_error_turns * 2.0),
            selected_collapse_feedback.phase_flux_conservation,
            selected_collapse_feedback.nonce_collapse_confidence,
        });
        const bool target_resonance_ready =
            target_resonance_norm + 1.0e-12 >= config_.min_target_resonance_norm;
        const double resonance_activation_norm = mean_unit({
            worker_measurement.resonance_activation_norm,
            target_resonance_norm,
            phase_alignment,
            selected_collapse_feedback.phase_flux_conservation,
            selected_collapse_feedback.observer_collapse_strength,
            selected_collapse_feedback.nonce_collapse_confidence,
        });
        const bool share_target_pass =
            validated_candidate.validation_attempted && validated_candidate.evaluation.valid_share;
        const bool block_target_pass =
            validated_candidate.validation_attempted && validated_candidate.evaluation.valid_block;
        const std::size_t resonance_reinforcement_count = count_reinforced_harmonic_lanes(
            resonance_outcome,
            selected_nonce_hex,
            std::max<std::size_t>(validated_candidate.parallel_harmonic_count, 1U));
        const std::size_t noise_lane_count = resonance_outcome.attempted_nonce_count > resonance_outcome.valid_nonce_count
            ? (resonance_outcome.attempted_nonce_count - resonance_outcome.valid_nonce_count)
            : 0U;
        const double block_coherence_norm = measure_block_target_coherence_norm(
            validated_candidate,
            selected_collapse_feedback,
            target_resonance_norm,
            phase_alignment,
            validation_structure_norm);
        const std::uint32_t queue_quality_class = classify_submit_quality_class(
            share_target_pass,
            block_target_pass,
            block_coherence_norm);
        const bool valid_share_resonance =
            validated_candidate.valid_share
            && mining_program_binding.same_pulse_validation_ready;
        const bool phase_candidate_surface_available =
            mining_program_binding.candidate_nonce_surface_ready
            && (
                !selected_nonce_hex.empty()
                || !resonance_outcome.sampled_valid_nonce_hexes.empty()
                || resonance_outcome.valid_nonce_count > 0U
                || validated_candidate.parallel_harmonic_count > 0U
                || validated_candidate.validation_attempted);
        const bool actuation_permitted =
            valid_share_resonance
            && authority_state->submit_path_ready
            && mining_program_binding.pool_submit_ready;
        const std::string final_gate_reason = actuation_permitted
            ? "phase_clamped_share_actuation"
            : (valid_share_resonance
                ? (authority_state->submit_path_ready
                    ? (mining_program_binding.pool_submit_ready
                        ? "phase_clamped_share_actuation_ready"
                        : "phase_program_pool_submit_unready")
                    : authority_state->submit_gate_reason)
                : (!gate_failure.empty()
                    ? gate_failure
                    : (validated_candidate.validation_attempted
                        ? "phase_local_validation_failed"
                        : (phase_candidate_surface_available
                            ? "phase_resonant_candidate_pending_validation"
                            : "phase_resonance_not_detected"))));

        PhaseClampedShareActuation actuation;
        actuation.trace_id = trace.photonic_identity.trace_id;
        actuation.connection_id = authority_state->connection_ingress.connection_id;
        actuation.job_id = assignment.job_id;
        actuation.worker_index = assignment.worker_index;
        actuation.worker_name = assignment.worker_name;
        actuation.request_id = submit_payload.request_id;
        actuation.submit_payload_json = submit_payload.payload_json;
        actuation.target_compact_nbits = authority_state->active_job_nbits;
        actuation.nonce_hex = selected_nonce_hex.empty() ? submit_payload.nonce : selected_nonce_hex;
        actuation.hash_hex = validated_candidate.validation_attempted ? validated_candidate.evaluation.hash_hex : resonance_outcome.primary_hash_hex;
        actuation.target_hex = authority_state->active_share_target_hex;
        actuation.share_target_hex = authority_state->active_share_target_hex;
        actuation.block_target_hex = authority_state->active_block_target_hex;
        actuation.nonce_start = assignment.nonce_start;
        actuation.nonce_end = assignment.nonce_end;
        actuation.target_difficulty = authority_state->difficulty;
        actuation.block_difficulty = authority_state->active_block_difficulty;
        actuation.expected_hashes_for_share = authority_state->expected_hashes_for_share;
        actuation.target_network_share_fraction = authority_state->target_network_share_fraction;
        actuation.network_hashrate_hs = authority_state->network_hashrate_hs;
        actuation.required_hashrate_hs = authority_state->required_hashrate_hs;
        actuation.required_share_submissions_per_s = authority_state->required_share_submissions_per_s;
        actuation.target_phase_turns = target_phase;
        actuation.phase_position_turns = phase_position_turns;
        actuation.field_vector_phase_turns = wrap_turns(trace.photonic_identity.field_vector.phase);
        actuation.phase_transport_turns = wrap_turns(trace.phase_transport);
        actuation.phase_lock_delta_turns = phase_delta_turns(
            actuation.phase_transport_turns,
            actuation.field_vector_phase_turns);
        actuation.relative_phase_from_zero_point_turns = target_relative_alignment.zero_point_relative_turns;
        actuation.phase_window_turns = target_window_turns;
        actuation.phase_error_turns = candidate_phase_error_turns;
        actuation.phase_clamp_strength = candidate_phase_clamp_strength;
        actuation.hilbert_wavelength_domain = config_.hilbert_wavelength_domain;
        actuation.flux_phase_transport_norm = fanout_parameters.flux_phase_transport_norm;
        actuation.phi_hat = fanout_parameters.phi_hat;
        actuation.forcing_hat = fanout_parameters.forcing_hat;
        actuation.mining_boundary_norm = fanout_parameters.mining_boundary_norm;
        actuation.target_boundary_norm = fanout_parameters.target_boundary_norm;
        actuation.driver_kernel_duration_ms = trace.derived_constants.driver_kernel_duration_ms;
        actuation.driver_execution_density = trace.derived_constants.driver_execution_density;
        actuation.driver_invocation_pressure = trace.derived_constants.driver_invocation_pressure;
        actuation.driver_temporal_alignment = trace.derived_constants.driver_temporal_alignment;
        actuation.driver_resonance = trace.derived_constants.driver_resonance;
        actuation.rf_spin_axis_signed = rf_profile.spin_axis_signed;
        actuation.rf_spin_orientation_signed = rf_profile.spin_orientation_signed;
        actuation.rf_carrier_frequency_norm = worker_measurement.rf_carrier_frequency_norm;
        actuation.rf_envelope_amplitude_norm = worker_measurement.rf_envelope_amplitude_norm;
        actuation.rf_phase_position_turns = worker_measurement.rf_phase_position_turns;
        actuation.rf_phase_velocity_turns = worker_measurement.rf_phase_velocity_turns;
        actuation.rf_zero_point_displacement_turns = worker_measurement.rf_zero_point_displacement_turns;
        actuation.rf_zero_point_distance_norm = worker_measurement.rf_zero_point_distance_norm;
        actuation.rf_spin_drive_signed = worker_measurement.rf_spin_drive_signed;
        actuation.rf_rotation_orientation_signed = worker_measurement.rf_rotation_orientation_signed;
        actuation.rf_temporal_coupling_norm = worker_measurement.rf_temporal_coupling_norm;
        actuation.rf_resonance_hold_norm = worker_measurement.rf_resonance_hold_norm;
        actuation.rf_sideband_energy_norm = worker_measurement.rf_sideband_energy_norm;
        actuation.rf_energy_transfer_norm = worker_measurement.rf_energy_transfer_norm;
        actuation.rf_particle_stability_norm = worker_measurement.rf_particle_stability_norm;
        actuation.transfer_drive_norm = worker_measurement.transfer_drive_norm;
        actuation.stability_gate_norm = worker_measurement.stability_gate_norm;
        actuation.damping_norm = worker_measurement.damping_norm;
        actuation.spin_alignment_norm = worker_measurement.spin_alignment_norm;
        actuation.transport_drive_norm = worker_measurement.transport_drive_norm;
        actuation.target_resonance_norm = target_resonance_norm;
        actuation.resonance_activation_norm = resonance_activation_norm;
        actuation.relative_phase_vector_direction = artifact.phase_vector_direction;
        actuation.relative_phase_vector_magnitude = artifact.phase_vector_magnitude;
        actuation.phase_lock_error = artifact.phase_lock_error;
        actuation.phase_alignment = phase_alignment;
        actuation.zero_point_proximity = trace.derived_constants.zero_point_proximity;
        actuation.zero_point_line_distance = trace.derived_constants.zero_point_line_distance;
        actuation.temporal_admissibility = artifact.temporal_admissibility;
        actuation.interference_projection = artifact.interference_projection;
        actuation.transport_readiness = artifact.transport_readiness;
        actuation.share_confidence = mean_unit({
            candidate_phase_clamp_strength,
            1.0 - artifact.phase_lock_error,
            phase_alignment,
            trace.derived_constants.zero_point_proximity,
            1.0 - artifact.anchor_evm_norm,
            1.0 - artifact.sideband_energy_norm,
            artifact.transport_readiness,
            trace.derived_constants.driver_execution_density,
            trace.derived_constants.driver_temporal_alignment,
            worker_measurement.transfer_drive_norm,
            worker_measurement.stability_gate_norm,
            target_resonance_norm,
            resonance_activation_norm,
            1.0 - worker_measurement.damping_norm,
            selected_collapse_feedback.phase_flux_conservation,
            selected_collapse_feedback.nonce_collapse_confidence,
            validation_structure_norm,
        });
        actuation.measured_hash_phase_turns = selected_collapse_feedback.measured_hash_phase_turns;
        actuation.measured_nonce_phase_turns = selected_collapse_feedback.measured_nonce_phase_turns;
        actuation.collapse_feedback_phase_turns = selected_collapse_feedback.feedback_phase_turns;
        actuation.collapse_relock_error_turns = selected_collapse_feedback.relock_error_turns;
        actuation.observer_collapse_strength = selected_collapse_feedback.observer_collapse_strength;
        actuation.phase_flux_conservation = selected_collapse_feedback.phase_flux_conservation;
        actuation.nonce_collapse_confidence = selected_collapse_feedback.nonce_collapse_confidence;
        actuation.validation_structure_norm = validation_structure_norm;
        actuation.gpu_pulse_phase_turns = validated_candidate.gpu_pulse_phase_turns;
        actuation.resonance_field_phase_turns = validated_candidate.resonance_field_phase_turns;
        actuation.gpu_pulse_propagation_phase_turns = validated_candidate.gpu_pulse_propagation_phase_turns;
        actuation.selected_coherence_score = validated_candidate.coherence_score;
        actuation.op_phase_bias_q15 = fanout_parameters.op_phase_bias_q15;
        actuation.op_band_w_q15 = fanout_parameters.op_band_w_q15;
        actuation.fanout_lane_count = fanout_parameters.lane_count;
        actuation.attempted_nonce_count = resonance_outcome.attempted_nonce_count;
        actuation.valid_nonce_count = resonance_outcome.valid_nonce_count;
        actuation.parallel_harmonic_count = validated_candidate.parallel_harmonic_count;
        actuation.verified_parallel_harmonic_count = validated_candidate.verified_parallel_harmonic_count;
        actuation.validated_parallel_harmonic_count = validated_candidate.validated_parallel_harmonic_count;
        actuation.resonance_reinforcement_count = resonance_reinforcement_count;
        actuation.noise_lane_count = noise_lane_count;
        actuation.compute_invocation_count = trace.gpu_kernel_sync.compute_invocation_count;
        actuation.phase_program_title = mining_program_binding.metadata == nullptr
            ? std::string{}
            : mining_program_binding.metadata->title;
        actuation.phase_program_generated_dir = mining_program_binding.metadata == nullptr
            ? std::string{}
            : mining_program_binding.metadata->generated_dir;
        actuation.phase_program_block_count = mining_program_binding.metadata == nullptr
            ? 0U
            : mining_program_binding.metadata->block_count();
        actuation.phase_temporal_sequence = build_phase_temporal_sequence(
            trace,
            target_harmonic.phase_turns,
            target_phase,
            worker_measurement,
            selected_collapse_feedback);
        actuation.sampled_valid_nonce_hexes = resonance_outcome.sampled_valid_nonce_hexes;
        if (validated_candidate.valid_share && !validated_candidate.nonce_hex.empty()) {
            const auto existing = std::find(
                actuation.sampled_valid_nonce_hexes.begin(),
                actuation.sampled_valid_nonce_hexes.end(),
                validated_candidate.nonce_hex);
            if (existing == actuation.sampled_valid_nonce_hexes.end()) {
                actuation.sampled_valid_nonce_hexes.insert(
                    actuation.sampled_valid_nonce_hexes.begin(),
                    validated_candidate.nonce_hex);
                if (actuation.sampled_valid_nonce_hexes.size() > config_.max_reported_valid_nonces_per_worker) {
                    actuation.sampled_valid_nonce_hexes.pop_back();
                }
            }
        }
        actuation.sha256_phase_trace = validated_candidate.phase_trace;
        actuation.has_sha256_phase_trace = validated_candidate.phase_trace.performed;
        actuation.block_coherence_norm = block_coherence_norm;
        actuation.queue_quality_class = queue_quality_class;
        actuation.share_target_pass = share_target_pass;
        actuation.block_target_pass = block_target_pass;
        actuation.submit_priority_score = compute_submit_priority_score(
            queue_quality_class,
            block_coherence_norm,
            resonance_reinforcement_count,
            actuation.share_confidence,
            target_resonance_norm,
            block_target_pass);
        actuation.canonical_share_id = build_canonical_share_id(
            actuation.job_id,
            actuation.worker_index,
            actuation.nonce_hex,
            actuation.hash_hex);
        actuation.driver_timing_valid = trace.gpu_kernel_sync.driver_timing_valid;
        actuation.offline_pow_checked = validated_candidate.validation_attempted;
        actuation.offline_pow_valid = validated_candidate.valid_share;
        actuation.block_candidate_valid =
            validated_candidate.validation_attempted && validated_candidate.evaluation.valid_block;
        actuation.phase_clamped = phase_clamped || candidate_phase_clamped;
        actuation.measured_nonce_observed =
            validated_candidate.validation_attempted ? validated_candidate.measured_nonce_observed : resonance_outcome.measured_nonce_observed;
        actuation.resonant_candidate_available = phase_candidate_surface_available;
        actuation.valid_share_candidate = valid_share_resonance;
        actuation.actuation_permitted = actuation_permitted;
        actuation.target_resonance_ready = target_resonance_ready;
        actuation.all_parallel_harmonics_verified = validated_candidate.all_parallel_harmonics_verified;
        actuation.phase_program_substrate_native = mining_program_binding.substrate_native_ready;
        actuation.phase_program_same_pulse_validation = mining_program_binding.same_pulse_validation_ready;
        actuation.phase_program_pool_format_ready =
            mining_program_binding.pool_ingest_ready && mining_program_binding.pool_submit_ready;
        actuation.gate_reason = final_gate_reason;
        actuation.actuation_topic = actuation_topic(actuation.resonant_candidate_available, actuation_permitted);
        actuations.push_back(actuation);
    }

    return actuations;
}

}  // namespace qbit_miner
