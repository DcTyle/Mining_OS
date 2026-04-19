#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

class SubstrateCache {
public:
    explicit SubstrateCache(std::size_t capacity = 256);

    void store(SubstrateTrace trace);
    [[nodiscard]] std::optional<SubstrateTrace> find_by_trace_id(const std::string& trace_id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    std::size_t capacity_;
    std::deque<SubstrateTrace> traces_;
};

}  // namespace qbit_miner