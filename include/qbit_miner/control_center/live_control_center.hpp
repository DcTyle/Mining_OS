#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "qbit_miner/control_center/substrate_compute_runtime.hpp"
#include "qbit_miner/control_center/field_viewport.hpp"
#include "qbit_miner/runtime/gpu_kernel_telemetry_bridge.hpp"
#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/runtime/substrate_stratum_tcp_adapter.hpp"
#include "qbit_miner/substrate/photonic_identity.hpp"
#include "qbit_miner/telemetry/mining_metrics.hpp"

namespace qbit_miner {

enum class MiningPoolPolicy {
    TwoMiners,
    F2Pool,
};

enum class MetricValueFormat {
    Number,
    Hashrate,
    Rate,
    Percent,
    Difficulty,
    Currency,
    Coin,
    Temperature,
    Power,
    FrequencyGHz,
    Count,
};

struct MetricHistorySeries {
    std::string metric_id;
    std::string label;
    MetricValueFormat format = MetricValueFormat::Number;
    double current_value = 0.0;
    std::vector<double> samples;
};

struct MetricHistoryPanel {
    std::string panel_id;
    std::string title;
    std::vector<MetricHistorySeries> series;
};

struct MiningConnectionSettings {
    MiningPoolPolicy pool_policy = MiningPoolPolicy::TwoMiners;
    std::string pool_host;
    std::uint16_t pool_port = 3333;
    std::string payout_address;
    std::string worker_id = "rig-control-center-01";
    std::string worker_password = "x";
    bool allow_live_submit = true;
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
    bool run_indefinitely = true;
    double run_duration_minutes = 60.0;
};

struct MiningConnectionStatus {
    std::string pool_policy_label = "2Miners";
    std::string derived_worker_name;
    std::string connection_state = "not_configured";
    std::string status_message = "Pool host, payout wallet/account, and worker id are required.";
    bool configured = false;
    bool rewards_path_gated = true;
};

struct ControlCenterSnapshot {
    MiningMetricsSnapshot metrics;
    FieldViewportFrame viewport;
    std::vector<std::string> recent_events;
    std::vector<MetricHistoryPanel> metric_panels;
    bool live = false;
    bool mining_runtime_running = false;
    bool mining_session_run_indefinitely = true;
    double mining_session_duration_minutes = 0.0;
    double mining_session_elapsed_seconds = 0.0;
    double mining_session_remaining_seconds = 0.0;
    bool pool_connection_requested = false;
    bool audio_enabled = true;
    bool paused = false;
    bool compact_layout = false;
    bool always_on_top = false;
    std::string control_program_source;
    std::string viewport_program_source;
    MiningConnectionSettings mining_settings;
    MiningConnectionStatus mining_status;
    SubstrateStratumAuthorityState stratum_authority;
    bool has_stratum_authority = false;
    SubstrateStratumTcpAdapterState tcp_adapter_state;
    std::optional<MiningValidationSnapshot> last_gpu_mining_validation;
};

class LiveControlCenter {
public:
    LiveControlCenter();
    ~LiveControlCenter();

    LiveControlCenter(const LiveControlCenter&) = delete;
    LiveControlCenter& operator=(const LiveControlCenter&) = delete;

    void start();
    void stop();

    void set_reward_unit(RewardIntervalUnit unit);
    [[nodiscard]] RewardIntervalUnit reward_unit() const;

    void set_audio_enabled(bool enabled);
    [[nodiscard]] bool audio_enabled() const;

    void set_paused(bool paused);
    [[nodiscard]] bool paused() const;

    void set_compact_layout(bool compact_layout);
    [[nodiscard]] bool compact_layout() const;

    void set_always_on_top(bool always_on_top);
    [[nodiscard]] bool always_on_top() const;

    void set_mining_settings(const MiningConnectionSettings& settings);
    [[nodiscard]] MiningConnectionSettings mining_settings() const;
    void start_mining_session();
    void stop_mining_session();
    void connect_pool();
    void disconnect_pool();
    [[nodiscard]] bool pool_connection_requested() const;
    [[nodiscard]] bool running() const;
    void ingest_gpu_kernel_iteration(const GpuKernelIterationEvent& event);
    void ingest_gpu_mining_validation(const MiningValidationSnapshot& snapshot);
    void inject_feedback_frame(const GpuFeedbackFrame& frame);

    [[nodiscard]] ControlCenterSnapshot snapshot() const;
    [[nodiscard]] bool export_calibration_bundle(const std::filesystem::path& output_dir) const;
    [[nodiscard]] bool export_device_validation_bundle(const std::filesystem::path& output_dir) const;
    [[nodiscard]] std::wstring last_error_message() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qbit_miner
