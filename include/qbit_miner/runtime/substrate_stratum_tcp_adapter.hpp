#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "qbit_miner/runtime/runtime_bus.hpp"

namespace qbit_miner {

struct SubstrateStratumTcpAdapterState {
    bool winsock_ready = false;
    std::size_t active_session_count = 0;
    std::size_t live_dispatch_count = 0;
    std::size_t response_count = 0;
    std::size_t server_event_count = 0;
    std::size_t submit_response_count = 0;
    std::string last_connection_id;
    std::string last_request_line;
    std::string last_submit_request_line;
    std::string last_disconnect_reason;
};

class SubstrateStratumTcpAdapter {
public:
    explicit SubstrateStratumTcpAdapter(RuntimeBus& bus);
    ~SubstrateStratumTcpAdapter();

    SubstrateStratumTcpAdapter(const SubstrateStratumTcpAdapter&) = delete;
    SubstrateStratumTcpAdapter& operator=(const SubstrateStratumTcpAdapter&) = delete;

    [[nodiscard]] SubstrateStratumTcpAdapterState snapshot() const;

private:
    struct LiveSession;

    void handle_dispatch(const RuntimeEvent& event);
    void handle_connection_control(const RuntimeEvent& event);

    RuntimeBus& bus_;
    mutable std::mutex mutex_;
    SubstrateStratumTcpAdapterState state_;
    std::map<std::string, std::unique_ptr<LiveSession>> sessions_;
};

}  // namespace qbit_miner