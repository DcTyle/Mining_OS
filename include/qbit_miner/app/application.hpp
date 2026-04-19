#pragma once

#include <string>

#include "qbit_miner/cache/substrate_cache.hpp"
#include "qbit_miner/runtime/substrate_firmware_runtime.hpp"
#include "qbit_miner/runtime/substrate_stratum_tcp_adapter.hpp"
#include "qbit_miner/runtime/runtime_bus.hpp"
#include "qbit_miner/runtime/substrate_viewport_encoder.hpp"

namespace qbit_miner {

class QuantumMinerApplication {
public:
    explicit QuantumMinerApplication(FieldDynamicsConfig config = {});

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] SubstrateTrace process_feedback(const GpuFeedbackFrame& frame);
    [[nodiscard]] SubstrateViewportFrame encode_viewport_frame(const SubstrateTrace& trace) const;
    [[nodiscard]] const SubstrateFirmwareRuntime& firmware_runtime() const noexcept;
    [[nodiscard]] const SubstrateStratumTcpAdapter& stratum_tcp_adapter() const noexcept;
    [[nodiscard]] const SubstrateCache& cache() const noexcept;
    RuntimeBus& bus() noexcept;

private:
    std::string name_;
    FieldDynamicsEngine dynamics_;
    SubstrateViewportEncoder viewport_encoder_;
    SubstrateCache cache_;
    RuntimeBus bus_;
    SubstrateFirmwareRuntime firmware_runtime_;
    SubstrateStratumTcpAdapter stratum_tcp_adapter_;
};

}  // namespace qbit_miner