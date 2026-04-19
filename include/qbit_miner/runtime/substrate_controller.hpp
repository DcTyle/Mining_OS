#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "qbit_miner/app/application.hpp"
#include "qbit_miner/runtime/phase_clamped_mining_os.hpp"
#include "qbit_miner/runtime/phase_transport_scheduler.hpp"

namespace qbit_miner {

struct SubstrateControllerConfig {
    std::size_t runtime_ticks = 0;
    std::uint32_t tick_interval_ms = 0;
    bool emit_gui_trace_refresh = true;
    bool emit_network_trace_publish = true;
    bool emit_phase_shadow_schedule = true;
    bool emit_phase_clamped_share_actuation = true;
};

struct SubstrateRunSummary {
    std::size_t requested_ticks = 0;
    std::size_t processed_ticks = 0;
    std::size_t failed_ticks = 0;
    std::vector<SubstrateTrace> traces;
    std::vector<PhaseDispatchArtifact> phase_dispatch_artifacts;
    std::vector<PhaseClampedShareActuation> share_actuations;
};

class SubstrateController {
public:
    explicit SubstrateController(FieldDynamicsConfig field_config = {}, SubstrateControllerConfig controller_config = {});

    [[nodiscard]] const SubstrateControllerConfig& config() const noexcept;
    [[nodiscard]] QuantumMinerApplication& application() noexcept;
    [[nodiscard]] const QuantumMinerApplication& application() const noexcept;
    [[nodiscard]] RuntimeBus& bus() noexcept;
    [[nodiscard]] const SubstrateCache& cache() const noexcept;
    [[nodiscard]] SubstrateTrace process_feedback(const GpuFeedbackFrame& frame);
    [[nodiscard]] SubstrateRunSummary run_replay(const std::vector<GpuFeedbackFrame>& frames, std::size_t runtime_ticks = 0);

private:
    [[nodiscard]] PhaseDispatchArtifact build_phase_dispatch_artifact(const SubstrateTrace& trace) const;
    [[nodiscard]] std::vector<PhaseClampedShareActuation> build_share_actuations(
        const SubstrateTrace& trace,
        const PhaseDispatchArtifact& artifact
    ) const;
    void publish_trace_topics(const SubstrateTrace& trace);
    void publish_phase_shadow_schedule(const SubstrateTrace& trace, const PhaseDispatchArtifact& artifact);
    void publish_phase_clamped_share_actuations(
        const SubstrateTrace& trace,
        const std::vector<PhaseClampedShareActuation>& actuations);
    void publish_failure(const GpuFeedbackFrame& frame, const std::string& message);
    void sleep_if_needed(std::size_t tick_index, std::size_t total_ticks) const;

    QuantumMinerApplication application_;
    SubstrateControllerConfig config_;
    PhaseTransportShadowScheduler shadow_scheduler_;
    PhaseClampedMiningOperatingSystem mining_operating_system_;
};

}  // namespace qbit_miner