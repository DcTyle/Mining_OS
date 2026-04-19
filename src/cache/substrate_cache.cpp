#include "qbit_miner/cache/substrate_cache.hpp"

#include <utility>

namespace qbit_miner {

SubstrateCache::SubstrateCache(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void SubstrateCache::store(SubstrateTrace trace) {
    if (traces_.size() >= capacity_) {
        traces_.pop_front();
    }
    traces_.push_back(std::move(trace));
}

std::optional<SubstrateTrace> SubstrateCache::find_by_trace_id(const std::string& trace_id) const {
    for (auto iter = traces_.rbegin(); iter != traces_.rend(); ++iter) {
        if (iter->photonic_identity.trace_id == trace_id) {
            return *iter;
        }
    }
    return std::nullopt;
}

std::size_t SubstrateCache::size() const noexcept {
    return traces_.size();
}

std::size_t SubstrateCache::capacity() const noexcept {
    return capacity_;
}

}  // namespace qbit_miner