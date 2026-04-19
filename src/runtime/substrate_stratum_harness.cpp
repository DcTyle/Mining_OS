#include "qbit_miner/runtime/substrate_stratum_harness.hpp"

#include <utility>

namespace qbit_miner {

namespace {

constexpr double kDryRunShareDifficulty = 0.0000000003;
constexpr const char* kDryRunShareDifficultyJson = "0.0000000003";

SubstrateStratumResponsePayload build_response(const SubstrateStratumDispatchPayload& payload) {
    SubstrateStratumResponsePayload response;
    response.command_kind = payload.command_kind;
    response.connection_id = payload.connection_id;
    response.request_id = payload.request_id;
    response.method = payload.method;
    response.accepted = true;

    switch (payload.command_kind) {
    case StratumCommandKind::Connect:
        response.message = "Dry-run TCP transport connected";
        response.payload_json = "{\"connected\": true, \"transport\": \"tcp\"}";
        break;
    case StratumCommandKind::Subscribe:
        response.message = "Dry-run subscribe accepted";
        response.subscription_id = "dry-run-subscription";
        response.extranonce1 = "cafef00d";
        response.extranonce2_size = 4;
        response.payload_json =
            "{\"id\": \"" + payload.request_id
            + "\", \"result\": [[\"mining.notify\", \"dry-run-subscription\"], \"cafef00d\", 4], \"error\": null}";
        break;
    case StratumCommandKind::Authorize:
        response.message = "Dry-run authorize accepted";
        response.payload_json = "{\"id\": \"" + payload.request_id + "\", \"result\": true, \"error\": null}";
        break;
    case StratumCommandKind::Submit:
        response.message = "Dry-run submit accepted";
        response.payload_json = "{\"id\": \"" + payload.request_id + "\", \"result\": true, \"error\": null}";
        break;
    }

    return response;
}

SubstrateStratumServerEventPayload build_set_difficulty_event(const SubstrateStratumDispatchPayload& payload) {
    SubstrateStratumServerEventPayload event;
    event.event_kind = StratumServerEventKind::SetDifficulty;
    event.connection_id = payload.connection_id;
    event.method = stratum_server_event_method(StratumServerEventKind::SetDifficulty);
    event.difficulty = kDryRunShareDifficulty;
    event.payload_json = std::string("{\"id\": null, \"method\": \"mining.set_difficulty\", \"params\": [")
        + kDryRunShareDifficultyJson + "]}";
    return event;
}

SubstrateStratumServerEventPayload build_notify_event(const SubstrateStratumDispatchPayload& payload) {
    SubstrateStratumServerEventPayload event;
    event.event_kind = StratumServerEventKind::Notify;
    event.connection_id = payload.connection_id;
    event.method = stratum_server_event_method(StratumServerEventKind::Notify);
    event.job_id = "dry-run-job-0001";
    event.header_hex = std::string(160U, '0');
    event.prevhash = "0000000000000000000cafe0000000000000000000000000000000000000000";
    event.coinbase1 = "0100000001";
    event.coinbase2 = "ffffffff020000000000";
    event.version = "20000000";
    event.nbits = "207fffff";
    event.ntime = "65a0b100";
    event.clean_jobs = true;
    event.payload_json =
        "{\"id\": null, \"method\": \"mining.notify\", \"params\": [\"dry-run-job-0001\", \"0000000000000000000cafe0000000000000000000000000000000000000000\", \"0100000001\", \"ffffffff020000000000\", [], \"20000000\", \"207fffff\", \"65a0b100\", true]}";
    return event;
}

void publish_server_event(RuntimeBus& bus, const SubstrateStratumServerEventPayload& payload) {
    RuntimeEvent event;
    event.topic = stratum_server_event_topic(payload.event_kind);
    event.message = "Dry-run Stratum server event replayed from transcript";
    event.stratum_server_event_payload = payload;
    event.has_stratum_server_event_payload = true;
    bus.publish(event);
}

}  // namespace

SubstrateStratumDryRunHarness::SubstrateStratumDryRunHarness(RuntimeBus& bus)
    : bus_(bus) {
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

SubstrateStratumDryRunHarnessState SubstrateStratumDryRunHarness::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void SubstrateStratumDryRunHarness::handle_dispatch(const RuntimeEvent& event) {
    if (!event.has_stratum_dispatch_payload || !event.stratum_dispatch_payload.dry_run_only) {
        return;
    }

    const SubstrateStratumDispatchPayload payload = event.stratum_dispatch_payload;
    const SubstrateStratumResponsePayload response = build_response(payload);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++state_.dispatch_count;
        ++state_.response_count;
        state_.last_request_id = payload.request_id;
        state_.last_payload_json = payload.payload_json;
        state_.saw_connect = state_.saw_connect || payload.command_kind == StratumCommandKind::Connect;
        state_.saw_subscribe = state_.saw_subscribe || payload.command_kind == StratumCommandKind::Subscribe;
        state_.saw_authorize = state_.saw_authorize || payload.command_kind == StratumCommandKind::Authorize;
        state_.saw_submit = state_.saw_submit || payload.command_kind == StratumCommandKind::Submit;
    }

    RuntimeEvent response_event;
    response_event.topic = "substrate.stratum.response";
    response_event.message = response.message;
    response_event.stratum_response_payload = response;
    response_event.has_stratum_response_payload = true;
    bus_.publish(response_event);

    if (payload.command_kind == StratumCommandKind::Authorize) {
        const SubstrateStratumServerEventPayload difficulty_event = build_set_difficulty_event(payload);
        const SubstrateStratumServerEventPayload notify_event = build_notify_event(payload);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.notification_count += 2U;
            state_.saw_set_difficulty = true;
            state_.saw_notify = true;
        }

        publish_server_event(bus_, difficulty_event);
        publish_server_event(bus_, notify_event);
    }
}

}  // namespace qbit_miner