#include "qbit_miner/substrate/calibration_export.hpp"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "qbit_miner/substrate/trace_serialization.hpp"

namespace qbit_miner {

namespace {

std::string sanitize_component(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0 || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string("trace") : out;
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to write calibration export file: " + path.string());
    }
    output << text;
}

std::string make_sweep_filename(std::size_t step_index, const CalibrationSweepStep& step) {
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << step_index
        << '_' << sanitize_component(step.variable)
        << '_' << sanitize_component(step.direction)
        << ".json";
    return out.str();
}

}  // namespace

CalibrationExportResult export_calibration_bundle(
    const std::vector<SubstrateTrace>& traces,
    const std::filesystem::path& output_dir
) {
    std::filesystem::create_directories(output_dir);

    const std::filesystem::path manifest_path = output_dir / "manifest.json";
    const std::filesystem::path traces_jsonl_path = output_dir / "traces.jsonl";

    std::ofstream traces_jsonl(traces_jsonl_path, std::ios::binary);
    if (!traces_jsonl) {
        throw std::runtime_error("Unable to create calibration traces JSONL: " + traces_jsonl_path.string());
    }

    std::size_t sweep_file_count = 0;
    for (const auto& trace : traces) {
        const std::filesystem::path trace_dir = output_dir / sanitize_component(trace.photonic_identity.trace_id);
        const std::filesystem::path sweeps_dir = trace_dir / "sweeps";
        std::filesystem::create_directories(sweeps_dir);

        write_text_file(trace_dir / "trace.json", serialize_trace_json(trace));
        write_text_file(trace_dir / "calibration_plan.json", serialize_calibration_plan_json(trace));

        for (std::size_t step_index = 0; step_index < trace.calibration_plan.sweeps.size(); ++step_index) {
            const auto& step = trace.calibration_plan.sweeps[step_index];
            write_text_file(
                sweeps_dir / make_sweep_filename(step_index, step),
                serialize_calibration_sweep_json(trace, step, step_index)
            );
            ++sweep_file_count;
        }

        traces_jsonl << serialize_trace_json(trace) << '\n';
    }

    write_text_file(manifest_path, serialize_calibration_manifest_json(traces));

    return CalibrationExportResult{
        output_dir,
        manifest_path,
        traces_jsonl_path,
        traces.size(),
        sweep_file_count,
    };
}

}  // namespace qbit_miner