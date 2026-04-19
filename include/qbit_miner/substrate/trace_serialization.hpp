#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

[[nodiscard]] std::string serialize_trace_json(const SubstrateTrace& trace);
[[nodiscard]] std::string serialize_feedback_frame_json(const GpuFeedbackFrame& frame);
[[nodiscard]] std::string serialize_calibration_plan_json(const SubstrateTrace& trace);
[[nodiscard]] std::string serialize_calibration_sweep_json(const SubstrateTrace& trace, const CalibrationSweepStep& step, std::size_t step_index);
[[nodiscard]] std::string serialize_calibration_manifest_json(const std::vector<SubstrateTrace>& traces);

}  // namespace qbit_miner