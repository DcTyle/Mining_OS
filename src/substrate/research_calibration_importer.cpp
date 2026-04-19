#include "qbit_miner/substrate/research_calibration_importer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qbit_miner {

namespace {

std::string trim_copy(const std::string& text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false;
    for (char ch : line) {
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (ch == ',' && !in_quotes) {
            out.push_back(trim_copy(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    out.push_back(trim_copy(current));
    return out;
}

double parse_double(const std::unordered_map<std::string, std::string>& row, const std::string& key) {
    const auto iter = row.find(key);
    if (iter == row.end()) {
        throw std::runtime_error("Missing CSV column: " + key);
    }
    return std::stod(iter->second);
}

std::uint64_t parse_u64(const std::unordered_map<std::string, std::string>& row, const std::string& key) {
    const auto iter = row.find(key);
    if (iter == row.end()) {
        throw std::runtime_error("Missing CSV column: " + key);
    }
    return static_cast<std::uint64_t>(std::stoull(iter->second));
}

std::uint32_t parse_u32(const std::unordered_map<std::string, std::string>& row, const std::string& key) {
    return static_cast<std::uint32_t>(parse_u64(row, key));
}

double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

}  // namespace

std::vector<GpuFeedbackFrame> ResearchCalibrationImporter::import_run45_csv(
    const std::filesystem::path& csv_path,
    const std::string& gpu_device_id
) const {
    std::ifstream input(csv_path);
    if (!input) {
        throw std::runtime_error("Unable to open calibration CSV: " + csv_path.string());
    }

    std::string header_line;
    if (!std::getline(input, header_line)) {
        throw std::runtime_error("Calibration CSV is empty: " + csv_path.string());
    }

    const std::vector<std::string> header = split_csv_line(header_line);
    std::vector<GpuFeedbackFrame> frames;

    std::string line;
    while (std::getline(input, line)) {
        if (trim_copy(line).empty()) {
            continue;
        }
        std::vector<std::string> values = split_csv_line(line);
        if (values.size() < header.size()) {
            values.resize(header.size());
        }

        std::unordered_map<std::string, std::string> row;
        for (std::size_t index = 0; index < header.size(); ++index) {
            row.emplace(header[index], index < values.size() ? values[index] : std::string());
        }

        GpuFeedbackFrame frame;
        frame.photonic_identity.trace_id = row.at("photonic_identity");
        frame.photonic_identity.source_identity = row.at("photonic_identity");
        frame.photonic_identity.gpu_device_id = gpu_device_id;
        frame.photonic_identity.spectra_9d = {
            parse_double(row, "axis_scale_x"),
            parse_double(row, "axis_scale_y"),
            parse_double(row, "axis_scale_z"),
            parse_double(row, "vector_energy"),
            parse_double(row, "temporal_coupling_moment"),
            parse_double(row, "inertial_mass_proxy"),
            parse_double(row, "spin_momentum_score"),
            parse_double(row, "phase_transport_term"),
            parse_double(row, "flux_transport_term"),
        };

        frame.photonic_identity.field_vector.frequency = parse_double(row, "axis_scale_x");
        frame.photonic_identity.field_vector.amplitude = parse_double(row, "axis_scale_y");
        frame.photonic_identity.field_vector.current = parse_double(row, "axis_scale_z");
        frame.photonic_identity.field_vector.voltage = parse_double(row, "vector_energy");
        frame.photonic_identity.field_vector.phase = parse_double(row, "phase_transport_term");
        frame.photonic_identity.field_vector.flux = parse_double(row, "flux_transport_term");
        frame.photonic_identity.field_vector.thermal_noise = parse_double(row, "predictive_harmonic_noise_reaction_norm");
        frame.photonic_identity.field_vector.field_noise = parse_double(row, "predictive_anchor_interference_norm");

        frame.photonic_identity.spin_inertia.axis_spin = {
            parse_double(row, "phase_transport_term"),
            parse_double(row, "flux_transport_term"),
            parse_double(row, "spin_momentum_score"),
        };
        frame.photonic_identity.spin_inertia.axis_orientation = {
            parse_double(row, "predictive_anchor_interference_norm"),
            parse_double(row, "predictive_harmonic_noise_reaction_norm"),
            parse_double(row, "predictive_phase_ring_density"),
        };
        frame.photonic_identity.spin_inertia.momentum_score = parse_double(row, "spin_momentum_score");
        frame.photonic_identity.spin_inertia.inertial_mass_proxy = parse_double(row, "inertial_mass_proxy");
        frame.photonic_identity.spin_inertia.relativistic_correlation = parse_double(row, "observer_damping");
        frame.photonic_identity.spin_inertia.relative_temporal_coupling = parse_double(row, "temporal_coupling_moment");
        frame.photonic_identity.spin_inertia.temporal_coupling_count = parse_u32(row, "encodable_node_count");

        frame.photonic_identity.coherence = parse_double(row, "predictive_temporal_accuracy_score");
        frame.photonic_identity.memory = parse_double(row, "predictive_phase_ring_density");
        frame.photonic_identity.nexus = parse_double(row, "predictive_zero_point_crossover_norm");

        frame.timing.tick_index = parse_u64(row, "frame_index");
        frame.timing.request_time_ms = 0.0;
        frame.timing.response_time_ms = parse_double(row, "request_to_return_s") * 1000.0;
        frame.timing.accounting_time_ms = parse_double(row, "calculation_time_s") * 1000.0;
        frame.timing.next_feedback_time_ms = parse_double(row, "next_feedback_time_s") * 1000.0;
        frame.timing.closed_loop_latency_ms = parse_double(row, "closed_loop_latency_s") * 1000.0;
        frame.timing.encode_deadline_ms = frame.timing.closed_loop_latency_ms;

        frame.encodable_node_count = parse_u32(row, "encodable_node_count");
        frame.sent_signal = parse_double(row, "axis_scale_x");
        frame.measured_signal = parse_double(row, "axis_scale_z");
        frame.integrated_feedback = parse_double(row, "flux_transport_term");
        frame.derivative_signal = parse_double(row, "phase_transport_term");
        frame.lattice_closure = clamp_unit(parse_double(row, "observer_damping"));
        frame.phase_closure = clamp_unit(parse_double(row, "predictive_phase_ring_density"));
        frame.recurrence_alignment = clamp_unit(parse_double(row, "predictive_temporal_accuracy_score"));
        frame.conservation_alignment = clamp_unit(1.0 - parse_double(row, "predictive_anchor_interference_norm"));

        frames.push_back(std::move(frame));
    }

    return frames;
}

}  // namespace qbit_miner