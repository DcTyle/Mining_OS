#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "qbit_miner/runtime/substrate_controller.hpp"
#include "qbit_miner/substrate/calibration_export.hpp"
#include "qbit_miner/substrate/device_validation_export.hpp"
#include "qbit_miner/substrate/research_calibration_importer.hpp"
#include "qbit_miner/substrate/trace_serialization.hpp"

namespace {

std::string parse_string_option(int argc, char** argv, const std::string& flag, const std::string& fallback = "") {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == flag && (index + 1) < argc) {
            return std::string(argv[index + 1]);
        }
    }
    return fallback;
}

double parse_double_option(int argc, char** argv, const std::string& flag, double fallback) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == flag && (index + 1) < argc) {
            try {
                return std::stod(argv[index + 1]);
            } catch (...) {
                return fallback;
            }
        }
    }
    return fallback;
}

std::size_t parse_size_option(int argc, char** argv, const std::string& flag, std::size_t fallback) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == flag && (index + 1) < argc) {
            try {
                return static_cast<std::size_t>(std::stoull(argv[index + 1]));
            } catch (...) {
                return fallback;
            }
        }
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == flag) {
            return true;
        }
    }
    return false;
}

std::filesystem::path parse_import_path(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--import-run45-csv" && (index + 1) < argc) {
            return std::filesystem::path(argv[index + 1]);
        }
    }
    return {};
}

std::filesystem::path parse_export_dir(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--export-calibration-dir" && (index + 1) < argc) {
            return std::filesystem::path(argv[index + 1]);
        }
    }
    return {};
}

std::filesystem::path parse_device_validation_dir(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--export-device-validation-dir" && (index + 1) < argc) {
            return std::filesystem::path(argv[index + 1]);
        }
    }
    return {};
}

qbit_miner::GpuFeedbackFrame make_sample_frame() {
    qbit_miner::GpuFeedbackFrame frame;
    frame.photonic_identity.source_identity = "sample-photonic-identity";
    frame.photonic_identity.gpu_device_id = "gpu-rtx2060";
    frame.photonic_identity.coherence = 0.92;
    frame.photonic_identity.memory = 0.85;
    frame.photonic_identity.nexus = 0.54;
    frame.photonic_identity.field_vector.amplitude = 0.18;
    frame.photonic_identity.field_vector.voltage = 0.36;
    frame.photonic_identity.field_vector.current = 0.36;
    frame.photonic_identity.field_vector.frequency = 0.245;
    frame.photonic_identity.field_vector.phase = 0.25;
    frame.photonic_identity.field_vector.flux = 0.30;
    frame.photonic_identity.field_vector.thermal_noise = 0.08;
    frame.photonic_identity.field_vector.field_noise = 0.06;
    frame.photonic_identity.spin_inertia.axis_spin = {0.2061, 0.0298, -0.2905};
    frame.photonic_identity.spin_inertia.axis_orientation = {0.1440, 0.0810, -0.2250};
    frame.photonic_identity.spin_inertia.momentum_score = 0.2063;
    frame.photonic_identity.spin_inertia.inertial_mass_proxy = 0.2993;
    frame.photonic_identity.spin_inertia.relativistic_correlation = 0.0189;
    frame.photonic_identity.spin_inertia.relative_temporal_coupling = 0.72;
    frame.photonic_identity.spin_inertia.temporal_coupling_count = 6;
    frame.timing.tick_index = 1;
    frame.timing.request_time_ms = 0.0;
    frame.timing.response_time_ms = 1.25;
    frame.timing.accounting_time_ms = 10.0;
    frame.timing.next_feedback_time_ms = 23.0;
    frame.timing.closed_loop_latency_ms = 823.0;
    frame.timing.encode_deadline_ms = 823.0;
    frame.encodable_node_count = 6;
    frame.integrated_feedback = 0.30;
    frame.derivative_signal = 0.01;
    frame.lattice_closure = 0.88;
    frame.phase_closure = 0.81;
    frame.recurrence_alignment = 0.76;
    frame.conservation_alignment = 0.999;
    return frame;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path import_path = parse_import_path(argc, argv);
    const std::filesystem::path export_dir = parse_export_dir(argc, argv);
    const std::filesystem::path device_validation_dir = parse_device_validation_dir(argc, argv);
    const std::size_t runtime_ticks = parse_size_option(argc, argv, "--runtime-ticks", 0);
    const std::size_t tick_interval_ms = parse_size_option(argc, argv, "--tick-interval-ms", 0);

    qbit_miner::SubstrateControllerConfig controller_config;
    controller_config.runtime_ticks = runtime_ticks;
    controller_config.tick_interval_ms = static_cast<std::uint32_t>(std::min<std::size_t>(
        tick_interval_ms,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));

    qbit_miner::SubstrateController controller({}, controller_config);

    qbit_miner::DeviceValidationExportOptions validation_options;
    validation_options.hardware_profile.operator_id = parse_string_option(argc, argv, "--operator-id", validation_options.hardware_profile.operator_id);
    validation_options.hardware_profile.device_model = parse_string_option(argc, argv, "--device-model", validation_options.hardware_profile.device_model);
    validation_options.hardware_profile.driver_version = parse_string_option(argc, argv, "--driver-version", validation_options.hardware_profile.driver_version);
    validation_options.hardware_profile.cooling_class = parse_string_option(argc, argv, "--cooling-class", validation_options.hardware_profile.cooling_class);
    validation_options.hardware_profile.primary_target_network = parse_string_option(argc, argv, "--target-network", validation_options.hardware_profile.primary_target_network);
    validation_options.hardware_profile.power_draw_watts = parse_double_option(argc, argv, "--power-draw-watts", validation_options.hardware_profile.power_draw_watts);
    validation_options.hardware_profile.electricity_cost_usd_per_kwh = parse_double_option(argc, argv, "--electricity-usd-per-kwh", validation_options.hardware_profile.electricity_cost_usd_per_kwh);
    validation_options.hardware_profile.measured_device_window = has_flag(argc, argv, "--measured-device-window");
    validation_options.btc_price_usd = parse_double_option(argc, argv, "--btc-price-usd", validation_options.btc_price_usd);
    validation_options.estimated_reward_per_share_usd = parse_double_option(argc, argv, "--estimated-reward-per-share-usd", validation_options.estimated_reward_per_share_usd);
    validation_options.minimum_window_hours = parse_double_option(argc, argv, "--minimum-window-hours", validation_options.minimum_window_hours);

    std::cout << controller.application().name() << '\n';
    if (!import_path.empty()) {
        qbit_miner::ResearchCalibrationImporter importer;
        const auto frames = importer.import_run45_csv(import_path, "research-import");
        const qbit_miner::SubstrateRunSummary summary = controller.run_replay(frames, runtime_ticks);
        for (const auto& trace : summary.traces) {
            std::cout << qbit_miner::serialize_trace_json(trace) << '\n';
        }
        if (!export_dir.empty()) {
            (void)qbit_miner::export_calibration_bundle(summary.traces, export_dir);
        }
        if (!device_validation_dir.empty()) {
            (void)qbit_miner::export_device_validation_bundle(summary.traces, device_validation_dir, validation_options);
        }
        return summary.failed_ticks == 0 ? 0 : 1;
    }

    const qbit_miner::SubstrateRunSummary summary = controller.run_replay(
        std::vector<qbit_miner::GpuFeedbackFrame>{make_sample_frame()},
        runtime_ticks == 0 ? 1 : runtime_ticks);
    if (summary.traces.empty()) {
        return 1;
    }

    const qbit_miner::SubstrateTrace& trace = summary.traces.back();
    if (!export_dir.empty()) {
        (void)qbit_miner::export_calibration_bundle(summary.traces, export_dir);
    }
    if (!device_validation_dir.empty()) {
        (void)qbit_miner::export_device_validation_bundle(summary.traces, device_validation_dir, validation_options);
    }
    for (const auto& emitted_trace : summary.traces) {
        std::cout << std::fixed << std::setprecision(6)
                  << qbit_miner::serialize_trace_json(emitted_trace) << '\n';
    }
    return summary.failed_ticks == 0 ? 0 : 1;
}
