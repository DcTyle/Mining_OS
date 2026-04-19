#include "qbit_miner/runtime/substrate_control_surface.hpp"

#include <cmath>
#include <sstream>

namespace qbit_miner {

SubstrateControlIngress SubstrateControlSurface::build_ingress(
    RewardIntervalUnit reward_unit,
    bool audio_enabled,
    bool paused,
    bool compact_layout,
    bool always_on_top,
    double user_input_phase
) const {
    SubstrateControlIngress ingress;
    ingress.reward_unit = reward_unit;
    ingress.audio_enabled = audio_enabled;
    ingress.paused = paused;
    ingress.compact_layout = compact_layout;
    ingress.always_on_top = always_on_top;
    ingress.viewport_readonly = true;
    ingress.ui_refresh_ms = 100.0;
    ingress.user_input_phase = std::fmod(std::abs(user_input_phase), 1.0);
    ingress.authoritative_program_source = std::string(phase_programs::kControlSurfaceProgramSource);

    std::ostringstream out;
    out << "control-ingress-" << static_cast<int>(reward_unit)
        << '-' << (audio_enabled ? 'a' : 'm')
        << '-' << (paused ? 'p' : 'l')
        << '-' << (compact_layout ? 'c' : 'f')
        << '-' << (always_on_top ? 't' : 'n');
    ingress.ingress_id = out.str();
    return ingress;
}

}  // namespace qbit_miner