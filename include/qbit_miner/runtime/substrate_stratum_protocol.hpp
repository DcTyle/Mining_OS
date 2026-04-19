#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "qbit_miner/runtime/substrate_phase_programs.hpp"

namespace qbit_miner {

enum class StratumCommandKind {
    Connect,
    Subscribe,
    Authorize,
    Submit,
};

enum class StratumServerEventKind {
    SetDifficulty,
    Notify,
};

inline constexpr std::size_t kStratumWorkerSlotCount = 4;

struct SubstrateStratumConnectionIngress {
    std::string host;
    std::uint16_t port = 3333;
    std::string worker_name;
    std::string worker_password = "x";
    std::string operating_mode;
    bool dry_run_only = true;
    bool allow_live_submit = false;
    bool phase_guided_preview_test_mode = false;
    bool auto_promote_to_live_mode = false;
    double max_requests_per_second = 1.0;
    double target_network_share_fraction = 0.05;
    double target_hashrate_hs = 0.0;
    std::size_t max_invalid_pool_submissions = 3U;
    std::size_t allowed_worker_count = kStratumWorkerSlotCount;
    double validation_jitter_window_seconds = 60.0;
    std::size_t min_validation_jitter_samples = kStratumWorkerSlotCount;
    std::string validation_log_csv_path;
    std::string connection_id;
    std::string authoritative_program_source = std::string(phase_programs::kStratumConnectionProgramSource);
};

struct SubstrateStratumConnectionControl {
    std::string connection_id;
    bool disconnect_requested = false;
    std::string reason;
    std::string authoritative_program_source = std::string(phase_programs::kStratumConnectionProgramSource);
};

struct SubstrateStratumDispatchPayload {
    StratumCommandKind command_kind = StratumCommandKind::Connect;
    std::string connection_id;
    std::string request_id;
    std::string method;
    std::string payload_json;
    std::string host;
    std::uint16_t port = 3333;
    std::string worker_name;
    std::string operating_mode;
    bool dry_run_only = true;
    bool allow_live_submit = false;
    bool phase_guided_preview_test_mode = false;
    double max_requests_per_second = 1.0;
    double target_network_share_fraction = 0.05;
    double target_hashrate_hs = 0.0;
    std::size_t allowed_worker_count = kStratumWorkerSlotCount;
    double validation_jitter_window_seconds = 60.0;
    std::size_t min_validation_jitter_samples = kStratumWorkerSlotCount;
    std::string authoritative_program_source;
};

struct SubstrateStratumResponsePayload {
    StratumCommandKind command_kind = StratumCommandKind::Connect;
    std::string connection_id;
    std::string request_id;
    std::string method;
    bool accepted = false;
    std::string payload_json;
    std::string message;
    std::string subscription_id;
    std::string extranonce1;
    std::uint32_t extranonce2_size = 0;
};

struct SubstrateStratumServerEventPayload {
    StratumServerEventKind event_kind = StratumServerEventKind::SetDifficulty;
    std::string connection_id;
    std::string method;
    std::string payload_json;
    double difficulty = 0.0;
    std::string job_id;
    std::string header_hex;
    std::string prevhash;
    std::string coinbase1;
    std::string coinbase2;
    std::vector<std::string> merkle_branches;
    std::string version;
    std::string nbits;
    std::string ntime;
    bool clean_jobs = false;
};

struct SubstrateStratumWorkerAssignment {
    std::size_t worker_index = 0;
    bool active = false;
    std::string job_id;
    std::string worker_name;
    std::uint32_t nonce_start = 0;
    std::uint32_t nonce_end = 0;
};

struct SubstrateStratumPoolPolicy {
    std::string pool_name = "generic";
    bool supports_client_suggest_difficulty = false;
    bool connection_cap_unbounded = false;
    std::size_t connection_cap = 0;
    double hashrate_window_seconds = 60.0;
    double performance_window_seconds = 60.0;
    double worker_online_reference_window_seconds = 60.0;
    double reasonable_delayed_share_fraction = 0.0;
};

struct SubstrateStratumSubmitPreviewPayload {
    std::string connection_id;
    std::string job_id;
    std::size_t worker_index = 0;
    std::string worker_name;
    std::string request_id;
    std::string method;
    std::string payload_json;
    std::string extranonce2;
    std::string ntime;
    std::string nonce;
    bool network_send_allowed = false;
    std::string gate_reason;
    bool offline_pow_checked = false;
    bool offline_pow_valid = false;
    std::string hash_hex;
    std::string target_hex;
    std::string share_target_hex;
    std::string block_target_hex;
    double share_difficulty = 0.0;
    double block_difficulty = 0.0;
    double expected_hashes_for_share = 0.0;
    double target_network_share_fraction = 0.05;
    double network_hashrate_hs = 0.0;
    double required_hashrate_hs = 0.0;
    double required_share_submissions_per_s = 0.0;
    bool block_candidate_valid = false;
    bool measured_nonce_observed = false;
    double measured_hash_phase_turns = 0.0;
    double measured_nonce_phase_turns = 0.0;
    double collapse_feedback_phase_turns = 0.0;
    double collapse_relock_error_turns = 0.0;
    double observer_collapse_strength = 0.0;
    double phase_flux_conservation = 0.0;
    double nonce_collapse_confidence = 0.0;
    std::string authoritative_program_source = std::string(phase_programs::kMiningResonanceProgramSource);
};

struct SubstrateStratumAuthorityState {
    SubstrateStratumConnectionIngress connection_ingress;
    bool has_connection_ingress = false;
    bool connect_dispatched = false;
    bool transport_connected = false;
    bool subscribe_dispatched = false;
    bool subscribed = false;
    bool authorize_dispatched = false;
    bool authorized = false;
    bool network_authority_granted = false;
    bool has_difficulty = false;
    double difficulty = 0.0;
    double target_network_share_fraction = 0.05;
    bool has_active_job = false;
    std::string active_job_id;
    std::string active_job_header_hex;
    std::string active_job_prevhash;
    std::string active_job_coinbase1;
    std::string active_job_coinbase2;
    std::vector<std::string> active_job_merkle_branches;
    std::string active_job_version;
    std::string active_job_nbits;
    std::string active_job_ntime;
    std::string active_share_target_hex;
    std::string active_block_target_hex;
    double active_block_difficulty = 0.0;
    double expected_hashes_for_share = 0.0;
    double network_hashrate_hs = 0.0;
    double required_hashrate_hs = 0.0;
    double required_share_submissions_per_s = 0.0;
    double required_share_submissions_per_s_per_worker = 0.0;
    double required_share_submissions_per_pool_window = 0.0;
    double required_share_submissions_per_worker_pool_window = 0.0;
    double target_hashrate_hs = 0.0;
    std::size_t allowed_worker_count = kStratumWorkerSlotCount;
    double validation_jitter_window_seconds = 60.0;
    SubstrateStratumPoolPolicy pool_policy;
    std::size_t min_validation_jitter_samples = kStratumWorkerSlotCount;
    std::size_t validation_sample_count = 0;
    std::size_t locally_validated_share_count = 0;
    std::size_t workers_with_validation_samples = 0;
    std::size_t workers_meeting_validation_sample_threshold = 0;
    std::size_t workers_meeting_target_rate_count = 0;
    std::size_t min_worker_validation_sample_count = 0;
    double measured_validation_share_rate_per_s = 0.0;
    double mean_worker_validation_share_rate_per_s = 0.0;
    double measured_validation_hashrate_hs_60s = 0.0;
    double mean_worker_validation_hashrate_hs_60s = 0.0;
    double measured_validation_jitter_s = 0.0;
    double measured_validation_jitter_fraction = 0.0;
    double max_worker_validation_jitter_s = 0.0;
    double max_worker_validation_jitter_fraction = 0.0;
    double submission_interval_target_s = 0.0;
    double effective_request_budget_per_s = 0.0;
    double last_validation_timestamp_unix_s = 0.0;
    double last_submit_timestamp_unix_s = 0.0;
    std::string validation_log_csv_path;
    bool active_job_clean = false;
    std::array<SubstrateStratumWorkerAssignment, kStratumWorkerSlotCount> worker_assignments {};
    std::array<std::size_t, kStratumWorkerSlotCount> worker_validation_sample_counts {};
    std::array<double, kStratumWorkerSlotCount> worker_measured_validation_share_rate_per_s {};
    std::array<double, kStratumWorkerSlotCount> worker_measured_validation_hashrate_hs_60s {};
    std::array<double, kStratumWorkerSlotCount> worker_measured_validation_jitter_s {};
    std::array<double, kStratumWorkerSlotCount> worker_measured_validation_jitter_fraction {};
    std::array<double, kStratumWorkerSlotCount> worker_submission_interval_target_s {};
    std::size_t active_worker_count = 0;
    std::size_t submit_preview_count = 0;
    std::size_t offline_valid_submit_preview_count = 0;
    std::size_t preview_validation_count = 0;
    std::size_t phase_clamped_actuation_count = 0;
    std::size_t phase_clamped_resonant_candidate_count = 0;
    std::size_t phase_clamped_candidate_count = 0;
    std::size_t phase_clamped_permitted_count = 0;
    std::size_t phase_clamped_harmonic_verified_count = 0;
    std::string last_preview_share_key;
    std::string last_phase_clamped_gate_reason;
    std::string last_phase_program_title;
    std::string last_phase_program_generated_dir;
    bool last_phase_clamped_resonant_candidate_available = false;
    bool last_phase_clamped_valid_share_candidate = false;
    bool last_phase_clamped_share_target_pass = false;
    bool last_phase_clamped_block_target_pass = false;
    bool last_phase_clamped_actuation_permitted = false;
    bool last_phase_clamped_all_parallel_harmonics_verified = false;
    bool last_phase_program_substrate_native = false;
    bool last_phase_program_same_pulse_validation = false;
    bool last_phase_program_pool_format_ready = false;
    std::size_t last_phase_program_block_count = 0U;
    std::size_t last_phase_clamped_attempted_nonce_count = 0;
    std::size_t last_phase_clamped_valid_nonce_count = 0;
    double last_phase_clamped_selected_coherence_score = 0.0;
    double last_phase_clamped_phase_lock_error = 0.0;
    double last_phase_clamped_phase_clamp_strength = 0.0;
    double last_phase_clamped_target_resonance_norm = 0.0;
    double last_phase_clamped_phase_alignment = 0.0;
    double last_phase_clamped_validation_structure_norm = 0.0;
    double last_phase_clamped_field_vector_phase_turns = 0.0;
    double last_phase_clamped_phase_transport_turns = 0.0;
    double last_phase_clamped_phase_lock_delta_turns = 0.0;
    double last_phase_clamped_transfer_drive_norm = 0.0;
    double last_phase_clamped_stability_gate_norm = 0.0;
    double last_phase_clamped_damping_norm = 0.0;
    double last_phase_clamped_transport_drive_norm = 0.0;
    double last_phase_clamped_resonance_activation_norm = 0.0;
    double last_phase_clamped_temporal_admissibility = 0.0;
    double last_phase_clamped_zero_point_proximity = 0.0;
    double last_phase_clamped_transport_readiness = 0.0;
    double last_phase_clamped_share_confidence = 0.0;
    double last_phase_clamped_block_coherence_norm = 0.0;
    double last_phase_clamped_submit_priority_score = 0.0;
    std::size_t last_phase_clamped_parallel_harmonic_count = 0;
    std::size_t last_phase_clamped_verified_parallel_harmonic_count = 0;
    std::size_t last_phase_clamped_validated_parallel_harmonic_count = 0;
    std::size_t last_phase_clamped_resonance_reinforcement_count = 0;
    std::size_t last_phase_clamped_noise_lane_count = 0;
    std::uint32_t last_phase_clamped_queue_quality_class = 0;
    bool has_last_phase_trace = false;
    double last_phase_trace_nonce_seed_phase_turns = 0.0;
    double last_phase_trace_header_phase_turns = 0.0;
    double last_phase_trace_share_target_phase_turns = 0.0;
    double last_phase_trace_block_target_phase_turns = 0.0;
    double last_phase_trace_sha256_schedule_phase_turns = 0.0;
    double last_phase_trace_sha256_round_phase_turns = 0.0;
    double last_phase_trace_sha256_digest_phase_turns = 0.0;
    std::string last_phase_temporal_sequence;
    bool last_measured_nonce_observed = false;
    double last_measured_hash_phase_turns = 0.0;
    double last_measured_nonce_phase_turns = 0.0;
    double last_collapse_feedback_phase_turns = 0.0;
    double last_collapse_relock_error_turns = 0.0;
    double last_observer_collapse_strength = 0.0;
    double last_phase_flux_conservation = 0.0;
    double last_nonce_collapse_confidence = 0.0;
    double last_target_resonance_norm = 0.0;
    double target_resonance_floor = 0.72;
    bool phase_guided_preview_test_mode = false;
    bool auto_promote_to_live_mode = false;
    std::string operating_mode;
    bool submit_policy_enabled = false;
    bool submit_path_ready = false;
    bool submit_dispatched = false;
    std::size_t submit_dispatch_count = 0;
    std::size_t accepted_submit_count = 0;
    std::size_t refused_submit_count = 0;
    std::size_t queued_submit_candidate_count = 0;
    std::size_t queued_priority_submit_candidate_count = 0;
    std::size_t queued_block_submit_candidate_count = 0;
    std::size_t normal_shares_since_priority_submit = 0;
    std::size_t next_priority_spacing_target = 4;
    std::size_t max_invalid_pool_submissions = 3U;
    std::string submit_gate_reason = "phase_local_validation_required";
    std::string last_submitted_share_key;
    std::string last_submit_request_id;
    std::string last_submit_job_id;
    std::size_t last_submit_worker_index = 0;
    std::string last_submit_worker_name;
    std::string last_submit_nonce;
    std::string last_submit_hash_hex;
    double reserve_jitter_fraction = 0.18;
    double max_requests_per_second = 0.0;
    std::size_t dispatch_count = 0;
    std::string connection_state = "idle";
    std::string last_request_id;
    std::string last_command;
    std::string subscription_id;
    std::string extranonce1;
    std::uint32_t extranonce2_size = 0;
    std::string last_response_message;
    std::string last_event_topic;
};

[[nodiscard]] std::string stratum_command_label(StratumCommandKind kind);
[[nodiscard]] std::string stratum_command_method(StratumCommandKind kind);
[[nodiscard]] std::string stratum_dispatch_topic(StratumCommandKind kind);
[[nodiscard]] std::string stratum_server_event_label(StratumServerEventKind kind);
[[nodiscard]] std::string stratum_server_event_method(StratumServerEventKind kind);
[[nodiscard]] std::string stratum_server_event_topic(StratumServerEventKind kind);
[[nodiscard]] double clamp_stratum_request_rate(double requested_rate);
[[nodiscard]] SubstrateStratumPoolPolicy resolve_stratum_pool_policy(const std::string& host);
[[nodiscard]] SubstrateStratumConnectionIngress sanitize_stratum_connection_ingress(
    const SubstrateStratumConnectionIngress& ingress
);
[[nodiscard]] bool is_valid_stratum_connection_ingress(const SubstrateStratumConnectionIngress& ingress);
[[nodiscard]] SubstrateStratumConnectionControl sanitize_stratum_connection_control(
    const SubstrateStratumConnectionControl& control
);
[[nodiscard]] bool is_valid_stratum_connection_control(const SubstrateStratumConnectionControl& control);
[[nodiscard]] std::string build_stratum_request_id(const std::string& connection_id, StratumCommandKind kind);
[[nodiscard]] SubstrateStratumDispatchPayload build_stratum_dispatch_payload(
    const SubstrateStratumConnectionIngress& ingress,
    StratumCommandKind kind
);
[[nodiscard]] std::array<SubstrateStratumWorkerAssignment, kStratumWorkerSlotCount> build_stratum_worker_assignments(
    const SubstrateStratumConnectionIngress& ingress,
    const std::string& job_id
);
[[nodiscard]] SubstrateStratumSubmitPreviewPayload build_stratum_submit_preview_payload(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
);
[[nodiscard]] std::string build_stratum_worker_extranonce2_hex(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
);
[[nodiscard]] std::string resolve_stratum_job_header_hex(
    const SubstrateStratumAuthorityState& authority_state,
    const SubstrateStratumWorkerAssignment& assignment
);

}  // namespace qbit_miner
