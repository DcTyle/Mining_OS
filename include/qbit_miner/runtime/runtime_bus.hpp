#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "qbit_miner/runtime/phase_clamped_mining_os.hpp"
#include "qbit_miner/runtime/substrate_control_surface.hpp"
#include "qbit_miner/runtime/phase_transport_scheduler.hpp"
#include "qbit_miner/runtime/substrate_stratum_protocol.hpp"
#include "qbit_miner/runtime/substrate_viewport_encoder.hpp"
#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct RuntimeEvent {
    std::string topic;
    std::string message;
    SubstrateTrace trace;
    PhaseDispatchArtifact phase_dispatch_artifact;
    bool has_phase_dispatch_artifact = false;
    SubstrateViewportFrame viewport_frame;
    bool has_viewport_frame = false;
    SubstrateControlIngress control_ingress;
    bool has_control_ingress = false;
    SubstrateStratumConnectionIngress stratum_connection_ingress;
    bool has_stratum_connection_ingress = false;
    SubstrateStratumConnectionControl stratum_connection_control;
    bool has_stratum_connection_control = false;
    SubstrateStratumDispatchPayload stratum_dispatch_payload;
    bool has_stratum_dispatch_payload = false;
    SubstrateStratumResponsePayload stratum_response_payload;
    bool has_stratum_response_payload = false;
    SubstrateStratumServerEventPayload stratum_server_event_payload;
    bool has_stratum_server_event_payload = false;
    SubstrateStratumSubmitPreviewPayload stratum_submit_preview_payload;
    bool has_stratum_submit_preview_payload = false;
    SubstrateStratumAuthorityState stratum_authority;
    bool has_stratum_authority = false;
    PhaseClampedShareActuation phase_clamped_share_actuation;
    bool has_phase_clamped_share_actuation = false;
};

class RuntimeBus {
public:
    using Subscriber = std::function<void(const RuntimeEvent&)>;

    void subscribe(const std::string& topic, Subscriber subscriber);
    void publish(const RuntimeEvent& event) const;

private:
    std::map<std::string, std::vector<Subscriber>> subscribers_;
};

}  // namespace qbit_miner