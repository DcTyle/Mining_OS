#include "qbit_miner/runtime/substrate_phase_program_metadata.hpp"

#include "qbit_miner/runtime/substrate_phase_programs.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbit_miner {

namespace {

[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size()
        && value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::size_t skip_whitespace(std::string_view document, std::size_t position) noexcept {
    while (position < document.size()) {
        const char ch = document[position];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++position;
    }
    return position;
}

[[nodiscard]] std::string parse_json_string(std::string_view document, std::size_t& position) {
    if (position >= document.size() || document[position] != '"') {
        return {};
    }

    ++position;
    std::string value;
    while (position < document.size()) {
        const char ch = document[position++];
        if (ch == '\\') {
            if (position >= document.size()) {
                break;
            }

            const char escaped = document[position++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(escaped);
                break;
            }
            continue;
        }

        if (ch == '"') {
            break;
        }

        value.push_back(ch);
    }

    return value;
}

[[nodiscard]] std::size_t find_key_position(
    std::string_view document,
    std::string_view key,
    std::size_t start_position
) noexcept {
    const std::string needle = std::string{"\""} + std::string(key) + '"';
    return document.find(needle, start_position);
}

[[nodiscard]] std::string find_string_value(
    std::string_view document,
    std::string_view key,
    std::size_t start_position,
    std::size_t* key_position = nullptr
) {
    const std::size_t located_key = find_key_position(document, key, start_position);
    if (located_key == std::string_view::npos) {
        return {};
    }
    if (key_position != nullptr) {
        *key_position = located_key;
    }

    std::size_t value_position = document.find(':', located_key);
    if (value_position == std::string_view::npos) {
        return {};
    }
    value_position = skip_whitespace(document, value_position + 1U);
    return parse_json_string(document, value_position);
}

[[nodiscard]] std::vector<std::string> find_string_array_value(
    std::string_view document,
    std::string_view key,
    std::size_t start_position,
    std::size_t* key_position = nullptr
) {
    std::size_t located_key = std::string_view::npos;
    const std::string ignored = find_string_value(document, key, start_position, &located_key);
    (void)ignored;
    if (located_key == std::string_view::npos) {
        return {};
    }
    if (key_position != nullptr) {
        *key_position = located_key;
    }

    std::size_t array_position = document.find('[', located_key);
    if (array_position == std::string_view::npos) {
        return {};
    }

    ++array_position;
    std::vector<std::string> values;
    while (array_position < document.size()) {
        array_position = skip_whitespace(document, array_position);
        if (array_position >= document.size()) {
            break;
        }
        if (document[array_position] == ']') {
            ++array_position;
            break;
        }
        if (document[array_position] == ',') {
            ++array_position;
            continue;
        }
        if (document[array_position] != '"') {
            break;
        }
        values.push_back(parse_json_string(document, array_position));
    }
    return values;
}

[[nodiscard]] std::filesystem::path resolve_ir_path(std::string_view generated_dir) {
    const auto resolve_generated_directory = [](std::string_view generated_directory) {
        const std::filesystem::path direct_path(generated_directory);
        if (direct_path.is_absolute()) {
            return direct_path;
        }

        const std::filesystem::path cwd_candidate = std::filesystem::current_path() / direct_path;
        if (std::filesystem::exists(cwd_candidate)) {
            return cwd_candidate;
        }

        const std::filesystem::path source_root =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        return source_root / direct_path;
    };

    const std::filesystem::path directory = resolve_generated_directory(generated_dir);
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (ends_with(filename, ".ir.json")) {
            return entry.path();
        }
    }

    return {};
}

[[nodiscard]] SubstratePhaseProgramMetadata parse_program_metadata(
    std::string_view document,
    std::string_view generated_dir
) {
    SubstratePhaseProgramMetadata metadata;
    metadata.generated_dir = std::string(generated_dir);
    metadata.title = find_string_value(document, "title", 0U);
    metadata.source_path = find_string_value(document, "source", 0U);

    std::size_t search_position = 0U;
    while (true) {
        std::size_t kind_position = std::string_view::npos;
        const std::string kind = find_string_value(document, "kind", search_position, &kind_position);
        if (kind_position == std::string_view::npos) {
            break;
        }

        std::size_t name_position = std::string_view::npos;
        const std::string name = find_string_value(document, "name", kind_position, &name_position);
        std::size_t rules_position = std::string_view::npos;
        std::vector<std::string> rules = find_string_array_value(document, "rules", name_position, &rules_position);

        if (!kind.empty() && !name.empty()) {
            metadata.blocks.push_back(SubstratePhaseProgramBlockMetadata{
                kind,
                name,
                std::move(rules),
            });
        }

        search_position = rules_position == std::string_view::npos ? (kind_position + 1U) : (rules_position + 1U);
    }

    return metadata;
}

[[nodiscard]] SubstratePhaseProgramMetadata load_program_metadata(std::string_view generated_dir) {
    const std::filesystem::path ir_path = resolve_ir_path(generated_dir);
    if (ir_path.empty()) {
        return SubstratePhaseProgramMetadata{.generated_dir = std::string(generated_dir)};
    }

    const std::string document = read_text_file(ir_path);
    if (document.empty()) {
        return SubstratePhaseProgramMetadata{.generated_dir = std::string(generated_dir)};
    }

    return parse_program_metadata(document, generated_dir);
}

}  // namespace

bool SubstratePhaseProgramBlockMetadata::contains_rule_token(std::string_view token) const noexcept {
    for (const std::string& rule : rules) {
        if (rule.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool SubstratePhaseProgramMetadata::empty() const noexcept {
    return title.empty() && blocks.empty();
}

std::size_t SubstratePhaseProgramMetadata::block_count() const noexcept {
    return blocks.size();
}

const SubstratePhaseProgramBlockMetadata* SubstratePhaseProgramMetadata::find_block(std::string_view name) const noexcept {
    for (const auto& block : blocks) {
        if (block.name == name) {
            return &block;
        }
    }
    return nullptr;
}

bool SubstratePhaseProgramMetadata::has_block(std::string_view name, std::string_view kind) const noexcept {
    for (const auto& block : blocks) {
        if (block.name != name) {
            continue;
        }
        if (!kind.empty() && block.kind != kind) {
            continue;
        }
        return true;
    }
    return false;
}

bool SubstratePhaseProgramMetadata::block_has_rule_token(
    std::string_view block_name,
    std::string_view token
) const noexcept {
    const SubstratePhaseProgramBlockMetadata* block = find_block(block_name);
    return block != nullptr && block->contains_rule_token(token);
}

const SubstratePhaseProgramMetadata& phase_program_metadata_for_generated_dir(std::string_view generated_dir) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, SubstratePhaseProgramMetadata> cache;

    const std::string cache_key(generated_dir);
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto found = cache.find(cache_key);
    if (found == cache.end()) {
        found = cache.emplace(cache_key, load_program_metadata(cache_key)).first;
    }
    return found->second;
}

const SubstratePhaseProgramMetadata& viewport_program_metadata() {
    return phase_program_metadata_for_generated_dir(phase_programs::kViewportGeneratedDir);
}

const SubstratePhaseProgramMetadata& control_surface_program_metadata() {
    return phase_program_metadata_for_generated_dir(phase_programs::kControlSurfaceGeneratedDir);
}

const SubstratePhaseProgramMetadata& stratum_connection_program_metadata() {
    return phase_program_metadata_for_generated_dir(phase_programs::kStratumConnectionGeneratedDir);
}

const SubstratePhaseProgramMetadata& stratum_worker_program_metadata() {
    return phase_program_metadata_for_generated_dir(phase_programs::kStratumWorkerGeneratedDir);
}

const SubstratePhaseProgramMetadata& mining_resonance_program_metadata() {
    return phase_program_metadata_for_generated_dir(phase_programs::kMiningResonanceGeneratedDir);
}

}  // namespace qbit_miner