#include "qbit_miner/substrate/device_validation_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace qbit_miner {

namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

std::string escape_json(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

template <std::size_t N>
void append_array(std::ostringstream& out, const std::array<double, N>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << values[index];
    }
    out << ']';
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to write device validation export file: " + path.string());
    }
    output << text;
}

std::uint64_t stable_hash64(const std::string& text) {
    std::uint64_t state = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        state ^= static_cast<std::uint64_t>(ch);
        state *= 1099511628211ULL;
    }
    return state;
}

std::string make_profile_id(const DeviceHardwareProfile& profile, const std::vector<SubstrateTrace>& traces) {
    const std::string seed = profile.operator_id + "|" + profile.device_model + "|" + profile.gpu_device_id + "|"
        + (traces.empty() ? std::string("none") : traces.front().photonic_identity.trace_id);
    std::ostringstream out;
    out << "HW-" << std::hex << std::uppercase << stable_hash64(seed);
    return out.str();
}

std::string format_utc(std::int64_t epoch_seconds) {
    std::time_t raw = static_cast<std::time_t>(epoch_seconds);
    std::tm utc_tm {};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &raw);
#else
    gmtime_r(&raw, &utc_tm);
#endif
    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

double mean_signal_quality(const SubstrateTrace& trace) {
    return clamp01(
        (0.24 * clamp01(trace.photonic_identity.coherence))
        + (0.20 * clamp01(trace.coupling_strength))
        + (0.18 * clamp01(trace.zero_point_overlap_score))
        + (0.18 * clamp01(trace.trajectory_conservation_score))
        + (0.12 * clamp01(trace.reverse_causal_flux_coherence))
        + (0.08 * clamp01(trace.observer_factor))
    );
}

double load_factor(const SubstrateTrace& trace) {
    return clamp01(
        (0.34 * clamp01(trace.coupling_strength))
        + (0.24 * clamp01(trace.observer_factor))
        + (0.18 * clamp01(trace.photonic_identity.coherence))
        + (0.14 * clamp01(static_cast<double>(trace.encodable_node_count) / 16.0))
        + (0.10 * clamp01(trace.phase_transport / 8.0))
    );
}

std::string serialize_hardware_profile_json(const DeviceHardwareProfile& profile, const std::string& hardware_profile_id) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{'
        << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\", "
        << "\"operator_id\": \"" << escape_json(profile.operator_id) << "\", "
        << "\"device_model\": \"" << escape_json(profile.device_model) << "\", "
        << "\"gpu_device_id\": \"" << escape_json(profile.gpu_device_id) << "\", "
        << "\"driver_version\": \"" << escape_json(profile.driver_version) << "\", "
        << "\"cooling_class\": \"" << escape_json(profile.cooling_class) << "\", "
        << "\"primary_target_network\": \"" << escape_json(profile.primary_target_network) << "\", "
        << "\"power_draw_watts\": " << profile.power_draw_watts << ", "
        << "\"electricity_cost_usd_per_kwh\": " << profile.electricity_cost_usd_per_kwh << ", "
        << "\"measured_device_window\": " << (profile.measured_device_window ? "true" : "false")
        << '}';
    return out.str();
}

std::string serialize_profit_window_json(
    const std::string& hardware_profile_id,
    const DeviceHardwareProfile& profile,
    std::size_t accepted_share_count,
    double gross_mined_value_usd,
    double power_cost_usd,
    double net_profit_usd,
    double window_hours,
    bool completion_gate_passed
) {
    const std::int64_t start_epoch = 0;
    const std::int64_t end_epoch = static_cast<std::int64_t>(std::llround(window_hours * 3600.0));
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{'
        << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\", "
        << "\"window_start_utc\": \"" << format_utc(start_epoch) << "\", "
        << "\"window_end_utc\": \"" << format_utc(end_epoch) << "\", "
        << "\"validation_mode\": \"" << (profile.measured_device_window ? "measured_device_window" : "prototype_estimate") << "\", "
        << "\"gross_mined_value_usd\": " << gross_mined_value_usd << ", "
        << "\"power_cost_usd\": " << power_cost_usd << ", "
        << "\"net_profit_usd\": " << net_profit_usd << ", "
        << "\"accepted_share_count\": " << accepted_share_count << ", "
        << "\"completion_gate_passed\": " << (completion_gate_passed ? "true" : "false")
        << '}';
    return out.str();
}

std::string serialize_substrate_state_snapshot_json(
    const std::string& hardware_profile_id,
    const std::vector<SubstrateTrace>& traces
) {
    std::array<double, 9> mean_trajectory {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double mean_phase_alignment = 0.0;
    double mean_zero_point_overlap = 0.0;
    double mean_coupling_strength = 0.0;
    for (const auto& trace : traces) {
        for (std::size_t index = 0; index < mean_trajectory.size(); ++index) {
            mean_trajectory[index] += trace.trajectory_9d[index];
        }
        mean_phase_alignment += trace.constant_phase_alignment;
        mean_zero_point_overlap += trace.zero_point_overlap_score;
        mean_coupling_strength += trace.coupling_strength;
    }
    const double trace_count = static_cast<double>(std::max<std::size_t>(traces.size(), 1U));
    for (double& value : mean_trajectory) {
        value /= trace_count;
    }
    const SubstrateTrace& last_trace = traces.back();
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << '{'
        << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\", "
        << "\"trace_count\": " << traces.size() << ", "
        << "\"mean_trajectory_9d\": ";
    append_array(out, mean_trajectory);
    out << ", \"mean_constant_phase_alignment\": " << (mean_phase_alignment / trace_count)
        << ", \"mean_zero_point_overlap_score\": " << (mean_zero_point_overlap / trace_count)
        << ", \"mean_coupling_strength\": " << (mean_coupling_strength / trace_count)
        << ", \"last_trace_id\": \"" << escape_json(last_trace.photonic_identity.trace_id) << "\", "
        << "\"last_source_identity\": \"" << escape_json(last_trace.photonic_identity.source_identity) << "\""
        << '}';
    return out.str();
}

}  // namespace

DeviceValidationExportResult export_device_validation_bundle(
    const std::vector<SubstrateTrace>& traces,
    const std::filesystem::path& output_dir,
    const DeviceValidationExportOptions& options
) {
    if (traces.empty()) {
        throw std::runtime_error("Device validation export requires at least one substrate trace");
    }

    std::filesystem::create_directories(output_dir);

    DeviceHardwareProfile profile = options.hardware_profile;
    if (profile.gpu_device_id.empty()) {
        profile.gpu_device_id = traces.front().photonic_identity.gpu_device_id;
    }
    const std::string hardware_profile_id = profile.hardware_profile_id.empty()
        ? make_profile_id(profile, traces)
        : profile.hardware_profile_id;

    const std::filesystem::path hardware_profile_path = output_dir / "hardware_profile.json";
    const std::filesystem::path profit_window_path = output_dir / "profit_window.json";
    const std::filesystem::path accepted_share_log_path = output_dir / "accepted_share_log.jsonl";
    const std::filesystem::path power_telemetry_path = output_dir / "power_telemetry.jsonl";
    const std::filesystem::path substrate_state_snapshot_path = output_dir / "substrate_state_snapshot.json";
    const std::filesystem::path phase_vector_ledger_path = output_dir / "phase_vector_ledger.jsonl";

    std::ofstream accepted_share_log(accepted_share_log_path, std::ios::binary);
    std::ofstream power_telemetry_log(power_telemetry_path, std::ios::binary);
    std::ofstream phase_vector_ledger(phase_vector_ledger_path, std::ios::binary);
    if (!accepted_share_log || !power_telemetry_log || !phase_vector_ledger) {
        throw std::runtime_error("Unable to create device validation export logs under: " + output_dir.string());
    }

    const double trace_count = static_cast<double>(std::max<std::size_t>(traces.size(), 1U));
    const double window_hours = std::max(options.minimum_window_hours, trace_count * 0.05);
    const double slice_hours = window_hours / trace_count;
    const double electricity_rate = std::max(profile.electricity_cost_usd_per_kwh, 0.0);
    const double base_power_draw = std::max(profile.power_draw_watts, 0.0);

    std::size_t accepted_share_count = 0;
    double gross_mined_value_usd = 0.0;
    double power_cost_usd = 0.0;

    for (std::size_t index = 0; index < traces.size(); ++index) {
        const auto& trace = traces[index];
        const double share_quality = mean_signal_quality(trace);
        const bool accepted = share_quality >= 0.68;
        const double reward_usd = share_quality * std::max(options.estimated_reward_per_share_usd, 0.0);
        if (accepted) {
            ++accepted_share_count;
            gross_mined_value_usd += reward_usd;
        }

        const double sample_load = load_factor(trace);
        const double sample_power_draw = base_power_draw * std::clamp(0.65 + (0.35 * sample_load), 0.25, 1.35);
        const double sample_energy_kwh = (sample_power_draw / 1000.0) * slice_hours;
        const double sample_power_cost_usd = sample_energy_kwh * electricity_rate;
        power_cost_usd += sample_power_cost_usd;

        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(6);
            out << '{'
                << "\"trace_id\": \"" << escape_json(trace.photonic_identity.trace_id) << "\", "
                << "\"source_identity\": \"" << escape_json(trace.photonic_identity.source_identity) << "\", "
                << "\"accepted\": " << (accepted ? "true" : "false") << ", "
                << "\"accepted_share_score\": " << share_quality << ", "
                << "\"estimated_reward_usd\": " << reward_usd << ", "
                << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\""
                << '}';
            accepted_share_log << out.str() << '\n';
        }

        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(6);
            out << '{'
                << "\"trace_id\": \"" << escape_json(trace.photonic_identity.trace_id) << "\", "
                << "\"tick_index\": " << trace.timing.tick_index << ", "
                << "\"power_draw_watts\": " << sample_power_draw << ", "
                << "\"energy_kwh\": " << sample_energy_kwh << ", "
                << "\"power_cost_usd\": " << sample_power_cost_usd << ", "
                << "\"load_factor\": " << sample_load << ", "
                << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\""
                << '}';
            power_telemetry_log << out.str() << '\n';
        }

        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(6);
            out << '{'
                << "\"trace_id\": \"" << escape_json(trace.photonic_identity.trace_id) << "\", "
                << "\"source_identity\": \"" << escape_json(trace.photonic_identity.source_identity) << "\", "
                << "\"hardware_profile_id\": \"" << escape_json(hardware_profile_id) << "\", "
                << "\"trajectory_9d\": ";
            append_array(out, trace.trajectory_9d);
            out << ", \"encoded_pulse\": ";
            append_array(out, trace.encoded_pulse);
            out << ", \"phase_transport\": " << trace.phase_transport
                << ", \"flux_transport\": " << trace.flux_transport
                << ", \"observer_factor\": " << trace.observer_factor
                << ", \"coupling_strength\": " << trace.coupling_strength
                << '}';
            phase_vector_ledger << out.str() << '\n';
        }
    }

    const double net_profit_usd = gross_mined_value_usd - power_cost_usd;
    const bool completion_gate_passed = profile.measured_device_window && accepted_share_count > 0U && net_profit_usd > 0.0;

    write_text_file(hardware_profile_path, serialize_hardware_profile_json(profile, hardware_profile_id));
    write_text_file(
        profit_window_path,
        serialize_profit_window_json(
            hardware_profile_id,
            profile,
            accepted_share_count,
            gross_mined_value_usd,
            power_cost_usd,
            net_profit_usd,
            window_hours,
            completion_gate_passed
        )
    );
    write_text_file(
        substrate_state_snapshot_path,
        serialize_substrate_state_snapshot_json(hardware_profile_id, traces)
    );

    return DeviceValidationExportResult{
        output_dir,
        hardware_profile_path,
        profit_window_path,
        accepted_share_log_path,
        power_telemetry_path,
        substrate_state_snapshot_path,
        phase_vector_ledger_path,
        traces.size(),
        accepted_share_count,
        net_profit_usd,
        completion_gate_passed,
    };
}

}  // namespace qbit_miner