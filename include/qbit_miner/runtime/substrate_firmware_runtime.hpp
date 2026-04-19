#pragma once

#include <array>
#include <deque>
#include <mutex>
#include <string>

#include "qbit_miner/runtime/runtime_bus.hpp"

namespace qbit_miner {

struct StratumValidationSample {
    double timestamp_unix_s = 0.0;
    double share_difficulty = 0.0;
    double expected_hashes = 0.0;
};

struct SubstrateFirmwareState {
    SubstrateControlIngress control_ingress;
    bool has_control_ingress = false;
    SubstrateStratumAuthorityState stratum_authority;
    bool has_stratum_authority = false;
    std::string last_event_topic;
};

struct QueuedSubmitCandidate {
    PhaseClampedShareActuation actuation;
    SubstrateStratumPowPhaseTrace phase_trace;
    std::string share_key;
    double validation_timestamp_unix_s = 0.0;
};

class SubstrateFirmwareRuntime {
public:
    explicit SubstrateFirmwareRuntime(RuntimeBus& bus);

    [[nodiscard]] SubstrateFirmwareState snapshot() const;

private:
    void handle_control_ingress(const RuntimeEvent& event);
    void handle_stratum_connection_ingress(const RuntimeEvent& event);
    void handle_stratum_connection_control(const RuntimeEvent& event);
    void handle_stratum_response(const RuntimeEvent& event);
    void handle_stratum_server_event(const RuntimeEvent& event);
    void handle_phase_clamped_share_actuation(const RuntimeEvent& event);
    void publish_stratum_dispatch(StratumCommandKind kind, const SubstrateStratumAuthorityState& authority_state);
    void publish_submit_preview(const SubstrateStratumAuthorityState& authority_state, const SubstrateStratumSubmitPreviewPayload& preview);

    RuntimeBus& bus_;
    mutable std::mutex mutex_;
    SubstrateFirmwareState state_;
    std::array<std::deque<StratumValidationSample>, kStratumWorkerSlotCount> worker_validation_histories_;
    std::deque<QueuedSubmitCandidate> pending_submit_candidates_;
};

}  // namespace qbit_miner
