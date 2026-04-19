cmake_minimum_required(VERSION 3.22)

project(QuantumMiner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QBIT_MINER_BUILD_TESTS "Build Q Bit Miner substrate tests" ON)

find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
    add_custom_target(phase_program_targets
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/transpile_frequency_language.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/runtime_substrate_viewport.freq.md
            --output-dir
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/runtime_substrate_viewport
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/transpile_frequency_language.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/control_surface_firmware.freq.md
            --output-dir
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/control_surface_firmware
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/transpile_frequency_language.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/stratum_connection_authority.freq.md
            --output-dir
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/stratum_connection_authority
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/transpile_frequency_language.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/stratum_worker_scheduler.freq.md
            --output-dir
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/stratum_worker_scheduler
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/transpile_frequency_language.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/mining_os_resonance_buffer.freq.md
            --output-dir
            ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/mining_os_resonance_buffer
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating phase-language runtime targets"
    )
endif()

add_library(qbit_miner_core
    src/app/application.cpp
    src/cache/substrate_cache.cpp
    src/control_center/field_viewport.cpp
    src/control_center/live_control_center.cpp
    src/runtime/phase_clamped_mining_os.cpp
    src/runtime/gpu_kernel_telemetry_bridge.cpp
    src/runtime/substrate_phase_program_metadata.cpp
    src/runtime/phase_transport_scheduler.cpp
    src/runtime/runtime_bus.cpp
    src/runtime/substrate_firmware_runtime.cpp
    src/runtime/substrate_stratum_harness.cpp
    src/runtime/substrate_stratum_tcp_adapter.cpp
    src/runtime/substrate_stratum_pow.cpp
    src/runtime/substrate_stratum_protocol.cpp
    src/runtime/substrate_control_surface.cpp
    src/runtime/substrate_viewport_encoder.cpp
    src/gpu_telemetry.cpp
    src/runtime/substrate_controller.cpp
    src/substrate/calibration_export.cpp
    src/substrate/device_validation_export.cpp
    src/substrate/field_dynamics.cpp
    src/substrate/research_calibration_importer.cpp
    src/telemetry/mining_metrics.cpp
    src/substrate/trace_serialization.cpp
)

target_include_directories(qbit_miner_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(qbit_miner_core PUBLIC cxx_std_20)

if(TARGET phase_program_targets)
    add_dependencies(qbit_miner_core phase_program_targets)
endif()

if(WIN32)
    target_compile_definitions(qbit_miner_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(qbit_miner_core PRIVATE ws2_32)
endif()

add_executable(quantum_miner
    src/main.cpp
)

target_link_libraries(quantum_miner PRIVATE qbit_miner_core)
set_target_properties(quantum_miner PROPERTIES OUTPUT_NAME "Quantum Miner")

if(WIN32)
    add_executable(quantum_miner_control_center WIN32
        src/control_center/main.cpp
        src/control_center/substrate_compute_runtime.cpp
    )

    target_link_libraries(quantum_miner_control_center PRIVATE qbit_miner_core winmm)
    set_target_properties(quantum_miner_control_center PROPERTIES OUTPUT_NAME "Quantum Miner Control Center")

    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        target_link_libraries(quantum_miner_control_center PRIVATE Vulkan::Vulkan)

        if(Vulkan_GLSLC_EXECUTABLE)
            set(QBIT_MINER_PHASE_COMPUTE_SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/phase_shaders/vkcompute_phase_substrate_update.spv")
            file(TO_CMAKE_PATH "${QBIT_MINER_PHASE_COMPUTE_SHADER_SPV}" QBIT_MINER_PHASE_COMPUTE_SHADER_SPV_CMAKE)

            add_custom_command(
                OUTPUT ${QBIT_MINER_PHASE_COMPUTE_SHADER_SPV}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/phase_shaders
                COMMAND ${Vulkan_GLSLC_EXECUTABLE}
                    -fshader-stage=compute
                    -o ${QBIT_MINER_PHASE_COMPUTE_SHADER_SPV}
                    ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/vkcompute_phase_substrate_update.glsl
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/vkcompute_phase_substrate_update.glsl
                COMMENT "Compiling phase compute shader"
            )
            add_custom_target(phase_vulkan_shader_assets DEPENDS ${QBIT_MINER_PHASE_COMPUTE_SHADER_SPV})
            add_dependencies(quantum_miner_control_center phase_vulkan_shader_assets)
            target_compile_definitions(
                quantum_miner_control_center PRIVATE
                QBIT_MINER_HAS_VULKAN=1
                QBIT_MINER_PHASE_COMPUTE_SHADER_PATH=\"${QBIT_MINER_PHASE_COMPUTE_SHADER_SPV_CMAKE}\"
            )
        else()
            target_compile_definitions(quantum_miner_control_center PRIVATE QBIT_MINER_HAS_VULKAN=1)
        endif()
    endif()

    add_executable(qbit_miner_live_pool_runner
        tools/live_pool_runner.cpp
    )

    target_link_libraries(qbit_miner_live_pool_runner PRIVATE qbit_miner_core)
endif()

if(QBIT_MINER_BUILD_TESTS)
    enable_testing()

    add_executable(qbit_miner_tests
        tests/test_photonic_identity.cpp
    )

    target_link_libraries(qbit_miner_tests PRIVATE qbit_miner_core)
    add_test(NAME qbit_miner_tests COMMAND qbit_miner_tests)

    add_executable(qbit_miner_control_center_tests
        tests/test_control_center_metrics.cpp
    )

    add_executable(qbit_miner_phase_program_tests
        tests/test_phase_program_metadata.cpp
    )

    target_link_libraries(qbit_miner_control_center_tests PRIVATE qbit_miner_core)
    target_link_libraries(qbit_miner_phase_program_tests PRIVATE qbit_miner_core)

    if(WIN32)
        target_link_libraries(qbit_miner_tests PRIVATE ws2_32)
        target_link_libraries(qbit_miner_control_center_tests PRIVATE ws2_32)
        target_link_libraries(qbit_miner_phase_program_tests PRIVATE ws2_32)
    endif()

    add_test(NAME qbit_miner_control_center_tests COMMAND qbit_miner_control_center_tests)
    add_test(NAME qbit_miner_phase_program_tests COMMAND qbit_miner_phase_program_tests)

    if(Python3_Interpreter_FOUND)
        add_test(
            NAME phase_control_surface_firmware_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/validate_generated_targets.py
                --program-dir
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/control_surface_firmware
                --python
                ${Python3_EXECUTABLE}
        )
        set_tests_properties(phase_control_surface_firmware_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME phase_runtime_substrate_viewport_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/validate_generated_targets.py
                --program-dir
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/runtime_substrate_viewport
                --python
                ${Python3_EXECUTABLE}
        )
        set_tests_properties(phase_runtime_substrate_viewport_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME phase_stratum_connection_authority_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/validate_generated_targets.py
                --program-dir
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/stratum_connection_authority
                --python
                ${Python3_EXECUTABLE}
        )
        set_tests_properties(phase_stratum_connection_authority_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME phase_stratum_worker_scheduler_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/validate_generated_targets.py
                --program-dir
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/stratum_worker_scheduler
                --python
                ${Python3_EXECUTABLE}
        )
        set_tests_properties(phase_stratum_worker_scheduler_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME phase_mining_os_resonance_buffer_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/scripts/validate_generated_targets.py
                --program-dir
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/generated/mining_os_resonance_buffer
                --python
                ${Python3_EXECUTABLE}
        )
        set_tests_properties(phase_mining_os_resonance_buffer_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME research_qbit_run45_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ResearchConfinement/q_bit_miner/tests/test_validate_qbit_run45.py
        )
        set_tests_properties(research_qbit_run45_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME research_qbit_fourier_parallel_hash_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ResearchConfinement/q_bit_miner/tests/test_fourier_parallel_hash_validation.py
        )
        set_tests_properties(research_qbit_fourier_parallel_hash_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        add_test(
            NAME research_qbit_substrate_contract_validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ResearchConfinement/QBitMinerSubstrate/tests/test_qbit_miner_research_contract.py
        )
        set_tests_properties(research_qbit_substrate_contract_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    if(Vulkan_GLSLC_EXECUTABLE)
        add_test(
            NAME phase_vulkan_compute_contract_validation
            COMMAND ${Vulkan_GLSLC_EXECUTABLE}
                -fshader-stage=compute
                -o
                ${CMAKE_CURRENT_BINARY_DIR}/runtime_substrate_viewport_validation.spv
                ${CMAKE_CURRENT_SOURCE_DIR}/.github/skills/phase-encoding/assets/vkcompute_phase_substrate_update.glsl
        )
        set_tests_properties(phase_vulkan_compute_contract_validation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
