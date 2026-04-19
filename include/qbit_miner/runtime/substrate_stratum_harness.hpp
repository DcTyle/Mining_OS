#pragma once

#include <cstddef>
#include <mutex>
#include <string>

#include "qbit_miner/runtime/runtime_bus.hpp"

namespace qbit_miner {

struct SubstrateStratumDryRunHarnessState {
    std::size_t dispatch_count = 0;
    std::size_t response_count = 0;
    std::size_t notification_count = 0;
    bool saw_connect = false;
    bool saw_subscribe = false;
    bool saw_authorize = false;
    bool saw_submit = false;
    bool saw_set_difficulty = false;
    bool saw_notify = false;
    std::string last_request_id;
    std::string last_payload_json;
};

class SubstrateStratumDryRunHarness {
public:
    explicit SubstrateStratumDryRunHarness(RuntimeBus& bus);

    [[nodiscard]] SubstrateStratumDryRunHarnessState snapshot() const;

private:
    void handle_dispatch(const RuntimeEvent& event);

    RuntimeBus& bus_;
    mutable std::mutex mutex_;
    SubstrateStratumDryRunHarnessState state_;
};

}  // namespace qbit_miner