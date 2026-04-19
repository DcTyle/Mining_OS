#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/runtime/substrate_stratum_pow.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace qbit_miner {

namespace {

constexpr const char* kStratumClientLabel = "QBitMiner/0.1";
constexpr std::uint16_t kDefaultStratumPort = 3333;
constexpr std::uint16_t kF2PoolRecommendedPort = 1314;

[[nodiscard]] std::string escape_json(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string build_connection_id(const SubstrateStratumConnectionIngress& ingress) {
    if (!ingress.connection_id.empty()) {
        return ingress.connection_id;
    }
    std::ostringstream stream;
    stream << ingress.host << ':' << ingress.port << '/' << ingress.worker_name;
    return stream.str();
}

[[nodiscard]] double clamp_network_share_fraction(double requested_fraction) {
    if (requested_fraction <= 0.0 || !std::isfinite(requested_fraction)) {
        return 0.05;
    }
    return std::clamp(requested_fraction, 1.0e-9, 1.0);
}

[[nodiscard]] double clamp_target_hashrate(double requested_hashrate_hs) {
    if (!std::isfinite(requested_hashrate_hs) || requested_hashrate_hs <= 0.0) {
        return 0.0;
    }
    return requested_hashrate_hs;
}

[[nodiscard]] double clamp_validation_jitter_window(double requested_window_seconds) {
    if (!std::isfinite(requested_window_seconds) || requested_window_seconds <= 0.0) {
        return 60.0;
    }
    return std::clamp(requested_window_seconds, 1.0, 3600.0);
}

[[nodiscard]] std::size_t clamp_validation_sample_count(std::size_t requested_samples) {
    return std::clamp<std::size_t>(requested_samples == 0U ? kStratumWorkerSlotCount : requested_samples, 1U, 1024U);
}

[[nodiscard]] std::size_t clamp_allowed_worker_count(std::size_t requested_count) {
    return std::clamp<std::size_t>(requested_count == 0U ? kStratumWorkerSlotCount : requested_count, 1U, kStratumWorkerSlotCount);
}

[[nodiscard]] std::string normalize_host(std::string host) {
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return host;
}

[[nodiscard]] bool host_matches_suffix(const std::string& host, const std::string& suffix) {
    if (host.size() < suffix.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), host.rbegin());
}

[[nodiscard]] std::uint16_t default_stratum_port_for_host(const std::string& host) {
    const SubstrateStratumPoolPolicy pool_policy = resolve_stratum_pool_policy(host);
    return pool_policy.pool_name == "f2pool"
        ? kF2PoolRecommendedPort
        : kDefaultStratumPort;
}

[[nodiscard]] std::string normalize_operating_mode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (mode == "livemode" || mode == "testmode") {
        return mode;
    }
    return {};
}

[[nodiscard]] bool is_f2pool_worker_name_valid(const std::string& worker_name) {
    const std::size_t separator_index = worker_name.rfind('.');
    if (separator_index == std::string::npos || separator_index == 0U || separator_index + 1U >= worker_name.size()) {
        return false;
    }

    const std::string worker_suffix = worker_name.substr(separator_index + 1U);
    return std::all_of(worker_suffix.begin(), worker_suffix.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0;
    });
}

[[nodiscard]] std::string format_worker_extranonce2_hex(std::size_t worker_index, std::uint32_t extranonce2_size) {
    const std::size_t width = std::max<std::size_t>(8U, static_cast<std::size_t>(extranonce2_size) * 2U);
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << static_cast<unsigned long long>(worker_index + 1U);
    std::string hex = stream.str();
    if (hex.size() < width) {
        hex.insert(hex.begin(), width - hex.size(), '0');
    } else if (hex.size() > width) {
        hex = hex.substr(hex.size() - width);
    }
    return hex;
}

}  // namespace

std::string stratum_command_label(StratumCommandKind kind) {
    switch (kind) {
    case StratumCommandKind::Connect:
        return "connect";
    case StratumCommandKind::Subscribe:
        return "subscribe";
    case StratumCommandKind::Authorize:
        return "authorize";
    case StratumCommandKind::Submit:
        return "submit";
    }
    return "connect";
}

std::string stratum_command_method(StratumCommandKind kind) {
    switch (kind) {
    case StratumCommandKind::Connect:
        return "tcp.connect";
    case StratumCommandKind::Subscribe:
        return "mining.subscribe";
    case StratumCommandKind::Authorize:
        return "mining.authorize";
    case StratumCommandKind::Submit:
        return "mining.submit";
    }
    return "tcp.connect";
}

std::string stratum_dispatch_topic(StratumCommandKind kind) {
    return "substrate.stratum.dispatch." + stratum_command_label(kind);
}

std::string stratum_server_event_label(StratumServerEventKind kind) {
    switch (kind) {
    case StratumServerEventKind::SetDifficulty:
        return "set_difficulty";
    case StratumServerEventKind::Notify:
        return "notify";
    }
    return "set_difficulty";
}

std::string stratum_server_event_method(StratumServerEventKind kind) {
    switch (kind) {
    case StratumServerEventKind::SetDifficulty:
        return "mining.set_difficulty";
    case StratumServerEventKind::Notify:
        return "mining.notify";
    }
    return "mining.set_difficulty";
}

std::string stratum_server_event_topic(StratumServerEventKind kind) {
    return "substrate.stratum.server." + stratum_server_event_label(kind);
}

double clamp_stratum_request_rate(double requested_rate) {
    return std::clamp(requested_rate, 0.05, 2.0);
}

SubstrateStratumPoolPolicy resolve_stratum_pool_policy(const std::string& host) {
    const std::string normalized_host = normalize_host(host);
    if (normalized_host == "f2pool.com"
        || host_matches_suffix(normalized_host, ".f2pool.com")) {
        SubstrateStratumPoolPolicy policy;
        policy.pool_name = "f2pool";
        policy.supports_client_suggest_difficulty = false;
        policy.connection_cap_unbounded = true;
        policy.connection_cap = 0U;
        policy.hashrate_window_seconds = 900.0;
        policy.performance_window_seconds = 86400.0;
        policy.worker_online_reference_window_seconds = 900.0;
        policy.reasonable_delayed_share_fraction = 0.02;
        return policy;
    }

    return {};
}

SubstrateStratumConnectionIngress sanitize_stratum_connection_ingress(const SubstrateStratumConnectionIngress& ingress) {
    SubstrateStratumConnectionIngress sanitized = ingress;
    const SubstrateStratumPoolPolicy pool_policy = resolve_stratum_pool_policy(sanitized.host);
    if (sanitized.port == 0U) {
        sanitized.port = default_stratum_port_for_host(sanitized.host);
    }
    if (sanitized.worker_password.empty()) {
        sanitized.worker_password = "x";
    }
    sanitized.max_requests_per_second = clamp_stratum_request_rate(sanitized.max_requests_per_second);
    sanitized.target_network_share_fraction = clamp_network_share_fraction(sanitized.target_network_share_fraction);
    sanitized.target_hashrate_hs = clamp_target_hashrate(sanitized.target_hashrate_hs);
    sanitized.allowed_worker_count = clamp_allowed_worker_count(sanitized.allowed_worker_count);
    sanitized.validation_jitter_window_seconds =
        clamp_validation_jitter_window(sanitized.validation_jitter_window_seconds);
    if (pool_policy.hashrate_window_seconds > 0.0) {
        sanitized.validation_jitter_window_seconds =
            std::max(sanitized.validation_jitter_window_seconds, pool_policy.hashrate_window_seconds);
    }
    sanitized.min_validation_jitter_samples =
        clamp_validation_sample_count(sanitized.min_validation_jitter_samples);
    sanitized.operating_mode = normalize_operating_mode(sanitized.operating_mode);
    if (sanitized.authoritative_program_source.empty()) {
        sanitized.authoritative_program_source = std::string(phase_programs::kStratumConnectionProgramSource);
    }
    if (sanitized.operating_mode == "livemode") {
        sanitized.dry_run_only = false;
        sanitized.allow_live_submit = true;
        sanitized.phase_guided_preview_test_mode = false;
    } else if (sanitized.operating_mode == "testmode") {
        sanitized.dry_run_only = false;
        sanitized.allow_live_submit = false;
        sanitized.phase_guided_preview_test_mode = true;
    } else if (sanitized.allow_live_submit) {
        sanitized.dry_run_only = false;
        sanitized.phase_guided_preview_test_mode = false;
    } else if (sanitized.phase_guided_preview_test_mode) {
        sanitized.dry_run_only = false;
    }
    if (sanitized.dry_run_only) {
        sanitized.allow_live_submit = false;
        sanitized.phase_guided_preview_test_mode = false;
    }
    sanitized.connection_id = build_connection_id(sanitized);
    return sanitized;
}

bool is_valid_stratum_connection_ingress(const SubstrateStratumConnectionIngress& ingress) {
    const SubstrateStratumPoolPolicy pool_policy = resolve_stratum_pool_policy(ingress.host);
    const bool pool_worker_name_valid = pool_policy.pool_name != "f2pool"
        || is_f2pool_worker_name_valid(ingress.worker_name);

    return !ingress.host.empty()
        && ingress.port > 0U
        && !ingress.worker_name.empty()
        && pool_worker_name_valid
        && ingress.max_requests_per_second > 0.0
        && ingress.target_network_share_fraction > 0.0
        && ingress.target_network_share_fraction <= 1.0
        && ingress.allowed_worker_count > 0U
        && ingress.allowed_worker_count <= kStratumWorkerSlotCount
        && ingress.validation_jitter_window_seconds > 0.0
        && ingress.min_validation_jitter_samples > 0U
        && (ingress.operating_mode.empty()
            || ingress.operating_mode == "livemode"
            || ingress.operating_mode == "testmode")
        && ingress.authoritative_program_source == phase_programs::kStratumConnectionProgramSource;
}

SubstrateStratumConnectionControl sanitize_stratum_connection_control(const SubstrateStratumConnectionControl& control) {
    SubstrateStratumConnectionControl sanitized = control;
    if (sanitized.reason.empty()) {
        sanitized.reason = "Stratum session disconnected by operator";
    }
    if (sanitized.authoritative_program_source.empty()) {
        sanitized.authoritative_program_source = std::string(phase_programs::kStratumConnectionProgramSource);
    }
    return sanitized;
}

bool is_valid_stratum_connection_control(const SubstrateStratumConnectionControl& control) {
    return control.disconnect_requested
        && control.authoritative_program_source == phase_programs::kStratumConnectionProgramSource;
}

std::string build_stratum_request_id(const std::string& connection_id, StratumCommandKind kind) {
    return connection_id + ':' + stratum_command_label(kind);
}

SubstrateStratumDispatchPayload build_stratum_dispatch_payload(
    const SubstrateStratumConnectionIngress& ingress,
    StratumCommandKind kind
) {
    const SubstrateStratumConnectionIngress sanitized = sanitize_stratum_connection_ingress(ingress);

    SubstrateStratumDispatchPayload payload;
    payload.command_kind = kind;
    payload.connection_id = sanitized.connection_id;
    payload.request_id = build_stratum_request_id(sanitized.connection_id, kind);
    payload.method = stratum_command_method(kind);
    payload.host = sanitized.host;
    payload.port = sanitized.port;
    payload.worker_name = sanitized.worker_name;
    payload.operating_mode = sanitized.operating_mode;
    payload.dry_run_only = sanitized.dry_run_only;
    payload.allow_live_submit = sanitized.allow_live_submit;
    payload.phase_guided_preview_test_mode = sanitized.phase_guided_preview_test_mode;
    payload.max_requests_per_second = sanitized.max_requests_per_second;
    payload.target_network_share_fraction = sanitized.target_network_share_fraction;
    payload.target_hashrate_hs = sanitized.target_hashrate_hs;
    payload.allowed_worker_count = sanitized.allowed_worker_count;
    payload.validation_jitter_window_seconds = sanitized.validation_jitter_window_seconds;
    payload.min_validation_jitter_samples = sanitized.min_validation_jitter_samples;
    payload.authoritative_program_source = sanitized.authoritative_program_source;

    std::ostringstream stream;
    stream << '{';
    switch (kind) {
    case StratumCommandKind::Connect:
        stream << "\"transport\": \"tcp\", "
               << "\"host\": \"" << escape_json(sanitized.host) << "\", "
               << "\"port\": " << sanitized.port << ", "
               << "\"connection_id\": \"" << escape_json(payload.request_id) << "\", "
               << "\"operating_mode\": \"" << escape_json(sanitized.operating_mode) << "\", "
               << "\"dry_run_only\": " << (sanitized.dry_run_only ? "true" : "false") << ", "
               << "\"allow_live_submit\": " << (sanitized.allow_live_submit ? "true" : "false") << ", "
               << "\"phase_guided_preview_test_mode\": " << (sanitized.phase_guided_preview_test_mode ? "true" : "false") << ", "
               << "\"max_requests_per_second\": " << sanitized.max_requests_per_second << ", "
               << "\"target_network_share_fraction\": " << sanitized.target_network_share_fraction << ", "
               << "\"target_hashrate_hs\": " << sanitized.target_hashrate_hs << ", "
               << "\"allowed_worker_count\": " << sanitized.allowed_worker_count << ", "
               << "\"validation_jitter_window_seconds\": " << sanitized.validation_jitter_window_seconds << ", "
               << "\"min_validation_jitter_samples\": " << sanitized.min_validation_jitter_samples;
        break;
    case StratumCommandKind::Subscribe:
        stream << "\"id\": \"" << escape_json(payload.request_id) << "\", "
               << "\"method\": \"mining.subscribe\", "
               << "\"params\": [\"" << kStratumClientLabel << "\"]";
        break;
    case StratumCommandKind::Authorize:
        stream << "\"id\": \"" << escape_json(payload.request_id) << "\", "
               << "\"method\": \"mining.authorize\", "
               << "\"params\": [\"" << escape_json(sanitized.worker_name) << "\", \""
               << escape_json(sanitized.worker_password) << "\"]";
        break;
    case StratumCommandKind::Submit:
        stream << "\"id\": \"" << escape_json(payload.request_id) << "\", "
               << "\"method\": \"mining.submit\", "
               << "\"params\": []";
        break;
    }
    stream << '}';
    payload.payload_json = stream.str();
    return payload;
}

std::array<SubstrateStratumWorkerAssignment, kStratumWorkerSlotCount> build_stratum_worker_assignments(
    const SubstrateStratumConnectionIngress& ingress,
    const std::string& job_id
) {
    std::array<SubstrateStratumWorkerAssignment, kStratumWorkerSlotCount> assignments {};

    const std::uint64_t nonce_space = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ULL;
    const std::size_t active_worker_count = clamp_allowed_worker_count(ingress.allowed_worker_count);
    const std::uint64_t window = nonce_space / active_worker_count;
    for (std::size_t index = 0; index < assignments.size(); ++index) {
        SubstrateStratumWorkerAssignment assignment;
        assignment.worker_index = index;
        assignment.active = index < active_worker_count;
        assignment.job_id = job_id;
        if (!assignment.active) {
            assignments[index] = assignment;
            continue;
        }

        std::ostringstream worker_name;
        worker_name << ingress.worker_name << ".slot" << index;
        assignment.worker_name = worker_name.str();

        const std::uint64_t start = window * index;
        const std::uint64_t end = index + 1 == active_worker_count
            ? std::numeric_limits<std::uint32_t>::max()
            : (start + window - 1ULL);
        assignment.nonce_start = static_cast<std::uint32_t>(start);
        assignment.nonce_end = static_cast<std::uint32_t>(end);
        assignments[index] = assignment;
    }

    return assignments;
}

SubstrateStratumSubmitPreviewPayload build_stratum_submit_preview_payload(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
) {
    SubstrateStratumSubmitPreviewPayload payload;
    const std::string submit_identity = authority_state.connection_ingress.worker_name.empty()
        ? assignment.worker_name
        : authority_state.connection_ingress.worker_name;
    payload.connection_id = authority_state.connection_ingress.connection_id;
    payload.job_id = assignment.job_id;
    payload.worker_index = assignment.worker_index;
    payload.worker_name = assignment.worker_name;
    payload.method = "mining.submit";
    payload.ntime = authority_state.active_job_ntime;
    payload.network_send_allowed = false;
    payload.gate_reason = authority_state.submit_gate_reason;
    payload.target_hex = authority_state.active_share_target_hex;
    payload.share_target_hex = authority_state.active_share_target_hex;
    payload.block_target_hex = authority_state.active_block_target_hex;
    payload.share_difficulty = authority_state.difficulty;
    payload.block_difficulty = authority_state.active_block_difficulty;
    payload.expected_hashes_for_share = authority_state.expected_hashes_for_share;
    payload.target_network_share_fraction = authority_state.target_network_share_fraction;
    payload.network_hashrate_hs = authority_state.network_hashrate_hs;
    payload.required_hashrate_hs = authority_state.required_hashrate_hs;
    payload.required_share_submissions_per_s = authority_state.required_share_submissions_per_s;
    payload.measured_nonce_observed = authority_state.last_measured_nonce_observed;
    payload.measured_hash_phase_turns = authority_state.last_measured_hash_phase_turns;
    payload.measured_nonce_phase_turns = authority_state.last_measured_nonce_phase_turns;
    payload.collapse_feedback_phase_turns = authority_state.last_collapse_feedback_phase_turns;
    payload.collapse_relock_error_turns = authority_state.last_collapse_relock_error_turns;
    payload.observer_collapse_strength = authority_state.last_observer_collapse_strength;
    payload.phase_flux_conservation = authority_state.last_phase_flux_conservation;
    payload.nonce_collapse_confidence = authority_state.last_nonce_collapse_confidence;
    payload.authoritative_program_source = std::string(phase_programs::kMiningResonanceProgramSource);

    payload.extranonce2 = build_stratum_worker_extranonce2_hex(authority_state, assignment);

    std::ostringstream nonce;
    nonce << std::hex << std::setfill('0') << std::setw(8) << assignment.nonce_start;
    payload.nonce = nonce.str();

    std::ostringstream request_id;
    request_id << payload.connection_id << ":submit-preview:" << payload.job_id << ':' << assignment.worker_index;
    payload.request_id = request_id.str();

    std::ostringstream stream;
    stream << '{'
           << "\"id\": \"" << escape_json(payload.request_id) << "\", "
           << "\"method\": \"mining.submit\", "
            << "\"params\": [\"" << escape_json(submit_identity) << "\", \""
           << escape_json(payload.job_id) << "\", \"" << payload.extranonce2 << "\", \""
           << escape_json(payload.ntime) << "\", \"" << payload.nonce << "\"]"
           << '}';
    payload.payload_json = stream.str();
    return payload;
}

std::string build_stratum_worker_extranonce2_hex(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
) {
    return format_worker_extranonce2_hex(assignment.worker_index, authority_state.extranonce2_size);
}

std::string resolve_stratum_job_header_hex(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
) {
    const bool can_build_worker_header = !authority_state.active_job_prevhash.empty()
        && !authority_state.active_job_coinbase1.empty()
        && !authority_state.active_job_coinbase2.empty()
        && !authority_state.active_job_version.empty()
        && !authority_state.active_job_nbits.empty()
        && !authority_state.active_job_ntime.empty()
        && !authority_state.extranonce1.empty()
        && authority_state.extranonce2_size > 0U;
    if (can_build_worker_header) {
        return build_stratum_header_hex(
            authority_state.active_job_prevhash,
            authority_state.active_job_coinbase1,
            authority_state.extranonce1,
            authority_state.extranonce2_size,
            authority_state.active_job_coinbase2,
            authority_state.active_job_merkle_branches,
            authority_state.active_job_version,
            authority_state.active_job_nbits,
            authority_state.active_job_ntime,
            build_stratum_worker_extranonce2_hex(authority_state, assignment)
        );
    }
    return authority_state.active_job_header_hex;
}

}  // namespace qbit_miner
