#include "qbit_miner/runtime/runtime_bus.hpp"

#include <utility>

namespace qbit_miner {

void RuntimeBus::subscribe(const std::string& topic, Subscriber subscriber) {
    subscribers_[topic].push_back(std::move(subscriber));
}

void RuntimeBus::publish(const RuntimeEvent& event) const {
    const auto exact = subscribers_.find(event.topic);
    if (exact != subscribers_.end()) {
        for (const auto& subscriber : exact->second) {
            subscriber(event);
        }
    }
    const auto wildcard = subscribers_.find("*");
    if (wildcard != subscribers_.end()) {
        for (const auto& subscriber : wildcard->second) {
            subscriber(event);
        }
    }
}

}  // namespace qbit_miner