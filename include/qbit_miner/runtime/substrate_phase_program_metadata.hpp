#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qbit_miner {

struct SubstratePhaseProgramBlockMetadata {
    std::string kind;
    std::string name;
    std::vector<std::string> rules;

    [[nodiscard]] bool contains_rule_token(std::string_view token) const noexcept;
};

struct SubstratePhaseProgramMetadata {
    std::string title;
    std::string source_path;
    std::string generated_dir;
    std::vector<SubstratePhaseProgramBlockMetadata> blocks;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t block_count() const noexcept;
    [[nodiscard]] const SubstratePhaseProgramBlockMetadata* find_block(std::string_view name) const noexcept;
    [[nodiscard]] bool has_block(std::string_view name, std::string_view kind = {}) const noexcept;
    [[nodiscard]] bool block_has_rule_token(std::string_view block_name, std::string_view token) const noexcept;
};

[[nodiscard]] const SubstratePhaseProgramMetadata& phase_program_metadata_for_generated_dir(
    std::string_view generated_dir
);
[[nodiscard]] const SubstratePhaseProgramMetadata& viewport_program_metadata();
[[nodiscard]] const SubstratePhaseProgramMetadata& control_surface_program_metadata();
[[nodiscard]] const SubstratePhaseProgramMetadata& stratum_connection_program_metadata();
[[nodiscard]] const SubstratePhaseProgramMetadata& stratum_worker_program_metadata();
[[nodiscard]] const SubstratePhaseProgramMetadata& mining_resonance_program_metadata();

}  // namespace qbit_miner