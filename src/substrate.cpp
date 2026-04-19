// substrate.cpp: Substrate implementation
#include "substrate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

double vector_energy3(double x, double y, double z) {
    return std::sqrt(std::max(0.0, ((x * x) + (y * y) + (z * z)) / 3.0));
}

}  // namespace

Substrate::Substrate()
    : _has_pending(false),
      _phase_turns(0.0),
      _last_phase_transport(0.0),
      _frame_index(0) {
}

double Substrate::clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double Substrate::clamp_signed(double value, double limit) {
    const double bound = std::abs(limit);
    return std::max(-bound, std::min(bound, value));
}

std::uint32_t Substrate::quantize_sig9(double value) {
    return static_cast<std::uint32_t>(std::llround(clamp01(value) * 1000000.0));
}

std::uint32_t Substrate::stable_word(const std::string& text) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint32_t>(ch);
        hash *= 16777619u;
    }
    return hash;
}

std::string Substrate::build_photonic_identity(const std::array<std::uint32_t, 9>& sig9) {
    std::ostringstream stream;
    stream << "PID9";
    for (std::uint32_t component : sig9) {
        stream << "-" << std::hex << std::nouppercase << std::setw(6) << std::setfill('0') << component;
    }
    return stream.str();
}

double Substrate::normalize(double value, double lower, double upper) const {
    const double span = std::max(upper - lower, 1.0e-9);
    return clamp01((value - lower) / span);
}

double Substrate::denormalize(double normalized, double lower, double upper) const {
    return lower + (clamp01(normalized) * (upper - lower));
}

double Substrate::normalize_frequency(double graphics_frequency_hz) const {
    const double coeff = _data.frequency_min
        + ((_data.frequency_max - _data.frequency_min) * clamp01(graphics_frequency_hz / _data.graphics_hz_reference));
    return normalize(coeff, _data.frequency_min, _data.frequency_max);
}

double Substrate::normalize_amplitude(double amplitude_norm) const {
    const double coeff = denormalize(amplitude_norm, _data.amplitude_min, _data.amplitude_max);
    return normalize(coeff, _data.amplitude_min, _data.amplitude_max);
}

double Substrate::normalize_amperage(double amperage_a) const {
    const double coeff = _data.amperage_min
        + ((_data.amperage_max - _data.amperage_min) * clamp01(amperage_a / _data.amperage_reference));
    return normalize(coeff, _data.amperage_min, _data.amperage_max);
}

double Substrate::normalize_voltage(double voltage_v) const {
    const double live_norm = normalize(voltage_v, _data.voltage_reference_min, _data.voltage_reference_max);
    const double coeff = denormalize(live_norm, _data.voltage_min, _data.voltage_max);
    return normalize(coeff, _data.voltage_min, _data.voltage_max);
}

const EncodedSubstrateState& Substrate::update_from_telemetry(const GPUTelemetrySample& telemetry) {
    _pending = telemetry;
    _has_pending = true;

    _state.frame_index = _frame_index++;
    _state.telemetry = telemetry;
    _state.quartet.frequency_norm = normalize_frequency(telemetry.graphics_frequency_hz);
    _state.quartet.amplitude_norm = normalize_amplitude(telemetry.amplitude_norm);
    _state.quartet.amperage_norm = normalize_amperage(telemetry.amperage_a);
    _state.quartet.voltage_norm = normalize_voltage(telemetry.voltage_v);

    return _state;
}

void Substrate::compute_axis_dynamics() {
    const double frequency = _state.quartet.frequency_norm;
    const double amplitude = _state.quartet.amplitude_norm;
    const double amperage = _state.quartet.amperage_norm;
    const double voltage = _state.quartet.voltage_norm;
    const double flux = clamp01(0.55 * _state.telemetry.memory_util_norm + 0.45 * clamp01(_state.telemetry.power_w / 350.0));
    const double resonance_gate = clamp01(1.0 - _state.telemetry.thermal_interference_norm);
    const double overlap = clamp01(0.50 * _state.telemetry.gpu_util_norm + 0.30 * _state.telemetry.memory_util_norm + 0.20 * resonance_gate);
    const double x = clamp_signed((_state.telemetry.gpu_util_norm * 2.0) - 1.0);
    const double y = clamp_signed((_state.telemetry.memory_util_norm * 2.0) - 1.0);
    const double z = clamp_signed((_state.telemetry.thermal_interference_norm * 2.0) - 1.0);
    const double joint_gate = clamp01(std::sqrt(std::max(frequency * amplitude, 0.0)));

    _state.axis_scale_x = clamp01(0.20 + (0.60 * frequency) + (0.10 * resonance_gate) + (0.10 * std::abs(x)));
    _state.axis_scale_y = clamp01(0.20 + (0.60 * amplitude) + (0.10 * overlap) + (0.10 * std::abs(y)));
    _state.axis_scale_z = clamp01(0.20 + (0.52 * joint_gate) + (0.16 * flux) + (0.12 * std::abs(z)));

    _state.axis_resonance = clamp01(
        1.0 - (
            std::abs(_state.axis_scale_x - _state.axis_scale_y)
            + std::abs(_state.axis_scale_y - _state.axis_scale_z)
            + std::abs(_state.axis_scale_x - _state.axis_scale_z)) / 3.0);

    const double scaled_x = x * (0.5 + (0.5 * _state.axis_scale_x));
    const double scaled_y = y * (0.5 + (0.5 * _state.axis_scale_y));
    const double scaled_z = z * (0.5 + (0.5 * _state.axis_scale_z));

    _state.vector_energy = clamp01(vector_energy3(scaled_x, scaled_y, scaled_z) + (0.20 * clamp01(_state.telemetry.power_w / 350.0)));
    const double speed_measure = clamp01((0.52 * _state.vector_energy) + (0.26 * frequency) + (0.22 * amplitude));
    const double gamma = 1.0 / std::sqrt(std::max(0.08, 1.0 - (0.92 * speed_measure * speed_measure)));
    _state.relativistic_correlation = clamp01((gamma - 1.0) / 2.5);
    _state.temporal_coupling_moment = clamp01(
        (0.34 * resonance_gate)
        + (0.24 * _state.axis_resonance)
        + (0.18 * overlap)
        + (0.12 * joint_gate)
        + (0.12 * flux));

    _state.spin_axis_x = clamp_signed((scaled_y * _state.axis_scale_z) - (scaled_z * _state.axis_scale_y));
    _state.spin_axis_y = clamp_signed((scaled_z * _state.axis_scale_x) - (scaled_x * _state.axis_scale_z));
    _state.spin_axis_z = clamp_signed((scaled_x * _state.axis_scale_y) - (scaled_y * _state.axis_scale_x));
    _state.spin_momentum_score = clamp01(vector_energy3(_state.spin_axis_x, _state.spin_axis_y, _state.spin_axis_z));

    _state.inertial_mass_proxy = clamp01(
        (0.46 * _state.vector_energy)
        + (0.22 * _state.relativistic_correlation)
        + (0.18 * _state.spin_momentum_score)
        + (0.14 * _state.temporal_coupling_moment));

    const double abs_x = std::abs(_state.spin_axis_x);
    const double abs_y = std::abs(_state.spin_axis_y);
    const double abs_z = std::abs(_state.spin_axis_z);
    if (abs_x >= abs_y && abs_x >= abs_z) {
        _state.dominant_spin_axis = "x";
    } else if (abs_y >= abs_z) {
        _state.dominant_spin_axis = "y";
    } else {
        _state.dominant_spin_axis = "z";
    }
}

void Substrate::compute_transport() {
    const double gpu_memory_skew = std::abs(_state.telemetry.gpu_util_norm - _state.telemetry.memory_util_norm);
    const double latency_norm = clamp01(_state.telemetry.delta_s / 0.25);

    _state.observer_damping = clamp01(
        (0.28 * _state.telemetry.thermal_interference_norm)
        + (0.22 * latency_norm)
        + (0.18 * std::abs(_state.quartet.voltage_norm - _state.quartet.amperage_norm))
        + (0.14 * (1.0 - _state.axis_resonance))
        + (0.10 * (1.0 - _state.quartet.frequency_norm))
        + (0.08 * gpu_memory_skew));

    _state.phase_transport_term = clamp_signed(
        (0.26 * _last_phase_transport)
        + (0.22 * _state.quartet.voltage_norm)
        + (0.18 * _state.quartet.amperage_norm)
        + (0.14 * _state.temporal_coupling_moment)
        + (0.10 * _state.axis_resonance)
        + (0.10 * _state.quartet.frequency_norm)
        - (0.20 * _state.observer_damping));

    _state.flux_transport_term = clamp01(
        (0.24 * _state.quartet.amplitude_norm)
        + (0.22 * _state.telemetry.memory_util_norm)
        + (0.18 * clamp01(_state.telemetry.power_w / 350.0))
        + (0.16 * _state.vector_energy)
        + (0.12 * _state.axis_resonance)
        + (0.08 * (1.0 - _state.observer_damping)));

    _state.phase_correction_norm = clamp01(0.5 + (0.5 * _state.phase_transport_term));
    _state.flux_correction_norm = clamp01(
        (0.55 * _state.flux_transport_term)
        + (0.20 * _state.spin_momentum_score)
        + (0.15 * _state.temporal_coupling_moment)
        + (0.10 * (1.0 - _state.observer_damping)));

    _state.request_to_return_s = std::max(0.02, _state.telemetry.delta_s)
        + (0.12 * _state.observer_damping)
        + (0.08 * _state.telemetry.thermal_interference_norm);
}

void Substrate::compute_next_pulse() {
    _state.next_quartet.frequency_norm = clamp01(
        _state.quartet.frequency_norm
        + (0.18 * _state.phase_transport_term)
        + (0.08 * _state.temporal_coupling_moment)
        - (0.10 * _state.observer_damping));

    _state.next_quartet.amplitude_norm = clamp01(
        _state.quartet.amplitude_norm
        + (0.16 * _state.flux_transport_term)
        + (0.08 * _state.spin_momentum_score)
        - (0.06 * _state.telemetry.thermal_interference_norm));

    _state.next_quartet.amperage_norm = clamp01(
        _state.quartet.amperage_norm
        + (0.12 * _state.flux_correction_norm)
        + (0.10 * _state.inertial_mass_proxy)
        - (0.05 * _state.observer_damping));

    _state.next_quartet.voltage_norm = clamp01(
        _state.quartet.voltage_norm
        + (0.14 * _state.phase_correction_norm)
        + (0.10 * _state.vector_energy)
        - (0.06 * _state.observer_damping));

    _state.predicted_frequency_hz = _state.telemetry.graphics_frequency_hz * (0.90 + (0.20 * _state.next_quartet.frequency_norm));
    _state.predicted_amplitude_norm = _state.next_quartet.amplitude_norm;
    _state.predicted_voltage_v = _data.voltage_reference_min
        + ((_data.voltage_reference_max - _data.voltage_reference_min) * _state.next_quartet.voltage_norm);
    _state.predicted_amperage_a = std::max(1.0, _state.telemetry.amperage_a * (0.85 + (0.30 * _state.next_quartet.amperage_norm)));

    _phase_turns = std::fmod(_phase_turns + _state.phase_correction_norm, 1.0);
    if (_phase_turns < 0.0) {
        _phase_turns += 1.0;
    }
}

void Substrate::compute_signature() {
    _state.spectra_sig9 = {
        quantize_sig9(_state.axis_scale_x),
        quantize_sig9(_state.axis_scale_y),
        quantize_sig9(_state.axis_scale_z),
        quantize_sig9(_state.phase_correction_norm),
        quantize_sig9(_state.flux_correction_norm),
        quantize_sig9(_state.observer_damping),
        quantize_sig9(_state.spin_momentum_score),
        quantize_sig9(_state.inertial_mass_proxy),
        quantize_sig9(_state.temporal_coupling_moment),
    };

    std::ostringstream axis_stream;
    axis_stream
        << _state.axis_scale_x << ":"
        << _state.axis_scale_y << ":"
        << _state.axis_scale_z << ":"
        << _state.axis_resonance;
    _state.axis_word = stable_word(axis_stream.str());

    std::ostringstream spin_stream;
    spin_stream
        << _state.spin_axis_x << ":"
        << _state.spin_axis_y << ":"
        << _state.spin_axis_z << ":"
        << _state.spin_momentum_score;
    _state.spin_word = stable_word(spin_stream.str());

    std::ostringstream inertia_stream;
    inertia_stream
        << _state.inertial_mass_proxy << ":"
        << _state.temporal_coupling_moment << ":"
        << _state.phase_transport_term << ":"
        << _state.flux_transport_term;
    _state.inertia_word = stable_word(inertia_stream.str());

    _state.photonic_identity = build_photonic_identity(_state.spectra_sig9);
}

void Substrate::compute_dynamics() {
    if (!_has_pending) {
        return;
    }

    const auto started = std::chrono::steady_clock::now();

    compute_axis_dynamics();
    compute_transport();
    compute_next_pulse();
    compute_signature();

    const auto finished = std::chrono::steady_clock::now();
    _state.calculation_time_s = std::chrono::duration<double>(finished - started).count();
    _state.closed_loop_latency_s = _state.request_to_return_s + _state.calculation_time_s;
    _state.next_feedback_time_s = _state.closed_loop_latency_s + 0.02 + (0.01 * _state.observer_damping);
    _last_phase_transport = _state.phase_transport_term;
    _has_pending = false;
}

void Substrate::feedback() {
    std::cout
        << "[Substrate] id=" << _state.photonic_identity
        << " axis_res=" << _state.axis_resonance
        << " inertial_mass=" << _state.inertial_mass_proxy
        << " spin=" << _state.spin_momentum_score
        << " phase_transport=" << _state.phase_transport_term
        << " flux_transport=" << _state.flux_transport_term
        << " next_feedback_s=" << _state.next_feedback_time_s
        << "\n";
}

const EncodedSubstrateState& Substrate::state() const {
    return _state;
}
