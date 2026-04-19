#include "qbit_miner/app/application.hpp"

namespace qbit_miner {

QuantumMinerApplication::QuantumMinerApplication(FieldDynamicsConfig config)
    : name_("Quantum Miner"),
      dynamics_(config),
    cache_(512),
        firmware_runtime_(bus_),
        stratum_tcp_adapter_(bus_) {}

const std::string& QuantumMinerApplication::name() const noexcept {
    return name_;
}

SubstrateTrace QuantumMinerApplication::process_feedback(const GpuFeedbackFrame& frame) {
    SubstrateTrace trace = dynamics_.trace_feedback(frame);
    const SubstrateViewportFrame viewport_frame = encode_viewport_frame(trace);
    cache_.store(trace);
    bus_.publish(RuntimeEvent{
        "substrate.trace.ready",
        "Substrate trace cached and ready for GUI/network hooks",
        trace,
    });
    bus_.publish(RuntimeEvent{
        "substrate.viewport.frame.ready",
        "Substrate viewport frame encoded from field dynamics for future viewport consumers",
        trace,
        {},
        false,
        viewport_frame,
        true,
    });
    return trace;
}

SubstrateViewportFrame QuantumMinerApplication::encode_viewport_frame(const SubstrateTrace& trace) const {
    return viewport_encoder_.encode_frame(trace);
}

const SubstrateFirmwareRuntime& QuantumMinerApplication::firmware_runtime() const noexcept {
    return firmware_runtime_;
}

const SubstrateStratumTcpAdapter& QuantumMinerApplication::stratum_tcp_adapter() const noexcept {
    return stratum_tcp_adapter_;
}

const SubstrateCache& QuantumMinerApplication::cache() const noexcept {
    return cache_;
}

RuntimeBus& QuantumMinerApplication::bus() noexcept {
    return bus_;
}

}  // namespace qbit_miner