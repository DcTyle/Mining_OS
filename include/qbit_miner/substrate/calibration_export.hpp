#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct CalibrationExportResult {
    std::filesystem::path output_dir;
    std::filesystem::path manifest_path;
    std::filesystem::path traces_jsonl_path;
    std::size_t trace_count = 0;
    std::size_t sweep_file_count = 0;
};

[[nodiscard]] CalibrationExportResult export_calibration_bundle(
    const std::vector<SubstrateTrace>& traces,
    const std::filesystem::path& output_dir
);

}  // namespace qbit_miner