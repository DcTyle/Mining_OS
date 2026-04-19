#pragma once

#include <array>
#include <string>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct PhaseDispatchArtifact {
    std::string phase_id;
    double fourier_transport_frequency = 0.0;
    double phase_vector_magnitude = 0.0;
    std::array<double, 3> phase_vector_direction {0.0, 0.0, 1.0};
    double phase_lock_error = 0.0;
    double relock_pressure = 0.0;
    double anchor_correlation = 0.0;
    double anchor_evm_norm = 0.0;
    double sideband_energy_norm = 0.0;
    double group_delay_skew = 0.0;
    double dynamic_range_headroom = 0.0;
    double temporal_admissibility = 0.0;
    double transport_readiness = 0.0;
    double zero_point_proximity = 0.0;
    double coherence_alignment = 0.0;
    double interference_projection = 0.0;
    double phase_lock_pressure = 0.0;
    std::string transport_mode;
    std::string activation_surface;
    std::string egress_topic;
    bool dispatch_permitted = false;
};

class PhaseTransportShadowScheduler {
public:
    explicit PhaseTransportShadowScheduler(double dispatch_readiness_threshold = 0.64);

    [[nodiscard]] double dispatch_readiness_threshold() const noexcept;
    [[nodiscard]] PhaseDispatchArtifact compute_artifact(const SubstrateTrace& trace) const;

private:
    double dispatch_readiness_threshold_;
};

}  // namespace qbit_miner