#include "qbit_miner/control_center/live_control_center.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "gpu_telemetry.h"
#include "qbit_miner/runtime/substrate_phase_programs.hpp"
#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/runtime/substrate_control_surface.hpp"
#include "qbit_miner/runtime/substrate_controller.hpp"
#include "qbit_miner/substrate/calibration_export.hpp"
#include "qbit_miner/substrate/device_validation_export.hpp"

namespace qbit_miner {

namespace {

constexpr std::size_t kMetricHistoryCapacity = 160U;
constexpr std::size_t kPendingKernelIterationCapacity = 64U;
constexpr double kBootstrapFallbackTimeoutS = 0.75;
constexpr double kDefaultRunDurationMinutes = 60.0;
constexpr double kTau = 6.28318530717958647692;
constexpr auto kTelemetryStreamInterval = std::chrono::milliseconds(20);
constexpr auto kTelemetryDrainInterval = std::chrono::milliseconds(10);

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double wrap01(double value) {
    const double wrapped = std::fmod(value, 1.0);
    return wrapped < 0.0 ? (wrapped + 1.0) : wrapped;
}

double current_unix_time_s() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

MetricHistorySeries make_metric_series(const char* metric_id, const char* label, MetricValueFormat format) {
    MetricHistorySeries series;
    series.metric_id = metric_id;
    series.label = label;
    series.format = format;
    return series;
}

MetricHistoryPanel make_metric_panel(
    const char* panel_id,
    const char* title,
    std::initializer_list<MetricHistorySeries> series
) {
    MetricHistoryPanel panel;
    panel.panel_id = panel_id;
    panel.title = title;
    panel.series.assign(series.begin(), series.end());
    return panel;
}

std::vector<MetricHistoryPanel> build_default_metric_panels() {
    return {
        make_metric_panel(
            "system",
            "System Metrics",
            {
                make_metric_series("graphics_frequency_ghz", "Graphics Clock", MetricValueFormat::FrequencyGHz),
                make_metric_series("memory_frequency_ghz", "Memory Clock", MetricValueFormat::FrequencyGHz),
                make_metric_series("power_w", "Board Power", MetricValueFormat::Power),
                make_metric_series("temperature_c", "GPU Temperature", MetricValueFormat::Temperature),
                make_metric_series("gpu_util_percent", "GPU Utilization", MetricValueFormat::Percent),
                make_metric_series("memory_util_percent", "Memory Utilization", MetricValueFormat::Percent),
            }
        ),
        make_metric_panel(
            "mining",
            "Mining Metrics",
            {
                make_metric_series("rig_hashrate_hs", "Rig Hashrate", MetricValueFormat::Hashrate),
                make_metric_series("submission_rate_per_s", "Submission Rate", MetricValueFormat::Rate),
                make_metric_series("acceptance_rate_percent", "Acceptance", MetricValueFormat::Percent),
                make_metric_series("share_valid_rate_percent", "Share Valid", MetricValueFormat::Percent),
                make_metric_series("rig_difficulty", "Rig Difficulty", MetricValueFormat::Difficulty),
                make_metric_series("coherence_percent", "Coherence", MetricValueFormat::Percent),
            }
        ),
        make_metric_panel(
            "network",
            "Network Metrics",
            {
                make_metric_series("pool_hashrate_hs", "Pool Hashrate", MetricValueFormat::Hashrate),
                make_metric_series("network_hashrate_hs", "Network Hashrate", MetricValueFormat::Hashrate),
                make_metric_series("pool_difficulty", "Pool Difficulty", MetricValueFormat::Difficulty),
                make_metric_series("network_difficulty", "Network Difficulty", MetricValueFormat::Difficulty),
                make_metric_series("worker_pool_share_percent", "Worker Pool Share", MetricValueFormat::Percent),
                make_metric_series("required_hashrate_hs", "Required Hashrate", MetricValueFormat::Hashrate),
            }
        ),
        make_metric_panel(
            "payout",
            "Payout Metrics",
            {
                make_metric_series("rig_reward_coin", "Rig Reward", MetricValueFormat::Coin),
                make_metric_series("rig_reward_usd", "Rig Reward Value", MetricValueFormat::Currency),
                make_metric_series("pool_reward_usd", "Pool Reward Value", MetricValueFormat::Currency),
                make_metric_series("network_reward_usd", "Network Reward Value", MetricValueFormat::Currency),
                make_metric_series("accepted_submits", "Accepted Shares", MetricValueFormat::Count),
                make_metric_series("valid_candidates", "Valid Candidates", MetricValueFormat::Count),
            }
        ),
    };
}

MetricHistorySeries* find_metric_series(
    std::vector<MetricHistoryPanel>& panels,
    const char* panel_id,
    const char* metric_id
) {
    for (auto& panel : panels) {
        if (panel.panel_id != panel_id) {
            continue;
        }
        for (auto& series : panel.series) {
            if (series.metric_id == metric_id) {
                return &series;
            }
        }
    }
    return nullptr;
}

void append_metric_sample(MetricHistorySeries& series, double value) {
    series.current_value = value;
    series.samples.push_back(value);
    if (series.samples.size() > kMetricHistoryCapacity) {
        series.samples.erase(series.samples.begin(), series.samples.begin() + static_cast<std::ptrdiff_t>(series.samples.size() - kMetricHistoryCapacity));
    }
}

void update_metric_panels(
    std::vector<MetricHistoryPanel>& panels,
    const MiningMetricsSnapshot& metrics,
    const MiningTelemetryObservation& observation,
    const std::optional<SubstrateStratumAuthorityState>& authority_state
) {
    if (panels.empty()) {
        panels = build_default_metric_panels();
    }

    const auto update_series = [&panels](const char* panel_id, const char* metric_id, double value) {
        if (MetricHistorySeries* series = find_metric_series(panels, panel_id, metric_id)) {
            append_metric_sample(*series, value);
        }
    };

    update_series("system", "graphics_frequency_ghz", observation.graphics_frequency_hz / 1.0e9);
    update_series("system", "memory_frequency_ghz", observation.memory_frequency_hz / 1.0e9);
    update_series("system", "power_w", observation.power_w);
    update_series("system", "temperature_c", observation.temperature_c);
    update_series("system", "gpu_util_percent", observation.gpu_util_norm * 100.0);
    update_series("system", "memory_util_percent", observation.memory_util_norm * 100.0);

    update_series("mining", "rig_hashrate_hs", metrics.rig.hashrate_hs);
    update_series("mining", "submission_rate_per_s", metrics.rig.submission_rate_per_s);
    update_series("mining", "acceptance_rate_percent", metrics.rig.submission_acceptance_rate * 100.0);
    update_series("mining", "share_valid_rate_percent", metrics.rig.share_valid_rate * 100.0);
    update_series("mining", "rig_difficulty", metrics.rig.difficulty);
    update_series("mining", "coherence_percent", metrics.rig.coherence * 100.0);

    update_series("network", "pool_hashrate_hs", metrics.pool.pool_hashrate_hs);
    update_series("network", "network_hashrate_hs", metrics.blockchain.network_hashrate_hs);
    update_series("network", "pool_difficulty", metrics.pool.pool_difficulty);
    update_series("network", "network_difficulty", metrics.blockchain.network_difficulty);
    update_series("network", "worker_pool_share_percent", metrics.pool.worker_pool_share_norm * 100.0);
    update_series(
        "network",
        "required_hashrate_hs",
        authority_state.has_value() ? authority_state->required_hashrate_hs : 0.0
    );

    update_series("payout", "rig_reward_coin", metrics.rig.reward.coin);
    update_series("payout", "rig_reward_usd", metrics.rig.reward.value_usd);
    update_series("payout", "pool_reward_usd", metrics.pool.reward.value_usd);
    update_series("payout", "network_reward_usd", metrics.blockchain.reward.value_usd);
    update_series(
        "payout",
        "accepted_submits",
        authority_state.has_value() ? static_cast<double>(authority_state->accepted_submit_count) : 0.0
    );
    update_series(
        "payout",
        "valid_candidates",
        authority_state.has_value() ? static_cast<double>(authority_state->offline_valid_submit_preview_count) : 0.0
    );
}

MiningTelemetryObservation to_observation(const GPUTelemetrySample& sample) {
    return MiningTelemetryObservation{
        sample.graphics_frequency_hz,
        sample.memory_frequency_hz,
        sample.amplitude_norm,
        sample.voltage_v,
        sample.amperage_a,
        sample.power_w,
        sample.temperature_c,
        sample.gpu_util_norm,
        sample.memory_util_norm,
        sample.thermal_interference_norm,
        sample.live,
        sample.provider,
    };
}

GpuFeedbackFrame build_harmonic_bootstrap_feedback_frame(
    const SubstrateStratumAuthorityState* authority_state,
    std::uint64_t tick_index
) {
    const double worker_norm = authority_state != nullptr
        ? clamp01(static_cast<double>(authority_state->active_worker_count)
            / static_cast<double>(std::max<std::size_t>(1U, kStratumWorkerSlotCount)))
        : 0.25;
    const double resonance_norm = authority_state != nullptr
        ? clamp01(std::max(authority_state->last_target_resonance_norm, authority_state->target_resonance_floor))
        : 0.72;
    const double difficulty_norm = authority_state != nullptr && authority_state->difficulty > 0.0
        ? clamp01(std::log10(authority_state->difficulty + 1.0) / 6.0)
        : 0.10;
    const double phase_seed = wrap01(
        (0.071 * static_cast<double>(tick_index))
        + (0.19 * worker_norm)
        + (0.23 * resonance_norm)
        + (0.17 * difficulty_norm));

    GpuFeedbackFrame frame;
    frame.photonic_identity.gpu_device_id = "rig-harmonic-bootstrap";
    frame.photonic_identity.coherence = 0.86 + (0.10 * resonance_norm);
    frame.photonic_identity.memory = 0.70 + (0.18 * worker_norm);
    frame.photonic_identity.nexus = 0.46 + (0.22 * resonance_norm);
    frame.photonic_identity.field_vector.amplitude = 0.18 + (0.24 * resonance_norm);
    frame.photonic_identity.field_vector.voltage = 0.34 + (0.28 * worker_norm);
    frame.photonic_identity.field_vector.current = 0.26 + (0.22 * difficulty_norm);
    frame.photonic_identity.field_vector.frequency = 0.20 + (0.34 * difficulty_norm);
    frame.photonic_identity.field_vector.phase = phase_seed;
    frame.photonic_identity.field_vector.flux = 0.22 + (0.30 * resonance_norm);
    frame.photonic_identity.field_vector.thermal_noise = 0.03 + (0.04 * (1.0 - resonance_norm));
    frame.photonic_identity.field_vector.field_noise = 0.02 + (0.03 * difficulty_norm);
    frame.photonic_identity.spin_inertia.axis_spin = {
        0.14 + (0.10 * worker_norm),
        0.09 + (0.08 * resonance_norm),
        -0.07 - (0.05 * difficulty_norm),
    };
    frame.photonic_identity.spin_inertia.axis_orientation = {
        0.11 + (0.06 * resonance_norm),
        0.08 + (0.05 * worker_norm),
        -0.05 - (0.04 * difficulty_norm),
    };
    frame.photonic_identity.spin_inertia.momentum_score = 0.20 + (0.18 * resonance_norm);
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = 0.16 + (0.12 * difficulty_norm);
    frame.photonic_identity.spin_inertia.relativistic_correlation = 0.06 + (0.10 * worker_norm);
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = 0.58 + (0.24 * resonance_norm);
    frame.photonic_identity.spin_inertia.temporal_coupling_count =
        4U + static_cast<std::uint32_t>(std::llround(worker_norm * 4.0));
    frame.timing.tick_index = tick_index;
    frame.timing.request_time_ms = static_cast<double>(tick_index);
    frame.timing.response_time_ms = frame.timing.request_time_ms + 1.2;
    frame.timing.encode_deadline_ms = frame.timing.response_time_ms + 1.4;
    frame.integrated_feedback = 0.40 + (0.22 * resonance_norm);
    frame.derivative_signal = 0.02 + (0.03 * difficulty_norm);
    frame.lattice_closure = 0.88 + (0.08 * resonance_norm);
    frame.phase_closure = 0.84 + (0.10 * resonance_norm);
    frame.recurrence_alignment = 0.76 + (0.16 * worker_norm);
    frame.conservation_alignment = 0.97 + (0.02 * resonance_norm);
    return frame;
}

GpuFeedbackFrame build_feedback_frame_from_mining_validation(
    const MiningValidationSnapshot& validation,
    std::uint64_t tick_index
) {
    const double same_pulse = clamp01(validation.same_pulse_validation_norm);
    const double candidate_surface = clamp01(validation.candidate_surface_norm);
    const double validation_structure = clamp01(validation.validation_structure_norm);
    const double pool_ingest = clamp01(validation.pool_ingest_vector_norm);
    const double pool_submit = clamp01(validation.pool_submit_vector_norm);
    const double target_sequence_phase = clamp01(validation.target_sequence_phase_turns);
    const double target_sequence_frequency = clamp01(validation.target_sequence_frequency_norm);
    const double target_repeat_flux = clamp01(validation.target_repeat_flux_norm);
    const double reverse_observer = clamp01(validation.reverse_observer_collapse_norm);
    const double spider_frequency = clamp01(validation.spider_code_frequency_norm);
    const double spider_amplitude = clamp01(validation.spider_code_amplitude_norm);
    const double spider_voltage = clamp01(validation.spider_code_voltage_norm);
    const double spider_amperage = clamp01(validation.spider_code_amperage_norm);
    const double spider_projection = clamp01(validation.spider_projection_coherence_norm);
    const double spider_gate = clamp01(validation.spider_harmonic_gate_norm);
    const double spider_noise_sink = clamp01(validation.spider_noise_sink_norm);
    const double frontier_budget = clamp01(validation.frontier_activation_budget_norm);
    const double cumulative_budget = clamp01(validation.cumulative_activation_budget_norm);
    const double pulse_density = clamp01(validation.pulse_operator_density_norm);
    const double nested_resonance = clamp01(validation.nested_fourier_resonance_norm);
    const double target_phase_alignment = clamp01(validation.target_phase_alignment_norm);
    const double header_phase_alignment = clamp01(validation.header_phase_alignment_norm);
    const double nonce_phase_alignment = clamp01(validation.nonce_phase_alignment_norm);
    const double sha_phase_alignment = clamp01(validation.sha_phase_alignment_norm);
    const double validation_certainty = clamp01(validation.validation_certainty_norm);
    const double accepted_lane_fraction = clamp01(validation.accepted_lane_fraction);
    const double selected_lane_phase = clamp01(validation.selected_lane_phase_turns);
    const double worker_norm = clamp01(
        static_cast<double>(validation.active_worker_count)
        / static_cast<double>(std::max<std::size_t>(kStratumWorkerSlotCount, 1U)));
    const double phase_seed = wrap01(
        (0.071 * static_cast<double>(tick_index))
        + (0.24 * selected_lane_phase)
        + (0.18 * target_sequence_phase)
        + (0.20 * validation_certainty)
        + (0.12 * reverse_observer)
        + (0.09 * spider_frequency));

    GpuFeedbackFrame frame;
    frame.photonic_identity.source_identity = "gpu-mining-validation";
    frame.photonic_identity.gpu_device_id = "rig-phase-substrate";
    frame.photonic_identity.coherence = clamp01(
        (0.24 * validation_structure)
        + (0.22 * validation_certainty)
        + (0.18 * same_pulse)
        + (0.14 * sha_phase_alignment)
        + (0.12 * target_phase_alignment)
        + (0.05 * header_phase_alignment)
        + (0.05 * spider_projection));
    frame.photonic_identity.memory = clamp01(
        (0.20 * candidate_surface)
        + (0.18 * pool_ingest)
        + (0.18 * pool_submit)
        + (0.16 * target_sequence_frequency)
        + (0.14 * accepted_lane_fraction)
        + (0.08 * worker_norm)
        + (0.06 * spider_gate));
    frame.photonic_identity.nexus = clamp01(
        (0.18 * same_pulse)
        + (0.18 * validation_certainty)
        + (0.16 * reverse_observer)
        + (0.14 * pool_submit)
        + (0.12 * target_sequence_frequency)
        + (0.12 * nonce_phase_alignment)
        + (0.06 * sha_phase_alignment)
        + (0.04 * spider_voltage));
    frame.photonic_identity.field_vector.amplitude = clamp01(
        (0.28 * same_pulse)
        + (0.22 * validation_structure)
        + (0.16 * validation_certainty)
        + (0.12 * accepted_lane_fraction)
        + (0.12 * reverse_observer)
        + (0.06 * target_repeat_flux)
        + (0.04 * spider_amplitude));
    frame.photonic_identity.field_vector.voltage = clamp01(
        (0.26 * pool_submit)
        + (0.18 * validation_certainty)
        + (0.16 * reverse_observer)
        + (0.12 * pool_ingest)
        + (0.14 * accepted_lane_fraction)
        + (0.10 * target_phase_alignment)
        + (0.04 * spider_voltage));
    frame.photonic_identity.field_vector.current = clamp01(
        (0.26 * candidate_surface)
        + (0.18 * same_pulse)
        + (0.14 * target_repeat_flux)
        + (0.14 * nonce_phase_alignment)
        + (0.14 * sha_phase_alignment)
        + (0.10 * worker_norm)
        + (0.04 * spider_amperage));
    frame.photonic_identity.field_vector.frequency = clamp01(
        (0.22 * spider_frequency)
        + (0.20 * pool_ingest)
        + (0.16 * pool_submit)
        + (0.14 * validation_certainty)
        + (0.14 * sha_phase_alignment)
        + (0.08 * target_phase_alignment)
        + (0.06 * pulse_density));
    frame.photonic_identity.field_vector.phase = phase_seed;
    frame.photonic_identity.field_vector.flux = clamp01(
        (0.24 * candidate_surface)
        + (0.18 * target_repeat_flux)
        + (0.18 * reverse_observer)
        + (0.14 * accepted_lane_fraction)
        + (0.14 * pool_submit)
        + (0.08 * same_pulse)
        + (0.06 * spider_gate));
    frame.photonic_identity.field_vector.thermal_noise = clamp01(
        0.02
        + (0.06 * (1.0 - validation_certainty))
        + (0.04 * (1.0 - same_pulse)));
    frame.photonic_identity.field_vector.field_noise = clamp01(
        0.02
        + (0.06 * (1.0 - validation_structure))
        + (0.04 * (1.0 - accepted_lane_fraction)));
    frame.photonic_identity.spin_inertia.axis_spin = {
        0.12 + (0.20 * std::sin(kTau * target_sequence_phase)),
        0.08 + (0.18 * std::sin(kTau * selected_lane_phase)),
        -0.06 + (0.16 * std::cos(kTau * phase_seed)),
    };
    frame.photonic_identity.spin_inertia.axis_orientation = {
        0.10 + (0.16 * target_phase_alignment),
        0.08 + (0.16 * nonce_phase_alignment),
        -0.04 + (0.20 * sha_phase_alignment),
    };
    frame.photonic_identity.spin_inertia.momentum_score = clamp01(
        (0.22 * candidate_surface)
        + (0.20 * same_pulse)
        + (0.18 * target_repeat_flux)
        + (0.20 * reverse_observer)
        + (0.14 * accepted_lane_fraction)
        + (0.06 * spider_amperage));
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = clamp01(
        (0.26 * worker_norm)
        + (0.22 * pool_ingest)
        + (0.20 * pool_submit)
        + (0.16 * validation_structure)
        + (0.10 * accepted_lane_fraction)
        + (0.06 * pulse_density));
    frame.photonic_identity.spin_inertia.relativistic_correlation = clamp01(
        (0.22 * target_phase_alignment)
        + (0.20 * header_phase_alignment)
        + (0.20 * nonce_phase_alignment)
        + (0.20 * sha_phase_alignment)
        + (0.12 * validation_certainty)
        + (0.06 * spider_projection));
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = clamp01(
        (0.22 * same_pulse)
        + (0.18 * validation_structure)
        + (0.18 * reverse_observer)
        + (0.14 * target_sequence_frequency)
        + (0.14 * pool_submit)
        + (0.08 * accepted_lane_fraction)
        + (0.06 * spider_gate));
    frame.photonic_identity.spin_inertia.temporal_coupling_count = std::max<std::uint32_t>(validation.active_worker_count, 1U);
    frame.photonic_identity.spectra_9d = {
        target_sequence_phase,
        selected_lane_phase,
        spider_frequency,
        reverse_observer,
        same_pulse,
        validation_structure,
        clamp01((pool_ingest + spider_gate) * 0.5),
        clamp01((pool_submit + spider_voltage) * 0.5),
        clamp01((validation_certainty + spider_projection) * 0.5),
    };
    frame.timing.tick_index = tick_index;
    frame.timing.request_time_ms = static_cast<double>(tick_index);
    frame.timing.response_time_ms = frame.timing.request_time_ms + 0.8;
    frame.timing.accounting_time_ms = frame.timing.response_time_ms + 0.9;
    frame.timing.next_feedback_time_ms = frame.timing.accounting_time_ms + 1.1;
    frame.timing.closed_loop_latency_ms = 2.8;
    frame.timing.encode_deadline_ms = frame.timing.closed_loop_latency_ms + 1.0;
    frame.sent_signal = clamp01((0.24 * candidate_surface) + (0.22 * same_pulse) + (0.16 * pool_submit) + (0.14 * validation_certainty) + (0.12 * reverse_observer) + (0.06 * spider_amplitude) + (0.06 * spider_frequency));
    frame.measured_signal = clamp01((0.22 * validation_structure) + (0.20 * accepted_lane_fraction) + (0.16 * target_phase_alignment) + (0.16 * nonce_phase_alignment) + (0.16 * sha_phase_alignment) + (0.05 * spider_projection) + (0.05 * spider_gate));
    frame.integrated_feedback = clamp01((0.22 * validation_certainty) + (0.20 * same_pulse) + (0.16 * pool_submit) + (0.16 * reverse_observer) + (0.14 * accepted_lane_fraction) + (0.06 * spider_voltage) + (0.06 * spider_amperage));
    frame.derivative_signal = clamp01((0.16 * std::abs(target_sequence_phase - selected_lane_phase)) + (0.16 * std::abs(target_phase_alignment - nonce_phase_alignment)) + (0.16 * (1.0 - validation_certainty)) + (0.14 * (1.0 - same_pulse)) + (0.12 * target_repeat_flux) + (0.12 * (1.0 - accepted_lane_fraction)) + (0.07 * spider_noise_sink) + (0.07 * (1.0 - spider_projection)));
    frame.lattice_closure = clamp01((0.20 * validation_structure) + (0.18 * same_pulse) + (0.16 * candidate_surface) + (0.18 * pool_ingest) + (0.16 * reverse_observer) + (0.06 * spider_gate) + (0.06 * spider_projection));
    frame.phase_closure = clamp01((0.22 * target_phase_alignment) + (0.18 * header_phase_alignment) + (0.18 * nonce_phase_alignment) + (0.22 * sha_phase_alignment) + (0.20 * validation_certainty));
    frame.recurrence_alignment = clamp01((0.20 * spider_frequency) + (0.18 * target_repeat_flux) + (0.16 * reverse_observer) + (0.16 * same_pulse) + (0.16 * pool_submit) + (0.07 * spider_amplitude) + (0.07 * spider_projection));
    frame.conservation_alignment = clamp01((0.20 * validation_certainty) + (0.20 * accepted_lane_fraction) + (0.16 * same_pulse) + (0.16 * pool_submit) + (0.16 * reverse_observer) + (0.06 * spider_gate) + (0.06 * (1.0 - spider_noise_sink)));
    return frame;
}

MiningTelemetryObservation to_observation(const GpuKernelTelemetrySample& sample) {
    return MiningTelemetryObservation{
        sample.graphics_frequency_hz,
        sample.memory_frequency_hz,
        sample.amplitude_norm,
        sample.voltage_v,
        sample.amperage_a,
        sample.power_w,
        sample.temperature_c,
        sample.gpu_util_norm,
        sample.memory_util_norm,
        sample.thermal_interference_norm,
        sample.live,
        sample.provider,
    };
}

GpuKernelTelemetrySample to_kernel_sample(const GPUTelemetrySample& sample) {
    return GpuKernelTelemetrySample{
        sample.telemetry_sequence,
        sample.sample_window_start_s,
        sample.sample_window_end_s,
        sample.timestamp_s,
        sample.delta_s,
        sample.graphics_frequency_hz,
        sample.memory_frequency_hz,
        sample.amplitude_norm,
        sample.voltage_v,
        sample.amperage_a,
        sample.power_w,
        sample.temperature_c,
        sample.gpu_util_norm,
        sample.memory_util_norm,
        sample.thermal_interference_norm,
        sample.live,
        sample.provider,
    };
}

GpuFeedbackFrame build_feedback_frame(const GPUTelemetrySample& sample, std::uint64_t tick_index) {
    const double phase_seed = std::fmod(sample.timestamp_s * 0.17, 1.0);

    GpuFeedbackFrame frame;
    frame.photonic_identity.source_identity = sample.live ? "live-control-center" : "deterministic-control-center";
    frame.photonic_identity.gpu_device_id = "rig-control-center-01";
    frame.photonic_identity.coherence = clamp01(
        (0.42 * sample.gpu_util_norm)
        + (0.28 * sample.amplitude_norm)
        + (0.30 * (1.0 - sample.thermal_interference_norm))
    );
    frame.photonic_identity.memory = clamp01(sample.memory_util_norm);
    frame.photonic_identity.nexus = clamp01(
        (0.40 * sample.gpu_util_norm)
        + (0.30 * sample.memory_util_norm)
        + (0.30 * sample.amplitude_norm)
    );
    frame.photonic_identity.observed_latency_ms = std::max(2.0, sample.delta_s * 1000.0);
    frame.photonic_identity.field_vector.amplitude = clamp01(sample.amplitude_norm);
    frame.photonic_identity.field_vector.voltage = sample.voltage_v;
    frame.photonic_identity.field_vector.current = sample.amperage_a;
    frame.photonic_identity.field_vector.frequency = clamp01(sample.graphics_frequency_hz / 2500000000.0);
    frame.photonic_identity.field_vector.phase = std::fmod(phase_seed + (0.25 * sample.amplitude_norm), 1.0);
    frame.photonic_identity.field_vector.flux = clamp01(sample.power_w / 350.0);
    frame.photonic_identity.field_vector.thermal_noise = clamp01(sample.thermal_interference_norm);
    frame.photonic_identity.field_vector.field_noise = clamp01(1.0 - frame.photonic_identity.coherence);
    frame.photonic_identity.spin_inertia.axis_spin = {
        0.18 + (0.22 * std::sin(sample.timestamp_s * 0.91)),
        0.14 + (0.18 * std::sin(sample.timestamp_s * 0.64)),
        -0.12 + (0.20 * std::cos(sample.timestamp_s * 0.58)),
    };
    frame.photonic_identity.spin_inertia.axis_orientation = {
        0.10 + (0.12 * std::cos(sample.timestamp_s * 0.47)),
        0.08 + (0.10 * std::sin(sample.timestamp_s * 0.42)),
        -0.09 + (0.11 * std::cos(sample.timestamp_s * 0.35)),
    };
    frame.photonic_identity.spin_inertia.momentum_score = clamp01(0.16 + (0.52 * sample.gpu_util_norm));
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = clamp01(0.12 + (0.58 * sample.memory_util_norm));
    frame.photonic_identity.spin_inertia.relativistic_correlation = clamp01(0.08 + (0.30 * sample.amplitude_norm));
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = clamp01(
        0.45 + (0.35 * sample.gpu_util_norm) + (0.20 * sample.memory_util_norm)
    );
    frame.photonic_identity.spin_inertia.temporal_coupling_count = static_cast<std::uint32_t>(
        4U + std::lround((sample.gpu_util_norm + sample.memory_util_norm) * 4.0)
    );

    frame.timing.tick_index = tick_index;
    frame.timing.request_time_ms = sample.timestamp_s * 1000.0;
    frame.timing.response_time_ms = frame.timing.request_time_ms + std::max(0.25, sample.delta_s * 500.0);
    frame.timing.accounting_time_ms = frame.timing.response_time_ms + std::max(0.4, sample.delta_s * 250.0);
    frame.timing.next_feedback_time_ms = frame.timing.accounting_time_ms + std::max(8.0, sample.delta_s * 1000.0);
    frame.timing.closed_loop_latency_ms = std::max(24.0, sample.delta_s * 1000.0);
    frame.timing.encode_deadline_ms = frame.timing.closed_loop_latency_ms + 2.0;
    frame.gpu_kernel_sync.kernel_iteration = tick_index;
    frame.gpu_kernel_sync.kernel_name = "telemetry-heartbeat";
    frame.gpu_kernel_sync.kernel_phase = GpuKernelIterationPhase::Completion;
    frame.gpu_kernel_sync.telemetry_sequence = sample.telemetry_sequence;
    frame.gpu_kernel_sync.telemetry_timestamp_s = sample.timestamp_s;
    frame.gpu_kernel_sync.telemetry_skew_s = 0.0;
    frame.gpu_kernel_sync.kernel_launch_time_s = sample.sample_window_start_s;
    frame.gpu_kernel_sync.kernel_completion_time_s = sample.sample_window_end_s;
    frame.gpu_kernel_sync.exact_telemetry_match = false;
    frame.encodable_node_count = static_cast<std::uint32_t>(8U + std::lround(sample.memory_util_norm * 10.0));
    frame.sent_signal = clamp01((0.55 * sample.gpu_util_norm) + (0.45 * sample.amplitude_norm));
    frame.measured_signal = clamp01((0.50 * sample.memory_util_norm) + (0.50 * sample.amplitude_norm));
    frame.integrated_feedback = clamp01((0.62 * sample.gpu_util_norm) + (0.38 * sample.memory_util_norm));
    frame.derivative_signal = clamp01(std::abs(sample.gpu_util_norm - sample.memory_util_norm));
    frame.lattice_closure = clamp01((0.55 * frame.photonic_identity.coherence) + (0.45 * frame.photonic_identity.memory));
    frame.phase_closure = clamp01((0.60 * frame.photonic_identity.coherence) + (0.40 * frame.photonic_identity.field_vector.phase));
    frame.recurrence_alignment = clamp01((0.45 * frame.integrated_feedback) + (0.55 * (1.0 - sample.thermal_interference_norm)));
    frame.conservation_alignment = clamp01((0.50 * frame.lattice_closure) + (0.50 * frame.phase_closure));
    return frame;
}

void apply_mining_phase_encoding(
    GpuFeedbackFrame& frame,
    const MiningPhaseEncodingState& mining_phase_encoding
) {
    if (!mining_phase_encoding.active) {
        return;
    }

    const double target_phase = clamp01(mining_phase_encoding.share_target_phase_turns);
    const double header_phase = clamp01(mining_phase_encoding.header_phase_turns);
    const double nonce_phase = clamp01(mining_phase_encoding.nonce_origin_phase_turns);
    const double target_resonance = clamp01(mining_phase_encoding.target_resonance_norm);
    const double sha_schedule_phase = clamp01(mining_phase_encoding.sha256_schedule_phase_turns);
    const double sha_round_phase = clamp01(mining_phase_encoding.sha256_round_phase_turns);
    const double sha_digest_phase = clamp01(mining_phase_encoding.sha256_digest_phase_turns);
    const double sha_frequency_bias = clamp01(mining_phase_encoding.sha256_frequency_bias_norm);
    const double sha_harmonic_density = clamp01(mining_phase_encoding.sha256_harmonic_density_norm);
    const double target_frequency = clamp01(mining_phase_encoding.target_frequency_norm);
    const double target_sequence_phase = clamp01(mining_phase_encoding.target_sequence_phase_turns);
    const double target_sequence_frequency = clamp01(mining_phase_encoding.target_sequence_frequency_norm);
    const double target_repeat_flux = clamp01(mining_phase_encoding.target_repeat_flux_norm);
    const double reverse_observer_collapse = clamp01(mining_phase_encoding.reverse_observer_collapse_norm);
    const double spider_frequency = clamp01(mining_phase_encoding.spider_code_frequency_norm);
    const double spider_amplitude = clamp01(mining_phase_encoding.spider_code_amplitude_norm);
    const double spider_voltage = clamp01(mining_phase_encoding.spider_code_voltage_norm);
    const double spider_amperage = clamp01(mining_phase_encoding.spider_code_amperage_norm);
    const double spider_projection = clamp01(mining_phase_encoding.spider_projection_coherence_norm);
    const double spider_gate = clamp01(mining_phase_encoding.spider_harmonic_gate_norm);
    const double spider_noise_sink = clamp01(mining_phase_encoding.spider_noise_sink_norm);
    const double frontier_activation_budget = clamp01(mining_phase_encoding.frontier_activation_budget_norm);
    const double cumulative_activation_budget = clamp01(mining_phase_encoding.cumulative_activation_budget_norm);
    const double pulse_operator_density = clamp01(mining_phase_encoding.pulse_operator_density_norm);
    const double nested_fourier_resonance = clamp01(mining_phase_encoding.nested_fourier_resonance_norm);
    const double pool_ingest_vector = clamp01(mining_phase_encoding.pool_ingest_vector_norm);
    const double pool_submit_vector = clamp01(mining_phase_encoding.pool_submit_vector_norm);
    const double phase_pressure = clamp01(mining_phase_encoding.phase_pressure_norm);
    const double transfer_drive = clamp01(mining_phase_encoding.transfer_drive_norm);
    const double stability_gate = clamp01(mining_phase_encoding.stability_gate_norm);
    const double damping = clamp01(mining_phase_encoding.damping_norm);
    const double transport_drive = clamp01(mining_phase_encoding.transport_drive_norm);
    const double worker_parallelism = clamp01(mining_phase_encoding.worker_parallelism_norm);
    const double lane_coherence = clamp01(mining_phase_encoding.lane_coherence_norm);
    const double resonance_activation = clamp01(mining_phase_encoding.resonance_activation_norm);
    const double phase_flux_conservation = clamp01(mining_phase_encoding.phase_flux_conservation_norm);
    const double nonce_collapse_confidence = clamp01(mining_phase_encoding.nonce_collapse_confidence_norm);
    const double observer_collapse_strength = clamp01(mining_phase_encoding.observer_collapse_strength_norm);
    const double temporal_admissibility = clamp01(mining_phase_encoding.temporal_admissibility_norm);
    const double zero_point_proximity = clamp01(mining_phase_encoding.zero_point_proximity_norm);
    const double transport_readiness = clamp01(mining_phase_encoding.transport_readiness_norm);
    const double share_confidence = clamp01(mining_phase_encoding.share_confidence_norm);
    const double validation_rate = clamp01(mining_phase_encoding.validation_rate_norm);
    const double submit_ready = mining_phase_encoding.submit_path_ready ? 1.0 : 0.35;
    const double encoded_phase = std::fmod(
        (0.24 * target_phase)
        + (0.18 * header_phase)
        + (0.12 * nonce_phase)
        + (0.08 * target_sequence_phase)
        + (0.16 * sha_schedule_phase)
        + (0.16 * sha_round_phase)
        + (0.08 * sha_digest_phase)
        + (0.08 * phase_flux_conservation)
        + (0.05 * reverse_observer_collapse)
        + (0.06 * nonce_collapse_confidence)
        + (0.05 * worker_parallelism)
        + (0.05 * spider_frequency)
        + (0.05 * spider_projection),
        1.0);

    frame.photonic_identity.field_vector.frequency = clamp01(
        (0.28 * frame.photonic_identity.field_vector.frequency)
        + (0.20 * spider_frequency)
        + (0.12 * target_frequency)
        + (0.14 * target_sequence_frequency)
        + (0.18 * sha_frequency_bias)
        + (0.16 * sha_harmonic_density)
        + (0.10 * transport_drive)
        + (0.08 * resonance_activation)
        + (0.06 * encoded_phase)
        + (0.06 * spider_gate));
    frame.photonic_identity.field_vector.phase = encoded_phase < 0.0 ? (encoded_phase + 1.0) : encoded_phase;
    frame.photonic_identity.field_vector.amplitude = clamp01(
        (0.24 * frame.photonic_identity.field_vector.amplitude)
        + (0.22 * phase_pressure)
        + (0.18 * lane_coherence)
        + (0.18 * target_resonance)
        + (0.10 * target_repeat_flux)
        + (0.10 * transfer_drive)
        + (0.10 * stability_gate)
        + (0.08 * reverse_observer_collapse)
        + (0.08 * phase_flux_conservation)
        + (0.06 * spider_amplitude)
        + (0.06 * spider_projection)
        - (0.06 * spider_noise_sink));
    frame.photonic_identity.field_vector.current = clamp01(
        (0.24 * frame.photonic_identity.field_vector.current)
        + (0.26 * phase_pressure)
        + (0.18 * worker_parallelism)
        + (0.12 * pool_ingest_vector)
        + (0.14 * sha_round_phase)
        + (0.12 * transfer_drive)
        + (0.10 * transport_drive)
        + (0.08 * target_resonance)
        + (0.06 * spider_amperage));
    frame.photonic_identity.field_vector.voltage = clamp01(
        (0.26 * frame.photonic_identity.field_vector.voltage)
        + (0.22 * target_frequency)
        + (0.12 * pool_submit_vector)
        + (0.16 * sha_frequency_bias)
        + (0.18 * target_resonance)
        + (0.08 * resonance_activation)
        + (0.10 * observer_collapse_strength)
        + (0.08 * submit_ready)
        + (0.06 * spider_voltage));
    frame.photonic_identity.field_vector.flux = clamp01(
        (0.22 * frame.photonic_identity.field_vector.flux)
        + (0.22 * phase_pressure)
        + (0.18 * worker_parallelism)
        + (0.08 * target_repeat_flux)
        + (0.18 * sha_harmonic_density)
        + (0.10 * phase_flux_conservation)
        + (0.10 * transport_readiness)
        + (0.08 * target_resonance)
        + (0.06 * spider_gate));
    frame.photonic_identity.coherence = clamp01(
        (0.20 * frame.photonic_identity.coherence)
        + (0.22 * lane_coherence)
        + (0.18 * target_resonance)
        + (0.12 * reverse_observer_collapse)
        + (0.16 * sha_frequency_bias)
        + (0.10 * phase_flux_conservation)
        + (0.08 * nonce_collapse_confidence)
        + (0.06 * spider_projection)
        + (0.10 * submit_ready)
        + (0.04 * spider_gate));
    frame.photonic_identity.memory = clamp01(
        (0.24 * frame.photonic_identity.memory)
        + (0.20 * worker_parallelism)
        + (0.18 * target_frequency)
        + (0.10 * pool_ingest_vector)
        + (0.20 * sha_schedule_phase)
        + (0.08 * validation_rate)
        + (0.10 * share_confidence)
        + (0.08 * sha_round_phase)
        + (0.06 * spider_frequency));
    frame.photonic_identity.nexus = clamp01(
        (0.18 * frame.photonic_identity.nexus)
        + (0.20 * phase_pressure)
        + (0.18 * lane_coherence)
        + (0.20 * target_resonance)
        + (0.10 * resonance_activation)
        + (0.08 * reverse_observer_collapse)
        + (0.08 * observer_collapse_strength)
        + (0.12 * sha_frequency_bias)
        + (0.10 * sha_harmonic_density)
        + (0.04 * spider_voltage));
    frame.photonic_identity.spin_inertia.axis_spin = {
        (0.55 * frame.photonic_identity.spin_inertia.axis_spin[0]) + (0.45 * mining_phase_encoding.target_direction_xyz[0]),
        (0.55 * frame.photonic_identity.spin_inertia.axis_spin[1]) + (0.45 * mining_phase_encoding.target_direction_xyz[1]),
        (0.45 * frame.photonic_identity.spin_inertia.axis_spin[2]) + (0.55 * mining_phase_encoding.target_direction_xyz[2]),
    };
    frame.photonic_identity.spin_inertia.axis_orientation = {
        (0.50 * frame.photonic_identity.spin_inertia.axis_orientation[0]) + (0.50 * mining_phase_encoding.target_direction_xyz[0]),
        (0.50 * frame.photonic_identity.spin_inertia.axis_orientation[1]) + (0.50 * mining_phase_encoding.target_direction_xyz[1]),
        (0.42 * frame.photonic_identity.spin_inertia.axis_orientation[2]) + (0.58 * mining_phase_encoding.target_direction_xyz[2]),
    };
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = clamp01(
        (0.28 * frame.photonic_identity.spin_inertia.relative_temporal_coupling)
        + (0.18 * lane_coherence)
        + (0.18 * target_resonance)
        + (0.12 * temporal_admissibility)
        + (0.10 * zero_point_proximity)
        + (0.18 * sha_frequency_bias)
        + (0.12 * submit_ready)
        + (0.04 * spider_projection));
    frame.photonic_identity.spin_inertia.temporal_coupling_count = std::max<std::uint32_t>(
        frame.photonic_identity.spin_inertia.temporal_coupling_count,
        mining_phase_encoding.target_lane_count);
    frame.photonic_identity.spectra_9d = {
        target_phase,
        header_phase,
        target_sequence_phase,
        sha_schedule_phase,
        sha_round_phase,
        clamp01((phase_flux_conservation + spider_gate) * 0.5),
        clamp01((nonce_collapse_confidence + spider_amplitude) * 0.5),
        clamp01((resonance_activation + spider_projection) * 0.5),
        clamp01((reverse_observer_collapse + spider_voltage) * 0.5),
    };
    frame.sent_signal = clamp01((0.26 * frame.sent_signal) + (0.20 * phase_pressure) + (0.14 * transfer_drive) + (0.14 * target_resonance) + (0.12 * resonance_activation) + (0.07 * spider_frequency) + (0.07 * spider_amplitude));
    frame.measured_signal = clamp01((0.22 * frame.measured_signal) + (0.16 * lane_coherence) + (0.16 * phase_flux_conservation) + (0.16 * nonce_collapse_confidence) + (0.10 * reverse_observer_collapse) + (0.12 * transport_readiness) + (0.06 * spider_projection) + (0.06 * spider_gate));
    frame.integrated_feedback = clamp01((0.18 * frame.integrated_feedback) + (0.16 * phase_pressure) + (0.14 * worker_parallelism) + (0.10 * transport_drive) + (0.10 * resonance_activation) + (0.10 * observer_collapse_strength) + (0.10 * validation_rate) + (0.08 * pool_submit_vector) + (0.04 * spider_voltage));
    frame.derivative_signal = clamp01((0.18 * frame.derivative_signal) + (0.14 * std::abs(target_phase - frame.photonic_identity.field_vector.phase)) + (0.12 * std::abs(sha_round_phase - sha_schedule_phase)) + (0.12 * std::abs(sha_digest_phase - target_phase)) + (0.10 * spider_noise_sink) + (0.10 * (1.0 - target_resonance)) + (0.10 * (1.0 - phase_flux_conservation)) + (0.07 * (1.0 - spider_amplitude)) + (0.07 * (1.0 - spider_projection)));
    frame.phase_closure = clamp01((0.16 * frame.phase_closure) + (0.16 * lane_coherence) + (0.14 * target_resonance) + (0.14 * phase_flux_conservation) + (0.12 * reverse_observer_collapse) + (0.14 * resonance_activation) + (0.12 * transport_readiness) + (0.14 * sha_frequency_bias));
    frame.conservation_alignment = clamp01((0.14 * frame.conservation_alignment) + (0.14 * submit_ready) + (0.12 * target_resonance) + (0.14 * phase_flux_conservation) + (0.10 * pool_submit_vector) + (0.12 * stability_gate) + (0.10 * share_confidence) + (0.12 * validation_rate) + (0.06 * spider_gate) + (0.06 * (1.0 - spider_noise_sink)));
}

double kernel_reference_time_s(const GpuKernelIterationEvent& event) {
    if (event.driver_timing_valid) {
        if (event.kernel_phase == GpuKernelIterationPhase::Launch && event.gpu_launch_timestamp_s > 0.0) {
            return event.gpu_launch_timestamp_s;
        }
        if (event.gpu_completion_timestamp_s > 0.0) {
            return event.gpu_completion_timestamp_s;
        }
    }
    if (event.kernel_phase == GpuKernelIterationPhase::Launch) {
        return event.launch_timestamp_s;
    }
    return event.completion_timestamp_s > 0.0 ? event.completion_timestamp_s : event.launch_timestamp_s;
}

std::wstring widen_text(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::string trim_copy(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    if (first == text.end()) {
        return {};
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    return std::string(first, last);
}

const char* mining_pool_policy_label(MiningPoolPolicy policy) {
    switch (policy) {
    case MiningPoolPolicy::TwoMiners:
        return "2Miners";
    case MiningPoolPolicy::F2Pool:
        return "F2Pool";
    }
    return "2Miners";
}

std::string build_policy_worker_name(const MiningConnectionSettings& settings) {
    const std::string payout_address = trim_copy(settings.payout_address);
    const std::string worker_id = trim_copy(settings.worker_id);
    if (payout_address.empty()) {
        return {};
    }
    if (worker_id.empty()) {
        return payout_address;
    }
    return payout_address + '.' + worker_id;
}

bool is_valid_f2pool_worker_id(const std::string& worker_id) {
    return !worker_id.empty() && std::all_of(worker_id.begin(), worker_id.end(), [](unsigned char value) {
        return std::isalnum(value) != 0;
    });
}

std::string mining_settings_validation_message(const MiningConnectionSettings& settings) {
    if (settings.pool_policy == MiningPoolPolicy::F2Pool && !is_valid_f2pool_worker_id(trim_copy(settings.worker_id))) {
        return "F2Pool worker id must use letters and digits only; do not include dots or underscores";
    }
    return {};
}

bool has_required_mining_fields(const MiningConnectionSettings& settings) {
    return !trim_copy(settings.pool_host).empty()
        && settings.pool_port > 0U
        && !trim_copy(settings.payout_address).empty()
        && !trim_copy(settings.worker_id).empty();
}

MiningConnectionSettings sanitize_mining_settings(const MiningConnectionSettings& settings) {
    MiningConnectionSettings sanitized = settings;
    sanitized.pool_host = trim_copy(settings.pool_host);
    sanitized.payout_address = trim_copy(settings.payout_address);
    sanitized.worker_id = trim_copy(settings.worker_id);
    sanitized.worker_password = trim_copy(settings.worker_password);
    sanitized.validation_log_csv_path = trim_copy(settings.validation_log_csv_path);
    if (sanitized.pool_port == 0U) {
        sanitized.pool_port = 3333;
    }
    if (sanitized.worker_password.empty()) {
        sanitized.worker_password = "x";
    }
    sanitized.max_requests_per_second = clamp_stratum_request_rate(settings.max_requests_per_second);
    sanitized.target_network_share_fraction = std::clamp(settings.target_network_share_fraction, 1.0e-9, 1.0);
    sanitized.target_hashrate_hs =
        (!std::isfinite(settings.target_hashrate_hs) || settings.target_hashrate_hs <= 0.0)
        ? 0.0
        : settings.target_hashrate_hs;
    sanitized.max_invalid_pool_submissions = std::max<std::size_t>(1U, settings.max_invalid_pool_submissions);
    sanitized.allowed_worker_count =
        std::clamp<std::size_t>(
            settings.allowed_worker_count == 0U ? kStratumWorkerSlotCount : settings.allowed_worker_count,
            1U,
            kStratumWorkerSlotCount);
    sanitized.validation_jitter_window_seconds =
        (!std::isfinite(settings.validation_jitter_window_seconds) || settings.validation_jitter_window_seconds <= 0.0)
        ? 60.0
        : std::clamp(settings.validation_jitter_window_seconds, 1.0, 3600.0);
    sanitized.min_validation_jitter_samples =
        std::clamp<std::size_t>(
            settings.min_validation_jitter_samples == 0U ? kStratumWorkerSlotCount : settings.min_validation_jitter_samples,
            1U,
            1024U);
    sanitized.run_indefinitely = settings.run_indefinitely;
    sanitized.run_duration_minutes =
        (!std::isfinite(settings.run_duration_minutes) || settings.run_duration_minutes <= 0.0)
        ? kDefaultRunDurationMinutes
        : std::clamp(settings.run_duration_minutes, 0.001, 10080.0);
    return sanitized;
}

SubstrateStratumConnectionIngress build_stratum_connection_ingress(const MiningConnectionSettings& settings) {
    const MiningConnectionSettings sanitized = sanitize_mining_settings(settings);

    SubstrateStratumConnectionIngress ingress;
    ingress.host = sanitized.pool_host;
    ingress.port = sanitized.pool_port;
    ingress.worker_name = build_policy_worker_name(sanitized);
    ingress.worker_password = sanitized.worker_password;
    ingress.max_requests_per_second = sanitized.max_requests_per_second;
    ingress.target_network_share_fraction = sanitized.target_network_share_fraction;
    ingress.target_hashrate_hs = sanitized.target_hashrate_hs;
    ingress.allowed_worker_count = sanitized.allowed_worker_count;
    ingress.validation_jitter_window_seconds = sanitized.validation_jitter_window_seconds;
    ingress.min_validation_jitter_samples = sanitized.min_validation_jitter_samples;
    ingress.validation_log_csv_path = sanitized.validation_log_csv_path;
    ingress.allow_live_submit = sanitized.allow_live_submit;
    ingress.phase_guided_preview_test_mode =
        sanitized.phase_guided_preview_test_mode && !sanitized.allow_live_submit;
    ingress.auto_promote_to_live_mode =
        sanitized.auto_promote_to_live_mode && ingress.phase_guided_preview_test_mode;
    ingress.max_invalid_pool_submissions = sanitized.max_invalid_pool_submissions;
    ingress.dry_run_only = !ingress.allow_live_submit && !ingress.phase_guided_preview_test_mode;
    ingress.authoritative_program_source = std::string(phase_programs::kStratumConnectionProgramSource);
    return ingress;
}

MiningConnectionStatus build_mining_connection_status(
    const MiningConnectionSettings& settings,
    bool connection_requested,
    const std::optional<SubstrateStratumAuthorityState>& authority_state
) {
    const MiningConnectionSettings sanitized = sanitize_mining_settings(settings);

    MiningConnectionStatus status;
    status.pool_policy_label = mining_pool_policy_label(sanitized.pool_policy);
    status.derived_worker_name = build_policy_worker_name(sanitized);
    status.configured = has_required_mining_fields(sanitized);
    status.rewards_path_gated = true;

    if (!status.configured) {
        status.connection_state = "not_configured";
        status.status_message = "Pool host, payout wallet/account, and worker id are required.";
        return status;
    }

    if (const std::string validation_message = mining_settings_validation_message(sanitized);
        !validation_message.empty()) {
        status.configured = false;
        status.connection_state = "invalid_worker_id";
        status.status_message = validation_message;
        return status;
    }

    if (!connection_requested) {
        status.connection_state = "ready_to_connect";
        if (sanitized.allow_live_submit) {
            status.status_message = "Settings saved. Use Connect to open the live Stratum session.";
        } else if (sanitized.phase_guided_preview_test_mode) {
            status.status_message = "Settings saved. Use Connect to open the online phase-guided header-resonance validation session.";
        } else {
            status.status_message = "Settings saved. Use Connect to open the preview Stratum session.";
        }
        return status;
    }

    status.connection_state = "connect_requested";
    if (sanitized.allow_live_submit) {
        status.status_message = "Connect requested. Waiting for pool handshake and share path authorization.";
    } else if (sanitized.phase_guided_preview_test_mode) {
        status.status_message = "Header-resonance validation connect requested. Waiting for the online Stratum handshake.";
    } else {
        status.status_message = "Preview connect requested. Waiting for deterministic Stratum handshake.";
    }

    if (!authority_state.has_value()) {
        return status;
    }

    const SubstrateStratumAuthorityState& current = authority_state.value();
    if (current.accepted_submit_count > 0U) {
        status.rewards_path_gated = false;
    }
    if (!current.connection_state.empty()) {
        status.connection_state = current.connection_state;
    }
    if (!current.last_response_message.empty()) {
        status.status_message = current.last_response_message;
    } else if (current.accepted_submit_count > 0U) {
        status.status_message = "Live share submit accepted; reward payout remains gated.";
    } else if (current.submit_dispatch_count > 0U) {
        status.status_message = "Live share submitted; awaiting pool acceptance.";
    } else if (current.phase_guided_preview_test_mode && current.offline_valid_submit_preview_count > 0U) {
        status.status_message = "Phase-guided header resonance validated against conventional hashing on live pool work.";
    } else if (current.phase_guided_preview_test_mode && current.submit_preview_count > 0U) {
        status.status_message = "Phase-guided header-resonant share failed conventional hash validation.";
    } else if (current.submit_path_ready && current.submit_policy_enabled) {
        status.status_message = "Job received and phase-clamped submit path armed.";
    } else if (current.has_active_job) {
        status.status_message = current.phase_guided_preview_test_mode
            ? "Job received and header-resonant share projections are being validated against conventional hashing."
            : "Job received and worker nonce windows assigned in preview mode.";
    } else if (current.network_authority_granted) {
        if (current.submit_policy_enabled) {
            status.status_message = "Stratum authority granted; submit path is waiting for a phase-clamped share actuation.";
        } else if (current.phase_guided_preview_test_mode) {
            status.status_message = "Stratum authority granted; phase-guided header-resonance validation is waiting for a header-resonant share projection.";
        } else {
            status.status_message = "Stratum authority granted; submit path remains preview-only.";
        }
    } else if (current.has_connection_ingress) {
        status.status_message = "Connection ingress accepted; runtime is sequencing the pool handshake.";
    }

    if (status.connection_state == "refused" && current.last_response_message.empty()) {
        status.status_message = "Stratum connection ingress failed validation.";
    }

    return status;
}

std::string format_nonce_hex_gpu(std::uint32_t nonce_value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << nonce_value;
    return out.str();
}

std::string hash_words_to_hex_gpu(const std::array<std::uint32_t, 8>& hash_words) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0');
    for (std::uint32_t word : hash_words) {
        out << std::setw(8) << word;
    }
    return out.str();
}

SubstrateStratumWorkerAssignment resolve_gpu_worker_assignment(
    const SubstrateStratumAuthorityState& authority_state,
    std::size_t worker_index
) {
    for (const auto& assignment : authority_state.worker_assignments) {
        if (assignment.active && assignment.worker_index == worker_index) {
            return assignment;
        }
    }

    SubstrateStratumWorkerAssignment fallback;
    fallback.active = true;
    fallback.worker_index = worker_index;
    fallback.job_id = authority_state.active_job_id;
    fallback.worker_name = authority_state.has_connection_ingress
        ? authority_state.connection_ingress.worker_name
        : std::string{};
    return fallback;
}

PhaseClampedShareActuation build_gpu_mining_validation_actuation(
    const SubstrateStratumAuthorityState& authority_state,
    const MiningValidationSnapshot& validation
) {
    const std::size_t worker_index = std::min<std::size_t>(
        validation.selected_worker_index,
        authority_state.worker_assignments.size() > 0U ? authority_state.worker_assignments.size() - 1U : 0U);
    const SubstrateStratumWorkerAssignment assignment =
        resolve_gpu_worker_assignment(authority_state, worker_index);
    const MiningPhaseEncodingState mining_encoding =
        build_mining_phase_encoding_state(&authority_state);

    PhaseClampedShareActuation actuation;
    actuation.trace_id = "gpu-mining-validation-" + std::to_string(validation.activation_tick);
    actuation.connection_id = authority_state.has_connection_ingress
        ? authority_state.connection_ingress.connection_id
        : std::string{};
    actuation.job_id = authority_state.active_job_id;
    actuation.worker_index = worker_index;
    actuation.worker_name = assignment.worker_name;
    actuation.request_id = actuation.connection_id + ".gpu_validation." + std::to_string(validation.activation_tick);
    actuation.target_network = "bitcoin";
    actuation.target_compact_nbits = authority_state.active_job_nbits;
    const bool selected_share_candidate =
        validation.valid_nonce_count > 0U
        || (validation.validation_flags & 0x40U) != 0U
        || (validation.validation_flags & 0x800U) != 0U;
    const bool selected_block_candidate =
        (validation.validation_flags & 0x80U) != 0U
        || (validation.validation_flags & 0x1000U) != 0U;
    actuation.actuation_topic = selected_share_candidate
        ? "substrate.bitcoin.share.candidate"
        : "substrate.bitcoin.share.noise";
    actuation.nonce_hex = format_nonce_hex_gpu(validation.selected_nonce_value);
    actuation.hash_hex = hash_words_to_hex_gpu(validation.selected_hash_words);
    actuation.target_hex = authority_state.active_share_target_hex;
    actuation.share_target_hex = authority_state.active_share_target_hex;
    actuation.block_target_hex = authority_state.active_block_target_hex;
    actuation.nonce_start = assignment.nonce_start;
    actuation.nonce_end = assignment.nonce_end;
    actuation.target_difficulty = authority_state.difficulty;
    actuation.block_difficulty = authority_state.active_block_difficulty;
    actuation.expected_hashes_for_share = authority_state.expected_hashes_for_share;
    actuation.target_network_share_fraction = authority_state.has_connection_ingress
        ? authority_state.connection_ingress.target_network_share_fraction
        : 0.05;
    actuation.network_hashrate_hs = authority_state.network_hashrate_hs;
    actuation.required_hashrate_hs = authority_state.required_hashrate_hs;
    actuation.required_share_submissions_per_s = authority_state.required_share_submissions_per_s;
    actuation.target_phase_turns = mining_encoding.share_target_phase_turns;
    actuation.phase_position_turns = validation.selected_lane_phase_turns;
    actuation.field_vector_phase_turns = validation.selected_lane_phase_turns;
    actuation.phase_transport_turns = authority_state.last_phase_clamped_phase_transport_turns;
    actuation.phase_lock_delta_turns = authority_state.last_phase_clamped_phase_lock_delta_turns;
    actuation.phase_clamp_strength = authority_state.last_phase_clamped_phase_clamp_strength;
    actuation.flux_phase_transport_norm = authority_state.last_phase_flux_conservation;
    actuation.transfer_drive_norm = mining_encoding.transfer_drive_norm;
    actuation.stability_gate_norm = mining_encoding.stability_gate_norm;
    actuation.damping_norm = mining_encoding.damping_norm;
    actuation.transport_drive_norm = mining_encoding.transport_drive_norm;
    actuation.target_resonance_norm = std::max<double>(
        mining_encoding.target_resonance_norm,
        static_cast<double>(validation.share_target_pass_norm));
    actuation.resonance_activation_norm = mining_encoding.resonance_activation_norm;
    actuation.phase_alignment = validation.target_phase_alignment_norm;
    actuation.zero_point_proximity = mining_encoding.zero_point_proximity_norm;
    actuation.temporal_admissibility = mining_encoding.temporal_admissibility_norm;
    actuation.transport_readiness = mining_encoding.transport_readiness_norm;
    actuation.share_confidence = validation.validation_certainty_norm;
    actuation.validation_structure_norm = validation.validation_structure_norm;
    actuation.parallel_harmonic_count = validation.attempted_nonce_count;
    actuation.verified_parallel_harmonic_count = validation.valid_nonce_count;
    actuation.validated_parallel_harmonic_count = validation.valid_nonce_count;
    actuation.attempted_nonce_count = validation.attempted_nonce_count;
    actuation.valid_nonce_count = validation.valid_nonce_count;
    actuation.selected_coherence_score = validation.validation_certainty_norm;
    actuation.share_target_pass = selected_share_candidate;
    actuation.block_target_pass = selected_block_candidate;
    actuation.block_candidate_valid = actuation.block_target_pass;
    actuation.block_coherence_norm = validation.block_coherence_norm;
    actuation.submit_priority_score = validation.submit_priority_score_norm;
    actuation.queue_quality_class = validation.queue_quality_class;
    actuation.resonance_reinforcement_count = validation.resonance_reinforcement_count;
    actuation.noise_lane_count = validation.noise_lane_count;
    actuation.resonant_candidate_available = validation.attempted_nonce_count > 0U;
    actuation.valid_share_candidate = actuation.share_target_pass;
    actuation.actuation_permitted = actuation.share_target_pass;
    actuation.target_resonance_ready = actuation.target_resonance_norm >= authority_state.target_resonance_floor;
    actuation.all_parallel_harmonics_verified =
        (validation.validation_flags & 0x8U) != 0U && validation.valid_nonce_count > 0U;
    actuation.phase_program_substrate_native = mining_encoding.phase_program_substrate_native;
    actuation.phase_program_same_pulse_validation = mining_encoding.phase_program_same_pulse_validation;
    actuation.phase_program_pool_format_ready = mining_encoding.phase_program_pool_format_ready;
    actuation.phase_program_block_count = mining_encoding.phase_program_block_count;
    actuation.phase_program_title = "gpu_phase_substrate_mining_validation";
    actuation.phase_temporal_sequence = "gpu.same_pulse.sha256d";
    actuation.sampled_valid_nonce_hexes = {actuation.nonce_hex};
    actuation.gate_reason = actuation.share_target_pass
        ? "gpu_same_pulse_share_validated"
        : "gpu_harmonic_noise_sink";
    return actuation;
}

}  // namespace

struct LiveControlCenter::Impl {
    Impl()
        : controller({}, SubstrateControllerConfig{}),
          telemetry_bridge({256U, 0.010, false}) {
        snapshot.metric_panels = build_default_metric_panels();
        controller.bus().subscribe("*", [this](const RuntimeEvent& event) {
            std::lock_guard<std::mutex> lock(mutex);
            append_recent_event_locked(event.topic + " | " + event.message);
            if (event.has_viewport_frame) {
                latest_phase_viewport_frame = event.viewport_frame;
                has_latest_phase_viewport_frame = true;
            }
            if (event.has_stratum_authority) {
                latest_stratum_authority = event.stratum_authority;
                has_latest_stratum_authority = true;
            }
            if ((event.topic == "substrate.stratum.job.received"
                    || event.topic == "substrate.stratum.workers.assigned")
                && event.has_stratum_authority
                && event.stratum_authority.has_active_job
                && event.stratum_authority.active_worker_count > 0U) {
                job_activation_pending.store(true);
            }
        });
    }

    ~Impl() {
        stop();
    }

    void start() {
        if (running.exchange(true)) {
            return;
        }
        try {
            telemetry.start_stream(kTelemetryStreamInterval);
            worker = std::thread([this]() { worker_loop(); });
        } catch (...) {
            running.store(false);
            telemetry.stop_stream();
            throw;
        }
    }

    void stop() {
        running.store(false);
        telemetry.stop_stream();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void set_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        last_error = widen_text(message);
        append_recent_event_locked("error | " + message);
    }

    void append_recent_event_locked(const std::string& message) {
        recent_events.push_front(message);
        while (recent_events.size() > 8U) {
            recent_events.pop_back();
        }
    }

    void log_local_event(const std::string& topic, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        append_recent_event_locked(topic + " | " + message);
    }

    void begin_mining_session(const MiningConnectionSettings& settings) {
        const double now_s = current_unix_time_s();
        std::lock_guard<std::mutex> lock(mutex);
        mining_session_active = true;
        mining_session_run_indefinitely = settings.run_indefinitely;
        mining_session_duration_minutes = settings.run_duration_minutes;
        mining_session_started_unix_s = now_s;
        mining_session_deadline_unix_s = settings.run_indefinitely
            ? 0.0
            : now_s + (settings.run_duration_minutes * 60.0);
    }

    void clear_mining_session_locked() {
        mining_session_active = false;
        mining_session_started_unix_s = 0.0;
        mining_session_deadline_unix_s = 0.0;
    }

    void clear_mining_session() {
        std::lock_guard<std::mutex> lock(mutex);
        clear_mining_session_locked();
    }

    void maybe_expire_mining_session() {
        std::string connection_id;
        bool should_disconnect = false;
        double elapsed_s = 0.0;
        double duration_minutes = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!mining_session_active || mining_session_run_indefinitely || mining_session_deadline_unix_s <= 0.0) {
                return;
            }

            const double now_s = current_unix_time_s();
            if (now_s < mining_session_deadline_unix_s) {
                return;
            }

            elapsed_s = std::max(0.0, now_s - mining_session_started_unix_s);
            duration_minutes = mining_session_duration_minutes;
            clear_mining_session_locked();
            if (connection_requested) {
                connection_requested = false;
                if (has_latest_stratum_authority && latest_stratum_authority.has_connection_ingress) {
                    connection_id = latest_stratum_authority.connection_ingress.connection_id;
                } else if (has_required_mining_fields(mining_settings)) {
                    connection_id = sanitize_stratum_connection_ingress(build_stratum_connection_ingress(mining_settings)).connection_id;
                }
                should_disconnect = true;
            }
        }

        std::ostringstream out;
        out << "timed run completed after " << std::fixed << std::setprecision(2)
            << (elapsed_s / 60.0) << " minutes";
        log_local_event("mining.session", out.str());

        if (should_disconnect) {
            publish_mining_disconnect_for_connection(
                connection_id,
                "Timed mining run completed after " + std::to_string(duration_minutes) + " minutes");
        }
    }

    void publish_control_ingress() {
        const std::uint64_t revision = ++control_revision;
        const SubstrateControlIngress ingress = control_surface.build_ingress(
            reward_unit.load(),
            audio_enabled.load(),
            paused.load(),
            compact_layout.load(),
            always_on_top.load(),
            static_cast<double>(revision) * 0.125
        );

        controller.bus().publish(RuntimeEvent{
            "substrate.control.ingress",
            "Control-surface ingress prepared for substrate firmware boundary",
            {},
            {},
            false,
            {},
            false,
            ingress,
            true,
        });
    }

    void publish_mining_ingress() {
        MiningConnectionSettings settings_copy;
        bool requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings_copy = mining_settings;
            requested = connection_requested;
        }

        if (!requested) {
            return;
        }

        if (!has_required_mining_fields(settings_copy)) {
            set_error("Pool host, payout wallet/account, and worker id are required before connecting");
            return;
        }

        if (const std::string validation_message = mining_settings_validation_message(settings_copy);
            !validation_message.empty()) {
            set_error(validation_message);
            return;
        }

        const SubstrateStratumConnectionIngress ingress = build_stratum_connection_ingress(settings_copy);
        controller.bus().publish(RuntimeEvent{
            "substrate.stratum.connection.ingress",
            "User settings prepared Stratum connection ingress for the substrate firmware boundary",
            {},
            {},
            false,
            {},
            false,
            {},
            false,
            ingress,
            true,
        });
    }

    void publish_mining_disconnect_for_connection(const std::string& connection_id, const std::string& reason) {
        RuntimeEvent event;
        event.topic = "substrate.stratum.connection.control";
        event.message = reason.empty() ? "User requested Stratum disconnect" : reason;
        event.stratum_connection_control.connection_id = connection_id;
        event.stratum_connection_control.disconnect_requested = true;
        event.stratum_connection_control.reason = reason;
        event.has_stratum_connection_control = true;
        controller.bus().publish(event);
    }

    void publish_mining_disconnect(const std::string& reason) {
        std::string connection_id;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (has_latest_stratum_authority && latest_stratum_authority.has_connection_ingress) {
                connection_id = latest_stratum_authority.connection_ingress.connection_id;
            } else if (has_required_mining_fields(mining_settings)) {
                connection_id = sanitize_stratum_connection_ingress(build_stratum_connection_ingress(mining_settings)).connection_id;
            }
        }
        publish_mining_disconnect_for_connection(connection_id, reason);
    }

    ControlCenterSnapshot snapshot_copy() const {
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        const std::optional<SubstrateStratumAuthorityState> authority_state = firmware_state.has_stratum_authority
            ? std::optional<SubstrateStratumAuthorityState>{firmware_state.stratum_authority}
            : std::nullopt;
        const SubstrateStratumTcpAdapterState tcp_adapter_state = controller.application().stratum_tcp_adapter().snapshot();

        std::lock_guard<std::mutex> lock(mutex);
        ControlCenterSnapshot copy = snapshot;
        copy.recent_events.assign(recent_events.begin(), recent_events.end());
        copy.mining_runtime_running = mining_session_active;
        copy.mining_session_run_indefinitely = mining_session_active
            ? mining_session_run_indefinitely
            : mining_settings.run_indefinitely;
        copy.mining_session_duration_minutes = mining_session_active
            ? mining_session_duration_minutes
            : mining_settings.run_duration_minutes;
        copy.mining_session_elapsed_seconds = 0.0;
        copy.mining_session_remaining_seconds = 0.0;
        if (mining_session_active && mining_session_started_unix_s > 0.0) {
            const double now_s = current_unix_time_s();
            copy.mining_session_elapsed_seconds = std::max(0.0, now_s - mining_session_started_unix_s);
            if (!mining_session_run_indefinitely && mining_session_deadline_unix_s > 0.0) {
                copy.mining_session_remaining_seconds = std::max(0.0, mining_session_deadline_unix_s - now_s);
            }
        }
        copy.pool_connection_requested = connection_requested;
        copy.audio_enabled = audio_enabled.load();
        copy.paused = paused.load();
        copy.compact_layout = compact_layout.load();
        copy.always_on_top = always_on_top.load();
        copy.live = snapshot.live;
        copy.mining_settings = mining_settings;
        if (authority_state.has_value()) {
            copy.stratum_authority = *authority_state;
            copy.has_stratum_authority = true;
        } else {
            copy.stratum_authority = {};
            copy.has_stratum_authority = false;
        }
        copy.tcp_adapter_state = tcp_adapter_state;
        copy.last_gpu_mining_validation = latest_gpu_mining_validation;
        copy.mining_status = build_mining_connection_status(
            mining_settings,
            connection_requested,
            authority_state
        );
        return copy;
    }

    bool export_calibration_bundle_to(const std::filesystem::path& output_dir) const {
        std::vector<SubstrateTrace> traces_copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            traces_copy.assign(traces.begin(), traces.end());
        }
        if (traces_copy.empty()) {
            return false;
        }
        (void)qbit_miner::export_calibration_bundle(traces_copy, output_dir);
        return true;
    }

    bool export_device_validation_bundle_to(const std::filesystem::path& output_dir) const {
        std::vector<SubstrateTrace> traces_copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            traces_copy.assign(traces.begin(), traces.end());
        }
        if (traces_copy.empty()) {
            return false;
        }
        DeviceValidationExportOptions options;
        options.hardware_profile.device_model = "Quantum Miner Control Center";
        options.hardware_profile.gpu_device_id = "rig-control-center-01";
        options.hardware_profile.driver_version = "live-viewport";
        options.hardware_profile.measured_device_window = true;
        (void)qbit_miner::export_device_validation_bundle(traces_copy, output_dir, options);
        return true;
    }

    void apply_trace_update(
        const SubstrateTrace& trace,
        const MiningTelemetryObservation& observation,
        double viewport_time_s
    ) {
        const MiningMetricsSnapshot metrics = build_mining_metrics_snapshot(trace, observation, reward_unit.load());
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        SubstrateViewportFrame authoritative_viewport_frame;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (has_latest_phase_viewport_frame && latest_phase_viewport_frame.phase_id == trace.photonic_identity.trace_id) {
                authoritative_viewport_frame = latest_phase_viewport_frame;
            } else {
                authoritative_viewport_frame = controller.application().encode_viewport_frame(trace);
            }
        }
        const FieldViewportFrame viewport = build_field_viewport_frame(
            authoritative_viewport_frame,
            viewport_time_s,
            18U,
            firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr);

        {
            std::lock_guard<std::mutex> lock(mutex);
            traces.push_back(trace);
            while (traces.size() > 96U) {
                traces.pop_front();
            }

            snapshot.metrics = metrics;
            update_metric_panels(
                snapshot.metric_panels,
                metrics,
                observation,
                has_latest_stratum_authority ? std::optional<SubstrateStratumAuthorityState>{latest_stratum_authority} : std::nullopt
            );
            snapshot.viewport = viewport;
            snapshot.audio_enabled = firmware_state.has_control_ingress ? firmware_state.control_ingress.audio_enabled : audio_enabled.load();
            snapshot.paused = firmware_state.has_control_ingress ? firmware_state.control_ingress.paused : paused.load();
            snapshot.compact_layout = firmware_state.has_control_ingress ? firmware_state.control_ingress.compact_layout : compact_layout.load();
            snapshot.always_on_top = firmware_state.has_control_ingress ? firmware_state.control_ingress.always_on_top : always_on_top.load();
            snapshot.live = observation.live;
            snapshot.control_program_source = std::string(phase_programs::kControlSurfaceProgramSource);
            snapshot.viewport_program_source = std::string(phase_programs::kViewportProgramSource);
        }
    }

    void process_heartbeat_feedback(const GPUTelemetrySample& telemetry_sample, std::uint64_t tick_index) {
        const MiningTelemetryObservation observation = to_observation(telemetry_sample);
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        GpuFeedbackFrame frame = build_feedback_frame(telemetry_sample, tick_index);
        apply_mining_phase_encoding(
            frame,
            build_mining_phase_encoding_state(
                firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr));
        const SubstrateTrace trace = controller.process_feedback(frame);
        apply_trace_update(trace, observation, telemetry_sample.timestamp_s);
        last_feedback_time_s.store(telemetry_sample.timestamp_s);
    }

    void process_synchronized_feedback(const GpuKernelTelemetryMatch& synchronized_match) {
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        GpuFeedbackFrame frame = telemetry_bridge.build_feedback_frame(synchronized_match, "rig-control-center-01");
        apply_mining_phase_encoding(
            frame,
            build_mining_phase_encoding_state(
                firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr));
        const SubstrateTrace trace = controller.process_feedback(frame);
        apply_trace_update(trace, to_observation(synchronized_match.telemetry_sample), synchronized_match.telemetry_sample.timestamp_s);
        has_processed_synchronized_feedback = true;
        last_synchronized_feedback_time_s = synchronized_match.telemetry_sample.timestamp_s;
        last_feedback_time_s.store(synchronized_match.telemetry_sample.timestamp_s);

        std::ostringstream out;
        out << "iteration=" << synchronized_match.kernel_event.kernel_iteration
            << " phase=" << (synchronized_match.kernel_event.kernel_phase == GpuKernelIterationPhase::Launch ? "launch" : "completion")
            << " telemetry_seq=" << synchronized_match.telemetry_sample.telemetry_sequence
            << " exact=" << (synchronized_match.exact_sequence_match ? "true" : "false")
            << " skew_ms=" << (synchronized_match.telemetry_skew_s * 1000.0);
        log_local_event("gpu.kernel.sync", out.str());
    }

    void inject_feedback_frame(const GpuFeedbackFrame& frame) {
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        MiningTelemetryObservation observation;
        observation.graphics_frequency_hz = std::max(0.0, frame.photonic_identity.field_vector.frequency) * 1.0e9;
        observation.memory_frequency_hz = std::max(0.0, frame.photonic_identity.memory) * 1.0e9;
        observation.amplitude_norm = clamp01(frame.photonic_identity.field_vector.amplitude);
        observation.voltage_v = std::max(0.0, frame.photonic_identity.field_vector.voltage);
        observation.amperage_a = std::max(0.0, frame.photonic_identity.field_vector.current);
        observation.power_w = observation.voltage_v * observation.amperage_a;
        observation.temperature_c = 35.0 + (25.0 * clamp01(frame.photonic_identity.field_vector.thermal_noise));
        observation.gpu_util_norm = clamp01(frame.integrated_feedback);
        observation.memory_util_norm = clamp01(frame.photonic_identity.memory);
        observation.thermal_interference_norm = clamp01(frame.photonic_identity.field_vector.thermal_noise);
        observation.live = false;
        observation.provider = "synthetic-feedback";

        GpuFeedbackFrame encoded_frame = frame;
        apply_mining_phase_encoding(
            encoded_frame,
            build_mining_phase_encoding_state(
                firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr));
        const SubstrateTrace trace = controller.process_feedback(encoded_frame);
        const double injected_time_s = current_unix_time_s();
        apply_trace_update(trace, observation, injected_time_s);
        last_feedback_time_s.store(injected_time_s);
    }

    void process_bootstrap_feedback(const SubstrateFirmwareState& firmware_state, std::uint64_t tick_index, double now_s) {
        const SubstrateStratumAuthorityState* authority_state =
            firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr;
        GpuFeedbackFrame frame = build_harmonic_bootstrap_feedback_frame(authority_state, tick_index);
        MiningTelemetryObservation observation;
        observation.graphics_frequency_hz = std::max(0.0, frame.photonic_identity.field_vector.frequency) * 1.0e9;
        observation.memory_frequency_hz = std::max(0.0, frame.photonic_identity.memory) * 1.0e9;
        observation.amplitude_norm = clamp01(frame.photonic_identity.field_vector.amplitude);
        observation.voltage_v = std::max(0.0, frame.photonic_identity.field_vector.voltage);
        observation.amperage_a = std::max(0.0, frame.photonic_identity.field_vector.current);
        observation.power_w = observation.voltage_v * observation.amperage_a;
        observation.temperature_c = 33.0 + (24.0 * clamp01(frame.photonic_identity.field_vector.thermal_noise));
        observation.gpu_util_norm = clamp01(frame.integrated_feedback);
        observation.memory_util_norm = clamp01(frame.photonic_identity.memory);
        observation.thermal_interference_norm = clamp01(frame.photonic_identity.field_vector.thermal_noise);
        observation.live = false;
        observation.provider = "harmonic-bootstrap";

        apply_mining_phase_encoding(
            frame,
            build_mining_phase_encoding_state(authority_state));
        const SubstrateTrace trace = controller.process_feedback(frame);
        apply_trace_update(trace, observation, now_s);
        last_feedback_time_s.store(now_s);
        std::ostringstream out;
        out << "tick=" << tick_index
            << " workers=" << (authority_state != nullptr ? authority_state->active_worker_count : 0U)
            << " gate=" << (authority_state != nullptr ? authority_state->submit_gate_reason : std::string("none"));
        log_local_event("mining.bootstrap", out.str());
    }

    bool drain_pending_kernel_iterations() {
        bool processed_any = false;
        while (true) {
            GpuKernelIterationEvent next_event;
            std::optional<GpuKernelTelemetrySample> latest_sample;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pending_kernel_iterations.empty()) {
                    break;
                }
                next_event = pending_kernel_iterations.front();
                latest_sample = latest_kernel_sample;
            }

            const auto synchronized_match = telemetry_bridge.synchronize_iteration(next_event);
            if (!synchronized_match.has_value()) {
                bool drop_stale_event = false;
                if (latest_sample.has_value()) {
                    const double reference_time = kernel_reference_time_s(next_event);
                    drop_stale_event = (latest_sample->timestamp_s - reference_time) > (telemetry_bridge.config().max_sync_skew_s * 4.0);
                }
                if (drop_stale_event) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!pending_kernel_iterations.empty()) {
                        pending_kernel_iterations.pop_front();
                    }
                    std::ostringstream out;
                    out << "dropped stale iteration=" << next_event.kernel_iteration
                        << " phase=" << (next_event.kernel_phase == GpuKernelIterationPhase::Launch ? "launch" : "completion");
                    append_recent_event_locked("gpu.kernel.drop | " + out.str());
                    continue;
                }
                break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!pending_kernel_iterations.empty()) {
                    pending_kernel_iterations.pop_front();
                }
            }
            process_synchronized_feedback(*synchronized_match);
            processed_any = true;
        }
        return processed_any;
    }

    void ingest_gpu_kernel_iteration(const GpuKernelIterationEvent& event) {
        const auto latest_stream_sample = telemetry.latest_sample();
        std::lock_guard<std::mutex> lock(mutex);
        GpuKernelIterationEvent synchronized_event = event;
        if (latest_stream_sample.has_value()) {
            const GpuKernelTelemetrySample telemetry_sample = to_kernel_sample(*latest_stream_sample);
            if (!latest_kernel_sample.has_value()
                || telemetry_sample.timestamp_s >= latest_kernel_sample->timestamp_s) {
                latest_kernel_sample = telemetry_sample;
            }
        }
        if (synchronized_event.telemetry_sequence_hint == 0U && latest_kernel_sample.has_value()) {
            const double reference_time = kernel_reference_time_s(synchronized_event);
            const auto& sample = latest_kernel_sample.value();
            const double sample_start = sample.sample_window_start_s > 0.0
                ? sample.sample_window_start_s
                : std::max(0.0, sample.timestamp_s - sample.delta_s);
            const double sample_end = sample.sample_window_end_s > 0.0
                ? sample.sample_window_end_s
                : sample.timestamp_s;
            if (reference_time >= (sample_start - telemetry_bridge.config().max_sync_skew_s)
                && reference_time <= (sample_end + telemetry_bridge.config().max_sync_skew_s)) {
                synchronized_event.telemetry_sequence_hint = sample.telemetry_sequence;
            }
        }

        pending_kernel_iterations.push_back(synchronized_event);
        while (pending_kernel_iterations.size() > kPendingKernelIterationCapacity) {
            pending_kernel_iterations.pop_front();
        }

        std::ostringstream out;
        out << "queued iteration=" << synchronized_event.kernel_iteration
            << " phase=" << (synchronized_event.kernel_phase == GpuKernelIterationPhase::Launch ? "launch" : "completion")
            << " hint=" << synchronized_event.telemetry_sequence_hint;
        append_recent_event_locked("gpu.kernel.queue | " + out.str());
    }

    void ingest_gpu_mining_validation(const MiningValidationSnapshot& validation) {
        std::lock_guard<std::mutex> lock(mutex);
        latest_gpu_mining_validation = validation;
        pending_gpu_mining_validation = validation;
        std::ostringstream out;
        out << "certainty=" << validation.validation_certainty_norm
            << " same_pulse=" << validation.same_pulse_validation_norm
            << " pool_submit=" << validation.pool_submit_vector_norm
            << " lane=" << validation.selected_lane_index;
        append_recent_event_locked("gpu.mining.validation | " + out.str());
    }

    void inject_mining_validation(const MiningValidationSnapshot& validation) {
        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        GpuFeedbackFrame frame = build_feedback_frame_from_mining_validation(validation, validation.activation_tick);
        apply_mining_phase_encoding(
            frame,
            build_mining_phase_encoding_state(
                firmware_state.has_stratum_authority ? &firmware_state.stratum_authority : nullptr));
        inject_feedback_frame(frame);
    }

    bool publish_gpu_mining_candidate_if_ready(const MiningValidationSnapshot& validation) {
    const bool has_hash_words = std::any_of(
        validation.selected_hash_words.begin(),
        validation.selected_hash_words.end(),
        [](std::uint32_t word) { return word != 0U; });
    if (validation.valid_nonce_count == 0U || !has_hash_words) {
        return false;
    }

        const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
        if (!firmware_state.has_stratum_authority || !firmware_state.stratum_authority.has_active_job) {
            return false;
        }

        const PhaseClampedShareActuation actuation =
            build_gpu_mining_validation_actuation(firmware_state.stratum_authority, validation);
        const std::string share_key =
            actuation.connection_id + "|" + actuation.job_id + "|" + std::to_string(actuation.worker_index) + "|" + actuation.nonce_hex;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (last_gpu_candidate_share_key == share_key) {
                return false;
            }
            last_gpu_candidate_share_key = share_key;
        }

        RuntimeEvent event;
        event.topic = actuation.actuation_topic;
        event.message = "GPU same-pulse mining validation produced a harmonic share candidate";
        event.phase_clamped_share_actuation = actuation;
        event.has_phase_clamped_share_actuation = true;
        controller.bus().publish(event);
        return true;
    }

    bool drain_pending_mining_validation() {
        std::optional<MiningValidationSnapshot> pending_validation;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_validation = pending_gpu_mining_validation;
            pending_gpu_mining_validation.reset();
        }
        if (!pending_validation.has_value()) {
            return false;
        }
        publish_gpu_mining_candidate_if_ready(*pending_validation);
        inject_mining_validation(*pending_validation);
        return true;
    }

    void worker_loop() {
        std::uint64_t tick_index = 1;
        while (running.load()) {
            maybe_expire_mining_session();

            const SubstrateFirmwareState firmware_state = controller.application().firmware_runtime().snapshot();
            const bool firmware_paused = firmware_state.has_control_ingress && firmware_state.control_ingress.paused;
            if (firmware_paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                continue;
            }

            try {
                const std::vector<GPUTelemetrySample> telemetry_samples = telemetry.consume_pending_samples();
                bool processed_feedback_this_pass = false;
                for (const GPUTelemetrySample& telemetry_sample : telemetry_samples) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        latest_kernel_sample = to_kernel_sample(telemetry_sample);
                    }
                    telemetry_bridge.push_telemetry_sample(to_kernel_sample(telemetry_sample));
                    const bool processed_synchronized_feedback = drain_pending_kernel_iterations();

                    if (!processed_synchronized_feedback) {
                        const bool should_bootstrap =
                            !has_processed_synchronized_feedback
                            || ((telemetry_sample.timestamp_s - last_synchronized_feedback_time_s) >= kBootstrapFallbackTimeoutS);
                        if (should_bootstrap) {
                            process_heartbeat_feedback(telemetry_sample, tick_index);
                            ++tick_index;
                            processed_feedback_this_pass = true;
                        }
                    } else {
                        processed_feedback_this_pass = true;
                    }
                }

                processed_feedback_this_pass = drain_pending_kernel_iterations() || processed_feedback_this_pass;
                processed_feedback_this_pass = drain_pending_mining_validation() || processed_feedback_this_pass;

                const SubstrateFirmwareState latest_firmware_state = controller.application().firmware_runtime().snapshot();
                const double now_s = current_unix_time_s();
                const double last_feedback_s = last_feedback_time_s.load();
                const bool consume_job_activation =
                    job_activation_pending.load()
                    && latest_firmware_state.has_stratum_authority
                    && latest_firmware_state.stratum_authority.has_active_job
                    && latest_firmware_state.stratum_authority.active_worker_count > 0U;
                if (!processed_feedback_this_pass && consume_job_activation) {
                    process_bootstrap_feedback(latest_firmware_state, tick_index, now_s);
                    job_activation_pending.store(false);
                    ++tick_index;
                    processed_feedback_this_pass = true;
                }
                const bool active_job_waiting_for_feedback =
                    latest_firmware_state.has_stratum_authority
                    && latest_firmware_state.stratum_authority.has_active_job
                    && latest_firmware_state.stratum_authority.active_worker_count > 0U
                    && (
                        last_feedback_s <= 0.0
                        || (now_s - last_feedback_s) >= kBootstrapFallbackTimeoutS);
                if (!processed_feedback_this_pass && active_job_waiting_for_feedback) {
                    process_bootstrap_feedback(latest_firmware_state, tick_index, now_s);
                    ++tick_index;
                }
            } catch (const std::exception& error) {
                set_error(error.what());
            } catch (...) {
                set_error("Unknown control-center update failure");
            }

            std::this_thread::sleep_for(kTelemetryDrainInterval);
        }
    }

    mutable std::mutex mutex;
    std::deque<SubstrateTrace> traces;
    std::deque<std::string> recent_events;
    SubstrateViewportFrame latest_phase_viewport_frame;
    bool has_latest_phase_viewport_frame = false;
    ControlCenterSnapshot snapshot;
    mutable std::wstring last_error;
    std::atomic<bool> running {false};
    std::atomic<bool> audio_enabled {true};
    std::atomic<bool> paused {false};
    std::atomic<bool> compact_layout {false};
    std::atomic<bool> always_on_top {false};
    std::atomic<RewardIntervalUnit> reward_unit {RewardIntervalUnit::PerHour};
    std::atomic<std::uint64_t> control_revision {0};
    bool connection_requested = false;
    MiningConnectionSettings mining_settings;
    SubstrateStratumAuthorityState latest_stratum_authority;
    bool has_latest_stratum_authority = false;
    std::thread worker;
    GPUTelemetry telemetry;
    GpuKernelTelemetryBridge telemetry_bridge;
    std::optional<GpuKernelTelemetrySample> latest_kernel_sample;
    std::deque<GpuKernelIterationEvent> pending_kernel_iterations;
    std::optional<MiningValidationSnapshot> latest_gpu_mining_validation;
    std::optional<MiningValidationSnapshot> pending_gpu_mining_validation;
    std::string last_gpu_candidate_share_key;
    bool has_processed_synchronized_feedback = false;
    double last_synchronized_feedback_time_s = 0.0;
    std::atomic<double> last_feedback_time_s {0.0};
    std::atomic<bool> job_activation_pending {false};
    bool mining_session_active = false;
    bool mining_session_run_indefinitely = true;
    double mining_session_duration_minutes = kDefaultRunDurationMinutes;
    double mining_session_started_unix_s = 0.0;
    double mining_session_deadline_unix_s = 0.0;
    SubstrateControlSurface control_surface;
    SubstrateController controller;
};

LiveControlCenter::LiveControlCenter()
    : impl_(std::make_unique<Impl>()) {}

LiveControlCenter::~LiveControlCenter() = default;

void LiveControlCenter::start() {
    impl_->start();
    impl_->publish_control_ingress();
    impl_->publish_mining_ingress();
}

void LiveControlCenter::stop() {
    impl_->stop();
}

void LiveControlCenter::set_reward_unit(RewardIntervalUnit unit) {
    impl_->reward_unit.store(unit);
    impl_->publish_control_ingress();
}

RewardIntervalUnit LiveControlCenter::reward_unit() const {
    return impl_->reward_unit.load();
}

void LiveControlCenter::set_audio_enabled(bool enabled) {
    impl_->audio_enabled.store(enabled);
    impl_->publish_control_ingress();
}

bool LiveControlCenter::audio_enabled() const {
    return impl_->audio_enabled.load();
}

void LiveControlCenter::set_paused(bool paused) {
    impl_->paused.store(paused);
    impl_->publish_control_ingress();
}

bool LiveControlCenter::paused() const {
    return impl_->paused.load();
}

void LiveControlCenter::set_compact_layout(bool compact_layout) {
    impl_->compact_layout.store(compact_layout);
    impl_->publish_control_ingress();
}

bool LiveControlCenter::compact_layout() const {
    return impl_->compact_layout.load();
}

void LiveControlCenter::set_always_on_top(bool always_on_top) {
    impl_->always_on_top.store(always_on_top);
    impl_->publish_control_ingress();
}

bool LiveControlCenter::always_on_top() const {
    return impl_->always_on_top.load();
}

void LiveControlCenter::set_mining_settings(const MiningConnectionSettings& settings) {
    std::string previous_connection_id;
    std::string updated_connection_id;
    bool reconnect_requested = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->has_latest_stratum_authority && impl_->latest_stratum_authority.has_connection_ingress) {
            previous_connection_id = impl_->latest_stratum_authority.connection_ingress.connection_id;
        }
        impl_->mining_settings = sanitize_mining_settings(settings);
        updated_connection_id = has_required_mining_fields(impl_->mining_settings)
            ? sanitize_stratum_connection_ingress(build_stratum_connection_ingress(impl_->mining_settings)).connection_id
            : std::string();
        reconnect_requested = impl_->connection_requested;
    }

    if (reconnect_requested && !previous_connection_id.empty() && previous_connection_id != updated_connection_id) {
        impl_->publish_mining_disconnect_for_connection(previous_connection_id, "Stratum session reconfigured by operator");
    }
    impl_->publish_mining_ingress();
}

MiningConnectionSettings LiveControlCenter::mining_settings() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->mining_settings;
}

void LiveControlCenter::start_mining_session() {
    MiningConnectionSettings settings_copy;
    bool should_connect = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        settings_copy = sanitize_mining_settings(impl_->mining_settings);
        if (!has_required_mining_fields(settings_copy)) {
            impl_->last_error = L"Pool host, payout wallet/account, and worker id are required before starting a mining run.";
            impl_->append_recent_event_locked("error | missing mining session settings");
            return;
        }
        should_connect = !impl_->connection_requested;
        impl_->connection_requested = true;
    }

    impl_->begin_mining_session(settings_copy);

    std::ostringstream out;
    if (settings_copy.run_indefinitely) {
        out << "started indefinite run";
    } else {
        out << "started timed run for " << std::fixed << std::setprecision(2)
            << settings_copy.run_duration_minutes << " minutes";
    }
    impl_->log_local_event("mining.session", out.str());

    if (should_connect) {
        impl_->publish_mining_ingress();
    }
}

void LiveControlCenter::stop_mining_session() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->connection_requested = false;
    }
    impl_->clear_mining_session();
    impl_->log_local_event("mining.session", "run stopped by operator");
    impl_->publish_mining_disconnect("Mining run stopped by operator");
}

void LiveControlCenter::connect_pool() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->connection_requested = true;
    }
    impl_->publish_mining_ingress();
}

void LiveControlCenter::disconnect_pool() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->connection_requested = false;
    }
    impl_->clear_mining_session();
    impl_->publish_mining_disconnect("Stratum session disconnected by operator");
}

bool LiveControlCenter::pool_connection_requested() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->connection_requested;
}

bool LiveControlCenter::running() const {
    return impl_->running.load();
}

void LiveControlCenter::ingest_gpu_kernel_iteration(const GpuKernelIterationEvent& event) {
    impl_->ingest_gpu_kernel_iteration(event);
}

void LiveControlCenter::ingest_gpu_mining_validation(const MiningValidationSnapshot& snapshot) {
    impl_->ingest_gpu_mining_validation(snapshot);
}

void LiveControlCenter::inject_feedback_frame(const GpuFeedbackFrame& frame) {
    impl_->inject_feedback_frame(frame);
}

ControlCenterSnapshot LiveControlCenter::snapshot() const {
    impl_->maybe_expire_mining_session();
    return impl_->snapshot_copy();
}

bool LiveControlCenter::export_calibration_bundle(const std::filesystem::path& output_dir) const {
    try {
        return impl_->export_calibration_bundle_to(output_dir);
    } catch (const std::exception& error) {
        impl_->set_error(error.what());
    } catch (...) {
        impl_->set_error("Unknown calibration export failure");
    }
    return false;
}

bool LiveControlCenter::export_device_validation_bundle(const std::filesystem::path& output_dir) const {
    try {
        return impl_->export_device_validation_bundle_to(output_dir);
    } catch (const std::exception& error) {
        impl_->set_error(error.what());
    } catch (...) {
        impl_->set_error("Unknown device validation export failure");
    }
    return false;
}

std::wstring LiveControlCenter::last_error_message() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace qbit_miner
