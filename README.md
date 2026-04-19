# Q Bit Miner

## Control Center

The repository now includes a Win64 control-center target named `quantum_miner_control_center`.

It provides:
- a dark metallic GUI shell with `File`, `Edit`, and `Window` menu tabs
- live mining metrics grouped by rig, pool, and blockchain sections
- reward projections selectable as per-minute, per-hour, or per-day values
- a Vulkan Win64 viewport shell that consumes the phase-derived `runtime_substrate_viewport.freq.md` contract through `SubstrateViewportFrame` and binds those field outputs into voxels, PBR-style material parameters, and field-driven audio output
- calibration and validation export actions from the GUI backend

The authoritative viewport behavior source is the frequency-language program at `.github/skills/phase-encoding/assets/runtime_substrate_viewport.freq.md`.
Host-side C++ is only the Win64/Vulkan boundary, runtime binding, and validation surface.

The authoritative control-plane source is `.github/skills/phase-encoding/assets/control_surface_firmware.freq.md`.
The authoritative Stratum connection-authority source is `.github/skills/phase-encoding/assets/stratum_connection_authority.freq.md`.
The authoritative Stratum worker-scheduling source is `.github/skills/phase-encoding/assets/stratum_worker_scheduler.freq.md`.
The Win64/Vulkan application is intended to stay a lazy read-only inspection surface plus user-input ingress boundary that feeds low-level substrate firmware contracts rather than owning engine behavior.

Build from the workspace root with CMake:

```powershell
cmake -S . -B build
cmake --build build --config Debug --target quantum_miner_control_center
```

If a Vulkan SDK is available at configure time, the viewport uses a Vulkan swapchain presentation path. Otherwise the target still builds and uses a software viewport fallback inside the same Win64 GUI.

Validation includes:
- `qbit_miner_control_center_tests`
- `phase_control_surface_firmware_validation`
- `phase_runtime_substrate_viewport_validation`
- `phase_stratum_connection_authority_validation`
- `phase_stratum_worker_scheduler_validation`
- `phase_vulkan_compute_contract_validation` when `glslc` is available from the Vulkan SDK

