// substrate.h: Encoded substrate engine
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "gpu_telemetry.h"

struct EncodedQuartet {
    double frequency_norm = 0.0;
    double amplitude_norm = 0.0;
    double amperage_norm = 0.0;
    double voltage_norm = 0.0;
};

struct EncodedSubstrateState {
    std::uint64_t frame_index = 0;
    GPUTelemetrySample telemetry;
    EncodedQuartet quartet;
    EncodedQuartet next_quartet;
    double axis_scale_x = 0.0;
    double axis_scale_y = 0.0;
    double axis_scale_z = 0.0;
    double axis_resonance = 0.0;
    double vector_energy = 0.0;
    double temporal_coupling_moment = 0.0;
    double relativistic_correlation = 0.0;
    double inertial_mass_proxy = 0.0;
    double spin_axis_x = 0.0;
    double spin_axis_y = 0.0;
    double spin_axis_z = 0.0;
    double spin_momentum_score = 0.0;
    double phase_transport_term = 0.0;
    double flux_transport_term = 0.0;
    double observer_damping = 0.0;
    double phase_correction_norm = 0.0;
    double flux_correction_norm = 0.0;
    double request_to_return_s = 0.0;
    double closed_loop_latency_s = 0.0;
    double calculation_time_s = 0.0;
    double next_feedback_time_s = 0.0;
    double predicted_frequency_hz = 0.0;
    double predicted_amplitude_norm = 0.0;
    double predicted_voltage_v = 0.0;
    double predicted_amperage_a = 0.0;
    std::string dominant_spin_axis;
    std::array<std::uint32_t, 9> spectra_sig9{};
    std::string photonic_identity;
    std::uint32_t axis_word = 0;
    std::uint32_t spin_word = 0;
    std::uint32_t inertia_word = 0;
};

class Substrate {
public:
    Substrate();

    const EncodedSubstrateState& update_from_telemetry(const GPUTelemetrySample& telemetry);
    void compute_dynamics();
    void feedback();
    const EncodedSubstrateState& state() const;

private:
    struct EncodedData {
        double frequency_min = 0.145;
        double frequency_max = 0.275;
        double amplitude_min = 0.12;
        double amplitude_max = 0.24;
        double amperage_min = 0.27;
        double amperage_max = 0.53;
        double voltage_min = 0.27;
        double voltage_max = 0.45;
        double graphics_hz_reference = 2500000000.0;
        double memory_hz_reference = 12000000000.0;
        double amperage_reference = 120.0;
        double voltage_reference_min = 0.65;
        double voltage_reference_max = 1.25;
    };

    EncodedData _data;
    GPUTelemetrySample _pending;
    EncodedSubstrateState _state;
    bool _has_pending;
    double _phase_turns;
    double _last_phase_transport;
    std::uint64_t _frame_index;

    static double clamp01(double value);
    static double clamp_signed(double value, double limit = 1.0);
    static std::uint32_t quantize_sig9(double value);
    static std::uint32_t stable_word(const std::string& text);
    static std::string build_photonic_identity(const std::array<std::uint32_t, 9>& sig9);

    double normalize(double value, double lower, double upper) const;
    double denormalize(double normalized, double lower, double upper) const;
    double normalize_frequency(double graphics_frequency_hz) const;
    double normalize_amplitude(double amplitude_norm) const;
    double normalize_amperage(double amperage_a) const;
    double normalize_voltage(double voltage_v) const;

    void compute_axis_dynamics();
    void compute_transport();
    void compute_next_pulse();
    void compute_signature();
};
