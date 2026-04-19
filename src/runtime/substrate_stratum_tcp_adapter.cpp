#include "qbit_miner/runtime/substrate_stratum_tcp_adapter.hpp"

#include <cstdint>
#include <chrono>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace qbit_miner {

struct SubstrateStratumTcpAdapter::LiveSession {
#if defined(_WIN32)
    SOCKET socket = INVALID_SOCKET;
#endif
    std::string connection_id;
    std::string host;
    std::uint16_t port = 0;
};

namespace {

#if defined(_WIN32)

constexpr int kStratumCommandResponseTimeoutMs = 5000;
constexpr int kStratumAuthorizeEventPollTimeoutMs = 250;
constexpr auto kStratumAuthorizeEventDrainWindow = std::chrono::seconds(3);

void close_socket_if_open(SOCKET& socket_handle) {
    if (socket_handle != INVALID_SOCKET) {
        closesocket(socket_handle);
        socket_handle = INVALID_SOCKET;
    }
}

[[nodiscard]] bool ensure_winsock() {
    static bool initialized = false;
    static bool success = false;
    if (initialized) {
        return success;
    }

    WSADATA data {};
    success = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    initialized = true;
    return success;
}

[[nodiscard]] std::optional<std::string> regex_group(const std::string& text, const std::regex& pattern, std::size_t group = 1U) {
    std::smatch match;
    if (std::regex_search(text, match, pattern) && match.size() > group) {
        return match[group].str();
    }
    return std::nullopt;
}

[[nodiscard]] bool send_line(SOCKET socket_handle, const std::string& line) {
    const std::string outbound = line + "\n";
    const char* buffer = outbound.data();
    int remaining = static_cast<int>(outbound.size());
    while (remaining > 0) {
        const int sent = send(socket_handle, buffer, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        buffer += sent;
        remaining -= sent;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> receive_line(SOCKET socket_handle, int timeout_ms) {
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string line;
    char ch = 0;
    while (true) {
        const int received = recv(socket_handle, &ch, 1, 0);
        if (received == SOCKET_ERROR || received == 0) {
            if (line.empty()) {
                return std::nullopt;
            }
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    return line;
}

[[nodiscard]] SOCKET connect_socket(const std::string& host, std::uint16_t port) {
    addrinfoW hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::wstring host_w(host.begin(), host.end());
    std::wostringstream port_stream;
    port_stream << port;

    addrinfoW* result = nullptr;
    if (GetAddrInfoW(host_w.c_str(), port_stream.str().c_str(), &hints, &result) != 0) {
        return INVALID_SOCKET;
    }

    SOCKET socket_handle = INVALID_SOCKET;
    for (addrinfoW* current = result; current != nullptr; current = current->ai_next) {
        socket_handle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket_handle == INVALID_SOCKET) {
            continue;
        }
        if (connect(socket_handle, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
            break;
        }
        closesocket(socket_handle);
        socket_handle = INVALID_SOCKET;
    }

    FreeAddrInfoW(result);
    return socket_handle;
}

[[nodiscard]] SubstrateStratumResponsePayload build_connect_response(
    const SubstrateStratumDispatchPayload& payload,
    bool accepted,
    const std::string& message
) {
    SubstrateStratumResponsePayload response;
    response.command_kind = payload.command_kind;
    response.connection_id = payload.connection_id;
    response.request_id = payload.request_id;
    response.method = payload.method;
    response.accepted = accepted;
    response.message = message;
    response.payload_json = "{\"connected\": " + std::string(accepted ? "true" : "false") + "}";
    return response;
}

[[nodiscard]] SubstrateStratumResponsePayload parse_command_response(
    const SubstrateStratumDispatchPayload& payload,
    const std::string& line
) {
    SubstrateStratumResponsePayload response;
    response.command_kind = payload.command_kind;
    response.connection_id = payload.connection_id;
    response.request_id = payload.request_id;
    response.method = payload.method;
    response.payload_json = line;

    const bool has_null_error = line.find("\"error\": null") != std::string::npos || line.find("\"error\":null") != std::string::npos;
    switch (payload.command_kind) {
    case StratumCommandKind::Subscribe:
        response.accepted = has_null_error;
        if (const auto subscription_id = regex_group(line, std::regex(R"rx(\[\s*\[\s*"[^"]+"\s*,\s*"([^"]+)")rx"))) {
            response.subscription_id = *subscription_id;
        }
        if (const auto extranonce1 = regex_group(line, std::regex(R"rx(\]\s*,\s*"([0-9a-fA-F]+)")rx"))) {
            response.extranonce1 = *extranonce1;
        }
        if (const auto extranonce2_size = regex_group(line, std::regex(R"rx(\]\s*,\s*"[0-9a-fA-F]+"\s*,\s*([0-9]+))rx"))) {
            response.extranonce2_size = static_cast<std::uint32_t>(std::stoul(*extranonce2_size));
        }
        response.message = response.accepted ? "TCP subscribe accepted" : "TCP subscribe rejected";
        break;
    case StratumCommandKind::Authorize:
        response.accepted = has_null_error && (line.find("\"result\": true") != std::string::npos || line.find("\"result\":true") != std::string::npos);
        response.message = response.accepted ? "TCP authorize accepted" : "TCP authorize rejected";
        break;
    case StratumCommandKind::Submit:
        response.accepted = has_null_error && (line.find("\"result\": true") != std::string::npos || line.find("\"result\":true") != std::string::npos);
        response.message = response.accepted ? "TCP submit accepted" : "TCP submit rejected";
        break;
    case StratumCommandKind::Connect:
        response.accepted = true;
        response.message = "TCP connect accepted";
        break;
    }
    return response;
}

[[nodiscard]] std::optional<SubstrateStratumServerEventPayload> parse_server_event(
    const std::string& connection_id,
    const std::string& line
) {
    const auto method = regex_group(line, std::regex(R"rx("method"\s*:\s*"([^"]+)")rx"));
    if (!method.has_value()) {
        return std::nullopt;
    }

    SubstrateStratumServerEventPayload event;
    event.connection_id = connection_id;
    event.method = *method;
    event.payload_json = line;

    if (*method == "mining.set_difficulty") {
        event.event_kind = StratumServerEventKind::SetDifficulty;
        if (const auto difficulty = regex_group(line, std::regex(R"rx(\[\s*([0-9]+(?:\.[0-9]+)?)\s*\])rx"))) {
            event.difficulty = std::stod(*difficulty);
        }
        return event;
    }

    if (*method == "mining.notify") {
        event.event_kind = StratumServerEventKind::Notify;
        if (const auto header_hex = regex_group(line, std::regex(R"rx("header_hex"\s*:\s*"([0-9a-fA-F]+)")rx"))) {
            event.header_hex = *header_hex;
        }
        const std::regex params_pattern(R"rx(\[\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\[(.*?)\]\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(true|false))rx");
        std::smatch match;
        if (std::regex_search(line, match, params_pattern) && match.size() >= 10U) {
            event.job_id = match[1].str();
            event.prevhash = match[2].str();
            event.coinbase1 = match[3].str();
            event.coinbase2 = match[4].str();
            const std::string merkle_branch_section = match[5].str();
            const std::regex branch_pattern(R"rx("([0-9a-fA-F]+)")rx");
            for (std::sregex_iterator it(merkle_branch_section.begin(), merkle_branch_section.end(), branch_pattern), end;
                 it != end;
                 ++it) {
                event.merkle_branches.push_back((*it)[1].str());
            }
            event.version = match[6].str();
            event.nbits = match[7].str();
            event.ntime = match[8].str();
            event.clean_jobs = match[9].str() == "true";
        }
        return event;
    }

    return std::nullopt;
}

#endif

}  // namespace

SubstrateStratumTcpAdapter::SubstrateStratumTcpAdapter(RuntimeBus& bus)
    : bus_(bus) {
    bus_.subscribe("substrate.stratum.connection.control", [this](const RuntimeEvent& event) {
        handle_connection_control(event);
    });
    bus_.subscribe(stratum_dispatch_topic(StratumCommandKind::Connect), [this](const RuntimeEvent& event) {
        handle_dispatch(event);
    });
    bus_.subscribe(stratum_dispatch_topic(StratumCommandKind::Subscribe), [this](const RuntimeEvent& event) {
        handle_dispatch(event);
    });
    bus_.subscribe(stratum_dispatch_topic(StratumCommandKind::Authorize), [this](const RuntimeEvent& event) {
        handle_dispatch(event);
    });
    bus_.subscribe(stratum_dispatch_topic(StratumCommandKind::Submit), [this](const RuntimeEvent& event) {
        handle_dispatch(event);
    });
}

SubstrateStratumTcpAdapter::~SubstrateStratumTcpAdapter() {
#if defined(_WIN32)
    for (auto& [connection_id, session] : sessions_) {
        if (session && session->socket != INVALID_SOCKET) {
            close_socket_if_open(session->socket);
        }
    }
#endif
}

SubstrateStratumTcpAdapterState SubstrateStratumTcpAdapter::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void SubstrateStratumTcpAdapter::handle_dispatch(const RuntimeEvent& event) {
    if (!event.has_stratum_dispatch_payload || event.stratum_dispatch_payload.dry_run_only) {
        return;
    }

#if !defined(_WIN32)
    RuntimeEvent response_event;
    response_event.topic = "substrate.stratum.response";
    response_event.message = "TCP Stratum adapter is only implemented on Windows";
    response_event.stratum_response_payload = build_connect_response(
        event.stratum_dispatch_payload,
        false,
        "TCP Stratum adapter is only implemented on Windows"
    );
    response_event.has_stratum_response_payload = true;
    bus_.publish(response_event);
    return;
#else
    const SubstrateStratumDispatchPayload payload = event.stratum_dispatch_payload;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.last_connection_id = payload.connection_id;
        state_.last_request_line = payload.payload_json;
        if (payload.command_kind == StratumCommandKind::Submit) {
            state_.last_submit_request_line = payload.payload_json;
        }
        ++state_.live_dispatch_count;
        state_.winsock_ready = ensure_winsock();
    }

    if (!ensure_winsock()) {
        RuntimeEvent response_event;
        response_event.topic = "substrate.stratum.response";
        response_event.message = "WSAStartup failed for Stratum TCP adapter";
        response_event.stratum_response_payload = build_connect_response(payload, false, "WSAStartup failed for Stratum TCP adapter");
        response_event.has_stratum_response_payload = true;
        bus_.publish(response_event);
        return;
    }

    LiveSession* session_ptr = nullptr;
    if (payload.command_kind == StratumCommandKind::Connect) {
        auto session = std::make_unique<LiveSession>();
        session->connection_id = payload.connection_id;
        session->host = payload.host;
        session->port = payload.port;
        session->socket = connect_socket(payload.host, payload.port);

        const bool connected = session->socket != INVALID_SOCKET;
        RuntimeEvent response_event;
        response_event.topic = "substrate.stratum.response";
        response_event.message = connected ? "TCP Stratum transport connected" : "TCP Stratum transport connection failed";
        response_event.stratum_response_payload = build_connect_response(
            payload,
            connected,
            connected ? "TCP Stratum transport connected" : "TCP Stratum transport connection failed"
        );
        response_event.has_stratum_response_payload = true;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto existing = sessions_.find(payload.connection_id);
            if (existing != sessions_.end() && existing->second) {
                close_socket_if_open(existing->second->socket);
            }
            if (connected) {
                sessions_[payload.connection_id] = std::move(session);
            } else if (existing != sessions_.end()) {
                sessions_.erase(payload.connection_id);
            }
            state_.active_session_count = sessions_.size();
            ++state_.response_count;
        }

        bus_.publish(response_event);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(payload.connection_id);
        if (found != sessions_.end()) {
            session_ptr = found->second.get();
        }
    }

    if (session_ptr == nullptr || session_ptr->socket == INVALID_SOCKET) {
        RuntimeEvent response_event;
        response_event.topic = "substrate.stratum.response";
        response_event.message = "TCP Stratum session missing for dispatch";
        response_event.stratum_response_payload = build_connect_response(payload, false, "TCP Stratum session missing for dispatch");
        response_event.has_stratum_response_payload = true;
        bus_.publish(response_event);
        return;
    }

    if (!send_line(session_ptr->socket, payload.payload_json)) {
        RuntimeEvent response_event;
        response_event.topic = "substrate.stratum.response";
        response_event.message = "TCP Stratum send failed";
        response_event.stratum_response_payload = build_connect_response(payload, false, "TCP Stratum send failed");
        response_event.has_stratum_response_payload = true;
        bus_.publish(response_event);
        return;
    }

    const std::optional<std::string> response_line = receive_line(session_ptr->socket, kStratumCommandResponseTimeoutMs);
    if (!response_line.has_value()) {
        RuntimeEvent response_event;
        response_event.topic = "substrate.stratum.response";
        response_event.message = "TCP Stratum response timeout";
        response_event.stratum_response_payload = build_connect_response(payload, false, "TCP Stratum response timeout");
        response_event.has_stratum_response_payload = true;
        bus_.publish(response_event);
        return;
    }

    RuntimeEvent response_event;
    response_event.topic = "substrate.stratum.response";
    response_event.message = "TCP Stratum command response received";
    response_event.stratum_response_payload = parse_command_response(payload, *response_line);
    response_event.has_stratum_response_payload = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.response_count;
        if (payload.command_kind == StratumCommandKind::Submit) {
            ++state_.submit_response_count;
        }
    }
    bus_.publish(response_event);

    if (payload.command_kind == StratumCommandKind::Authorize) {
        const auto drain_deadline = std::chrono::steady_clock::now() + kStratumAuthorizeEventDrainWindow;
        while (std::chrono::steady_clock::now() < drain_deadline) {
            const std::optional<std::string> line = receive_line(session_ptr->socket, kStratumAuthorizeEventPollTimeoutMs);
            if (!line.has_value()) {
                continue;
            }
            const auto server_event_payload = parse_server_event(payload.connection_id, *line);
            if (!server_event_payload.has_value()) {
                continue;
            }
            RuntimeEvent server_event;
            server_event.topic = stratum_server_event_topic(server_event_payload->event_kind);
            server_event.message = "TCP Stratum server event received";
            server_event.stratum_server_event_payload = *server_event_payload;
            server_event.has_stratum_server_event_payload = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++state_.server_event_count;
            }
            bus_.publish(server_event);
        }
    }
#endif
}

void SubstrateStratumTcpAdapter::handle_connection_control(const RuntimeEvent& event) {
    if (!event.has_stratum_connection_control || !event.stratum_connection_control.disconnect_requested) {
        return;
    }

    const SubstrateStratumConnectionControl control = sanitize_stratum_connection_control(event.stratum_connection_control);
    if (!is_valid_stratum_connection_control(control)) {
        return;
    }

#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);

    if (control.connection_id.empty()) {
        for (auto& [connection_id, session] : sessions_) {
            if (session) {
                close_socket_if_open(session->socket);
            }
        }
        sessions_.clear();
    } else {
        const auto found = sessions_.find(control.connection_id);
        if (found != sessions_.end()) {
            if (found->second) {
                close_socket_if_open(found->second->socket);
            }
            sessions_.erase(found);
        }
    }

    state_.last_connection_id = control.connection_id;
    state_.last_disconnect_reason = control.reason;
    state_.active_session_count = sessions_.size();
#else
    (void)event;
#endif
}

}  // namespace qbit_miner
