#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "qbit_miner/substrate/field_dynamics.hpp"

namespace qbit_miner {

struct DeviceHardwareProfile {
    std::string hardware_profile_id;
    std::string operator_id = "research-operator";
    std::string device_model = "substrate-prototype";
    std::string gpu_device_id = "";
    std::string driver_version = "prototype";
    std::string cooling_class = "air";
    std::string primary_target_network = "bitcoin";
    double power_draw_watts = 175.0;
    double electricity_cost_usd_per_kwh = 0.12;
    bool measured_device_window = false;
};

struct DeviceValidationExportOptions {
    DeviceHardwareProfile hardware_profile;
    double btc_price_usd = 70000.0;
    double estimated_reward_per_share_usd = 0.035;
    double minimum_window_hours = 0.25;
};

struct DeviceValidationExportResult {
    std::filesystem::path output_dir;
    std::filesystem::path hardware_profile_path;
    std::filesystem::path profit_window_path;
    std::filesystem::path accepted_share_log_path;
    std::filesystem::path power_telemetry_path;
    std::filesystem::path substrate_state_snapshot_path;
    std::filesystem::path phase_vector_ledger_path;
    std::size_t trace_count = 0;
    std::size_t accepted_share_count = 0;
    double net_profit_usd = 0.0;
    bool completion_gate_passed = false;
};

[[nodiscard]] DeviceValidationExportResult export_device_validation_bundle(
    const std::vector<SubstrateTrace>& traces,
    const std::filesystem::path& output_dir,
    const DeviceValidationExportOptions& options = {}
);

}  // namespace qbit_miner