#include "qbit_miner/runtime/substrate_firmware_runtime.hpp"

#include "qbit_miner/runtime/substrate_stratum_pow.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qbit_miner {

namespace {

using WorkerValidationHistories = std::array<std::deque<StratumValidationSample>, kStratumWorkerSlotCount>;

constexpr double kValidationHashrateWindowSeconds = 60.0;

struct WorkerValidationSampleStats {
    std::size_t sample_count = 0;
    double share_rate_per_s = 0.0;
    double hashrate_hs_60s = 0.0;
    double jitter_s = 0.0;
    double jitter_fraction = 0.0;
};

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double clamp_signed(double value, double limit = 1.0) {
    return std::clamp(value, -std::abs(limit), std::abs(limit));
}

[[nodiscard]] double wrap_turns(double turns) {
    double wrapped = std::fmod(turns, 1.0);
    if (wrapped < 0.0) {
        wrapped += 1.0;
    }
    return wrapped;
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

[[nodiscard]] double now_unix_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

[[nodiscard]] std::string format_timestamp_utc(double unix_seconds) {
    const auto whole_seconds = static_cast<std::time_t>(unix_seconds);
    const double fractional = std::max(0.0, unix_seconds - static_cast<double>(whole_seconds));
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &whole_seconds);
#else
    gmtime_r(&whole_seconds, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    stream << '.' << std::setw(3) << std::setfill('0')
           << static_cast<int>(std::floor(fractional * 1000.0 + 0.5))
           << 'Z';
    return stream.str();
}

[[nodiscard]] std::string csv_escape(const std::string& value) {
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::string sanitize_path_segment(std::string value) {
    std::replace_if(value.begin(), value.end(), [](char ch) {
        return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.');
    }, '_');
    if (value.empty()) {
        return "session";
    }
    return value;
}

[[nodiscard]] std::string default_validation_log_csv_path(const SubstrateStratumAuthorityState& authority_state) {
    const std::filesystem::path directory = std::filesystem::current_path() / "runtime_logs";
    const std::string mode = authority_state.operating_mode.empty() ? "runtime" : authority_state.operating_mode;
    const std::string worker = authority_state.connection_ingress.worker_name.empty()
        ? authority_state.connection_ingress.connection_id
        : authority_state.connection_ingress.worker_name;
    return (directory / ("stratum_validation_" + sanitize_path_segment(mode) + "_"
        + sanitize_path_segment(worker) + ".csv")).string();
}

void prune_validation_history(
    std::deque<StratumValidationSample>& validation_history,
    double retention_window_seconds,
    double now_seconds
) {
    while (!validation_history.empty()
        && (now_seconds - validation_history.front().timestamp_unix_s) > retention_window_seconds) {
        validation_history.pop_front();
    }
}

void prune_worker_validation_histories(
    WorkerValidationHistories& worker_validation_histories,
    double validation_window_seconds,
    double now_seconds
) {
    const double retention_window_seconds = std::max(
        validation_window_seconds,
        kValidationHashrateWindowSeconds);
    for (auto& history : worker_validation_histories) {
        prune_validation_history(history, retention_window_seconds, now_seconds);
    }
}

[[nodiscard]] WorkerValidationSampleStats compute_worker_validation_sample_stats(
    const std::deque<StratumValidationSample>& validation_history,
    double validation_window_seconds,
    double now_seconds
) {
    WorkerValidationSampleStats stats;
    const double validation_window_start = now_seconds - std::max(0.0, validation_window_seconds);
    std::vector<double> validation_timestamps;
    validation_timestamps.reserve(validation_history.size());
    for (const StratumValidationSample& sample : validation_history) {
        if ((now_seconds - sample.timestamp_unix_s) <= kValidationHashrateWindowSeconds) {
            stats.hashrate_hs_60s += sample.expected_hashes / kValidationHashrateWindowSeconds;
        }
        if (sample.timestamp_unix_s >= validation_window_start) {
            validation_timestamps.push_back(sample.timestamp_unix_s);
        }
    }

    stats.sample_count = validation_timestamps.size();
    if (validation_timestamps.size() == 1U && validation_window_seconds > 0.0) {
        stats.share_rate_per_s = 1.0 / validation_window_seconds;
        return stats;
    }
    if (validation_timestamps.size() < 2U) {
        return stats;
    }

    const double window_duration_s = validation_timestamps.back() - validation_timestamps.front();
    if (window_duration_s > 0.0) {
        stats.share_rate_per_s =
            static_cast<double>(validation_timestamps.size() - 1U) / window_duration_s;
    }

    std::vector<double> intervals;
    intervals.reserve(validation_timestamps.size() - 1U);
    for (std::size_t index = 1; index < validation_timestamps.size(); ++index) {
        intervals.push_back(validation_timestamps[index] - validation_timestamps[index - 1U]);
    }
    if (intervals.empty()) {
        return stats;
    }

    const double mean_interval_s = std::accumulate(intervals.begin(), intervals.end(), 0.0)
        / static_cast<double>(intervals.size());
    if (mean_interval_s <= 0.0) {
        return stats;
    }

    double variance = 0.0;
    for (double interval_s : intervals) {
        const double delta = interval_s - mean_interval_s;
        variance += delta * delta;
    }
    variance /= static_cast<double>(intervals.size());
    stats.jitter_s = std::sqrt(std::max(0.0, variance));
    stats.jitter_fraction = clamp_unit(stats.jitter_s / mean_interval_s);
    return stats;
}

void recompute_validation_metrics(
    SubstrateStratumAuthorityState& authority_state,
    const WorkerValidationHistories& worker_validation_histories,
    double now_seconds
) {
    authority_state.validation_sample_count = 0U;
    authority_state.measured_validation_share_rate_per_s = 0.0;
    authority_state.mean_worker_validation_share_rate_per_s = 0.0;
    authority_state.measured_validation_hashrate_hs_60s = 0.0;
    authority_state.mean_worker_validation_hashrate_hs_60s = 0.0;
    authority_state.measured_validation_jitter_s = 0.0;
    authority_state.measured_validation_jitter_fraction = 0.0;
    authority_state.max_worker_validation_jitter_s = 0.0;
    authority_state.max_worker_validation_jitter_fraction = 0.0;
    authority_state.submission_interval_target_s = 0.0;
    authority_state.required_share_submissions_per_s_per_worker = 0.0;
    authority_state.required_share_submissions_per_pool_window = 0.0;
    authority_state.required_share_submissions_per_worker_pool_window = 0.0;
    authority_state.effective_request_budget_per_s = 0.0;
    authority_state.workers_with_validation_samples = 0U;
    authority_state.workers_meeting_validation_sample_threshold = 0U;
    authority_state.workers_meeting_target_rate_count = 0U;
    authority_state.min_worker_validation_sample_count = 0U;
    authority_state.worker_validation_sample_counts = {};
    authority_state.worker_measured_validation_share_rate_per_s = {};
    authority_state.worker_measured_validation_hashrate_hs_60s = {};
    authority_state.worker_measured_validation_jitter_s = {};
    authority_state.worker_measured_validation_jitter_fraction = {};
    authority_state.worker_submission_interval_target_s = {};

    if (authority_state.active_worker_count == 0U) {
        const double pool_window_seconds = authority_state.pool_policy.hashrate_window_seconds > 0.0
            ? authority_state.pool_policy.hashrate_window_seconds
            : authority_state.validation_jitter_window_seconds;
        authority_state.required_share_submissions_per_pool_window =
            authority_state.required_share_submissions_per_s * std::max(0.0, pool_window_seconds);
        return;
    }

    authority_state.required_share_submissions_per_s_per_worker = authority_state.required_share_submissions_per_s > 0.0
        ? (authority_state.required_share_submissions_per_s / static_cast<double>(authority_state.active_worker_count))
        : 0.0;
    const double pool_window_seconds = authority_state.pool_policy.hashrate_window_seconds > 0.0
        ? authority_state.pool_policy.hashrate_window_seconds
        : authority_state.validation_jitter_window_seconds;
    authority_state.required_share_submissions_per_pool_window =
        authority_state.required_share_submissions_per_s * std::max(0.0, pool_window_seconds);
    authority_state.required_share_submissions_per_worker_pool_window =
        authority_state.required_share_submissions_per_s_per_worker * std::max(0.0, pool_window_seconds);
    authority_state.submission_interval_target_s = authority_state.required_share_submissions_per_s_per_worker > 0.0
        ? (1.0 / authority_state.required_share_submissions_per_s_per_worker)
        : 0.0;
    authority_state.effective_request_budget_per_s = authority_state.transport_connected
        ? authority_state.max_requests_per_second
        : 0.0;
    const double required_hashrate_hs_per_worker = authority_state.required_hashrate_hs > 0.0
        ? (authority_state.required_hashrate_hs / static_cast<double>(authority_state.active_worker_count))
        : 0.0;

    std::size_t min_sample_count = std::numeric_limits<std::size_t>::max();
    for (std::size_t worker_index = 0; worker_index < authority_state.active_worker_count; ++worker_index) {
        const WorkerValidationSampleStats stats =
            compute_worker_validation_sample_stats(
                worker_validation_histories[worker_index],
                authority_state.validation_jitter_window_seconds,
                now_seconds);
        authority_state.worker_validation_sample_counts[worker_index] = stats.sample_count;
        authority_state.worker_measured_validation_share_rate_per_s[worker_index] = stats.share_rate_per_s;
        authority_state.worker_measured_validation_hashrate_hs_60s[worker_index] = stats.hashrate_hs_60s;
        authority_state.worker_measured_validation_jitter_s[worker_index] = stats.jitter_s;
        authority_state.worker_measured_validation_jitter_fraction[worker_index] = stats.jitter_fraction;
        authority_state.worker_submission_interval_target_s[worker_index] = authority_state.submission_interval_target_s;

        authority_state.validation_sample_count += stats.sample_count;
        if (stats.sample_count > 0U) {
            ++authority_state.workers_with_validation_samples;
        }
        if (stats.sample_count >= authority_state.min_validation_jitter_samples) {
            ++authority_state.workers_meeting_validation_sample_threshold;
        }
        if ((required_hashrate_hs_per_worker > 0.0
                && stats.hashrate_hs_60s + 1.0e-12 >= required_hashrate_hs_per_worker)
            || (required_hashrate_hs_per_worker <= 0.0
                && (authority_state.required_share_submissions_per_s_per_worker <= 0.0
                    || stats.share_rate_per_s + 1.0e-12 >= authority_state.required_share_submissions_per_s_per_worker))) {
            ++authority_state.workers_meeting_target_rate_count;
        }

        authority_state.measured_validation_share_rate_per_s += stats.share_rate_per_s;
        authority_state.measured_validation_hashrate_hs_60s += stats.hashrate_hs_60s;
        authority_state.max_worker_validation_jitter_s =
            std::max(authority_state.max_worker_validation_jitter_s, stats.jitter_s);
        authority_state.max_worker_validation_jitter_fraction =
            std::max(authority_state.max_worker_validation_jitter_fraction, stats.jitter_fraction);
        min_sample_count = std::min(min_sample_count, stats.sample_count);
    }

    authority_state.min_worker_validation_sample_count =
        min_sample_count == std::numeric_limits<std::size_t>::max() ? 0U : min_sample_count;
    authority_state.mean_worker_validation_share_rate_per_s =
        authority_state.measured_validation_share_rate_per_s / static_cast<double>(authority_state.active_worker_count);
    authority_state.mean_worker_validation_hashrate_hs_60s =
        authority_state.measured_validation_hashrate_hs_60s / static_cast<double>(authority_state.active_worker_count);
    authority_state.measured_validation_jitter_s = authority_state.max_worker_validation_jitter_s;
    authority_state.measured_validation_jitter_fraction = authority_state.max_worker_validation_jitter_fraction;
}

void refresh_submission_control(
    SubstrateStratumAuthorityState& authority_state,
    const WorkerValidationHistories& worker_validation_histories,
    double now_seconds
) {
    recompute_validation_metrics(authority_state, worker_validation_histories, now_seconds);

    if (authority_state.phase_guided_preview_test_mode) {
        authority_state.submit_path_ready = false;
        if (authority_state.submit_preview_count == 0U) {
            authority_state.submit_gate_reason = "phase_guided_preview_required";
        }
        return;
    }

    if (!authority_state.submit_policy_enabled) {
        authority_state.submit_path_ready = false;
        authority_state.submit_gate_reason = "live_submit_disabled";
        return;
    }

    if (!authority_state.transport_connected || !authority_state.network_authority_granted) {
        authority_state.submit_path_ready = false;
        authority_state.submit_gate_reason = "live_submit_transport_unready";
        return;
    }

    if (!authority_state.has_difficulty || !authority_state.has_active_job
        || authority_state.active_job_nbits.empty() || authority_state.active_share_target_hex.empty()) {
        authority_state.submit_path_ready = false;
        authority_state.submit_gate_reason = "live_submit_job_unready";
        return;
    }

    if (authority_state.active_worker_count == 0U) {
        authority_state.submit_path_ready = false;
        authority_state.submit_gate_reason = "no_active_workers";
        return;
    }

    if (authority_state.required_share_submissions_per_s > authority_state.effective_request_budget_per_s) {
        authority_state.submit_path_ready = false;
        authority_state.submit_gate_reason = "live_submit_armed_with_rate_deficit";
        return;
    }

    authority_state.submit_path_ready = true;
    authority_state.submit_gate_reason = "phase_clamped_share_actuation_required";
}

void append_validation_log_row(
    const SubstrateStratumAuthorityState& authority_state,
    double timestamp_unix_s,
    const std::string& event_label,
    const std::string& worker_name,
    std::size_t worker_index,
    const std::string& job_id,
    const std::string& nonce_hex,
    const std::string& hash_hex,
    double share_difficulty,
    bool valid_share,
    bool block_candidate_valid,
    bool network_send_allowed,
    const std::string& gate_reason
) {
    const std::filesystem::path csv_path = authority_state.validation_log_csv_path.empty()
        ? std::filesystem::path(default_validation_log_csv_path(authority_state))
        : std::filesystem::path(authority_state.validation_log_csv_path);
    if (!csv_path.parent_path().empty()) {
        std::filesystem::create_directories(csv_path.parent_path());
    }

    const bool file_exists = std::filesystem::exists(csv_path);
    std::ofstream output(csv_path, std::ios::binary | std::ios::app);
    if (!output) {
        return;
    }

    if (!file_exists) {
        output
            << "timestamp_utc,timestamp_unix_s,mode,event,connection_id,job_id,worker_index,worker_name,"
            << "nonce_hex,hash_hex,valid_share,block_candidate_valid,share_difficulty,block_difficulty,"
            << "required_hashrate_hs,target_hashrate_hs,allowed_worker_count,active_worker_count,"
            << "required_share_submissions_per_s,required_share_submissions_per_s_per_worker,"
            << "required_share_submissions_per_pool_window,required_share_submissions_per_worker_pool_window,"
            << "measured_validation_share_rate_per_s,mean_worker_validation_share_rate_per_s,"
            << "measured_validation_hashrate_hs_60s,mean_worker_validation_hashrate_hs_60s,"
            << "worker_measured_validation_share_rate_per_s,worker_measured_validation_hashrate_hs_60s,"
            << "measured_validation_jitter_s,"
            << "measured_validation_jitter_fraction,worker_measured_validation_jitter_s,"
            << "worker_measured_validation_jitter_fraction,submission_interval_target_s,validation_sample_count,"
            << "worker_validation_sample_count,effective_request_budget_per_s,"
            << "pool_policy_name,pool_hashrate_window_seconds,pool_connection_cap,"
            << "pool_connection_cap_unbounded,pool_supports_client_suggest_difficulty,"
            << "cumulative_valid_shares,accepted_submit_count,refused_submit_count,max_invalid_pool_submissions,"
            << "network_send_allowed,gate_reason\n";
    }

    const std::size_t safe_worker_index = std::min(worker_index, kStratumWorkerSlotCount - 1U);

    output
        << csv_escape(format_timestamp_utc(timestamp_unix_s)) << ','
        << std::fixed << std::setprecision(6) << timestamp_unix_s << ','
        << csv_escape(authority_state.operating_mode) << ','
        << csv_escape(event_label) << ','
        << csv_escape(authority_state.connection_ingress.connection_id) << ','
        << csv_escape(job_id) << ','
        << worker_index << ','
        << csv_escape(worker_name) << ','
        << csv_escape(nonce_hex) << ','
        << csv_escape(hash_hex) << ','
        << (valid_share ? "true" : "false") << ','
        << (block_candidate_valid ? "true" : "false") << ','
        << share_difficulty << ','
        << authority_state.active_block_difficulty << ','
        << authority_state.required_hashrate_hs << ','
        << authority_state.target_hashrate_hs << ','
        << authority_state.allowed_worker_count << ','
        << authority_state.active_worker_count << ','
        << authority_state.required_share_submissions_per_s << ','
        << authority_state.required_share_submissions_per_s_per_worker << ','
        << authority_state.required_share_submissions_per_pool_window << ','
        << authority_state.required_share_submissions_per_worker_pool_window << ','
        << authority_state.measured_validation_share_rate_per_s << ','
        << authority_state.mean_worker_validation_share_rate_per_s << ','
        << authority_state.measured_validation_hashrate_hs_60s << ','
        << authority_state.mean_worker_validation_hashrate_hs_60s << ','
        << authority_state.worker_measured_validation_share_rate_per_s[safe_worker_index] << ','
        << authority_state.worker_measured_validation_hashrate_hs_60s[safe_worker_index] << ','
        << authority_state.measured_validation_jitter_s << ','
        << authority_state.measured_validation_jitter_fraction << ','
        << authority_state.worker_measured_validation_jitter_s[safe_worker_index] << ','
        << authority_state.worker_measured_validation_jitter_fraction[safe_worker_index] << ','
        << authority_state.submission_interval_target_s << ','
        << authority_state.validation_sample_count << ','
        << authority_state.worker_validation_sample_counts[safe_worker_index] << ','
        << authority_state.effective_request_budget_per_s << ','
        << csv_escape(authority_state.pool_policy.pool_name) << ','
        << authority_state.pool_policy.hashrate_window_seconds << ','
        << authority_state.pool_policy.connection_cap << ','
        << (authority_state.pool_policy.connection_cap_unbounded ? "true" : "false") << ','
        << (authority_state.pool_policy.supports_client_suggest_difficulty ? "true" : "false") << ','
        << authority_state.locally_validated_share_count << ','
        << authority_state.accepted_submit_count << ','
        << authority_state.refused_submit_count << ','
        << authority_state.max_invalid_pool_submissions << ','
        << (network_send_allowed ? "true" : "false") << ','
        << csv_escape(gate_reason)
        << '\n';
}

void record_local_validation_sample(
    SubstrateStratumAuthorityState& authority_state,
    WorkerValidationHistories& worker_validation_histories,
    double timestamp_unix_s,
    const std::string& event_label,
    const std::string& worker_name,
    std::size_t worker_index,
    const std::string& job_id,
    const std::string& nonce_hex,
    const std::string& hash_hex,
    double share_difficulty,
    bool valid_share,
    bool block_candidate_valid,
    bool network_send_allowed,
    const std::string& gate_reason
) {
    authority_state.last_validation_timestamp_unix_s = timestamp_unix_s;
    if (valid_share) {
        ++authority_state.locally_validated_share_count;
        if (worker_index < worker_validation_histories.size()) {
            const double effective_share_difficulty = share_difficulty > 0.0
                ? share_difficulty
                : authority_state.difficulty;
            worker_validation_histories[worker_index].push_back(StratumValidationSample{
                timestamp_unix_s,
                effective_share_difficulty,
                expected_hashes_for_difficulty(effective_share_difficulty),
            });
        }
    }
    prune_worker_validation_histories(
        worker_validation_histories,
        authority_state.validation_jitter_window_seconds,
        timestamp_unix_s);
    refresh_submission_control(authority_state, worker_validation_histories, timestamp_unix_s);
    append_validation_log_row(
        authority_state,
        timestamp_unix_s,
        event_label,
        worker_name,
        worker_index,
        job_id,
        nonce_hex,
        hash_hex,
        share_difficulty,
        valid_share,
        block_candidate_valid,
        network_send_allowed,
        gate_reason);
}

[[nodiscard]] bool is_phase_guided_preview_mode(const SubstrateStratumAuthorityState& authority_state) {
    return authority_state.has_connection_ingress
    && authority_state.phase_guided_preview_test_mode;
}

[[nodiscard]] bool should_auto_promote_to_live_mode(const SubstrateStratumAuthorityState& authority_state) {
    return authority_state.phase_guided_preview_test_mode
        && authority_state.auto_promote_to_live_mode
        && authority_state.active_worker_count > 0U
        && authority_state.offline_valid_submit_preview_count > 0U
        && authority_state.required_hashrate_hs > 0.0
        && authority_state.workers_meeting_validation_sample_threshold >= authority_state.active_worker_count
        && authority_state.measured_validation_hashrate_hs_60s + 1.0e-12 >= authority_state.required_hashrate_hs;
}

[[nodiscard]] SubstrateStratumConnectionIngress build_promoted_live_mode_ingress(
    const SubstrateStratumAuthorityState& authority_state
) {
    SubstrateStratumConnectionIngress ingress = authority_state.connection_ingress;
    ingress.operating_mode = "livemode";
    ingress.dry_run_only = false;
    ingress.allow_live_submit = true;
    ingress.phase_guided_preview_test_mode = false;
    ingress.auto_promote_to_live_mode = false;
    return ingress;
}

[[nodiscard]] SubstrateStratumConnectionControl build_invalid_submit_disconnect_control(
    const SubstrateStratumAuthorityState& authority_state,
    std::size_t refused_submit_count
) {
    std::ostringstream stream;
    stream << "Exceeded invalid live Stratum share threshold after "
           << refused_submit_count
           << " pool-refused submissions";

    SubstrateStratumConnectionControl control;
    control.connection_id = authority_state.connection_ingress.connection_id;
    control.disconnect_requested = true;
    control.reason = stream.str();
    return control;
}

[[nodiscard]] SubstrateStratumConnectionControl build_auto_promotion_disconnect_control(
    const SubstrateStratumAuthorityState& authority_state
) {
    SubstrateStratumConnectionControl control;
    control.connection_id = authority_state.connection_ingress.connection_id;
    control.disconnect_requested = true;
    control.reason = "Phase-guided header resonance reached the target hashrate envelope; reconnecting in livemode";
    return control;
}

[[nodiscard]] std::string resolve_operating_mode(const SubstrateStratumConnectionIngress& ingress) {
    if (!ingress.operating_mode.empty()) {
        return ingress.operating_mode;
    }
    if (ingress.allow_live_submit && !ingress.dry_run_only) {
        return "livemode";
    }
    if (ingress.phase_guided_preview_test_mode || ingress.dry_run_only) {
        return "testmode";
    }
    return {};
}

[[nodiscard]] SubstrateStratumWorkerAssignment resolve_preview_assignment(
    const SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation
) {
    for (const auto& assignment : authority_state.worker_assignments) {
        if (assignment.active && assignment.worker_index == actuation.worker_index) {
            return assignment;
        }
    }

    SubstrateStratumWorkerAssignment fallback;
    fallback.worker_index = actuation.worker_index;
    fallback.active = true;
    fallback.job_id = actuation.job_id;
    fallback.worker_name = actuation.worker_name;
    fallback.nonce_start = actuation.nonce_start;
    fallback.nonce_end = actuation.nonce_end;
    return fallback;
}

struct HarmonicCandidateLocalValidation {
    bool attempted = false;
    PhaseClampedShareActuation actuation;
    SubstrateStratumPowPhaseTrace phase_trace;
    bool nonce_parsed = false;
    bool hash_mismatch = false;
    bool valid = false;
};

[[nodiscard]] bool actuation_has_local_validation_candidates(
    const PhaseClampedShareActuation& actuation
) {
    return !actuation.nonce_hex.empty() || !actuation.sampled_valid_nonce_hexes.empty();
}

[[nodiscard]] std::string normalize_hex_for_phase_trace(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if ((normalized.size() % 2U) != 0U) {
        normalized.insert(normalized.begin(), '0');
    }
    return normalized;
}

[[nodiscard]] SubstrateStratumPhaseFluxMeasurement build_phase_trace_measurement_from_actuation(
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
    if (actuation.has_sha256_phase_trace && actuation.sha256_phase_trace.performed) {
        measurement.sha256_schedule_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_schedule_phase_turns;
        measurement.sha256_round_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_round_phase_turns;
        measurement.sha256_digest_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_digest_phase_turns;
        measurement.sha256_frequency_bias_norm =
            actuation.sha256_phase_trace.resonant_measurement.sha256_frequency_bias_norm;
        measurement.sha256_harmonic_density_norm =
            actuation.sha256_phase_trace.resonant_measurement.sha256_harmonic_density_norm;
    }
    return measurement;
}

void apply_phase_trace_to_actuation(
    PhaseClampedShareActuation& actuation,
    const SubstrateStratumPowPhaseTrace& phase_trace
) {
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

[[nodiscard]] HarmonicCandidateLocalValidation validate_harmonic_candidates_locally(
    const SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation
) {
    HarmonicCandidateLocalValidation result;
    result.actuation = actuation;

    const SubstrateStratumWorkerAssignment assignment = resolve_preview_assignment(authority_state, actuation);
    const std::string worker_header_hex = resolve_stratum_job_header_hex(authority_state, assignment);
    if (worker_header_hex.empty() || authority_state.active_job_nbits.empty()) {
        return result;
    }

    std::vector<std::string> candidate_nonces;
    candidate_nonces.reserve(1U + actuation.sampled_valid_nonce_hexes.size());
    std::unordered_set<std::string> seen_nonces;
    seen_nonces.reserve(1U + actuation.sampled_valid_nonce_hexes.size());

    const auto enqueue_nonce = [&candidate_nonces, &seen_nonces](const std::string& nonce_hex) {
        if (nonce_hex.empty()) {
            return;
        }
        if (!seen_nonces.insert(nonce_hex).second) {
            return;
        }
        candidate_nonces.push_back(nonce_hex);
    };

    const bool explicit_selected_candidate = !actuation.nonce_hex.empty()
        && (!actuation.hash_hex.empty()
            || actuation.resonant_candidate_available
            || actuation.valid_share_candidate);
    enqueue_nonce(actuation.nonce_hex);
    if (!explicit_selected_candidate) {
        for (const std::string& sampled_nonce_hex : actuation.sampled_valid_nonce_hexes) {
            enqueue_nonce(sampled_nonce_hex);
        }
    }

    const SubstrateStratumPhaseFluxMeasurement trace_measurement =
        build_phase_trace_measurement_from_actuation(actuation);

    for (const std::string& candidate_nonce_hex : candidate_nonces) {
        std::uint32_t nonce_value = 0U;
        if (!try_parse_stratum_nonce_hex(candidate_nonce_hex, nonce_value)) {
            continue;
        }

        const bool selected_nonce = candidate_nonce_hex == actuation.nonce_hex;
        const SubstrateStratumPowPhaseTrace phase_trace = trace_stratum_pow_phase(
            worker_header_hex,
            authority_state.active_job_nbits,
            nonce_value,
            authority_state.difficulty,
            trace_measurement
        );
        if (!phase_trace.performed) {
            continue;
        }

        const std::string normalized_reported_hash =
            selected_nonce ? normalize_hex_for_phase_trace(actuation.hash_hex) : std::string{};
        const bool hash_mismatch = !normalized_reported_hash.empty()
            && normalized_reported_hash != phase_trace.evaluation.hash_hex;
        const bool valid = phase_trace.evaluation.valid_share && !hash_mismatch;
        result.attempted = true;
        if (!result.nonce_parsed) {
            result.phase_trace = phase_trace;
            result.nonce_parsed = true;
            result.hash_mismatch = hash_mismatch;
            result.valid = valid;
            result.actuation.nonce_hex = candidate_nonce_hex;
            apply_phase_trace_to_actuation(result.actuation, phase_trace);
        }

        if (!valid) {
            continue;
        }

        result.phase_trace = phase_trace;
        result.nonce_parsed = true;
        result.hash_mismatch = false;
        result.valid = true;
        result.actuation.nonce_hex = candidate_nonce_hex;
        apply_phase_trace_to_actuation(result.actuation, phase_trace);
        result.actuation.valid_share_candidate = true;
        return result;
    }

    return result;
}

void populate_preview_collapse_feedback(
    SubstrateStratumSubmitPreviewPayload& preview,
    bool measured_nonce_observed,
    const SubstrateStratumPowCollapseFeedback& collapse_feedback
) {
    preview.measured_nonce_observed = measured_nonce_observed;
    preview.measured_hash_phase_turns = collapse_feedback.measured_hash_phase_turns;
    preview.measured_nonce_phase_turns = collapse_feedback.measured_nonce_phase_turns;
    preview.collapse_feedback_phase_turns = collapse_feedback.feedback_phase_turns;
    preview.collapse_relock_error_turns = collapse_feedback.relock_error_turns;
    preview.observer_collapse_strength = collapse_feedback.observer_collapse_strength;
    preview.phase_flux_conservation = collapse_feedback.phase_flux_conservation;
    preview.nonce_collapse_confidence = collapse_feedback.nonce_collapse_confidence;
}

void persist_authority_collapse_feedback(
    SubstrateStratumAuthorityState& authority_state,
    bool measured_nonce_observed,
    const SubstrateStratumPowCollapseFeedback& collapse_feedback
) {
    if (!measured_nonce_observed) {
        return;
    }

    authority_state.last_measured_nonce_observed = true;
    authority_state.last_measured_hash_phase_turns = collapse_feedback.measured_hash_phase_turns;
    authority_state.last_measured_nonce_phase_turns = collapse_feedback.measured_nonce_phase_turns;
    authority_state.last_collapse_feedback_phase_turns = collapse_feedback.feedback_phase_turns;
    authority_state.last_collapse_relock_error_turns = collapse_feedback.relock_error_turns;
    authority_state.last_observer_collapse_strength = collapse_feedback.observer_collapse_strength;
    authority_state.last_phase_flux_conservation = collapse_feedback.phase_flux_conservation;
    authority_state.last_nonce_collapse_confidence = collapse_feedback.nonce_collapse_confidence;
}

void persist_authority_collapse_feedback(
    SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation
) {
    if (!actuation.measured_nonce_observed) {
        return;
    }

    authority_state.last_measured_nonce_observed = true;
    authority_state.last_measured_hash_phase_turns = actuation.measured_hash_phase_turns;
    authority_state.last_measured_nonce_phase_turns = actuation.measured_nonce_phase_turns;
    authority_state.last_collapse_feedback_phase_turns = actuation.collapse_feedback_phase_turns;
    authority_state.last_collapse_relock_error_turns = actuation.collapse_relock_error_turns;
    authority_state.last_observer_collapse_strength = actuation.observer_collapse_strength;
    authority_state.last_phase_flux_conservation = actuation.phase_flux_conservation;
    authority_state.last_nonce_collapse_confidence = actuation.nonce_collapse_confidence;
    authority_state.last_target_resonance_norm = actuation.target_resonance_norm;
    authority_state.last_phase_clamped_validation_structure_norm = actuation.validation_structure_norm;
    authority_state.last_phase_clamped_transfer_drive_norm = actuation.transfer_drive_norm;
    authority_state.last_phase_clamped_stability_gate_norm = actuation.stability_gate_norm;
    authority_state.last_phase_clamped_damping_norm = actuation.damping_norm;
    authority_state.last_phase_clamped_transport_drive_norm = actuation.transport_drive_norm;
    authority_state.last_phase_clamped_resonance_activation_norm = actuation.resonance_activation_norm;
    authority_state.last_phase_clamped_temporal_admissibility = actuation.temporal_admissibility;
    authority_state.last_phase_clamped_zero_point_proximity = actuation.zero_point_proximity;
    authority_state.last_phase_clamped_transport_readiness = actuation.transport_readiness;
    authority_state.last_phase_clamped_share_confidence = actuation.share_confidence;
    authority_state.last_phase_clamped_parallel_harmonic_count = actuation.parallel_harmonic_count;
    authority_state.last_phase_clamped_verified_parallel_harmonic_count =
        actuation.verified_parallel_harmonic_count;
    authority_state.last_phase_clamped_validated_parallel_harmonic_count =
        actuation.validated_parallel_harmonic_count;
    authority_state.has_last_phase_trace =
        actuation.has_sha256_phase_trace && actuation.sha256_phase_trace.performed;
    if (authority_state.has_last_phase_trace) {
        authority_state.last_phase_trace_nonce_seed_phase_turns =
            actuation.sha256_phase_trace.nonce_seed_phase_turns;
        authority_state.last_phase_trace_header_phase_turns =
            actuation.sha256_phase_trace.header_phase_turns;
        authority_state.last_phase_trace_share_target_phase_turns =
            actuation.sha256_phase_trace.share_target_phase_turns;
        authority_state.last_phase_trace_block_target_phase_turns =
            actuation.sha256_phase_trace.block_target_phase_turns;
        authority_state.last_phase_trace_sha256_schedule_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_schedule_phase_turns;
        authority_state.last_phase_trace_sha256_round_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_round_phase_turns;
        authority_state.last_phase_trace_sha256_digest_phase_turns =
            actuation.sha256_phase_trace.resonant_measurement.sha256_digest_phase_turns;
    } else {
        authority_state.last_phase_trace_nonce_seed_phase_turns = 0.0;
        authority_state.last_phase_trace_header_phase_turns = 0.0;
        authority_state.last_phase_trace_share_target_phase_turns = 0.0;
        authority_state.last_phase_trace_block_target_phase_turns = 0.0;
        authority_state.last_phase_trace_sha256_schedule_phase_turns = 0.0;
        authority_state.last_phase_trace_sha256_round_phase_turns = 0.0;
        authority_state.last_phase_trace_sha256_digest_phase_turns = 0.0;
    }
    authority_state.last_phase_program_title = actuation.phase_program_title;
    authority_state.last_phase_program_generated_dir = actuation.phase_program_generated_dir;
    authority_state.last_phase_program_block_count = actuation.phase_program_block_count;
    authority_state.last_phase_program_substrate_native = actuation.phase_program_substrate_native;
    authority_state.last_phase_program_same_pulse_validation =
        actuation.phase_program_same_pulse_validation;
    authority_state.last_phase_program_pool_format_ready = actuation.phase_program_pool_format_ready;
}

void clear_authority_collapse_feedback(SubstrateStratumAuthorityState& authority_state) {
    authority_state.last_measured_nonce_observed = false;
    authority_state.last_measured_hash_phase_turns = 0.0;
    authority_state.last_measured_nonce_phase_turns = 0.0;
    authority_state.last_collapse_feedback_phase_turns = 0.0;
    authority_state.last_collapse_relock_error_turns = 0.0;
    authority_state.last_observer_collapse_strength = 0.0;
    authority_state.last_phase_flux_conservation = 0.0;
    authority_state.last_nonce_collapse_confidence = 0.0;
    authority_state.last_target_resonance_norm = 0.0;
    authority_state.last_phase_clamped_validation_structure_norm = 0.0;
    authority_state.last_phase_clamped_transfer_drive_norm = 0.0;
    authority_state.last_phase_clamped_stability_gate_norm = 0.0;
    authority_state.last_phase_clamped_damping_norm = 0.0;
    authority_state.last_phase_clamped_transport_drive_norm = 0.0;
    authority_state.last_phase_clamped_resonance_activation_norm = 0.0;
    authority_state.last_phase_clamped_temporal_admissibility = 0.0;
    authority_state.last_phase_clamped_zero_point_proximity = 0.0;
    authority_state.last_phase_clamped_transport_readiness = 0.0;
    authority_state.last_phase_clamped_share_confidence = 0.0;
    authority_state.last_phase_clamped_parallel_harmonic_count = 0U;
    authority_state.last_phase_clamped_verified_parallel_harmonic_count = 0U;
    authority_state.last_phase_clamped_validated_parallel_harmonic_count = 0U;
    authority_state.has_last_phase_trace = false;
    authority_state.last_phase_trace_nonce_seed_phase_turns = 0.0;
    authority_state.last_phase_trace_header_phase_turns = 0.0;
    authority_state.last_phase_trace_share_target_phase_turns = 0.0;
    authority_state.last_phase_trace_block_target_phase_turns = 0.0;
    authority_state.last_phase_trace_sha256_schedule_phase_turns = 0.0;
    authority_state.last_phase_trace_sha256_round_phase_turns = 0.0;
    authority_state.last_phase_trace_sha256_digest_phase_turns = 0.0;
    authority_state.last_phase_program_title.clear();
    authority_state.last_phase_program_generated_dir.clear();
    authority_state.last_phase_program_block_count = 0U;
    authority_state.last_phase_program_substrate_native = false;
    authority_state.last_phase_program_same_pulse_validation = false;
    authority_state.last_phase_program_pool_format_ready = false;
}

[[nodiscard]] SubstrateStratumPhaseFluxMeasurement build_authority_phase_flux_measurement(
    const SubstrateStratumAuthorityState& authority_state
) {
    SubstrateStratumPhaseFluxMeasurement measurement;
    const std::uint64_t exploratory_counter = static_cast<std::uint64_t>(
        authority_state.submit_preview_count
        + authority_state.validation_sample_count
        + authority_state.locally_validated_share_count
        + authority_state.dispatch_count);
    if (!authority_state.last_measured_nonce_observed) {
        measurement.search_epoch_turns = wrap_turns(
            static_cast<double>(exploratory_counter & 0xffffULL) / 65536.0);
        return measurement;
    }

    measurement.carrier_phase_turns = wrap_turns(
        (0.625 * authority_state.last_collapse_feedback_phase_turns)
        + (0.375 * authority_state.last_measured_nonce_phase_turns)
    );
    measurement.target_phase_turns = wrap_turns(authority_state.last_measured_hash_phase_turns);
    measurement.search_epoch_turns = wrap_turns(
        (static_cast<double>(exploratory_counter & 0xffffULL) / 65536.0)
        + (0.25 * measurement.carrier_phase_turns)
        + (0.25 * measurement.target_phase_turns));
    measurement.phase_pressure = mean_unit({
        authority_state.last_phase_flux_conservation,
        authority_state.last_nonce_collapse_confidence,
        authority_state.last_target_resonance_norm,
        1.0 - clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
    });
    measurement.flux_transport_norm = mean_unit({
        authority_state.last_phase_flux_conservation,
        authority_state.last_observer_collapse_strength,
    });
    measurement.observer_factor = mean_unit({
        authority_state.last_observer_collapse_strength,
        authority_state.last_nonce_collapse_confidence,
        authority_state.last_target_resonance_norm,
        1.0 - clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
    });
    measurement.zero_point_proximity = mean_unit({
        authority_state.last_phase_flux_conservation,
        authority_state.last_nonce_collapse_confidence,
    });
    measurement.temporal_admissibility = mean_unit({
        authority_state.last_phase_flux_conservation,
        authority_state.last_observer_collapse_strength,
        authority_state.last_target_resonance_norm,
        1.0 - clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0),
    });
    measurement.trajectory_conservation = mean_unit({
        authority_state.last_phase_flux_conservation,
        authority_state.last_nonce_collapse_confidence,
    });
    measurement.phase_lock_error = clamp_unit(authority_state.last_collapse_relock_error_turns * 2.0);
    measurement.anchor_evm_norm = 1.0 - clamp_unit(authority_state.last_phase_flux_conservation);
    measurement.sideband_energy_norm = 1.0 - clamp_unit(authority_state.last_observer_collapse_strength);
    measurement.interference_projection = 1.0 - clamp_unit(authority_state.last_nonce_collapse_confidence);
    measurement.rf_phase_position_turns = measurement.carrier_phase_turns;
    measurement.rf_phase_velocity_turns = phase_delta_turns(
        measurement.target_phase_turns,
        measurement.rf_phase_position_turns
    );
    measurement.rf_zero_point_displacement_turns =
        phase_delta_turns(measurement.rf_phase_position_turns, 0.0);
    measurement.rf_zero_point_distance_norm = clamp_unit(
        std::abs(measurement.rf_zero_point_displacement_turns) * 4.0
    );
    measurement.rf_carrier_frequency_norm = mean_unit({
        measurement.phase_pressure,
        measurement.flux_transport_norm,
        measurement.temporal_admissibility,
    });
    measurement.rf_envelope_amplitude_norm = mean_unit({
        measurement.zero_point_proximity,
        measurement.trajectory_conservation,
        1.0 - measurement.anchor_evm_norm,
    });
    measurement.rf_spin_drive_signed = clamp_signed(
        (measurement.observer_factor * 2.0) - 1.0
    );
    measurement.rf_rotation_orientation_signed = clamp_signed(
        phase_delta_turns(measurement.target_phase_turns, measurement.carrier_phase_turns) * 4.0
    );
    measurement.rf_temporal_coupling_norm = mean_unit({
        measurement.temporal_admissibility,
        measurement.phase_pressure,
        measurement.trajectory_conservation,
    });
    measurement.rf_resonance_hold_norm = mean_unit({
        measurement.trajectory_conservation,
        1.0 - measurement.phase_lock_error,
        1.0 - measurement.sideband_energy_norm,
    });
    measurement.rf_sideband_energy_norm = mean_unit({
        measurement.sideband_energy_norm,
        measurement.interference_projection,
        1.0 - measurement.rf_resonance_hold_norm,
    });
    measurement.rf_energy_transfer_norm = mean_unit({
        measurement.zero_point_proximity,
        1.0 - measurement.rf_zero_point_distance_norm,
        std::abs(measurement.rf_spin_drive_signed),
        measurement.rf_temporal_coupling_norm,
    });
    measurement.rf_particle_stability_norm = mean_unit({
        measurement.rf_resonance_hold_norm,
        measurement.trajectory_conservation,
        1.0 - measurement.rf_sideband_energy_norm,
    });
    measurement.spin_alignment_norm = mean_unit({
        std::abs(measurement.rf_spin_drive_signed),
        std::abs(measurement.rf_rotation_orientation_signed),
        1.0 - measurement.interference_projection,
    });
    measurement.transfer_drive_norm = clamp_unit(
        measurement.zero_point_proximity
        * (1.0 - measurement.rf_zero_point_distance_norm)
        * measurement.rf_temporal_coupling_norm
        * std::abs(measurement.rf_spin_drive_signed)
        * 6.0
    );
    measurement.stability_gate_norm = clamp_unit(
        measurement.trajectory_conservation
        * measurement.rf_particle_stability_norm
        * measurement.rf_resonance_hold_norm
        * (1.0 - measurement.phase_lock_error)
    );
    measurement.damping_norm = clamp_unit(
        (0.38 * measurement.rf_sideband_energy_norm)
        + (0.32 * measurement.interference_projection)
        + (0.30 * measurement.anchor_evm_norm)
    );
    measurement.transport_drive_norm = clamp_unit(
        (0.36 * measurement.phase_pressure)
        + (0.22 * measurement.observer_factor)
        + (0.16 * measurement.flux_transport_norm)
        + (0.14 * measurement.rf_temporal_coupling_norm)
        + (0.12 * measurement.stability_gate_norm)
        + (0.18 * measurement.transfer_drive_norm)
        + (0.12 * authority_state.last_target_resonance_norm)
        - (0.12 * measurement.damping_norm)
    );
    measurement.transfer_drive_norm = mean_unit({
        measurement.transfer_drive_norm,
        authority_state.last_phase_clamped_transfer_drive_norm,
        authority_state.last_phase_flux_conservation,
    });
    measurement.stability_gate_norm = mean_unit({
        measurement.stability_gate_norm,
        authority_state.last_phase_clamped_stability_gate_norm,
        authority_state.last_phase_flux_conservation,
        authority_state.last_nonce_collapse_confidence,
    });
    measurement.damping_norm = mean_unit({
        measurement.damping_norm,
        authority_state.last_phase_clamped_damping_norm,
        1.0 - authority_state.last_observer_collapse_strength,
    });
    measurement.transport_drive_norm = mean_unit({
        measurement.transport_drive_norm,
        authority_state.last_phase_clamped_transport_drive_norm,
        authority_state.last_phase_flux_conservation,
        authority_state.last_phase_clamped_transport_readiness,
    });
    measurement.target_resonance_norm = mean_unit({
        authority_state.last_target_resonance_norm,
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            measurement.target_phase_turns,
            measurement.carrier_phase_turns
        )) * 2.0),
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            measurement.target_phase_turns,
            measurement.rf_phase_position_turns
        )) * 2.0),
        measurement.transfer_drive_norm,
        measurement.stability_gate_norm,
        1.0 - measurement.damping_norm,
    });
    measurement.resonance_activation_norm = mean_unit({
        authority_state.last_phase_clamped_resonance_activation_norm,
        measurement.target_resonance_norm,
        measurement.transfer_drive_norm,
        measurement.stability_gate_norm,
        measurement.transport_drive_norm,
        authority_state.last_phase_flux_conservation,
        authority_state.last_nonce_collapse_confidence,
        1.0 - measurement.damping_norm,
    });
    measurement.temporal_admissibility = mean_unit({
        measurement.temporal_admissibility,
        authority_state.last_phase_clamped_temporal_admissibility,
        authority_state.last_phase_clamped_phase_alignment,
    });
    measurement.zero_point_proximity = mean_unit({
        measurement.zero_point_proximity,
        authority_state.last_phase_clamped_zero_point_proximity,
        authority_state.last_phase_flux_conservation,
    });
    return measurement;
}

[[nodiscard]] SubstrateStratumSubmitPreviewPayload build_phase_guided_preview_payload(
    const SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation,
    const SubstrateStratumPowEvaluation& evaluation
) {
    const SubstrateStratumWorkerAssignment assignment = resolve_preview_assignment(authority_state, actuation);
    SubstrateStratumSubmitPreviewPayload preview = build_stratum_submit_preview_payload(authority_state, assignment);
    const std::string original_request_id = preview.request_id;
    const std::string original_nonce = preview.nonce;

    if (!actuation.request_id.empty()) {
        preview.request_id = actuation.request_id;
        const std::size_t request_position = preview.payload_json.find(original_request_id);
        if (request_position != std::string::npos) {
            preview.payload_json.replace(request_position, original_request_id.size(), preview.request_id);
        }
    }

    preview.connection_id = authority_state.connection_ingress.connection_id;
    preview.job_id = actuation.job_id;
    preview.worker_index = actuation.worker_index;
    preview.worker_name = actuation.worker_name;
    preview.nonce = actuation.nonce_hex;
    preview.offline_pow_checked = true;
    preview.offline_pow_valid = evaluation.valid_share;
    preview.hash_hex = evaluation.hash_hex;
    preview.target_hex = authority_state.active_share_target_hex;
    preview.share_target_hex = authority_state.active_share_target_hex;
    preview.block_target_hex = authority_state.active_block_target_hex;
    preview.share_difficulty = authority_state.difficulty;
    preview.block_difficulty = authority_state.active_block_difficulty;
    preview.expected_hashes_for_share = authority_state.expected_hashes_for_share;
    preview.target_network_share_fraction = authority_state.target_network_share_fraction;
    preview.network_hashrate_hs = authority_state.network_hashrate_hs;
    preview.required_hashrate_hs = authority_state.required_hashrate_hs;
    preview.required_share_submissions_per_s = authority_state.required_share_submissions_per_s;
    preview.block_candidate_valid = evaluation.valid_block;
    preview.network_send_allowed = false;
    preview.gate_reason = evaluation.valid_share
        ? "phase_guided_preview_validation"
        : "phase_guided_resonant_candidate_preview";

    const std::size_t nonce_position = preview.payload_json.rfind(original_nonce);
    if (nonce_position != std::string::npos) {
        preview.payload_json.replace(nonce_position, original_nonce.size(), preview.nonce);
    }

    return preview;
}

RuntimeEvent build_stratum_state_event(
    const std::string& topic,
    const std::string& message,
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumConnectionIngress* ingress,
    const SubstrateStratumDispatchPayload* dispatch,
    const SubstrateStratumResponsePayload* response,
    const SubstrateStratumServerEventPayload* server_event,
    const SubstrateStratumSubmitPreviewPayload* submit_preview
) {
    RuntimeEvent event;
    event.topic = topic;
    event.message = message;
    event.stratum_authority = authority_state;
    event.has_stratum_authority = true;
    if (ingress != nullptr) {
        event.stratum_connection_ingress = *ingress;
        event.has_stratum_connection_ingress = true;
    }
    if (dispatch != nullptr) {
        event.stratum_dispatch_payload = *dispatch;
        event.has_stratum_dispatch_payload = true;
    }
    if (response != nullptr) {
        event.stratum_response_payload = *response;
        event.has_stratum_response_payload = true;
    }
    if (server_event != nullptr) {
        event.stratum_server_event_payload = *server_event;
        event.has_stratum_server_event_payload = true;
    }
    if (submit_preview != nullptr) {
        event.stratum_submit_preview_payload = *submit_preview;
        event.has_stratum_submit_preview_payload = true;
    }
    return event;
}

void update_work_projection(SubstrateStratumAuthorityState& authority_state) {
    authority_state.active_share_target_hex.clear();
    authority_state.active_block_target_hex.clear();
    authority_state.active_block_difficulty = 0.0;
    authority_state.expected_hashes_for_share = 0.0;
    authority_state.network_hashrate_hs = 0.0;
    authority_state.required_hashrate_hs = 0.0;
    authority_state.required_share_submissions_per_s = 0.0;
    authority_state.required_share_submissions_per_s_per_worker = 0.0;
    authority_state.required_share_submissions_per_pool_window = 0.0;
    authority_state.required_share_submissions_per_worker_pool_window = 0.0;
    authority_state.effective_request_budget_per_s = 0.0;

    if (authority_state.has_difficulty) {
        authority_state.active_share_target_hex = stratum_difficulty_to_target_hex(authority_state.difficulty);
        authority_state.expected_hashes_for_share = expected_hashes_for_difficulty(authority_state.difficulty);
    }

    if (authority_state.active_job_nbits.empty()) {
        return;
    }

    authority_state.active_block_target_hex = stratum_bits_to_target_hex(authority_state.active_job_nbits);
    authority_state.active_block_difficulty = stratum_nbits_to_difficulty(authority_state.active_job_nbits);
    const BitcoinWorkProjection projection = build_bitcoin_work_projection(
        authority_state.difficulty,
        authority_state.active_job_nbits,
        authority_state.target_network_share_fraction);
    authority_state.network_hashrate_hs = projection.network_hashrate_hs;
    authority_state.required_hashrate_hs = projection.required_hashrate_hs;
    authority_state.required_share_submissions_per_s = projection.required_share_submissions_per_s;
    if (authority_state.target_hashrate_hs > 0.0) {
        authority_state.required_hashrate_hs = authority_state.target_hashrate_hs;
        authority_state.required_share_submissions_per_s =
            authority_state.expected_hashes_for_share > 0.0
            ? (authority_state.required_hashrate_hs / authority_state.expected_hashes_for_share)
            : 0.0;
    }
    authority_state.submission_interval_target_s = 0.0;
}

[[nodiscard]] std::string build_submit_share_key(const PhaseClampedShareActuation& actuation) {
    std::ostringstream stream;
    stream << actuation.job_id << ':' << actuation.worker_index << ':' << actuation.nonce_hex;
    if (!actuation.hash_hex.empty()) {
        stream << ':' << actuation.hash_hex;
    }
    return stream.str();
}

[[nodiscard]] std::string build_live_submit_request_id(const PhaseClampedShareActuation& actuation) {
    std::ostringstream stream;
    stream << actuation.connection_id << ":submit:" << actuation.job_id << ':' << actuation.worker_index << ':' << actuation.nonce_hex;
    return stream.str();
}

[[nodiscard]] std::string build_live_submit_payload_json(
    const SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation,
    const std::string& request_id
) {
    const std::string submit_identity = authority_state.connection_ingress.worker_name.empty()
        ? actuation.worker_name
        : authority_state.connection_ingress.worker_name;
    SubstrateStratumWorkerAssignment assignment;
    assignment.worker_index = actuation.worker_index;
    assignment.active = true;
    assignment.job_id = actuation.job_id;
    assignment.worker_name = actuation.worker_name;
    std::ostringstream stream;
    stream << '{'
           << "\"id\": \"" << request_id << "\", "
           << "\"method\": \"mining.submit\", "
            << "\"params\": [\"" << submit_identity << "\", \""
           << actuation.job_id << "\", \"" << build_stratum_worker_extranonce2_hex(authority_state, assignment) << "\", \""
           << authority_state.active_job_ntime << "\", \""
           << actuation.nonce_hex << "\"]"
           << '}';
    return stream.str();
}

[[nodiscard]] SubstrateStratumDispatchPayload build_live_submit_dispatch_payload(
    const SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation
) {
    SubstrateStratumDispatchPayload dispatch;
    dispatch.command_kind = StratumCommandKind::Submit;
    dispatch.connection_id = authority_state.connection_ingress.connection_id;
    dispatch.request_id = build_live_submit_request_id(actuation);
    dispatch.method = stratum_command_method(StratumCommandKind::Submit);
    dispatch.payload_json = build_live_submit_payload_json(authority_state, actuation, dispatch.request_id);
    dispatch.host = authority_state.connection_ingress.host;
    dispatch.port = authority_state.connection_ingress.port;
    dispatch.worker_name = authority_state.connection_ingress.worker_name.empty()
        ? actuation.worker_name
        : authority_state.connection_ingress.worker_name;
    dispatch.dry_run_only = authority_state.connection_ingress.dry_run_only;
    dispatch.allow_live_submit = authority_state.connection_ingress.allow_live_submit;
    dispatch.max_requests_per_second = authority_state.connection_ingress.max_requests_per_second;
    dispatch.target_network_share_fraction = authority_state.connection_ingress.target_network_share_fraction;
    dispatch.allowed_worker_count = authority_state.connection_ingress.allowed_worker_count;
    dispatch.authoritative_program_source = std::string(phase_programs::kMiningResonanceProgramSource);
    return dispatch;
}

constexpr std::uint32_t kNormalShareQualityClass = 0U;
constexpr std::uint32_t kPriorityShareQualityClass = 1U;
constexpr std::uint32_t kExactBlockShareQualityClass = 2U;

[[nodiscard]] double compute_block_coherence_norm(
    const PhaseClampedShareActuation& actuation,
    const SubstrateStratumPowPhaseTrace& phase_trace
) {
    if (!phase_trace.performed) {
        return clamp_unit(actuation.block_coherence_norm);
    }
    if (phase_trace.evaluation.valid_block) {
        return 1.0;
    }

    const double block_digest_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        phase_trace.resonant_measurement.sha256_digest_phase_turns,
        phase_trace.block_target_phase_turns)) * 2.0);
    const double block_hash_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        phase_trace.collapse_feedback.measured_hash_phase_turns,
        phase_trace.block_target_phase_turns)) * 2.0);
    const double share_block_alignment = 1.0 - clamp_unit(std::abs(phase_delta_turns(
        phase_trace.share_target_phase_turns,
        phase_trace.block_target_phase_turns)) * 2.0);
    return mean_unit({
        clamp_unit(actuation.block_coherence_norm),
        block_digest_alignment,
        block_hash_alignment,
        share_block_alignment,
        actuation.target_resonance_norm,
        actuation.phase_alignment,
        actuation.validation_structure_norm,
        actuation.phase_flux_conservation,
        actuation.nonce_collapse_confidence,
    });
}

[[nodiscard]] std::uint32_t classify_queue_quality_class(const PhaseClampedShareActuation& actuation) {
    if (!actuation.share_target_pass) {
        return kNormalShareQualityClass;
    }
    if (actuation.block_target_pass || actuation.block_candidate_valid) {
        return kExactBlockShareQualityClass;
    }
    return actuation.block_coherence_norm >= 0.85
        ? kPriorityShareQualityClass
        : kNormalShareQualityClass;
}

[[nodiscard]] double compute_submit_priority_score(const PhaseClampedShareActuation& actuation) {
    if (actuation.block_target_pass || actuation.block_candidate_valid) {
        return 1.0;
    }

    const double class_bias = actuation.queue_quality_class >= kPriorityShareQualityClass ? 0.56 : 0.34;
    const double reinforcement_norm = clamp_unit(
        static_cast<double>(std::max<std::size_t>(actuation.resonance_reinforcement_count, 1U)) / 8.0);
    return clamp_unit(
        class_bias
        + (0.18 * clamp_unit(actuation.block_coherence_norm))
        + (0.14 * reinforcement_norm)
        + (0.08 * clamp_unit(actuation.share_confidence))
        + (0.10 * clamp_unit(actuation.target_resonance_norm)));
}

[[nodiscard]] std::size_t compute_priority_spacing_target(const std::string& canonical_share_id) {
    return 4U + (std::hash<std::string>{}(canonical_share_id) % 3U);
}

[[nodiscard]] PhaseClampedShareActuation enrich_validated_share_candidate(
    PhaseClampedShareActuation actuation,
    const SubstrateStratumPowPhaseTrace& phase_trace
) {
    actuation.share_target_pass = phase_trace.evaluation.valid_share;
    actuation.block_target_pass = phase_trace.evaluation.valid_block;
    actuation.block_candidate_valid = phase_trace.evaluation.valid_block;
    actuation.block_coherence_norm = std::max(
        clamp_unit(actuation.block_coherence_norm),
        compute_block_coherence_norm(actuation, phase_trace));
    actuation.resonance_reinforcement_count = std::max<std::size_t>(
        actuation.resonance_reinforcement_count,
        1U);
    if (actuation.canonical_share_id.empty()) {
        actuation.canonical_share_id = build_submit_share_key(actuation);
    }
    actuation.queue_quality_class = classify_queue_quality_class(actuation);
    actuation.submit_priority_score = std::max(
        clamp_unit(actuation.submit_priority_score),
        compute_submit_priority_score(actuation));
    return actuation;
}

void refresh_pending_submit_queue_metrics(
    SubstrateStratumAuthorityState& authority_state,
    const std::deque<QueuedSubmitCandidate>& pending_submit_candidates
) {
    authority_state.queued_submit_candidate_count = pending_submit_candidates.size();
    authority_state.queued_priority_submit_candidate_count = static_cast<std::size_t>(std::count_if(
        pending_submit_candidates.begin(),
        pending_submit_candidates.end(),
        [](const QueuedSubmitCandidate& candidate) {
            return candidate.actuation.queue_quality_class == kPriorityShareQualityClass;
        }));
    authority_state.queued_block_submit_candidate_count = static_cast<std::size_t>(std::count_if(
        pending_submit_candidates.begin(),
        pending_submit_candidates.end(),
        [](const QueuedSubmitCandidate& candidate) {
            return candidate.actuation.queue_quality_class >= kExactBlockShareQualityClass;
        }));
}

void upsert_pending_submit_candidate(
    std::deque<QueuedSubmitCandidate>& pending_submit_candidates,
    const QueuedSubmitCandidate& candidate
) {
    const auto existing = std::find_if(
        pending_submit_candidates.begin(),
        pending_submit_candidates.end(),
        [&candidate](const QueuedSubmitCandidate& queued_candidate) {
            return queued_candidate.share_key == candidate.share_key;
        });
    if (existing == pending_submit_candidates.end()) {
        pending_submit_candidates.push_back(candidate);
        return;
    }

    existing->validation_timestamp_unix_s = std::max(
        existing->validation_timestamp_unix_s,
        candidate.validation_timestamp_unix_s);
    if (candidate.actuation.submit_priority_score > existing->actuation.submit_priority_score) {
        existing->actuation = candidate.actuation;
        existing->phase_trace = candidate.phase_trace;
        return;
    }

    existing->actuation.share_target_pass =
        existing->actuation.share_target_pass || candidate.actuation.share_target_pass;
    existing->actuation.block_target_pass =
        existing->actuation.block_target_pass || candidate.actuation.block_target_pass;
    existing->actuation.block_candidate_valid =
        existing->actuation.block_candidate_valid || candidate.actuation.block_candidate_valid;
    existing->actuation.block_coherence_norm = std::max(
        existing->actuation.block_coherence_norm,
        candidate.actuation.block_coherence_norm);
    existing->actuation.resonance_reinforcement_count = std::max(
        existing->actuation.resonance_reinforcement_count,
        candidate.actuation.resonance_reinforcement_count);
    existing->actuation.noise_lane_count = std::max(
        existing->actuation.noise_lane_count,
        candidate.actuation.noise_lane_count);
    existing->actuation.queue_quality_class = std::max(
        existing->actuation.queue_quality_class,
        candidate.actuation.queue_quality_class);
    existing->actuation.submit_priority_score = std::max(
        existing->actuation.submit_priority_score,
        candidate.actuation.submit_priority_score);
}

template <typename Predicate>
[[nodiscard]] std::deque<QueuedSubmitCandidate>::iterator select_best_candidate(
    std::deque<QueuedSubmitCandidate>& pending_submit_candidates,
    Predicate predicate
) {
    return std::max_element(
        pending_submit_candidates.begin(),
        pending_submit_candidates.end(),
        [&predicate](const QueuedSubmitCandidate& lhs, const QueuedSubmitCandidate& rhs) {
            const bool lhs_matches = predicate(lhs);
            const bool rhs_matches = predicate(rhs);
            if (lhs_matches != rhs_matches) {
                return lhs_matches < rhs_matches;
            }
            if (lhs_matches && rhs_matches
                && lhs.actuation.submit_priority_score != rhs.actuation.submit_priority_score) {
                return lhs.actuation.submit_priority_score < rhs.actuation.submit_priority_score;
            }
            return lhs.validation_timestamp_unix_s > rhs.validation_timestamp_unix_s;
        });
}

[[nodiscard]] std::deque<QueuedSubmitCandidate>::iterator select_next_submit_candidate(
    std::deque<QueuedSubmitCandidate>& pending_submit_candidates,
    const SubstrateStratumAuthorityState& authority_state
) {
    if (pending_submit_candidates.empty()) {
        return pending_submit_candidates.end();
    }

    const auto block_candidate = select_best_candidate(
        pending_submit_candidates,
        [](const QueuedSubmitCandidate& candidate) {
            return candidate.actuation.queue_quality_class >= kExactBlockShareQualityClass;
        });
    if (block_candidate != pending_submit_candidates.end()
        && block_candidate->actuation.queue_quality_class >= kExactBlockShareQualityClass) {
        return block_candidate;
    }

    const auto normal_candidate = select_best_candidate(
        pending_submit_candidates,
        [](const QueuedSubmitCandidate& candidate) {
            return candidate.actuation.queue_quality_class == kNormalShareQualityClass;
        });
    const auto priority_candidate = select_best_candidate(
        pending_submit_candidates,
        [](const QueuedSubmitCandidate& candidate) {
            return candidate.actuation.queue_quality_class == kPriorityShareQualityClass;
        });

    const bool has_normal = normal_candidate != pending_submit_candidates.end()
        && normal_candidate->actuation.queue_quality_class == kNormalShareQualityClass;
    const bool has_priority = priority_candidate != pending_submit_candidates.end()
        && priority_candidate->actuation.queue_quality_class == kPriorityShareQualityClass;

    if (has_normal && has_priority) {
        return authority_state.normal_shares_since_priority_submit >= authority_state.next_priority_spacing_target
            ? priority_candidate
            : normal_candidate;
    }
    if (has_normal) {
        return normal_candidate;
    }
    if (has_priority) {
        return priority_candidate;
    }
    return pending_submit_candidates.end();
}

void update_post_dispatch_mix_state(
    SubstrateStratumAuthorityState& authority_state,
    const PhaseClampedShareActuation& actuation
) {
    if (actuation.queue_quality_class == kNormalShareQualityClass) {
        ++authority_state.normal_shares_since_priority_submit;
        return;
    }

    if (actuation.queue_quality_class == kPriorityShareQualityClass) {
        authority_state.normal_shares_since_priority_submit = 0U;
        authority_state.next_priority_spacing_target = compute_priority_spacing_target(
            actuation.canonical_share_id.empty() ? build_submit_share_key(actuation) : actuation.canonical_share_id);
    }
}

}  // namespace

SubstrateFirmwareRuntime::SubstrateFirmwareRuntime(RuntimeBus& bus)
    : bus_(bus) {
    bus_.subscribe("substrate.control.ingress", [this](const RuntimeEvent& event) {
        handle_control_ingress(event);
    });
    bus_.subscribe("substrate.stratum.connection.ingress", [this](const RuntimeEvent& event) {
        handle_stratum_connection_ingress(event);
    });
    bus_.subscribe("substrate.stratum.connection.control", [this](const RuntimeEvent& event) {
        handle_stratum_connection_control(event);
    });
    bus_.subscribe("substrate.stratum.response", [this](const RuntimeEvent& event) {
        handle_stratum_response(event);
    });
    bus_.subscribe(stratum_server_event_topic(StratumServerEventKind::SetDifficulty), [this](const RuntimeEvent& event) {
        handle_stratum_server_event(event);
    });
    bus_.subscribe(stratum_server_event_topic(StratumServerEventKind::Notify), [this](const RuntimeEvent& event) {
        handle_stratum_server_event(event);
    });
    bus_.subscribe("substrate.bitcoin.share.refused", [this](const RuntimeEvent& event) {
        handle_phase_clamped_share_actuation(event);
    });
    bus_.subscribe("substrate.bitcoin.share.candidate", [this](const RuntimeEvent& event) {
        handle_phase_clamped_share_actuation(event);
    });
    bus_.subscribe("substrate.bitcoin.share.actuation", [this](const RuntimeEvent& event) {
        handle_phase_clamped_share_actuation(event);
    });
}

SubstrateFirmwareState SubstrateFirmwareRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void SubstrateFirmwareRuntime::handle_control_ingress(const RuntimeEvent& event) {
    if (!event.has_control_ingress) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.control_ingress = event.control_ingress;
        state_.has_control_ingress = true;
        state_.last_event_topic = event.topic;
    }

    bus_.publish(RuntimeEvent{
        "substrate.firmware.control.accepted",
        "Substrate firmware accepted control ingress at the runtime boundary",
        {},
        {},
        false,
        {},
        false,
        event.control_ingress,
        true,
    });
}

void SubstrateFirmwareRuntime::handle_stratum_connection_ingress(const RuntimeEvent& event) {
    if (!event.has_stratum_connection_ingress) {
        return;
    }

    const SubstrateStratumConnectionIngress ingress = sanitize_stratum_connection_ingress(event.stratum_connection_ingress);
    SubstrateStratumAuthorityState authority_state;
    bool accepted = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.stratum_authority = {};
        state_.stratum_authority.connection_ingress = ingress;
        state_.stratum_authority.has_connection_ingress = true;
        state_.stratum_authority.max_requests_per_second = ingress.max_requests_per_second;
        state_.stratum_authority.target_network_share_fraction = ingress.target_network_share_fraction;
        state_.stratum_authority.target_hashrate_hs = ingress.target_hashrate_hs;
        state_.stratum_authority.allowed_worker_count = ingress.allowed_worker_count;
        state_.stratum_authority.validation_jitter_window_seconds = ingress.validation_jitter_window_seconds;
        state_.stratum_authority.pool_policy = resolve_stratum_pool_policy(ingress.host);
        state_.stratum_authority.min_validation_jitter_samples = ingress.min_validation_jitter_samples;
        state_.stratum_authority.validation_log_csv_path = ingress.validation_log_csv_path;
        state_.stratum_authority.phase_guided_preview_test_mode = ingress.phase_guided_preview_test_mode;
        state_.stratum_authority.auto_promote_to_live_mode = ingress.auto_promote_to_live_mode;
        state_.stratum_authority.max_invalid_pool_submissions = ingress.max_invalid_pool_submissions;
        state_.stratum_authority.operating_mode = resolve_operating_mode(ingress);
        state_.stratum_authority.submit_policy_enabled = ingress.allow_live_submit && !ingress.dry_run_only;
        state_.stratum_authority.last_event_topic = event.topic;
        state_.has_stratum_authority = true;
        state_.last_event_topic = event.topic;
        worker_validation_histories_ = {};

        if (is_valid_stratum_connection_ingress(ingress)) {
            if (state_.stratum_authority.validation_log_csv_path.empty()) {
                state_.stratum_authority.validation_log_csv_path =
                    default_validation_log_csv_path(state_.stratum_authority);
            }
            state_.stratum_authority.connection_state = "connection_ingress_accepted";
            accepted = true;
        } else {
            state_.stratum_authority.connection_state = "refused";
            state_.stratum_authority.last_response_message = "Stratum connection ingress failed validation";
        }
        authority_state = state_.stratum_authority;
    }

    if (!accepted) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.connection.refused",
            "Substrate firmware refused Stratum connection ingress",
            authority_state,
            &ingress,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
        return;
    }

    bus_.publish(build_stratum_state_event(
        "substrate.stratum.connection.accepted",
        "Substrate firmware accepted Stratum connection ingress",
        authority_state,
        &ingress,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    ));

    publish_stratum_dispatch(StratumCommandKind::Connect, authority_state);
}

void SubstrateFirmwareRuntime::handle_stratum_connection_control(const RuntimeEvent& event) {
    if (!event.has_stratum_connection_control || !event.stratum_connection_control.disconnect_requested) {
        return;
    }

    const SubstrateStratumConnectionControl control = sanitize_stratum_connection_control(event.stratum_connection_control);
    if (!is_valid_stratum_connection_control(control)) {
        RuntimeEvent refused_event;
        refused_event.topic = "substrate.stratum.connection.control.refused";
        refused_event.message = "Stratum connection control failed validation";
        refused_event.stratum_connection_control = control;
        refused_event.has_stratum_connection_control = true;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.has_stratum_authority) {
                refused_event.stratum_authority = state_.stratum_authority;
                refused_event.has_stratum_authority = true;
            }
        }

        bus_.publish(refused_event);
        return;
    }

    SubstrateStratumAuthorityState authority_state;
    bool publish_disconnect = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.has_stratum_authority) {
            return;
        }

        if (!control.connection_id.empty() && state_.stratum_authority.has_connection_ingress
            && control.connection_id != state_.stratum_authority.connection_ingress.connection_id) {
            return;
        }

        state_.stratum_authority.transport_connected = false;
        state_.stratum_authority.subscribed = false;
        state_.stratum_authority.authorized = false;
        state_.stratum_authority.network_authority_granted = false;
        state_.stratum_authority.has_active_job = false;
        state_.stratum_authority.active_job_id.clear();
        state_.stratum_authority.active_job_header_hex.clear();
        state_.stratum_authority.active_job_prevhash.clear();
        state_.stratum_authority.active_job_coinbase1.clear();
        state_.stratum_authority.active_job_coinbase2.clear();
        state_.stratum_authority.active_job_merkle_branches.clear();
        state_.stratum_authority.active_job_version.clear();
        state_.stratum_authority.active_job_nbits.clear();
        state_.stratum_authority.active_job_ntime.clear();
        state_.stratum_authority.active_share_target_hex.clear();
        state_.stratum_authority.active_block_target_hex.clear();
        state_.stratum_authority.active_block_difficulty = 0.0;
        state_.stratum_authority.expected_hashes_for_share = 0.0;
        state_.stratum_authority.network_hashrate_hs = 0.0;
        state_.stratum_authority.required_hashrate_hs = 0.0;
        state_.stratum_authority.required_share_submissions_per_s = 0.0;
        state_.stratum_authority.required_share_submissions_per_s_per_worker = 0.0;
        state_.stratum_authority.required_share_submissions_per_pool_window = 0.0;
        state_.stratum_authority.required_share_submissions_per_worker_pool_window = 0.0;
        state_.stratum_authority.validation_sample_count = 0U;
        state_.stratum_authority.locally_validated_share_count = 0U;
        state_.stratum_authority.workers_with_validation_samples = 0U;
        state_.stratum_authority.workers_meeting_validation_sample_threshold = 0U;
        state_.stratum_authority.workers_meeting_target_rate_count = 0U;
        state_.stratum_authority.min_worker_validation_sample_count = 0U;
        state_.stratum_authority.measured_validation_share_rate_per_s = 0.0;
        state_.stratum_authority.mean_worker_validation_share_rate_per_s = 0.0;
        state_.stratum_authority.measured_validation_hashrate_hs_60s = 0.0;
        state_.stratum_authority.mean_worker_validation_hashrate_hs_60s = 0.0;
        state_.stratum_authority.measured_validation_jitter_s = 0.0;
        state_.stratum_authority.measured_validation_jitter_fraction = 0.0;
        state_.stratum_authority.max_worker_validation_jitter_s = 0.0;
        state_.stratum_authority.max_worker_validation_jitter_fraction = 0.0;
        state_.stratum_authority.submission_interval_target_s = 0.0;
        state_.stratum_authority.effective_request_budget_per_s = 0.0;
        state_.stratum_authority.last_validation_timestamp_unix_s = 0.0;
        state_.stratum_authority.last_submit_timestamp_unix_s = 0.0;
        state_.stratum_authority.active_worker_count = 0;
        state_.stratum_authority.worker_assignments = {};
        state_.stratum_authority.worker_validation_sample_counts = {};
        state_.stratum_authority.worker_measured_validation_share_rate_per_s = {};
        state_.stratum_authority.worker_measured_validation_hashrate_hs_60s = {};
        state_.stratum_authority.worker_measured_validation_jitter_s = {};
        state_.stratum_authority.worker_measured_validation_jitter_fraction = {};
        state_.stratum_authority.worker_submission_interval_target_s = {};
        state_.stratum_authority.submit_preview_count = 0;
        state_.stratum_authority.offline_valid_submit_preview_count = 0;
        state_.stratum_authority.last_preview_share_key.clear();
        state_.stratum_authority.submit_path_ready = false;
        state_.stratum_authority.submit_dispatched = false;
        state_.stratum_authority.submit_dispatch_count = 0;
        state_.stratum_authority.accepted_submit_count = 0;
        state_.stratum_authority.refused_submit_count = 0;
        state_.stratum_authority.queued_submit_candidate_count = 0;
        state_.stratum_authority.queued_priority_submit_candidate_count = 0;
        state_.stratum_authority.queued_block_submit_candidate_count = 0;
        state_.stratum_authority.normal_shares_since_priority_submit = 0;
        state_.stratum_authority.next_priority_spacing_target = 4;
        state_.stratum_authority.last_submitted_share_key.clear();
        state_.stratum_authority.last_submit_request_id.clear();
        state_.stratum_authority.last_submit_job_id.clear();
        state_.stratum_authority.last_submit_worker_index = 0U;
        state_.stratum_authority.last_submit_worker_name.clear();
        state_.stratum_authority.last_submit_nonce.clear();
        state_.stratum_authority.last_submit_hash_hex.clear();
        pending_submit_candidates_.clear();
        state_.stratum_authority.connection_state = "disconnected";
        state_.stratum_authority.last_response_message = control.reason;
        state_.stratum_authority.last_event_topic = event.topic;
        state_.last_event_topic = event.topic;
        clear_authority_collapse_feedback(state_.stratum_authority);
        worker_validation_histories_ = {};
        authority_state = state_.stratum_authority;
        publish_disconnect = true;
    }

    if (publish_disconnect) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.connection.disconnected",
            authority_state.last_response_message,
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
    }
}

void SubstrateFirmwareRuntime::handle_stratum_response(const RuntimeEvent& event) {
    if (!event.has_stratum_response_payload) {
        return;
    }

    const SubstrateStratumResponsePayload response = event.stratum_response_payload;
    SubstrateStratumAuthorityState authority_state;
    bool publish_next_dispatch = false;
    StratumCommandKind next_command = StratumCommandKind::Connect;
    bool publish_granted = false;
    bool publish_disconnect_control = false;
    bool append_submit_refusal_log = false;
    double submit_refusal_timestamp_s = 0.0;
    SubstrateStratumConnectionControl disconnect_control;
    std::string event_topic;
    std::string event_message;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.has_stratum_authority || !state_.stratum_authority.has_connection_ingress
            || response.connection_id != state_.stratum_authority.connection_ingress.connection_id) {
            return;
        }

        state_.stratum_authority.last_event_topic = event.topic;
        state_.last_event_topic = event.topic;
        state_.stratum_authority.last_request_id = response.request_id;
        state_.stratum_authority.last_command = response.method;
        state_.stratum_authority.last_response_message = response.message;

        if (!response.accepted && response.command_kind != StratumCommandKind::Submit) {
            state_.stratum_authority.connection_state = "refused";
            authority_state = state_.stratum_authority;
            event_topic = "substrate.stratum.response.refused";
            event_message = "Substrate firmware refused Stratum authority progression";
        } else {
            switch (response.command_kind) {
            case StratumCommandKind::Connect:
                state_.stratum_authority.transport_connected = true;
                state_.stratum_authority.connection_state = "transport_connected";
                publish_next_dispatch = true;
                next_command = StratumCommandKind::Subscribe;
                break;
            case StratumCommandKind::Subscribe:
                state_.stratum_authority.subscribed = true;
                state_.stratum_authority.connection_state = "subscribed";
                state_.stratum_authority.subscription_id = response.subscription_id;
                state_.stratum_authority.extranonce1 = response.extranonce1;
                state_.stratum_authority.extranonce2_size = response.extranonce2_size;
                publish_next_dispatch = true;
                next_command = StratumCommandKind::Authorize;
                break;
            case StratumCommandKind::Authorize:
                state_.stratum_authority.authorized = true;
                state_.stratum_authority.network_authority_granted = true;
                state_.stratum_authority.connection_state = "network_authority_granted";
                publish_granted = true;
                break;
            case StratumCommandKind::Submit:
                if (response.accepted) {
                    ++state_.stratum_authority.accepted_submit_count;
                    state_.stratum_authority.connection_state = "submit_accepted";
                    state_.stratum_authority.last_submit_request_id = response.request_id;
                } else {
                    ++state_.stratum_authority.refused_submit_count;
                    state_.stratum_authority.connection_state = "submit_refused";
                    state_.stratum_authority.last_submit_request_id = response.request_id;
                    state_.stratum_authority.last_response_message = response.message.empty()
                        ? "Pool refused a live Stratum share submit"
                        : response.message;
                    submit_refusal_timestamp_s = now_unix_seconds();
                    append_submit_refusal_log = true;
                    if (state_.stratum_authority.refused_submit_count
                        > state_.stratum_authority.max_invalid_pool_submissions) {
                        disconnect_control = build_invalid_submit_disconnect_control(
                            state_.stratum_authority,
                            state_.stratum_authority.refused_submit_count);
                        publish_disconnect_control = true;
                    }
                }
                break;
            }
            authority_state = state_.stratum_authority;
            event_topic = response.accepted
                ? "substrate.stratum.response.accepted"
                : "substrate.stratum.response.refused";
            event_message = response.accepted
                ? "Substrate firmware accepted Stratum response payload"
                : "Substrate firmware observed a refused Stratum response payload";
        }
    }

    bus_.publish(build_stratum_state_event(
        event_topic,
        event_message,
        authority_state,
        nullptr,
        nullptr,
        &response,
        nullptr,
        nullptr
    ));

    if (response.command_kind == StratumCommandKind::Submit) {
        bus_.publish(build_stratum_state_event(
            response.accepted ? "substrate.stratum.submit.accepted" : "substrate.stratum.submit.refused",
            response.accepted
                ? "Substrate firmware accepted a live Stratum share submit response"
                : "Substrate firmware observed a refused live Stratum share submit response",
            authority_state,
            nullptr,
            nullptr,
            &response,
            nullptr,
            nullptr
        ));
    }

    if (append_submit_refusal_log) {
        append_validation_log_row(
            authority_state,
            submit_refusal_timestamp_s,
            "livemode_pool_submit_refused",
            authority_state.last_submit_worker_name,
            authority_state.last_submit_worker_index,
            authority_state.last_submit_job_id,
            authority_state.last_submit_nonce,
            authority_state.last_submit_hash_hex,
                authority_state.difficulty,
            true,
            false,
            true,
            "pool_submit_refused");
    }

    if (publish_next_dispatch) {
        publish_stratum_dispatch(next_command, authority_state);
    }

    if (publish_granted) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.authority.granted",
            "Substrate firmware granted Stratum network authority after subscribe and authorize",
            authority_state,
            nullptr,
            nullptr,
            &response,
            nullptr,
            nullptr
        ));
    }

    if (publish_disconnect_control) {
        RuntimeEvent disconnect_event;
        disconnect_event.topic = "substrate.stratum.connection.control";
        disconnect_event.message = disconnect_control.reason;
        disconnect_event.stratum_connection_control = disconnect_control;
        disconnect_event.has_stratum_connection_control = true;
        bus_.publish(disconnect_event);
    }
}

void SubstrateFirmwareRuntime::handle_stratum_server_event(const RuntimeEvent& event) {
    if (!event.has_stratum_server_event_payload) {
        return;
    }

    const SubstrateStratumServerEventPayload server_event = event.stratum_server_event_payload;
    SubstrateStratumAuthorityState authority_state;
    std::vector<SubstrateStratumSubmitPreviewPayload> previews;
    std::string event_topic;
    std::string event_message;
    bool publish_workers_assigned = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.has_stratum_authority || !state_.stratum_authority.has_connection_ingress
            || server_event.connection_id != state_.stratum_authority.connection_ingress.connection_id) {
            return;
        }

        state_.stratum_authority.last_event_topic = event.topic;
        state_.last_event_topic = event.topic;
        state_.stratum_authority.last_command = server_event.method;

        switch (server_event.event_kind) {
        case StratumServerEventKind::SetDifficulty:
            state_.stratum_authority.has_difficulty = true;
            state_.stratum_authority.difficulty = server_event.difficulty;
            update_work_projection(state_.stratum_authority);
            refresh_submission_control(state_.stratum_authority, worker_validation_histories_, now_unix_seconds());
            state_.stratum_authority.connection_state = "difficulty_received";
            event_topic = "substrate.stratum.difficulty.updated";
            event_message = "Substrate firmware accepted Stratum difficulty update";
            break;
        case StratumServerEventKind::Notify:
            if (!state_.stratum_authority.network_authority_granted) {
                state_.stratum_authority.connection_state = "job_refused";
                state_.stratum_authority.last_response_message = "Stratum notify arrived before authority grant";
                event_topic = "substrate.stratum.job.refused";
                event_message = "Substrate firmware refused Stratum job intake before authority grant";
                break;
            }

            state_.stratum_authority.has_active_job = true;
            state_.stratum_authority.active_job_id = server_event.job_id;
            state_.stratum_authority.active_job_prevhash = server_event.prevhash;
            state_.stratum_authority.active_job_coinbase1 = server_event.coinbase1;
            state_.stratum_authority.active_job_coinbase2 = server_event.coinbase2;
            state_.stratum_authority.active_job_merkle_branches = server_event.merkle_branches;
            state_.stratum_authority.active_job_version = server_event.version;
            state_.stratum_authority.active_job_nbits = server_event.nbits;
            state_.stratum_authority.active_job_ntime = server_event.ntime;
            state_.stratum_authority.active_job_clean = server_event.clean_jobs;
            update_work_projection(state_.stratum_authority);
            state_.stratum_authority.worker_assignments =
                build_stratum_worker_assignments(state_.stratum_authority.connection_ingress, server_event.job_id);
            state_.stratum_authority.active_worker_count = 0U;
            for (const auto& assignment : state_.stratum_authority.worker_assignments) {
                if (assignment.active) {
                    ++state_.stratum_authority.active_worker_count;
                }
            }
            state_.stratum_authority.active_job_header_hex =
                !server_event.header_hex.empty()
                ? server_event.header_hex
                : resolve_stratum_job_header_hex(
                    state_.stratum_authority,
                    state_.stratum_authority.worker_assignments.front());
            state_.stratum_authority.submit_dispatched = false;
            state_.stratum_authority.submit_path_ready = false;
            state_.stratum_authority.submit_dispatch_count = 0;
            state_.stratum_authority.accepted_submit_count = 0;
            state_.stratum_authority.refused_submit_count = 0;
            state_.stratum_authority.queued_submit_candidate_count = 0;
            state_.stratum_authority.queued_priority_submit_candidate_count = 0;
            state_.stratum_authority.queued_block_submit_candidate_count = 0;
            state_.stratum_authority.normal_shares_since_priority_submit = 0;
            state_.stratum_authority.next_priority_spacing_target = 4;
            state_.stratum_authority.submit_gate_reason = "phase_local_validation_required";
            state_.stratum_authority.last_submitted_share_key.clear();
            state_.stratum_authority.last_submit_request_id.clear();
            state_.stratum_authority.last_submit_job_id.clear();
            state_.stratum_authority.last_submit_worker_index = 0U;
            state_.stratum_authority.last_submit_worker_name.clear();
            state_.stratum_authority.last_submit_nonce.clear();
            state_.stratum_authority.last_submit_hash_hex.clear();
            pending_submit_candidates_.clear();
            state_.stratum_authority.submit_preview_count = 0;
            state_.stratum_authority.offline_valid_submit_preview_count = 0;
            state_.stratum_authority.last_preview_share_key.clear();
            refresh_submission_control(state_.stratum_authority, worker_validation_histories_, now_unix_seconds());
            state_.stratum_authority.connection_state = state_.stratum_authority.active_job_header_hex.empty()
                ? "job_header_unresolved"
                : "workers_assigned";
            event_topic = "substrate.stratum.job.received";
            event_message = state_.stratum_authority.active_job_header_hex.empty()
                ? "Substrate firmware accepted Stratum job notify payload but could not assemble the live header"
                : "Substrate firmware accepted Stratum job notify payload";
            publish_workers_assigned = true;
            break;
        }

        authority_state = state_.stratum_authority;
    }

    bus_.publish(build_stratum_state_event(
        event_topic,
        event_message,
        authority_state,
        nullptr,
        nullptr,
        nullptr,
        &server_event,
        nullptr
    ));

    if (publish_workers_assigned) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.workers.assigned",
            "Substrate firmware assigned worker nonce windows for the active Stratum job",
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            &server_event,
            nullptr
        ));
    }

    for (const auto& preview : previews) {
        publish_submit_preview(authority_state, preview);
    }
}

void SubstrateFirmwareRuntime::handle_phase_clamped_share_actuation(const RuntimeEvent& event) {
    if (!event.has_phase_clamped_share_actuation) {
        return;
    }

    const PhaseClampedShareActuation actuation = event.phase_clamped_share_actuation;
    SubstrateStratumAuthorityState authority_state;
    SubstrateStratumSubmitPreviewPayload preview;
    SubstrateStratumDispatchPayload dispatch;
    bool publish_preview = false;
    bool publish_preview_suppressed = false;
    bool publish_dispatch = false;
    bool publish_suppressed = false;
    bool publish_local_validation_refused = false;
    bool publish_rate_limited = false;
    bool publish_auto_promotion = false;
    SubstrateStratumConnectionControl promotion_disconnect_control;
    SubstrateStratumConnectionIngress promoted_live_ingress;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.has_stratum_authority || !state_.stratum_authority.has_connection_ingress) {
            return;
        }
        if (actuation.connection_id != state_.stratum_authority.connection_ingress.connection_id
            || actuation.job_id != state_.stratum_authority.active_job_id) {
            return;
        }

        ++state_.stratum_authority.phase_clamped_actuation_count;
        if (actuation.resonant_candidate_available) {
            ++state_.stratum_authority.phase_clamped_resonant_candidate_count;
        }
        if (actuation.valid_share_candidate) {
            ++state_.stratum_authority.phase_clamped_candidate_count;
        }
        if (actuation.actuation_permitted) {
            ++state_.stratum_authority.phase_clamped_permitted_count;
        }
        if (actuation.all_parallel_harmonics_verified) {
            ++state_.stratum_authority.phase_clamped_harmonic_verified_count;
        }
        state_.stratum_authority.last_phase_clamped_gate_reason = actuation.gate_reason;
        state_.stratum_authority.last_phase_clamped_resonant_candidate_available =
            actuation.resonant_candidate_available;
        state_.stratum_authority.last_phase_clamped_valid_share_candidate = actuation.valid_share_candidate;
        state_.stratum_authority.last_phase_clamped_share_target_pass = actuation.share_target_pass;
        state_.stratum_authority.last_phase_clamped_block_target_pass = actuation.block_target_pass;
        state_.stratum_authority.last_phase_clamped_actuation_permitted = actuation.actuation_permitted;
        state_.stratum_authority.last_phase_clamped_all_parallel_harmonics_verified =
            actuation.all_parallel_harmonics_verified;
        state_.stratum_authority.last_phase_clamped_attempted_nonce_count = actuation.attempted_nonce_count;
        state_.stratum_authority.last_phase_clamped_valid_nonce_count = actuation.valid_nonce_count;
        state_.stratum_authority.last_phase_clamped_selected_coherence_score =
            actuation.selected_coherence_score;
        state_.stratum_authority.last_phase_clamped_phase_lock_error = actuation.phase_lock_error;
        state_.stratum_authority.last_phase_clamped_phase_clamp_strength = actuation.phase_clamp_strength;
        state_.stratum_authority.last_phase_clamped_target_resonance_norm = actuation.target_resonance_norm;
        state_.stratum_authority.last_phase_clamped_phase_alignment = actuation.phase_alignment;
        state_.stratum_authority.last_phase_clamped_validation_structure_norm =
            actuation.validation_structure_norm;
        state_.stratum_authority.last_phase_clamped_field_vector_phase_turns = actuation.field_vector_phase_turns;
        state_.stratum_authority.last_phase_clamped_phase_transport_turns = actuation.phase_transport_turns;
        state_.stratum_authority.last_phase_clamped_phase_lock_delta_turns = actuation.phase_lock_delta_turns;
        state_.stratum_authority.last_phase_clamped_transfer_drive_norm = actuation.transfer_drive_norm;
        state_.stratum_authority.last_phase_clamped_stability_gate_norm = actuation.stability_gate_norm;
        state_.stratum_authority.last_phase_clamped_damping_norm = actuation.damping_norm;
        state_.stratum_authority.last_phase_clamped_transport_drive_norm = actuation.transport_drive_norm;
        state_.stratum_authority.last_phase_clamped_resonance_activation_norm =
            actuation.resonance_activation_norm;
        state_.stratum_authority.last_phase_clamped_temporal_admissibility =
            actuation.temporal_admissibility;
        state_.stratum_authority.last_phase_clamped_zero_point_proximity = actuation.zero_point_proximity;
        state_.stratum_authority.last_phase_clamped_transport_readiness = actuation.transport_readiness;
        state_.stratum_authority.last_phase_clamped_share_confidence = actuation.share_confidence;
        state_.stratum_authority.last_phase_clamped_block_coherence_norm = actuation.block_coherence_norm;
        state_.stratum_authority.last_phase_clamped_submit_priority_score = actuation.submit_priority_score;
        state_.stratum_authority.last_phase_clamped_parallel_harmonic_count =
            actuation.parallel_harmonic_count;
        state_.stratum_authority.last_phase_clamped_verified_parallel_harmonic_count =
            actuation.verified_parallel_harmonic_count;
        state_.stratum_authority.last_phase_clamped_validated_parallel_harmonic_count =
            actuation.validated_parallel_harmonic_count;
        state_.stratum_authority.last_phase_clamped_resonance_reinforcement_count =
            actuation.resonance_reinforcement_count;
        state_.stratum_authority.last_phase_clamped_noise_lane_count = actuation.noise_lane_count;
        state_.stratum_authority.last_phase_clamped_queue_quality_class = actuation.queue_quality_class;
        state_.stratum_authority.last_phase_program_title = actuation.phase_program_title;
        state_.stratum_authority.last_phase_program_generated_dir = actuation.phase_program_generated_dir;
        state_.stratum_authority.last_phase_program_block_count = actuation.phase_program_block_count;
        state_.stratum_authority.last_phase_program_substrate_native =
            actuation.phase_program_substrate_native;
        state_.stratum_authority.last_phase_program_same_pulse_validation =
            actuation.phase_program_same_pulse_validation;
        state_.stratum_authority.last_phase_program_pool_format_ready =
            actuation.phase_program_pool_format_ready;
        state_.stratum_authority.last_phase_temporal_sequence = actuation.phase_temporal_sequence;

        if (is_phase_guided_preview_mode(state_.stratum_authority)) {
            if (!actuation_has_local_validation_candidates(actuation)) {
                return;
            }

            const HarmonicCandidateLocalValidation preview_candidate_validation =
                validate_harmonic_candidates_locally(state_.stratum_authority, actuation);
            if (!preview_candidate_validation.attempted || !preview_candidate_validation.nonce_parsed) {
                return;
            }
            const PhaseClampedShareActuation& preview_actuation = preview_candidate_validation.actuation;
            const SubstrateStratumPowEvaluation& preview_evaluation =
                preview_candidate_validation.phase_trace.evaluation;

            const std::string preview_share_key = build_submit_share_key(preview_actuation);
            if (!state_.stratum_authority.last_preview_share_key.empty()
                && state_.stratum_authority.last_preview_share_key == preview_share_key) {
                authority_state = state_.stratum_authority;
                publish_preview_suppressed = true;
            } else {
                preview = build_phase_guided_preview_payload(
                    state_.stratum_authority,
                    preview_actuation,
                    preview_evaluation);
                state_.stratum_authority.last_preview_share_key = preview_share_key;
                ++state_.stratum_authority.submit_preview_count;
                ++state_.stratum_authority.preview_validation_count;
                if (preview.offline_pow_valid) {
                    ++state_.stratum_authority.offline_valid_submit_preview_count;
                }
                record_local_validation_sample(
                    state_.stratum_authority,
                    worker_validation_histories_,
                    now_unix_seconds(),
                    "testmode_preview_validation",
                    preview_actuation.worker_name,
                    preview_actuation.worker_index,
                    preview_actuation.job_id,
                    preview_actuation.nonce_hex,
                    preview_evaluation.hash_hex,
                    preview.share_difficulty,
                    preview.offline_pow_valid,
                    preview.block_candidate_valid,
                    false,
                    preview.gate_reason);
                state_.stratum_authority.submit_gate_reason = preview.gate_reason;
                state_.stratum_authority.connection_state = preview.offline_pow_valid
                    ? "phase_guided_preview_validated"
                    : "phase_guided_preview_candidate";
                if (preview.offline_pow_valid && should_auto_promote_to_live_mode(state_.stratum_authority)) {
                    state_.stratum_authority.connection_state = "auto_promotion_requested";
                    state_.stratum_authority.last_response_message =
                        "Phase-guided header resonance reached the target hashrate envelope; reconnecting in livemode";
                    promotion_disconnect_control =
                        build_auto_promotion_disconnect_control(state_.stratum_authority);
                    promoted_live_ingress = build_promoted_live_mode_ingress(state_.stratum_authority);
                    publish_auto_promotion = true;
                }
                authority_state = state_.stratum_authority;
                publish_preview = true;
            }
            goto publish_phase_guided_preview_result;
        }

        if (!state_.stratum_authority.submit_policy_enabled || !actuation_has_local_validation_candidates(actuation)) {
            return;
        }

        const HarmonicCandidateLocalValidation live_candidate_validation =
            validate_harmonic_candidates_locally(state_.stratum_authority, actuation);
        if (!live_candidate_validation.attempted || !live_candidate_validation.nonce_parsed) {
            return;
        }
        const PhaseClampedShareActuation& validated_actuation = live_candidate_validation.actuation;
        const SubstrateStratumPowEvaluation& local_evaluation = live_candidate_validation.phase_trace.evaluation;
        const std::string preview_share_key = build_submit_share_key(validated_actuation);
        if (!state_.stratum_authority.last_preview_share_key.empty()
            && state_.stratum_authority.last_preview_share_key == preview_share_key) {
            authority_state = state_.stratum_authority;
            publish_preview_suppressed = true;
            goto publish_phase_guided_preview_result;
        }
        state_.stratum_authority.last_preview_share_key = preview_share_key;
        ++state_.stratum_authority.submit_preview_count;
        const double validation_timestamp_s = now_unix_seconds();
        if (!live_candidate_validation.valid) {
            state_.stratum_authority.submit_gate_reason = "live_submit_local_validation_failed";
            state_.stratum_authority.connection_state = "submit_locally_refused";
            state_.stratum_authority.last_validation_timestamp_unix_s = validation_timestamp_s;
            state_.stratum_authority.last_response_message = live_candidate_validation.hash_mismatch
                ? "Local Stratum share validation rejected a hash mismatch before network submit"
                : "Local Stratum share validation rejected an invalid share before network submit";
            append_validation_log_row(
                state_.stratum_authority,
                validation_timestamp_s,
                "livemode_local_validation",
                validated_actuation.worker_name,
                validated_actuation.worker_index,
                validated_actuation.job_id,
                validated_actuation.nonce_hex,
                local_evaluation.hash_hex,
                local_evaluation.share_difficulty,
                local_evaluation.valid_share,
                local_evaluation.valid_block,
                false,
                state_.stratum_authority.submit_gate_reason);
            authority_state = state_.stratum_authority;
            publish_local_validation_refused = true;
            goto publish_phase_guided_preview_result;
        }

        ++state_.stratum_authority.offline_valid_submit_preview_count;
        state_.stratum_authority.last_validation_timestamp_unix_s = validation_timestamp_s;
        persist_authority_collapse_feedback(state_.stratum_authority, actuation);
        if (local_evaluation.valid_share) {
            state_.stratum_authority.last_target_resonance_norm = std::max(
                state_.stratum_authority.last_target_resonance_norm,
                std::max(
                    validated_actuation.target_resonance_norm,
                    state_.stratum_authority.target_resonance_floor));
        }
        ++state_.stratum_authority.locally_validated_share_count;
        if (validated_actuation.worker_index < worker_validation_histories_.size()) {
            const double effective_share_difficulty = local_evaluation.share_difficulty > 0.0
                ? local_evaluation.share_difficulty
                : state_.stratum_authority.difficulty;
            worker_validation_histories_[validated_actuation.worker_index].push_back(StratumValidationSample{
                validation_timestamp_s,
                effective_share_difficulty,
                expected_hashes_for_difficulty(effective_share_difficulty),
            });
        }
        prune_worker_validation_histories(
            worker_validation_histories_,
            state_.stratum_authority.validation_jitter_window_seconds,
            validation_timestamp_s);
        refresh_submission_control(state_.stratum_authority, worker_validation_histories_, validation_timestamp_s);
        if (!state_.stratum_authority.submit_path_ready) {
            state_.stratum_authority.connection_state = "submit_locally_validated";
            state_.stratum_authority.last_response_message =
                "Local Stratum share validation accepted the phase candidate, but the live submit gate remains closed";
            append_validation_log_row(
                state_.stratum_authority,
                validation_timestamp_s,
                "livemode_local_validation",
                validated_actuation.worker_name,
                validated_actuation.worker_index,
                validated_actuation.job_id,
                validated_actuation.nonce_hex,
                local_evaluation.hash_hex,
                local_evaluation.share_difficulty,
                true,
                local_evaluation.valid_block,
                false,
                state_.stratum_authority.submit_gate_reason);
            authority_state = state_.stratum_authority;
            goto publish_phase_guided_preview_result;
        }

        PhaseClampedShareActuation queued_actuation = enrich_validated_share_candidate(
            validated_actuation,
            live_candidate_validation.phase_trace);
        state_.stratum_authority.last_phase_clamped_share_target_pass = queued_actuation.share_target_pass;
        state_.stratum_authority.last_phase_clamped_block_target_pass = queued_actuation.block_target_pass;
        state_.stratum_authority.last_phase_clamped_block_coherence_norm = queued_actuation.block_coherence_norm;
        state_.stratum_authority.last_phase_clamped_submit_priority_score = queued_actuation.submit_priority_score;
        state_.stratum_authority.last_phase_clamped_resonance_reinforcement_count =
            queued_actuation.resonance_reinforcement_count;
        state_.stratum_authority.last_phase_clamped_noise_lane_count = queued_actuation.noise_lane_count;
        state_.stratum_authority.last_phase_clamped_queue_quality_class = queued_actuation.queue_quality_class;
        const std::string share_key = build_submit_share_key(queued_actuation);
        if (!state_.stratum_authority.last_submitted_share_key.empty()
            && state_.stratum_authority.last_submitted_share_key == share_key) {
            state_.stratum_authority.last_validation_timestamp_unix_s = validation_timestamp_s;
            append_validation_log_row(
                state_.stratum_authority,
                validation_timestamp_s,
                "livemode_local_validation",
                queued_actuation.worker_name,
                queued_actuation.worker_index,
                queued_actuation.job_id,
                queued_actuation.nonce_hex,
                local_evaluation.hash_hex,
                local_evaluation.share_difficulty,
                true,
                local_evaluation.valid_block,
                false,
                "duplicate_share_suppressed");
            authority_state = state_.stratum_authority;
            publish_suppressed = true;
            goto publish_phase_guided_preview_result;
        }

        upsert_pending_submit_candidate(
            pending_submit_candidates_,
            QueuedSubmitCandidate{
                queued_actuation,
                live_candidate_validation.phase_trace,
                share_key,
                validation_timestamp_s,
            });
        refresh_pending_submit_queue_metrics(state_.stratum_authority, pending_submit_candidates_);

        const double cadence_guard_s = state_.stratum_authority.submission_interval_target_s > 0.0
            ? std::max(
                0.0,
                state_.stratum_authority.submission_interval_target_s
                    - std::max(
                        state_.stratum_authority.measured_validation_jitter_s,
                        state_.stratum_authority.submission_interval_target_s
                            * state_.stratum_authority.reserve_jitter_fraction))
            : 0.0;
        if (state_.stratum_authority.last_submit_timestamp_unix_s > 0.0
            && cadence_guard_s > 0.0
            && (validation_timestamp_s - state_.stratum_authority.last_submit_timestamp_unix_s) < cadence_guard_s) {
            state_.stratum_authority.connection_state = "submit_rate_limited";
            state_.stratum_authority.submit_gate_reason = "submission_rate_limited";
            state_.stratum_authority.last_response_message =
                "Submission cadence control deferred a locally valid share to preserve the target hashrate envelope";
            append_validation_log_row(
                state_.stratum_authority,
                validation_timestamp_s,
                "livemode_local_validation",
                validated_actuation.worker_name,
                validated_actuation.worker_index,
                validated_actuation.job_id,
                validated_actuation.nonce_hex,
                local_evaluation.hash_hex,
                local_evaluation.share_difficulty,
                true,
                local_evaluation.valid_block,
                false,
                "submission_rate_limited");
            authority_state = state_.stratum_authority;
            publish_rate_limited = true;
            goto publish_phase_guided_preview_result;
        }

        const auto selected_candidate = select_next_submit_candidate(
            pending_submit_candidates_,
            state_.stratum_authority);
        if (selected_candidate == pending_submit_candidates_.end()) {
            authority_state = state_.stratum_authority;
            goto publish_phase_guided_preview_result;
        }

        const PhaseClampedShareActuation dispatch_actuation = selected_candidate->actuation;
        const SubstrateStratumPowEvaluation& dispatch_evaluation = selected_candidate->phase_trace.evaluation;
        append_validation_log_row(
            state_.stratum_authority,
            validation_timestamp_s,
            "livemode_local_validation",
            dispatch_actuation.worker_name,
            dispatch_actuation.worker_index,
            dispatch_actuation.job_id,
            dispatch_actuation.nonce_hex,
            dispatch_evaluation.hash_hex,
            dispatch_evaluation.share_difficulty,
            true,
            dispatch_evaluation.valid_block,
            true,
            state_.stratum_authority.submit_gate_reason);
        dispatch = build_live_submit_dispatch_payload(state_.stratum_authority, dispatch_actuation);
        state_.stratum_authority.submit_dispatched = true;
        ++state_.stratum_authority.submit_dispatch_count;
        ++state_.stratum_authority.dispatch_count;
        state_.stratum_authority.connection_state = "submit_dispatched";
        state_.stratum_authority.last_request_id = dispatch.request_id;
        state_.stratum_authority.last_command = dispatch.method;
        state_.stratum_authority.last_event_topic = stratum_dispatch_topic(StratumCommandKind::Submit);
        state_.stratum_authority.last_submitted_share_key = selected_candidate->share_key;
        state_.stratum_authority.last_submit_request_id = dispatch.request_id;
        state_.stratum_authority.last_submit_job_id = dispatch_actuation.job_id;
        state_.stratum_authority.last_submit_worker_index = dispatch_actuation.worker_index;
        state_.stratum_authority.last_submit_worker_name = dispatch_actuation.worker_name;
        state_.stratum_authority.last_submit_nonce = dispatch_actuation.nonce_hex;
        state_.stratum_authority.last_submit_hash_hex = dispatch_evaluation.hash_hex;
        state_.stratum_authority.last_submit_timestamp_unix_s = validation_timestamp_s;
        update_post_dispatch_mix_state(state_.stratum_authority, dispatch_actuation);
        pending_submit_candidates_.erase(selected_candidate);
        refresh_pending_submit_queue_metrics(state_.stratum_authority, pending_submit_candidates_);
        authority_state = state_.stratum_authority;
        publish_dispatch = true;
    }

publish_phase_guided_preview_result:
    if (publish_preview_suppressed) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.submit.preview.suppressed",
            "Substrate firmware suppressed a duplicate phase-guided header-resonance validation payload",
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
        return;
    }

    if (publish_preview) {
        publish_submit_preview(authority_state, preview);
        if (publish_auto_promotion) {
            RuntimeEvent disconnect_event;
            disconnect_event.topic = "substrate.stratum.connection.control";
            disconnect_event.message = promotion_disconnect_control.reason;
            disconnect_event.stratum_connection_control = promotion_disconnect_control;
            disconnect_event.has_stratum_connection_control = true;
            bus_.publish(disconnect_event);

            RuntimeEvent promoted_ingress_event;
            promoted_ingress_event.topic = "substrate.stratum.connection.ingress";
            promoted_ingress_event.message = "Auto-promoted phase-guided header-resonance session into livemode";
            promoted_ingress_event.stratum_connection_ingress = promoted_live_ingress;
            promoted_ingress_event.has_stratum_connection_ingress = true;
            bus_.publish(promoted_ingress_event);
        }
        return;
    }

    if (publish_suppressed) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.submit.suppressed",
            "Substrate firmware suppressed a duplicate live Stratum share submit request",
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
        return;
    }

    if (publish_local_validation_refused) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.submit.local_validation_refused",
            authority_state.last_response_message,
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
        return;
    }

    if (publish_rate_limited) {
        bus_.publish(build_stratum_state_event(
            "substrate.stratum.submit.rate_limited",
            authority_state.last_response_message,
            authority_state,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ));
        return;
    }

    if (publish_dispatch) {
        bus_.publish(build_stratum_state_event(
            stratum_dispatch_topic(StratumCommandKind::Submit),
            "Substrate firmware promoted a phase-clamped share actuation into a live Stratum submit dispatch",
            authority_state,
            nullptr,
            &dispatch,
            nullptr,
            nullptr,
            nullptr
        ));
    }
}

void SubstrateFirmwareRuntime::publish_stratum_dispatch(
    StratumCommandKind kind,
    const SubstrateStratumAuthorityState& authority_state
) {
    const SubstrateStratumDispatchPayload dispatch =
        build_stratum_dispatch_payload(authority_state.connection_ingress, kind);
    SubstrateStratumAuthorityState updated_authority_state;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (kind) {
        case StratumCommandKind::Connect:
            state_.stratum_authority.connect_dispatched = true;
            state_.stratum_authority.connection_state = "connect_dispatched";
            break;
        case StratumCommandKind::Subscribe:
            state_.stratum_authority.subscribe_dispatched = true;
            state_.stratum_authority.connection_state = "subscribe_dispatched";
            break;
        case StratumCommandKind::Authorize:
            state_.stratum_authority.authorize_dispatched = true;
            state_.stratum_authority.connection_state = "authorize_dispatched";
            break;
        case StratumCommandKind::Submit:
            state_.stratum_authority.submit_dispatched = true;
            state_.stratum_authority.connection_state = "submit_dispatched";
            break;
        }
        ++state_.stratum_authority.dispatch_count;
        state_.stratum_authority.last_request_id = dispatch.request_id;
        state_.stratum_authority.last_command = dispatch.method;
        state_.stratum_authority.last_event_topic = stratum_dispatch_topic(kind);
        updated_authority_state = state_.stratum_authority;
    }

    bus_.publish(build_stratum_state_event(
        stratum_dispatch_topic(kind),
        "API-anchor evaluator emitted a deterministic Stratum dispatch payload",
        updated_authority_state,
        nullptr,
        &dispatch,
        nullptr,
        nullptr,
        nullptr
    ));
}

void SubstrateFirmwareRuntime::publish_submit_preview(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumSubmitPreviewPayload& preview
) {
    bus_.publish(build_stratum_state_event(
        "substrate.stratum.submit.preview",
        "Substrate firmware prepared a deterministic Stratum submit payload preview and kept network send gated",
        authority_state,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &preview
    ));
}

}  // namespace qbit_miner
