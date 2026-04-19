#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "qbit_miner/substrate/photonic_identity.hpp"

namespace qbit_miner {

class ResearchCalibrationImporter {
public:
    [[nodiscard]] std::vector<GpuFeedbackFrame> import_run45_csv(
        const std::filesystem::path& csv_path,
        const std::string& gpu_device_id = "research-import"
    ) const;
};

}  // namespace qbit_miner