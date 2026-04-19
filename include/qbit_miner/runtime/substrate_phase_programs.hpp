#pragma once

#include <string_view>

namespace qbit_miner::phase_programs {

inline constexpr std::string_view kViewportProgramSource = ".github/skills/phase-encoding/assets/runtime_substrate_viewport.freq.md";
inline constexpr std::string_view kControlSurfaceProgramSource = ".github/skills/phase-encoding/assets/control_surface_firmware.freq.md";
inline constexpr std::string_view kStratumConnectionProgramSource = ".github/skills/phase-encoding/assets/stratum_connection_authority.freq.md";
inline constexpr std::string_view kStratumWorkerProgramSource = ".github/skills/phase-encoding/assets/stratum_worker_scheduler.freq.md";
inline constexpr std::string_view kMiningResonanceProgramSource = ".github/skills/phase-encoding/assets/mining_os_resonance_buffer.freq.md";
inline constexpr std::string_view kViewportGeneratedDir = ".github/skills/phase-encoding/generated/runtime_substrate_viewport";
inline constexpr std::string_view kControlSurfaceGeneratedDir = ".github/skills/phase-encoding/generated/control_surface_firmware";
inline constexpr std::string_view kStratumConnectionGeneratedDir = ".github/skills/phase-encoding/generated/stratum_connection_authority";
inline constexpr std::string_view kStratumWorkerGeneratedDir = ".github/skills/phase-encoding/generated/stratum_worker_scheduler";
inline constexpr std::string_view kMiningResonanceGeneratedDir = ".github/skills/phase-encoding/generated/mining_os_resonance_buffer";

}  // namespace qbit_miner::phase_programs