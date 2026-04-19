#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "qbit_miner/control_center/live_control_center.hpp"
#include "qbit_miner/substrate/photonic_identity.hpp"

namespace {

using qbit_miner::ControlCenterSnapshot;
using qbit_miner::LiveControlCenter;
using qbit_miner::MiningConnectionSettings;
using qbit_miner::MiningPoolPolicy;

struct RunnerOptions {
    MiningConnectionSettings settings;
    double run_seconds = 60.0;
    double poll_interval_seconds = 1.0;
    std::size_t synthetic_pulses_per_poll = 8U;
};

void print_usage() {
    std::cout
        << "Usage: qbit_miner_live_pool_runner --pool-host <host> --payout-address <account> --worker-id <worker> [options]\n"
        << "Options:\n"
        << "  --pool-port <port> (defaults to 1314 for F2Pool, otherwise 3333)\n"
        << "  --worker-password <password>\n"
        << "  --policy <f2pool|2miners>\n"
        << "  --mode <testmode|livemode>\n"
        << "  --auto-promote\n"
        << "  --target-hashrate-hs <value>\n"
        << "  --target-network-share-fraction <value>\n"
        << "  --max-requests-per-second <value>\n"
        << "  --allowed-workers <count>\n"
        << "  --validation-window-s <value>\n"
        << "  --min-validation-samples <count>\n"
        << "  --max-invalid-pool-submissions <count>\n"
        << "  --csv <path>\n"
        << "  --run-seconds <value>\n"
        << "  --poll-interval-seconds <value>\n"
        << "  --synthetic-pulses-per-poll <count>\n";
}

bool consume_value(const std::vector<std::string>& args, std::size_t& index, std::string& value) {
    if ((index + 1U) >= args.size()) {
        return false;
    }
    value = args[++index];
    return true;
}

std::optional<std::uint16_t> parse_port(const std::string& value) {
    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed > 65535UL) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(parsed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::size_t> parse_size(const std::string& value) {
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_double(const std::string& value) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string lowercase_ascii_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool host_matches_f2pool(const std::string& host) {
    const std::string normalized = lowercase_ascii_copy(host);
    return normalized == "f2pool.com"
        || normalized == "btc.f2pool.com"
        || (normalized.size() > std::string(".f2pool.com").size()
            && normalized.ends_with(".f2pool.com"));
}

std::optional<RunnerOptions> parse_args(int argc, char** argv) {
    RunnerOptions options;
    options.settings.allow_live_submit = true;
    options.settings.phase_guided_preview_test_mode = false;
    bool pool_port_explicit = false;
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        std::string value;

        if (arg == "--pool-host") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            options.settings.pool_host = value;
        } else if (arg == "--pool-port") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_port(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.pool_port = *parsed;
            pool_port_explicit = true;
        } else if (arg == "--payout-address") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            options.settings.payout_address = value;
        } else if (arg == "--worker-id") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            options.settings.worker_id = value;
        } else if (arg == "--worker-password") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            options.settings.worker_password = value;
        } else if (arg == "--policy") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            if (value == "f2pool") {
                options.settings.pool_policy = MiningPoolPolicy::F2Pool;
            } else if (value == "2miners") {
                options.settings.pool_policy = MiningPoolPolicy::TwoMiners;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--mode") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            if (value == "testmode") {
                options.settings.allow_live_submit = false;
                options.settings.phase_guided_preview_test_mode = true;
            } else if (value == "livemode") {
                options.settings.allow_live_submit = true;
                options.settings.phase_guided_preview_test_mode = false;
            } else {
                return std::nullopt;
            }
        } else if (arg == "--auto-promote") {
            options.settings.auto_promote_to_live_mode = true;
        } else if (arg == "--target-hashrate-hs") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.target_hashrate_hs = *parsed;
        } else if (arg == "--target-network-share-fraction") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.target_network_share_fraction = *parsed;
        } else if (arg == "--max-requests-per-second") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.max_requests_per_second = *parsed;
        } else if (arg == "--allowed-workers") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_size(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.allowed_worker_count = *parsed;
        } else if (arg == "--validation-window-s") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.validation_jitter_window_seconds = *parsed;
        } else if (arg == "--min-validation-samples") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_size(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.min_validation_jitter_samples = *parsed;
        } else if (arg == "--max-invalid-pool-submissions") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_size(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.settings.max_invalid_pool_submissions = *parsed;
        } else if (arg == "--csv") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            options.settings.validation_log_csv_path = value;
        } else if (arg == "--run-seconds") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.run_seconds = *parsed;
        } else if (arg == "--poll-interval-seconds") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_double(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.poll_interval_seconds = *parsed;
        } else if (arg == "--synthetic-pulses-per-poll") {
            if (!consume_value(args, index, value)) {
                return std::nullopt;
            }
            const auto parsed = parse_size(value);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            options.synthetic_pulses_per_poll = *parsed;
        } else {
            return std::nullopt;
        }
    }

    if (options.settings.pool_host.empty()
        || options.settings.payout_address.empty()
        || options.settings.worker_id.empty()) {
        return std::nullopt;
    }

    if (!pool_port_explicit) {
        options.settings.pool_port =
            options.settings.pool_policy == MiningPoolPolicy::F2Pool || host_matches_f2pool(options.settings.pool_host)
            ? 1314
            : 3333;
    }

    options.settings.run_indefinitely = false;
    options.settings.run_duration_minutes = options.run_seconds / 60.0;
    return options;
}

qbit_miner::GpuFeedbackFrame make_synthetic_feedback_frame(std::uint64_t tick_index) {
    qbit_miner::GpuFeedbackFrame frame;
    frame.photonic_identity.gpu_device_id = "runner-synthetic";
    frame.photonic_identity.coherence = 0.93;
    frame.photonic_identity.memory = 0.88;
    frame.photonic_identity.nexus = 0.51;
    frame.photonic_identity.field_vector.amplitude = 0.20;
    frame.photonic_identity.field_vector.voltage = 0.40;
    frame.photonic_identity.field_vector.current = 0.31;
    frame.photonic_identity.field_vector.frequency = 0.27;
    frame.photonic_identity.field_vector.phase = 0.18;
    frame.photonic_identity.field_vector.flux = 0.24;
    frame.photonic_identity.field_vector.thermal_noise = 0.05;
    frame.photonic_identity.field_vector.field_noise = 0.03;
    frame.photonic_identity.spin_inertia.axis_spin = {0.12, 0.09, -0.08};
    frame.photonic_identity.spin_inertia.axis_orientation = {0.10, 0.07, -0.05};
    frame.photonic_identity.spin_inertia.momentum_score = 0.22;
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = 0.18;
    frame.photonic_identity.spin_inertia.relativistic_correlation = 0.07;
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = 0.66;
    frame.photonic_identity.spin_inertia.temporal_coupling_count = 5;
    frame.timing.tick_index = tick_index;
    frame.timing.request_time_ms = static_cast<double>(tick_index);
    frame.timing.response_time_ms = frame.timing.request_time_ms + 1.7;
    frame.timing.encode_deadline_ms = frame.timing.response_time_ms + 1.8;
    frame.integrated_feedback = 0.44;
    frame.derivative_signal = 0.03;
    frame.lattice_closure = 0.91;
    frame.phase_closure = 0.89;
    frame.recurrence_alignment = 0.83;
    frame.conservation_alignment = 0.998;
    return frame;
}

void print_snapshot(const ControlCenterSnapshot& snapshot) {
    std::cout
        << "state=" << snapshot.mining_status.connection_state
        << " elapsed_s=" << std::fixed << std::setprecision(2) << snapshot.mining_session_elapsed_seconds
        << " tcp_sessions=" << snapshot.tcp_adapter_state.active_session_count
        << " tcp_responses=" << snapshot.tcp_adapter_state.response_count
        << " tcp_server_events=" << snapshot.tcp_adapter_state.server_event_count
        << " share_difficulty=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.difficulty : 0.0)
    << " active_workers=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.active_worker_count : 0U)
    << " submit_ready=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.submit_path_ready) ? 1 : 0)
        << " required_hashrate_hs=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.required_hashrate_hs : 0.0)
        << " measured_hashrate_hs_60s=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.measured_validation_hashrate_hs_60s : 0.0)
        << " valid_shares=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.locally_validated_share_count : 0U)
        << " previews=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.submit_preview_count : 0U)
        << " actuations=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.phase_clamped_actuation_count : 0U)
        << " candidate_actuations=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.phase_clamped_resonant_candidate_count : 0U)
        << " valid_share_candidates=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.phase_clamped_candidate_count : 0U)
        << " verified_harmonics=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.phase_clamped_harmonic_verified_count : 0U)
        << " permitted_actuations=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.phase_clamped_permitted_count : 0U)
        << " accepted_submits=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.accepted_submit_count : 0U)
        << " refused_submits=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.refused_submit_count : 0U)
        << " last_phase_attempted_nonces=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_attempted_nonce_count : 0U)
        << " last_phase_valid_nonces=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_valid_nonce_count : 0U)
        << " last_phase_candidate=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_clamped_resonant_candidate_available) ? 1 : 0)
        << " last_phase_share_valid=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_clamped_valid_share_candidate) ? 1 : 0)
        << " last_phase_harmonics_ok=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_clamped_all_parallel_harmonics_verified) ? 1 : 0)
        << " last_phase_score=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_selected_coherence_score : 0.0)
        << " last_phase_lock_error=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_phase_lock_error : 0.0)
        << " last_phase_clamp=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_phase_clamp_strength : 0.0)
        << " last_phase_target_resonance=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_target_resonance_norm : 0.0)
        << " last_phase_alignment=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_phase_alignment : 0.0)
        << " last_phase_validation_structure=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_validation_structure_norm : 0.0)
        << " phase_program_blocks=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_program_block_count : 0U)
        << " phase_program_native=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_program_substrate_native) ? 1 : 0)
        << " phase_program_same_pulse=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_program_same_pulse_validation) ? 1 : 0)
        << " phase_program_pool_ready=" << ((snapshot.has_stratum_authority && snapshot.stratum_authority.last_phase_program_pool_format_ready) ? 1 : 0)
        << " last_field_phase=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_field_vector_phase_turns : 0.0)
        << " last_transport_phase=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_phase_transport_turns : 0.0)
        << " last_phase_delta=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_phase_lock_delta_turns : 0.0)
        << " gate=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.submit_gate_reason : std::string("none"))
        << " phase_gate=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_clamped_gate_reason : std::string("none"))
        << " phase_program=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_program_title : std::string("none"))
        << " phase_sequence=" << (snapshot.has_stratum_authority ? snapshot.stratum_authority.last_phase_temporal_sequence : std::string("none"))
        << " message=" << snapshot.mining_status.status_message
        << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<RunnerOptions> options = parse_args(argc, argv);
    if (!options.has_value()) {
        print_usage();
        return 2;
    }

    LiveControlCenter control_center;
    control_center.start();
    control_center.set_mining_settings(options->settings);
    control_center.start_mining_session();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(options->run_seconds));
    const auto poll_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(options->poll_interval_seconds));
    auto next_poll = std::chrono::steady_clock::now();
    std::uint64_t synthetic_tick = 1U;

    while (std::chrono::steady_clock::now() < deadline) {
        const ControlCenterSnapshot snapshot = control_center.snapshot();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_poll) {
            print_snapshot(snapshot);
            next_poll = now + poll_interval;
        }
        if (!snapshot.mining_runtime_running && !snapshot.pool_connection_requested) {
            break;
        }

        if (snapshot.has_stratum_authority && snapshot.stratum_authority.has_active_job) {
            for (std::size_t pulse_index = 0; pulse_index < options->synthetic_pulses_per_poll; ++pulse_index) {
                control_center.inject_feedback_frame(make_synthetic_feedback_frame(synthetic_tick++));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const ControlCenterSnapshot final_snapshot = control_center.snapshot();
    print_snapshot(final_snapshot);
    if (!final_snapshot.mining_settings.validation_log_csv_path.empty()) {
        std::cout << "csv=" << final_snapshot.mining_settings.validation_log_csv_path << '\n';
    }

    control_center.stop();

    if (!final_snapshot.has_stratum_authority) {
        return 1;
    }
    if (final_snapshot.stratum_authority.refused_submit_count
        > final_snapshot.stratum_authority.max_invalid_pool_submissions) {
        return 3;
    }
    return 0;
}