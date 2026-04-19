#include "qbit_miner/control_center/field_viewport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>

#include "qbit_miner/runtime/substrate_phase_program_metadata.hpp"
#include "qbit_miner/runtime/substrate_phase_programs.hpp"
#include "qbit_miner/runtime/substrate_stratum_pow.hpp"
#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"

namespace qbit_miner {

namespace {

constexpr double kSiliconDensity = 2.33;
constexpr double kSiliconLatticeConstantMeters = 5.431020511e-10;
constexpr double kSiliconExcitationEnergyEv = 173.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 6.28318530717958647692;

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

float clamp01f(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

double clamp_signed(double value) {
    return std::clamp(value, -1.0, 1.0);
}

std::array<float, 3> normalize3(const std::array<float, 3>& value) {
    const float length_sq = (value[0] * value[0]) + (value[1] * value[1]) + (value[2] * value[2]);
    if (length_sq <= 1.0e-8f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv_length = 1.0f / std::sqrt(length_sq);
    return {
        value[0] * inv_length,
        value[1] * inv_length,
        value[2] * inv_length,
    };
}

double wrap_turns(double value) {
    const double wrapped = std::fmod(value, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

double phase_delta_turns(double lhs, double rhs) {
    double delta = wrap_turns(lhs) - wrap_turns(rhs);
    if (delta > 0.5) {
        delta -= 1.0;
    } else if (delta < -0.5) {
        delta += 1.0;
    }
    return delta;
}

double mean_unit(std::initializer_list<double> values) {
    if (values.size() == 0U) {
        return 0.0;
    }
    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return clamp01(total / static_cast<double>(values.size()));
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

std::string normalize_hex(const std::string& value) {
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

std::vector<std::uint8_t> hex_to_bytes(const std::string& value) {
    const std::string normalized = normalize_hex(value);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(normalized.size() / 2U);
    for (std::size_t index = 0; index + 1U < normalized.size(); index += 2U) {
        const int hi = hex_value(normalized[index]);
        const int lo = hex_value(normalized[index + 1U]);
        bytes.push_back(static_cast<std::uint8_t>((std::max(hi, 0) << 4) | std::max(lo, 0)));
    }
    return bytes;
}

template <std::size_t WordCount>
std::array<std::uint32_t, WordCount> bytes_to_words_be(const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint32_t, WordCount> words {};
    for (std::size_t index = 0; index < WordCount; ++index) {
        const std::size_t base = index * 4U;
        if (base >= bytes.size()) {
            break;
        }
        const std::uint8_t b0 = bytes[base];
        const std::uint8_t b1 = (base + 1U) < bytes.size() ? bytes[base + 1U] : 0U;
        const std::uint8_t b2 = (base + 2U) < bytes.size() ? bytes[base + 2U] : 0U;
        const std::uint8_t b3 = (base + 3U) < bytes.size() ? bytes[base + 3U] : 0U;
        words[index] = (static_cast<std::uint32_t>(b0) << 24U)
            | (static_cast<std::uint32_t>(b1) << 16U)
            | (static_cast<std::uint32_t>(b2) << 8U)
            | static_cast<std::uint32_t>(b3);
    }
    return words;
}

double bytes_to_phase_turns(
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
    return wrap_turns(static_cast<double>(accumulator & 0xffffffffULL) / 4294967296.0);
}

double uint32_phase_turns(std::uint32_t value) {
    return wrap_turns(static_cast<double>(value) / 4294967296.0);
}

double difficulty_norm(double difficulty) {
    return clamp01(std::log2(std::max(difficulty, 1.0)) / 32.0);
}

bool has_non_zero_texture(const std::array<double, 9>& value) {
    for (double entry : value) {
        if (std::abs(entry) > 1.0e-8) {
            return true;
        }
    }
    return false;
}

float spectral_value(double value) {
    return static_cast<float>(std::tanh(value));
}

double mean4(const std::array<double, 4>& values) {
    return (values[0] + values[1] + values[2] + values[3]) / 4.0;
}

double mean6(const std::array<double, 6>& values) {
    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return total / 6.0;
}

std::uint64_t saturating_mul_u64(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs == 0U || rhs == 0U) {
        return 0U;
    }
    if (lhs > (std::numeric_limits<std::uint64_t>::max() / rhs)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs * rhs;
}

std::uint64_t saturating_add_u64(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs > (std::numeric_limits<std::uint64_t>::max() - rhs)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
}

std::uint64_t pow_u64(std::uint64_t base, std::uint32_t exponent) {
    std::uint64_t value = 1U;
    for (std::uint32_t tier = 0U; tier < exponent; ++tier) {
        value = saturating_mul_u64(value, base);
    }
    return value;
}

std::uint64_t geometric_sum_u64(std::uint64_t base, std::uint32_t exponent) {
    std::uint64_t total = 1U;
    std::uint64_t term = 1U;
    for (std::uint32_t tier = 1U; tier <= exponent; ++tier) {
        term = saturating_mul_u64(term, base);
        total = saturating_add_u64(total, term);
    }
    return total;
}

struct DynamicFourierActivationField {
    double carrier_frequency_norm = 0.0;
    double carrier_amplitude_norm = 0.0;
    double carrier_voltage_norm = 0.0;
    double carrier_amperage_norm = 0.0;
    double temporal_drive_norm = 0.0;
    double harmonic_gate_norm = 0.0;
    double harmonic_noise_sink_norm = 0.0;
    std::uint32_t branch_factor = 1U;
    std::uint32_t inner_tier_depth = 1U;
    std::uint32_t frontier_tier_depth = 1U;
    std::uint32_t pulse_operator_bits = 1U;
    std::uint64_t frontier_activation_count = 1U;
    std::uint64_t cumulative_activation_count = 1U;
    double frontier_activation_budget_norm = 0.0;
    double cumulative_activation_budget_norm = 0.0;
    double pulse_operator_density_norm = 0.0;
    double nested_fourier_resonance_norm = 0.0;
};

struct TargetSequenceEncoding {
    double sequence_phase_turns = 0.0;
    double sequence_frequency_norm = 0.0;
    double repeat_flux_norm = 0.0;
    double reverse_observer_collapse_norm = 0.0;
};

TargetSequenceEncoding encode_target_sequence(
    const std::string& target_hex,
    double share_target_phase,
    double header_phase,
    double nonce_origin_phase,
    const SubstrateStratumPhaseFluxMeasurement& sha_measurement
) {
    const std::string normalized = normalize_hex(target_hex);
    if (normalized.empty()) {
        return {};
    }

    std::uint64_t weighted_accumulator = 0U;
    std::uint64_t rotational_accumulator = 0U;
    std::size_t repeated_edges = 0U;
    std::size_t maximum_run = 1U;
    std::size_t current_run = 1U;
    std::array<std::size_t, 16> digit_histogram {};
    double nibble_energy = 0.0;

    for (std::size_t index = 0; index < normalized.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(normalized[index]);
        const std::uint64_t position_weight = static_cast<std::uint64_t>((index + 1U) * (index + 3U));
        weighted_accumulator = (weighted_accumulator + (static_cast<std::uint64_t>(character) * position_weight)) & 0xffffffffULL;
        rotational_accumulator ^= (static_cast<std::uint64_t>(character) << ((index % 8U) * 8U));
        nibble_energy += static_cast<double>(character & 0x0fU) / 15.0;

        const int digit = hex_value(static_cast<char>(character));
        if (digit >= 0) {
            digit_histogram[static_cast<std::size_t>(digit)] += 1U;
        }

        if (index > 0U && normalized[index] == normalized[index - 1U]) {
            ++repeated_edges;
            ++current_run;
            maximum_run = std::max(maximum_run, current_run);
        } else {
            current_run = 1U;
        }
    }

    std::size_t unique_digit_count = 0U;
    std::size_t dominant_digit_count = 0U;
    for (const std::size_t count : digit_histogram) {
        if (count > 0U) {
            ++unique_digit_count;
            dominant_digit_count = std::max(dominant_digit_count, count);
        }
    }

    const double sequence_phase = wrap_turns(
        static_cast<double>((weighted_accumulator ^ rotational_accumulator) & 0xffffffffULL) / 4294967296.0);
    const double repeat_flux = clamp01(
        (0.58 * (normalized.size() <= 1U
            ? 0.0
            : static_cast<double>(repeated_edges) / static_cast<double>(normalized.size() - 1U)))
        + (0.24 * (static_cast<double>(maximum_run) / static_cast<double>(normalized.size())))
        + (0.18 * (static_cast<double>(dominant_digit_count) / static_cast<double>(normalized.size()))));
    const double sequence_frequency = mean_unit({
        sequence_phase,
        clamp01(nibble_energy / static_cast<double>(normalized.size())),
        clamp01(static_cast<double>(unique_digit_count) / 16.0),
        1.0 - repeat_flux,
    });
    const double reverse_observer = mean_unit({
        1.0 - clamp01(std::abs(phase_delta_turns(sequence_phase, share_target_phase)) * 2.0),
        1.0 - clamp01(std::abs(phase_delta_turns(sequence_phase, header_phase)) * 2.0),
        1.0 - clamp01(std::abs(phase_delta_turns(sequence_phase, nonce_origin_phase)) * 2.0),
        1.0 - clamp01(std::abs(phase_delta_turns(sequence_phase, sha_measurement.sha256_digest_phase_turns)) * 2.0),
        1.0 - clamp01(std::abs(phase_delta_turns(sequence_phase, sha_measurement.sha256_round_phase_turns)) * 2.0),
        repeat_flux,
    });

    return TargetSequenceEncoding{
        sequence_phase,
        sequence_frequency,
        repeat_flux,
        reverse_observer,
    };
}

DynamicFourierActivationField derive_dynamic_fourier_activation_field(
    std::size_t target_symbol_count,
    std::size_t active_worker_count,
    std::size_t attempted_nonce_count,
    double target_frequency_norm,
    double target_sequence_frequency_norm,
    double target_repeat_flux_norm,
    double reverse_observer_collapse_norm,
    double target_resonance_norm,
    double resonance_activation_norm,
    double phase_flux_conservation_norm,
    double validation_structure_norm,
    double transfer_drive_norm,
    double stability_gate_norm,
    double damping_norm,
    double transport_drive_norm,
    double worker_parallelism_norm,
    double lane_coherence_norm,
    double temporal_admissibility_norm,
    double zero_point_proximity_norm,
    double transport_readiness_norm,
    double share_confidence_norm,
    double validation_rate_norm,
    double sha_frequency_bias_norm,
    double sha_harmonic_density_norm,
    double observer_collapse_strength_norm,
    double nonce_collapse_confidence_norm,
    double pool_ingest_vector_norm,
    double pool_submit_vector_norm
) {
    const double symbol_count = static_cast<double>(std::max<std::size_t>(target_symbol_count, 1U));
    const double worker_count = static_cast<double>(std::max<std::size_t>(active_worker_count, 1U));
    const double attempt_energy = std::log2(1.0 + static_cast<double>(attempted_nonce_count));

    DynamicFourierActivationField field;
    field.carrier_frequency_norm = mean_unit({
        target_frequency_norm,
        target_sequence_frequency_norm,
        sha_frequency_bias_norm,
        sha_harmonic_density_norm,
        pool_ingest_vector_norm,
    });
    field.carrier_amplitude_norm = mean_unit({
        resonance_activation_norm,
        lane_coherence_norm,
        phase_flux_conservation_norm,
        target_repeat_flux_norm,
        share_confidence_norm,
        validation_structure_norm,
    });
    field.carrier_voltage_norm = mean_unit({
        target_resonance_norm,
        pool_submit_vector_norm,
        observer_collapse_strength_norm,
        validation_structure_norm,
        1.0 - damping_norm,
    });
    field.carrier_amperage_norm = mean_unit({
        transfer_drive_norm,
        transport_drive_norm,
        worker_parallelism_norm,
        validation_rate_norm,
        nonce_collapse_confidence_norm,
    });
    field.temporal_drive_norm = mean_unit({
        temporal_admissibility_norm,
        zero_point_proximity_norm,
        transport_readiness_norm,
        phase_flux_conservation_norm,
        reverse_observer_collapse_norm,
        target_sequence_frequency_norm,
    });
    field.harmonic_gate_norm = mean_unit({
        target_resonance_norm,
        resonance_activation_norm,
        validation_structure_norm,
        phase_flux_conservation_norm,
        lane_coherence_norm,
        share_confidence_norm,
        1.0 - damping_norm,
    });
    field.harmonic_noise_sink_norm = mean_unit({
        damping_norm,
        1.0 - validation_structure_norm,
        1.0 - phase_flux_conservation_norm,
        1.0 - lane_coherence_norm,
        1.0 - share_confidence_norm,
    });

    const double branch_observable =
        (symbol_count * field.carrier_frequency_norm * field.carrier_amplitude_norm * field.harmonic_gate_norm)
        + (worker_count * field.carrier_amperage_norm)
        + (attempt_energy * field.temporal_drive_norm);
    field.branch_factor = static_cast<std::uint32_t>(std::max<long long>(
        1LL,
        std::llround(branch_observable)));

    const double tier_seed =
        1.0 + (symbol_count * field.temporal_drive_norm * std::max(field.harmonic_gate_norm, 1.0e-9));
    field.inner_tier_depth = static_cast<std::uint32_t>(std::max<long long>(
        1LL,
        std::llround(1.0 + std::log2(1.0 + tier_seed + (worker_count * field.carrier_voltage_norm)))));

    const double frontier_seed =
        1.0
        + (symbol_count * resonance_activation_norm * phase_flux_conservation_norm * target_sequence_frequency_norm)
        + (worker_count * field.carrier_frequency_norm)
        + (attempt_energy * (1.0 - field.harmonic_noise_sink_norm));
    field.frontier_tier_depth = std::max<std::uint32_t>(
        field.inner_tier_depth,
        field.inner_tier_depth + static_cast<std::uint32_t>(std::max<long long>(
            1LL,
            std::llround(std::log2(1.0 + frontier_seed)))));

    const double pulse_operator_span =
        symbol_count
        * 8.0
        * (1.0
            + field.carrier_frequency_norm
            + field.carrier_amplitude_norm
            + field.carrier_voltage_norm
            + field.carrier_amperage_norm
            + field.temporal_drive_norm
            + field.harmonic_gate_norm);
    field.pulse_operator_bits = static_cast<std::uint32_t>(std::max<long long>(
        1LL,
        std::llround(pulse_operator_span)));

    field.frontier_activation_count = pow_u64(field.branch_factor, field.frontier_tier_depth);
    field.cumulative_activation_count = geometric_sum_u64(field.branch_factor, field.frontier_tier_depth);
    field.frontier_activation_budget_norm = clamp01(
        std::log2(1.0 + static_cast<double>(field.frontier_activation_count))
        / std::log2(2.0 + static_cast<double>(field.pulse_operator_bits) + symbol_count));
    field.cumulative_activation_budget_norm = clamp01(
        std::log2(1.0 + static_cast<double>(field.cumulative_activation_count))
        / std::log2(2.0 + static_cast<double>(field.pulse_operator_bits) + (symbol_count * worker_count)));
    field.pulse_operator_density_norm = mean_unit({
        field.carrier_frequency_norm,
        field.carrier_amplitude_norm,
        field.carrier_voltage_norm,
        field.carrier_amperage_norm,
        field.harmonic_gate_norm,
        1.0 - field.harmonic_noise_sink_norm,
    });
    field.nested_fourier_resonance_norm = mean_unit({
        field.frontier_activation_budget_norm,
        field.cumulative_activation_budget_norm,
        field.pulse_operator_density_norm,
        field.temporal_drive_norm,
        field.harmonic_gate_norm,
        1.0 - field.harmonic_noise_sink_norm,
    });
    return field;
}

}  // namespace

MiningPhaseEncodingState build_mining_phase_encoding_state(const SubstrateStratumAuthorityState* authority_state) {
    MiningPhaseEncodingState state;
    const SubstratePhaseProgramMetadata& mining_program = mining_resonance_program_metadata();
    state.phase_program_block_count = static_cast<std::uint32_t>(mining_program.block_count());
    state.phase_program_substrate_native =
        mining_program.has_block("mining_os_resonance_field", "carrier")
        && mining_program.has_block("harmonic_sha_field", "transport")
        && mining_program.has_block("temporal_phase_trajectory_accounting", "scheduler")
        && mining_program.has_block("mining_candidate_topology", "association")
        && mining_program.has_block("mining_submit_readiness", "gate")
        && mining_program.has_block("mining_resonance_buffer", "commit");
    state.phase_program_same_pulse_validation =
        mining_program.block_has_rule_token("harmonic_sha_field", "same_pulse_validation");
    state.phase_program_pool_format_ready =
        mining_program.block_has_rule_token("harmonic_sha_field", "pool_ingest_vector")
        && mining_program.block_has_rule_token("harmonic_sha_field", "pool_submit_vector");
    if (authority_state == nullptr
        || !authority_state->has_active_job
        || !authority_state->has_difficulty
        || authority_state->active_share_target_hex.empty()) {
        return state;
    }

    const std::vector<std::uint8_t> share_target_bytes = hex_to_bytes(authority_state->active_share_target_hex);
    const std::vector<std::uint8_t> block_target_bytes = hex_to_bytes(authority_state->active_block_target_hex);
    const std::vector<std::uint8_t> header_bytes = hex_to_bytes(authority_state->active_job_header_hex);
    const double share_target_phase = authority_state->has_last_phase_trace
        ? authority_state->last_phase_trace_share_target_phase_turns
        : bytes_to_phase_turns(share_target_bytes, 0U, 16U);
    const double block_target_phase = authority_state->has_last_phase_trace
        ? authority_state->last_phase_trace_block_target_phase_turns
        : bytes_to_phase_turns(block_target_bytes, 0U, 16U);
    const double header_phase = authority_state->has_last_phase_trace
        ? authority_state->last_phase_trace_header_phase_turns
        : bytes_to_phase_turns(header_bytes, 0U, 16U);
    SubstrateStratumPhaseFluxMeasurement sha_measurement =
        bias_phase_flux_measurement_with_sha256_frequency(
            authority_state->active_job_header_hex,
            authority_state->active_job_nbits,
            authority_state->difficulty);
    if (authority_state->has_last_phase_trace) {
        sha_measurement.sha256_schedule_phase_turns = authority_state->last_phase_trace_sha256_schedule_phase_turns;
        sha_measurement.sha256_round_phase_turns = authority_state->last_phase_trace_sha256_round_phase_turns;
        sha_measurement.sha256_digest_phase_turns = authority_state->last_phase_trace_sha256_digest_phase_turns;
    }

    std::uint32_t nonce_origin = 0U;
    bool has_nonce_origin = false;
    for (const auto& assignment : authority_state->worker_assignments) {
        if (!assignment.active) {
            continue;
        }
        nonce_origin = assignment.nonce_start;
        has_nonce_origin = true;
        break;
    }
    const double nonce_origin_phase = authority_state->has_last_phase_trace
        ? authority_state->last_phase_trace_nonce_seed_phase_turns
        : (has_nonce_origin ? uint32_phase_turns(nonce_origin) : 0.0);
    const TargetSequenceEncoding target_sequence = encode_target_sequence(
        authority_state->active_share_target_hex,
        share_target_phase,
        header_phase,
        nonce_origin_phase,
        sha_measurement);

    const double header_target_alignment = 1.0 - clamp01(std::abs(phase_delta_turns(header_phase, share_target_phase)) * 2.0);
    const double nonce_target_alignment = has_nonce_origin
        ? (1.0 - clamp01(std::abs(phase_delta_turns(nonce_origin_phase, share_target_phase)) * 2.0))
        : 0.5;
    const double sha_target_alignment = 1.0 - clamp01(std::abs(phase_delta_turns(
        sha_measurement.sha256_digest_phase_turns,
        share_target_phase
    )) * 2.0);
    const double worker_parallelism_norm = clamp01(
        static_cast<double>(authority_state->active_worker_count)
        / static_cast<double>(std::max<std::size_t>(authority_state->allowed_worker_count, 1U)));
    const double preview_validity_norm = authority_state->submit_preview_count == 0U
        ? 0.0
        : clamp01(
            static_cast<double>(authority_state->offline_valid_submit_preview_count)
            / static_cast<double>(authority_state->submit_preview_count));
    const double validation_alignment_norm = authority_state->active_worker_count == 0U
        ? 0.0
        : clamp01(
            static_cast<double>(authority_state->workers_meeting_validation_sample_threshold)
            / static_cast<double>(authority_state->active_worker_count));
    const double request_pressure_norm = authority_state->effective_request_budget_per_s <= 0.0
        ? 0.0
        : clamp01(authority_state->required_share_submissions_per_s / authority_state->effective_request_budget_per_s);
    const double validation_rate_norm = authority_state->required_share_submissions_per_s <= 0.0
        ? 0.0
        : clamp01(
            authority_state->measured_validation_share_rate_per_s
            / authority_state->required_share_submissions_per_s);
    const double phase_flux_conservation_norm = clamp01(authority_state->last_phase_flux_conservation);
    const double nonce_collapse_confidence_norm = clamp01(authority_state->last_nonce_collapse_confidence);
    const double observer_collapse_strength_norm = clamp01(authority_state->last_observer_collapse_strength);
    const double base_target_resonance_norm = mean_unit({
        sha_measurement.target_resonance_norm,
        authority_state->last_target_resonance_norm,
        header_target_alignment,
        nonce_target_alignment,
        sha_target_alignment,
    });
    const double base_resonance_activation_norm = clamp01(mean_unit({
        authority_state->last_phase_clamped_resonance_activation_norm,
        authority_state->last_phase_clamped_target_resonance_norm,
        phase_flux_conservation_norm,
        nonce_collapse_confidence_norm,
        observer_collapse_strength_norm,
    }));
    const DynamicFourierActivationField dynamic_fourier_field = derive_dynamic_fourier_activation_field(
        normalize_hex(authority_state->active_share_target_hex).size(),
        authority_state->active_worker_count,
        authority_state->last_phase_clamped_attempted_nonce_count,
        mean_unit({
            share_target_phase,
            block_target_phase,
            difficulty_norm(authority_state->difficulty),
            header_target_alignment,
            sha_measurement.sha256_frequency_bias_norm,
            sha_measurement.sha256_harmonic_density_norm,
            target_sequence.sequence_frequency_norm,
        }),
        target_sequence.sequence_frequency_norm,
        target_sequence.repeat_flux_norm,
        target_sequence.reverse_observer_collapse_norm,
        base_target_resonance_norm,
        base_resonance_activation_norm,
        phase_flux_conservation_norm,
        clamp01(std::max(
            authority_state->last_phase_clamped_validation_structure_norm,
            std::min({
                header_target_alignment,
                nonce_target_alignment,
                sha_target_alignment,
                1.0 - clamp01(std::abs(phase_delta_turns(target_sequence.sequence_phase_turns, share_target_phase)) * 2.0),
            }))),
        clamp01(mean_unit({
            authority_state->last_phase_clamped_transfer_drive_norm,
            sha_measurement.transfer_drive_norm,
            phase_flux_conservation_norm,
            base_resonance_activation_norm,
        })),
        clamp01(mean_unit({
            authority_state->last_phase_clamped_stability_gate_norm,
            sha_measurement.stability_gate_norm,
            base_target_resonance_norm,
            phase_flux_conservation_norm,
        })),
        clamp01(mean_unit({
            authority_state->last_phase_clamped_damping_norm,
            sha_measurement.damping_norm,
            1.0 - observer_collapse_strength_norm,
        })),
        clamp01(mean_unit({
            authority_state->last_phase_clamped_transport_drive_norm,
            sha_measurement.transport_drive_norm,
            base_resonance_activation_norm,
            phase_flux_conservation_norm,
            1.0 - clamp01(mean_unit({
                authority_state->last_phase_clamped_damping_norm,
                sha_measurement.damping_norm,
                1.0 - observer_collapse_strength_norm,
            })),
        })),
        worker_parallelism_norm,
        mean_unit({
            preview_validity_norm,
            validation_alignment_norm,
            validation_rate_norm,
            header_target_alignment,
            nonce_target_alignment,
            sha_target_alignment,
            base_target_resonance_norm,
            base_resonance_activation_norm,
            worker_parallelism_norm,
            authority_state->network_authority_granted ? 1.0 : 0.25,
            sha_measurement.sha256_harmonic_density_norm,
            target_sequence.sequence_frequency_norm,
            target_sequence.reverse_observer_collapse_norm,
        }),
        clamp01(authority_state->last_phase_clamped_temporal_admissibility),
        clamp01(authority_state->last_phase_clamped_zero_point_proximity),
        clamp01(authority_state->last_phase_clamped_transport_readiness),
        clamp01(authority_state->last_phase_clamped_share_confidence),
        validation_rate_norm,
        sha_measurement.sha256_frequency_bias_norm,
        sha_measurement.sha256_harmonic_density_norm,
        observer_collapse_strength_norm,
        nonce_collapse_confidence_norm,
        mean_unit({
            mean_unit({
                share_target_phase,
                block_target_phase,
                difficulty_norm(authority_state->difficulty),
                header_target_alignment,
                sha_measurement.sha256_frequency_bias_norm,
                sha_measurement.sha256_harmonic_density_norm,
                target_sequence.sequence_frequency_norm,
            }),
            target_sequence.sequence_frequency_norm,
            target_sequence.repeat_flux_norm,
            difficulty_norm(authority_state->difficulty),
            header_target_alignment,
            sha_measurement.sha256_frequency_bias_norm,
        }),
        mean_unit({
            authority_state->submit_path_ready ? 1.0 : 0.35,
            preview_validity_norm,
            validation_alignment_norm,
            validation_rate_norm,
            target_sequence.reverse_observer_collapse_norm,
            clamp01(std::max(
                authority_state->last_phase_clamped_validation_structure_norm,
                std::min({
                    header_target_alignment,
                    nonce_target_alignment,
                    sha_target_alignment,
                    1.0 - clamp01(std::abs(phase_delta_turns(target_sequence.sequence_phase_turns, share_target_phase)) * 2.0),
                }))),
        }));
    const double target_resonance_norm = mean_unit({
        base_target_resonance_norm,
        dynamic_fourier_field.harmonic_gate_norm,
        1.0 - dynamic_fourier_field.harmonic_noise_sink_norm,
    });
    const double resonance_activation_norm = clamp01(mean_unit({
        base_resonance_activation_norm,
        dynamic_fourier_field.nested_fourier_resonance_norm,
        dynamic_fourier_field.harmonic_gate_norm,
        1.0 - dynamic_fourier_field.harmonic_noise_sink_norm,
    }));
    const double spider_code_frequency_norm = mean_unit({
        dynamic_fourier_field.carrier_frequency_norm,
        target_sequence.sequence_frequency_norm,
        sha_measurement.sha256_frequency_bias_norm,
        header_target_alignment,
        target_resonance_norm,
    });
    const double spider_code_amplitude_norm = mean_unit({
        dynamic_fourier_field.carrier_amplitude_norm,
        resonance_activation_norm,
        phase_flux_conservation_norm,
        preview_validity_norm,
        validation_alignment_norm,
        1.0 - dynamic_fourier_field.harmonic_noise_sink_norm,
    });
    const double spider_code_voltage_norm = mean_unit({
        dynamic_fourier_field.carrier_voltage_norm,
        target_resonance_norm,
        observer_collapse_strength_norm,
        target_sequence.reverse_observer_collapse_norm,
        dynamic_fourier_field.harmonic_gate_norm,
    });
    const double spider_code_amperage_norm = mean_unit({
        dynamic_fourier_field.carrier_amperage_norm,
        worker_parallelism_norm,
        validation_rate_norm,
        request_pressure_norm,
        resonance_activation_norm,
    });
    const double spider_projection_coherence_norm = mean_unit({
        header_target_alignment,
        nonce_target_alignment,
        sha_target_alignment,
        dynamic_fourier_field.temporal_drive_norm,
        dynamic_fourier_field.harmonic_gate_norm,
        1.0 - dynamic_fourier_field.harmonic_noise_sink_norm,
        target_resonance_norm,
    });

    state.target_direction_xyz = normalize3({
        static_cast<float>(clamp_signed(std::cos(kTau * (
            share_target_phase
            + (0.18 * target_sequence.sequence_phase_turns)
            + (0.25 * sha_measurement.sha256_schedule_phase_turns)
            + (0.125 * sha_measurement.sha256_round_phase_turns))))),
        static_cast<float>(clamp_signed(std::sin(kTau * (
            header_phase
            + (0.25 * nonce_origin_phase)
            + (0.16 * target_sequence.sequence_phase_turns)
            + (0.125 * sha_measurement.sha256_digest_phase_turns))))),
        static_cast<float>(clamp_signed(
            -0.35
            + (0.55 * target_resonance_norm)
            + (0.40 * sha_measurement.sha256_frequency_bias_norm)
            + (0.24 * target_sequence.reverse_observer_collapse_norm)
            + (0.40 * worker_parallelism_norm))),
    });
    state.share_target_phase_turns = static_cast<float>(share_target_phase);
    state.header_phase_turns = static_cast<float>(header_phase);
    state.nonce_origin_phase_turns = static_cast<float>(nonce_origin_phase);
    state.target_resonance_norm = static_cast<float>(target_resonance_norm);
    state.resonance_activation_norm = static_cast<float>(resonance_activation_norm);
    state.phase_flux_conservation_norm = static_cast<float>(phase_flux_conservation_norm);
    state.nonce_collapse_confidence_norm = static_cast<float>(nonce_collapse_confidence_norm);
    state.observer_collapse_strength_norm = static_cast<float>(observer_collapse_strength_norm);
    state.validation_structure_norm = static_cast<float>(clamp01(std::max(
        authority_state->last_phase_clamped_validation_structure_norm,
        std::min({
            header_target_alignment,
            nonce_target_alignment,
            sha_target_alignment,
            1.0 - clamp01(std::abs(phase_delta_turns(target_sequence.sequence_phase_turns, share_target_phase)) * 2.0),
        }))));
    state.sha256_schedule_phase_turns = static_cast<float>(sha_measurement.sha256_schedule_phase_turns);
    state.sha256_round_phase_turns = static_cast<float>(sha_measurement.sha256_round_phase_turns);
    state.sha256_digest_phase_turns = static_cast<float>(sha_measurement.sha256_digest_phase_turns);
    state.sha256_frequency_bias_norm = static_cast<float>(sha_measurement.sha256_frequency_bias_norm);
    state.sha256_harmonic_density_norm = static_cast<float>(sha_measurement.sha256_harmonic_density_norm);
    state.target_sequence_phase_turns = static_cast<float>(target_sequence.sequence_phase_turns);
    state.target_frequency_norm = static_cast<float>(mean_unit({
        share_target_phase,
        block_target_phase,
        difficulty_norm(authority_state->difficulty),
        header_target_alignment,
        sha_measurement.sha256_frequency_bias_norm,
        sha_measurement.sha256_harmonic_density_norm,
        target_sequence.sequence_frequency_norm,
    }));
    state.target_sequence_frequency_norm = static_cast<float>(target_sequence.sequence_frequency_norm);
    state.target_repeat_flux_norm = static_cast<float>(target_sequence.repeat_flux_norm);
    state.reverse_observer_collapse_norm = static_cast<float>(target_sequence.reverse_observer_collapse_norm);
    state.spider_code_frequency_norm = static_cast<float>(spider_code_frequency_norm);
    state.spider_code_amplitude_norm = static_cast<float>(spider_code_amplitude_norm);
    state.spider_code_voltage_norm = static_cast<float>(spider_code_voltage_norm);
    state.spider_code_amperage_norm = static_cast<float>(spider_code_amperage_norm);
    state.spider_projection_coherence_norm = static_cast<float>(spider_projection_coherence_norm);
    state.spider_harmonic_gate_norm = static_cast<float>(dynamic_fourier_field.harmonic_gate_norm);
    state.spider_noise_sink_norm = static_cast<float>(dynamic_fourier_field.harmonic_noise_sink_norm);
    state.frontier_activation_budget_norm = static_cast<float>(dynamic_fourier_field.frontier_activation_budget_norm);
    state.cumulative_activation_budget_norm = static_cast<float>(dynamic_fourier_field.cumulative_activation_budget_norm);
    state.pulse_operator_density_norm = static_cast<float>(dynamic_fourier_field.pulse_operator_density_norm);
    state.nested_fourier_resonance_norm = static_cast<float>(dynamic_fourier_field.nested_fourier_resonance_norm);
    state.pool_ingest_vector_norm = static_cast<float>(mean_unit({
        spider_code_frequency_norm,
        spider_projection_coherence_norm,
        target_sequence.sequence_frequency_norm,
        target_sequence.repeat_flux_norm,
        difficulty_norm(authority_state->difficulty),
        header_target_alignment,
        sha_measurement.sha256_frequency_bias_norm,
        dynamic_fourier_field.harmonic_gate_norm,
    }));
    state.pool_submit_vector_norm = static_cast<float>(mean_unit({
        authority_state->submit_path_ready ? 1.0 : 0.35,
        preview_validity_norm,
        validation_alignment_norm,
        validation_rate_norm,
        target_sequence.reverse_observer_collapse_norm,
        state.validation_structure_norm,
        spider_code_voltage_norm,
        dynamic_fourier_field.harmonic_gate_norm,
    }));
    state.phase_pressure_norm = static_cast<float>(mean_unit({
        difficulty_norm(authority_state->difficulty),
        request_pressure_norm,
        header_target_alignment,
        nonce_target_alignment,
        target_resonance_norm,
        sha_measurement.sha256_frequency_bias_norm,
        authority_state->submit_path_ready ? 1.0 : 0.35,
        resonance_activation_norm,
        phase_flux_conservation_norm,
        observer_collapse_strength_norm,
        target_sequence.sequence_frequency_norm,
        target_sequence.reverse_observer_collapse_norm,
        spider_code_amperage_norm,
        dynamic_fourier_field.harmonic_gate_norm,
    }));
    state.transfer_drive_norm = static_cast<float>(clamp01(mean_unit({
        authority_state->last_phase_clamped_transfer_drive_norm,
        sha_measurement.transfer_drive_norm,
        phase_flux_conservation_norm,
        resonance_activation_norm,
    })));
    state.stability_gate_norm = static_cast<float>(clamp01(mean_unit({
        authority_state->last_phase_clamped_stability_gate_norm,
        sha_measurement.stability_gate_norm,
        target_resonance_norm,
        phase_flux_conservation_norm,
    })));
    state.damping_norm = static_cast<float>(clamp01(mean_unit({
        authority_state->last_phase_clamped_damping_norm,
        sha_measurement.damping_norm,
        1.0 - observer_collapse_strength_norm,
    })));
    state.transport_drive_norm = static_cast<float>(clamp01(mean_unit({
        authority_state->last_phase_clamped_transport_drive_norm,
        sha_measurement.transport_drive_norm,
        resonance_activation_norm,
        phase_flux_conservation_norm,
        1.0 - clamp01(state.damping_norm),
        dynamic_fourier_field.cumulative_activation_budget_norm,
    })));
    state.worker_parallelism_norm = static_cast<float>(mean_unit({
        worker_parallelism_norm,
        spider_code_amperage_norm,
        dynamic_fourier_field.harmonic_gate_norm,
        spider_projection_coherence_norm,
    }));
    state.lane_coherence_norm = static_cast<float>(mean_unit({
        preview_validity_norm,
        validation_alignment_norm,
        validation_rate_norm,
        header_target_alignment,
        nonce_target_alignment,
        sha_target_alignment,
        target_resonance_norm,
        resonance_activation_norm,
        worker_parallelism_norm,
        authority_state->network_authority_granted ? 1.0 : 0.25,
        sha_measurement.sha256_harmonic_density_norm,
        target_sequence.sequence_frequency_norm,
        target_sequence.reverse_observer_collapse_norm,
        spider_projection_coherence_norm,
        dynamic_fourier_field.harmonic_gate_norm,
        1.0 - dynamic_fourier_field.harmonic_noise_sink_norm,
        dynamic_fourier_field.nested_fourier_resonance_norm,
    }));
    state.target_lane_count = static_cast<std::uint32_t>(std::max<std::size_t>(authority_state->active_worker_count, 1U));
    state.phase_lock_error_norm = static_cast<float>(clamp01(authority_state->last_phase_clamped_phase_lock_error));
    state.phase_clamp_strength_norm = static_cast<float>(clamp01(authority_state->last_phase_clamped_phase_clamp_strength));
    state.temporal_admissibility_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_temporal_admissibility));
    state.zero_point_proximity_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_zero_point_proximity));
    state.transport_readiness_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_transport_readiness));
    state.share_confidence_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_share_confidence));
    state.validation_rate_norm = static_cast<float>(validation_rate_norm);
    state.active_worker_count = static_cast<std::uint32_t>(authority_state->active_worker_count);
    state.fourier_branch_factor = dynamic_fourier_field.branch_factor;
    state.fourier_inner_tier_depth = dynamic_fourier_field.inner_tier_depth;
    state.fourier_frontier_tier_depth = dynamic_fourier_field.frontier_tier_depth;
    state.pulse_operator_capacity_bits = dynamic_fourier_field.pulse_operator_bits;
    state.fourier_frontier_activation_count = dynamic_fourier_field.frontier_activation_count;
    state.fourier_cumulative_activation_count = dynamic_fourier_field.cumulative_activation_count;
    state.attempted_nonce_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        authority_state->last_phase_clamped_attempted_nonce_count,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    state.valid_nonce_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        authority_state->last_phase_clamped_valid_nonce_count,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    state.active = true;
    state.submit_path_ready = authority_state->submit_path_ready;
    state.all_parallel_harmonics_verified =
        authority_state->last_phase_clamped_all_parallel_harmonics_verified;
    const double attempted_nonce_count = static_cast<double>(std::max<std::size_t>(
        authority_state->last_phase_clamped_attempted_nonce_count,
        1U));
    const double valid_nonce_count = static_cast<double>(std::max<std::size_t>(
        authority_state->last_phase_clamped_valid_nonce_count,
        1U));
    state.share_target_pass_norm = authority_state->last_phase_clamped_share_target_pass ? 1.0f : 0.0f;
    state.block_target_pass_norm = authority_state->last_phase_clamped_block_target_pass ? 1.0f : 0.0f;
    state.block_coherence_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_block_coherence_norm));
    state.reinforcement_norm = static_cast<float>(clamp01(
        static_cast<double>(authority_state->last_phase_clamped_resonance_reinforcement_count)
        / valid_nonce_count));
    state.noise_lane_fraction_norm = static_cast<float>(clamp01(
        static_cast<double>(authority_state->last_phase_clamped_noise_lane_count)
        / attempted_nonce_count));
    state.submit_priority_score_norm =
        static_cast<float>(clamp01(authority_state->last_phase_clamped_submit_priority_score));
    state.resonance_reinforcement_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        authority_state->last_phase_clamped_resonance_reinforcement_count,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    state.noise_lane_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        authority_state->last_phase_clamped_noise_lane_count,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    state.queue_quality_class = authority_state->last_phase_clamped_queue_quality_class;
    return state;
}

MiningGpuAuthorityField build_mining_gpu_authority_field(const SubstrateStratumAuthorityState* authority_state) {
    MiningGpuAuthorityField field;
    if (authority_state == nullptr
        || !authority_state->has_active_job
        || !authority_state->has_difficulty
        || authority_state->active_share_target_hex.empty()
        || authority_state->active_job_nbits.empty()) {
        return field;
    }

    field.connection_id = authority_state->has_connection_ingress
        ? authority_state->connection_ingress.connection_id
        : std::string{};
    field.job_id = authority_state->active_job_id;
    field.nbits_hex = authority_state->active_job_nbits;
    field.share_target_hex = authority_state->active_share_target_hex;
    field.block_target_hex = authority_state->active_block_target_hex;
    field.share_difficulty = authority_state->difficulty;
    field.share_target_words = bytes_to_words_be<8>(hex_to_bytes(field.share_target_hex));
    field.block_target_words = bytes_to_words_be<8>(hex_to_bytes(field.block_target_hex));

    for (const auto& assignment : authority_state->worker_assignments) {
        if (!assignment.active || assignment.worker_index >= field.workers.size()) {
            continue;
        }
        MiningGpuWorkerAuthority worker;
        worker.active = true;
        worker.worker_index = static_cast<std::uint32_t>(assignment.worker_index);
        worker.nonce_start = assignment.nonce_start;
        worker.nonce_end = assignment.nonce_end;
        worker.header_template_words = bytes_to_words_be<20>(
            hex_to_bytes(resolve_stratum_job_header_hex(*authority_state, assignment)));
        field.workers[assignment.worker_index] = worker;
        ++field.active_worker_count;
    }

    field.active = field.active_worker_count > 0U;
    return field;
}

SavedFieldTextureState build_saved_field_texture_state(
    const SubstrateViewportFrame& viewport_frame,
    const SubstrateStratumAuthorityState* authority_state
) {
    SavedFieldTextureState state;
    const bool use_texture = has_non_zero_texture(viewport_frame.texture_map_9d);
    for (std::size_t index = 0; index < state.carrier_9d.size(); ++index) {
        const double texture_value = viewport_frame.texture_map_9d[index];
        const double tensor_value = viewport_frame.tensor_signature_6d[index % viewport_frame.tensor_signature_6d.size()];
        state.carrier_9d[index] = use_texture ? spectral_value(texture_value) : spectral_value(tensor_value);
        state.resume_trajectory_9d[index] = spectral_value(0.62 * texture_value + 0.38 * tensor_value);
    }

    state.pulse_quartet = {
        static_cast<float>(clamp01(viewport_frame.texture_map_9d[0])),
        static_cast<float>(clamp01(viewport_frame.visual_rgba[0])),
        static_cast<float>(clamp01(viewport_frame.audio_channels[1])),
        static_cast<float>(clamp01(viewport_frame.material_pbr[1])),
    };

    state.phase_direction_xyz = normalize3({
        static_cast<float>(viewport_frame.viewport_direction[0]),
        static_cast<float>(viewport_frame.viewport_direction[1]),
        static_cast<float>(viewport_frame.viewport_direction[2]),
    });
    state.phase_magnitude = static_cast<float>(clamp01(mean4({
        clamp01(viewport_frame.texture_map_9d[0]),
        viewport_frame.anchor_correlation,
        1.0 - viewport_frame.anchor_evm_norm,
        1.0 - viewport_frame.sideband_energy_norm,
    })));
    state.zero_point_proximity = static_cast<float>(clamp01(viewport_frame.material_pbr[3]));
    state.resonance_energy = static_cast<float>(clamp01(
        (0.45 * clamp01(viewport_frame.audio_channels[3]))
        + (0.30 * clamp01(viewport_frame.visual_rgba[2]))
        + (0.25 * viewport_frame.anchor_correlation)
    ));
    state.anchor_correlation = static_cast<float>(clamp01(viewport_frame.anchor_correlation));
    state.phase_lock_error = static_cast<float>(clamp01(viewport_frame.phase_lock_error));
    state.relock_pressure = static_cast<float>(clamp01(viewport_frame.relock_pressure));
    state.sideband_energy_norm = static_cast<float>(clamp01(viewport_frame.sideband_energy_norm));
    state.dynamic_range_headroom = static_cast<float>(clamp01(viewport_frame.dynamic_range_headroom));
    state.mining_phase_encoding = build_mining_phase_encoding_state(authority_state);
    if (state.mining_phase_encoding.active) {
        const auto& mining = state.mining_phase_encoding;
        const float target_alignment = clamp01f(1.0f - std::abs(mining.share_target_phase_turns - mining.header_phase_turns));
        state.pulse_quartet = {
            clamp01f(
                (0.18f * state.pulse_quartet[0])
                + (0.18f * mining.spider_code_frequency_norm)
                + (0.12f * mining.target_frequency_norm)
                + (0.12f * mining.target_sequence_frequency_norm)
                + (0.08f * mining.spider_projection_coherence_norm)
                + (0.16f * mining.share_target_phase_turns)
                + (0.14f * mining.header_phase_turns)
                + (0.12f * mining.sha256_schedule_phase_turns)
                + (0.08f * mining.sha256_frequency_bias_norm)
                + (0.08f * mining.spider_harmonic_gate_norm)
                + (0.08f * target_alignment)),
            clamp01f(
                (0.18f * state.pulse_quartet[1])
                + (0.24f * mining.phase_pressure_norm)
                + (0.12f * mining.spider_code_amplitude_norm)
                + (0.18f * mining.lane_coherence_norm)
                + (0.12f * mining.nonce_origin_phase_turns)
                + (0.10f * mining.reverse_observer_collapse_norm)
                + (0.08f * mining.spider_harmonic_gate_norm)
                + (0.16f * mining.sha256_round_phase_turns)
                + (0.10f * mining.target_resonance_norm)),
            clamp01f(
                (0.16f * state.pulse_quartet[2])
                + (0.22f * mining.phase_pressure_norm)
                + (0.12f * mining.spider_code_amperage_norm)
                + (0.16f * mining.worker_parallelism_norm)
                + (0.12f * target_alignment)
                + (0.10f * mining.target_repeat_flux_norm)
                + (0.08f * mining.spider_harmonic_gate_norm)
                + (0.18f * mining.sha256_digest_phase_turns)
                + (0.16f * mining.sha256_harmonic_density_norm)),
            clamp01f(
                (0.18f * state.pulse_quartet[3])
                + (0.16f * mining.spider_code_voltage_norm)
                + (0.12f * mining.target_frequency_norm)
                + (0.10f * mining.pool_submit_vector_norm)
                + (0.16f * mining.lane_coherence_norm)
                + (0.18f * mining.target_resonance_norm)
                + (0.08f * mining.spider_projection_coherence_norm)
                + (0.12f * mining.sha256_frequency_bias_norm)
                + (0.16f * (mining.submit_path_ready ? 1.0f : 0.35f))),
        };
        state.phase_direction_xyz = normalize3({
            (0.42f * state.phase_direction_xyz[0]) + (0.58f * mining.target_direction_xyz[0]),
            (0.42f * state.phase_direction_xyz[1]) + (0.58f * mining.target_direction_xyz[1]),
            (0.38f * state.phase_direction_xyz[2]) + (0.62f * mining.target_direction_xyz[2]),
        });
        state.phase_magnitude = clamp01f(
            (0.34f * state.phase_magnitude)
            + (0.28f * mining.phase_pressure_norm)
            + (0.20f * mining.lane_coherence_norm)
            + (0.10f * mining.target_resonance_norm)
            + (0.08f * mining.sha256_harmonic_density_norm)
            + (0.08f * mining.spider_projection_coherence_norm));
        state.zero_point_proximity = clamp01f(
            (0.56f * state.zero_point_proximity)
            + (0.22f * (1.0f - std::abs((2.0f * mining.share_target_phase_turns) - 1.0f)))
            + (0.12f * target_alignment)
            + (0.10f * mining.target_resonance_norm));
        state.resonance_energy = clamp01f(
            (0.36f * state.resonance_energy)
            + (0.18f * mining.spider_code_frequency_norm)
            + (0.20f * mining.worker_parallelism_norm)
            + (0.10f * mining.target_resonance_norm)
            + (0.10f * mining.sha256_frequency_bias_norm)
            + (0.08f * mining.spider_harmonic_gate_norm));
        state.anchor_correlation = clamp01f(
            (0.40f * state.anchor_correlation)
            + (0.32f * mining.lane_coherence_norm)
            + (0.18f * target_alignment)
            + (0.10f * mining.target_resonance_norm));
        state.phase_lock_error = clamp01f(
            (0.72f * state.phase_lock_error)
            + (0.28f * (1.0f - mining.lane_coherence_norm)));
        state.relock_pressure = clamp01f(
            (0.50f * state.relock_pressure)
            + (0.30f * mining.phase_pressure_norm)
            + (0.10f * (1.0f - target_alignment))
            + (0.10f * (1.0f - mining.target_resonance_norm)));
        state.sideband_energy_norm = clamp01f(
            (0.68f * state.sideband_energy_norm)
            + (0.18f * (1.0f - mining.lane_coherence_norm))
            + (0.14f * (1.0f - mining.sha256_harmonic_density_norm)));
        state.dynamic_range_headroom = clamp01f(
            (0.44f * state.dynamic_range_headroom)
            + (0.30f * (1.0f - mining.phase_pressure_norm))
            + (0.14f * mining.worker_parallelism_norm)
            + (0.12f * mining.target_resonance_norm));
    }
    state.trace_id = viewport_frame.phase_id;
    return state;
}

FieldViewportFrame build_field_viewport_frame(
    const SubstrateViewportFrame& viewport_frame,
    double time_s,
    std::uint32_t lattice_extent,
    const SubstrateStratumAuthorityState* authority_state
) {
    const std::uint32_t extent = std::max<std::uint32_t>(10U, lattice_extent);
    const SavedFieldTextureState saved_state = build_saved_field_texture_state(viewport_frame, authority_state);
    const double coherence = clamp01(viewport_frame.texture_map_9d[5]);
    const double flux = clamp01(viewport_frame.texture_map_9d[4]);
    const double temporal_noise = clamp01(viewport_frame.relock_pressure);
    const double boundary_energy = clamp01(
        (0.38 * clamp01(viewport_frame.tensor_signature_6d[1]))
        + (0.26 * clamp01(viewport_frame.sideband_energy_norm))
        + (0.20 * clamp01(viewport_frame.group_delay_skew))
        + (0.16 * temporal_noise)
    );
    const double gate_readiness = clamp01(
        (0.30 * clamp01(viewport_frame.anchor_correlation))
        + (0.25 * (1.0 - clamp01(viewport_frame.anchor_evm_norm)))
        + (0.25 * (1.0 - clamp01(viewport_frame.sideband_energy_norm)))
        + (0.20 * (1.0 - temporal_noise))
    );
    const float silicon_excitation_norm = static_cast<float>(clamp01(kSiliconExcitationEnergyEv / 300.0));
    const float silicon_density_norm = static_cast<float>(clamp01(kSiliconDensity / 3.0));
    const float lattice_scale_norm = static_cast<float>(clamp01(kSiliconLatticeConstantMeters * 1.0e10));

    FieldViewportFrame frame;
    frame.saved_state = saved_state;
    frame.gpu_mining_authority = build_mining_gpu_authority_field(authority_state);
    frame.authoritative_frame = viewport_frame;
    frame.extent_x = extent;
    frame.extent_y = extent;
    frame.extent_z = extent;
    frame.time_s = time_s;
    frame.material_trace_tag = viewport_frame.phase_id;
    frame.authoritative_program_source = std::string(phase_programs::kViewportProgramSource);
    frame.voxels.reserve(static_cast<std::size_t>(extent) * static_cast<std::size_t>(extent));

    std::array<double, 4> visual_accumulator {
        clamp01(viewport_frame.visual_rgba[0]),
        clamp01(viewport_frame.visual_rgba[1]),
        clamp01(viewport_frame.visual_rgba[2]),
        clamp01(viewport_frame.visual_rgba[3]),
    };
    std::array<double, 4> material_accumulator {0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> audio_accumulator {0.0, 0.0, 0.0, 0.0};
    double density_accumulator = 0.0;

    for (std::uint32_t z = 0; z < extent; ++z) {
        for (std::uint32_t y = 0; y < extent; ++y) {
            for (std::uint32_t x = 0; x < extent; ++x) {
                const double nx = ((static_cast<double>(x) + 0.5) / static_cast<double>(extent) * 2.0) - 1.0;
                const double ny = ((static_cast<double>(y) + 0.5) / static_cast<double>(extent) * 2.0) - 1.0;
                const double nz = ((static_cast<double>(z) + 0.5) / static_cast<double>(extent) * 2.0) - 1.0;
                const double radial = std::sqrt((nx * nx) + (ny * ny) + (nz * nz));

                const std::size_t index = static_cast<std::size_t>((x * 5U) + (y * 7U) + (z * 11U)) % saved_state.carrier_9d.size();
                const double carrier = clamp01((static_cast<double>(saved_state.carrier_9d[index]) * 0.5) + 0.5);
                const double resume = clamp01((static_cast<double>(saved_state.resume_trajectory_9d[index]) * 0.5) + 0.5);
                const double tensor_phase = clamp01(viewport_frame.tensor_signature_6d[index % viewport_frame.tensor_signature_6d.size()]);
                const double harmonic = 0.5 + (0.5 * std::sin(
                    (time_s * 0.75)
                    + (carrier * kPi * 4.0)
                    + (tensor_phase * kPi * 2.0)
                    + (nx * 5.2)
                    + (ny * 3.7)
                    + (nz * 4.4)));
                const double boundary_sharpness = clamp01(
                    std::abs(nx * ny) + std::abs(ny * nz) + std::abs(nz * nx)
                        + (0.24 * boundary_energy)
                        + (0.18 * clamp01(viewport_frame.sideband_energy_norm))
                );
                const double resonance = clamp01(
                    (0.28 * carrier)
                    + (0.16 * resume)
                    + (0.14 * saved_state.phase_magnitude)
                    + (0.16 * saved_state.zero_point_proximity)
                    + (0.14 * harmonic)
                    + (0.12 * clamp01(viewport_frame.audio_channels[3]))
                );
                const double density = clamp01(
                    (0.26 * resonance)
                    + (0.18 * coherence)
                    + (0.15 * flux)
                    + (0.14 * gate_readiness)
                    + (0.12 * (1.0 - radial))
                    + (0.08 * clamp01(viewport_frame.anchor_correlation))
                    + (0.07 * harmonic)
                    - (0.20 * radial)
                );

                if (density < 0.56) {
                    continue;
                }

                FieldVoxel voxel;
                voxel.position_xyz = {
                    static_cast<float>(nx),
                    static_cast<float>(ny),
                    static_cast<float>(nz),
                };
                voxel.density = static_cast<float>(density);
                voxel.size = 0.65f + static_cast<float>(density * 0.75);

                const float phase_tint = clamp01f(static_cast<float>(0.30 + (0.70 * clamp01(viewport_frame.visual_rgba[0] + (0.22 * carrier)))));
                const float flux_tint = clamp01f(static_cast<float>(0.24 + (0.76 * clamp01(viewport_frame.visual_rgba[1] + (0.22 * flux)))));
                const float emissive = clamp01f(static_cast<float>(viewport_frame.material_pbr[2] + (0.35 * saved_state.resonance_energy) + (0.12 * density)));
                const float roughness = clamp01f(static_cast<float>(viewport_frame.material_pbr[0] + (0.24 * temporal_noise) + (0.22 * boundary_sharpness)));
                const float metallic = clamp01f(static_cast<float>(viewport_frame.material_pbr[1] + (0.16 * coherence) + (0.12 * silicon_density_norm)));
                const float conductivity = clamp01f(static_cast<float>(viewport_frame.material_pbr[1] + (0.14 * saved_state.dynamic_range_headroom) + (0.18 * lattice_scale_norm)));

                voxel.material.base_color_rgba = {
                    clamp01f((static_cast<float>(viewport_frame.visual_rgba[0]) * 0.55f + (0.45f * phase_tint)) * (0.72f + (0.28f * silicon_density_norm))),
                    clamp01f((static_cast<float>(viewport_frame.visual_rgba[1]) * 0.55f + (0.45f * flux_tint)) * (0.74f + (0.26f * silicon_excitation_norm))),
                    clamp01f((static_cast<float>(viewport_frame.visual_rgba[2]) * 0.55f + (0.45f * emissive)) * (0.78f + (0.22f * lattice_scale_norm))),
                    clamp01f(static_cast<float>(0.45 + (0.50 * density))),
                };
                voxel.material.normal_xyz = normalize3({
                    static_cast<float>(nx + (0.35 * saved_state.phase_direction_xyz[0]) + (0.15 * viewport_frame.tensor_signature_6d[1])),
                    static_cast<float>(ny + (0.35 * saved_state.phase_direction_xyz[1]) + (0.10 * viewport_frame.tensor_signature_6d[4])),
                    static_cast<float>(nz + (0.35 * saved_state.phase_direction_xyz[2]) + (0.10 * viewport_frame.tensor_signature_6d[5])),
                });
                voxel.material.roughness = roughness;
                voxel.material.metallic = metallic;
                voxel.material.emissive = emissive;
                voxel.material.conductivity = conductivity;

                voxel.audio.channels = {
                    clamp01f(static_cast<float>(viewport_frame.audio_channels[0] * density)),
                    clamp01f(static_cast<float>(viewport_frame.audio_channels[1] * resonance)),
                    clamp01f(static_cast<float>(viewport_frame.audio_channels[2] * boundary_sharpness)),
                    clamp01f(static_cast<float>(viewport_frame.audio_channels[3] + (0.20 * emissive))),
                };
                voxel.audio.carrier_frequency_hz = static_cast<float>(
                    96.0
                    + (180.0 * clamp01(viewport_frame.texture_map_9d[0]))
                    + (140.0 * clamp01(viewport_frame.tensor_signature_6d[4]))
                    + (84.0 * clamp01(viewport_frame.audio_channels[3]))
                );
                voxel.audio.phase_progression = static_cast<float>(std::fmod(time_s + resonance + boundary_sharpness, 1.0));

                density_accumulator += density;
                visual_accumulator[0] += voxel.material.base_color_rgba[0] * density;
                visual_accumulator[1] += voxel.material.base_color_rgba[1] * density;
                visual_accumulator[2] += voxel.material.base_color_rgba[2] * density;
                visual_accumulator[3] += voxel.material.base_color_rgba[3] * density;
                material_accumulator[0] += roughness * density;
                material_accumulator[1] += metallic * density;
                material_accumulator[2] += emissive * density;
                material_accumulator[3] += conductivity * density;
                audio_accumulator[0] += voxel.audio.channels[0] * density;
                audio_accumulator[1] += voxel.audio.channels[1] * density;
                audio_accumulator[2] += voxel.audio.channels[2] * density;
                audio_accumulator[3] += voxel.audio.channels[3] * density;

                frame.voxels.push_back(voxel);
            }
        }
    }

    if (frame.voxels.empty()) {
        FieldVoxel voxel;
        voxel.position_xyz = {0.0f, 0.0f, 0.0f};
        voxel.density = 1.0f;
        voxel.size = 1.4f;
        voxel.material.base_color_rgba = {0.62f, 0.68f, 0.74f, 1.0f};
        voxel.material.normal_xyz = {0.0f, 0.0f, 1.0f};
        voxel.material.roughness = 0.32f;
        voxel.material.metallic = 0.74f;
        voxel.material.emissive = 0.24f;
        voxel.material.conductivity = 0.78f;
        voxel.audio.channels = {0.18f, 0.16f, 0.14f, 0.20f};
        voxel.audio.carrier_frequency_hz = 220.0f;
        voxel.audio.phase_progression = static_cast<float>(std::fmod(time_s, 1.0));
        frame.voxels.push_back(voxel);
        density_accumulator = 1.0;
        visual_accumulator = {
            clamp01(viewport_frame.visual_rgba[0]),
            clamp01(viewport_frame.visual_rgba[1]),
            clamp01(viewport_frame.visual_rgba[2]),
            1.0,
        };
        material_accumulator = {0.32, 0.74, 0.24, 0.78};
        audio_accumulator = {
            clamp01(viewport_frame.audio_channels[0]),
            clamp01(viewport_frame.audio_channels[1]),
            clamp01(viewport_frame.audio_channels[2]),
            clamp01(viewport_frame.audio_channels[3]),
        };
    }

    const double denom = std::max(density_accumulator, 1.0);
    frame.aggregate_visual_rgba = {
        static_cast<float>(visual_accumulator[0] / denom),
        static_cast<float>(visual_accumulator[1] / denom),
        static_cast<float>(visual_accumulator[2] / denom),
        static_cast<float>(visual_accumulator[3] / denom),
    };
    frame.aggregate_material_pbr = {
        static_cast<float>(material_accumulator[0] / denom),
        static_cast<float>(material_accumulator[1] / denom),
        static_cast<float>(material_accumulator[2] / denom),
        static_cast<float>(material_accumulator[3] / denom),
    };
    frame.aggregate_audio = {
        static_cast<float>(audio_accumulator[0] / denom),
        static_cast<float>(audio_accumulator[1] / denom),
        static_cast<float>(audio_accumulator[2] / denom),
        static_cast<float>(audio_accumulator[3] / denom),
    };

    return frame;
}

StereoPcmFrame synthesize_field_audio(
    const FieldViewportFrame& frame,
    std::size_t frame_count,
    std::uint32_t sample_rate_hz
) {
    StereoPcmFrame pcm;
    pcm.sample_rate_hz = sample_rate_hz == 0 ? 48000U : sample_rate_hz;
    pcm.interleaved_samples.resize(frame_count * 2U, 0);

    const double left_gain = clamp01(frame.aggregate_audio[0] + (0.32 * frame.aggregate_audio[2]));
    const double right_gain = clamp01(frame.aggregate_audio[1] + (0.32 * frame.aggregate_audio[3]));
    const double shimmer = clamp01(frame.aggregate_material_pbr[2] + (0.26 * frame.saved_state.resonance_energy));

    const double base_frequency_a = 96.0 + (180.0 * std::abs(frame.aggregate_audio[0])) + (90.0 * std::abs(frame.saved_state.carrier_9d[0]));
    const double base_frequency_b = 142.0 + (220.0 * std::abs(frame.aggregate_audio[1])) + (90.0 * std::abs(frame.saved_state.carrier_9d[4]));
    const double base_frequency_c = 210.0 + (260.0 * std::abs(frame.aggregate_audio[2])) + (70.0 * std::abs(frame.saved_state.carrier_9d[7]));
    const double base_frequency_d = 78.0 + (140.0 * std::abs(frame.aggregate_audio[3])) + (52.0 * frame.saved_state.phase_magnitude);

    for (std::size_t index = 0; index < frame_count; ++index) {
        const double t = frame.time_s + (static_cast<double>(index) / static_cast<double>(pcm.sample_rate_hz));
        const double carrier_a = std::sin((2.0 * kPi * base_frequency_a * t) + frame.saved_state.phase_lock_error);
        const double carrier_b = std::sin((2.0 * kPi * base_frequency_b * t) + frame.saved_state.zero_point_proximity);
        const double carrier_c = std::sin((2.0 * kPi * base_frequency_c * t) + frame.saved_state.relock_pressure);
        const double carrier_d = std::sin((2.0 * kPi * base_frequency_d * t) + frame.saved_state.anchor_correlation);

        const double left = std::clamp(
            (0.42 * left_gain * carrier_a)
            + (0.28 * shimmer * carrier_c)
            + (0.18 * frame.saved_state.phase_direction_xyz[0] * carrier_d),
            -1.0,
            1.0);
        const double right = std::clamp(
            (0.42 * right_gain * carrier_b)
            + (0.28 * shimmer * carrier_c)
            + (0.18 * frame.saved_state.phase_direction_xyz[1] * carrier_d),
            -1.0,
            1.0);

        pcm.interleaved_samples[(index * 2U)] = static_cast<std::int16_t>(left * 32767.0);
        pcm.interleaved_samples[(index * 2U) + 1U] = static_cast<std::int16_t>(right * 32767.0);
    }

    return pcm;
}

}  // namespace qbit_miner
