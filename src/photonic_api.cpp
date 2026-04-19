// photonic_api.cpp: External packet bridge for substrate state
#include "photonic_api.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

const char* packet_kind_name(PhotonicPacketKind kind) {
    return kind == PhotonicPacketKind::Request ? "substrate.request" : "substrate.feedback";
}

std::string sig9_to_json(const std::array<std::uint32_t, 9>& sig9) {
    std::ostringstream stream;
    stream << "[";
    for (std::size_t index = 0; index < sig9.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << sig9[index];
    }
    stream << "]";
    return stream.str();
}

}  // namespace

DecodedPhotonicFrame PhotonicDecoder::decode(const EncodedSubstrateState& state) const {
    DecodedPhotonicFrame frame;
    frame.photonic_identity = state.photonic_identity;
    frame.dominant_spin_axis = state.dominant_spin_axis;
    frame.predicted_frequency_hz = state.predicted_frequency_hz;
    frame.predicted_voltage_v = state.predicted_voltage_v;
    frame.predicted_amperage_a = state.predicted_amperage_a;
    frame.inertial_mass_proxy = state.inertial_mass_proxy;
    frame.spin_momentum_score = state.spin_momentum_score;
    frame.request_to_return_s = state.request_to_return_s;
    frame.closed_loop_latency_s = state.closed_loop_latency_s;
    return frame;
}

std::string PhotonicDecoder::to_json(const EncodedSubstrateState& state, PhotonicPacketKind kind) const {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6);
    stream << "{";
    stream << "\"api_version\": \"qbit.photonic.v1\", ";
    stream << "\"packet_type\": \"" << packet_kind_name(kind) << "\", ";
    stream << "\"frame_index\": " << state.frame_index << ", ";
    stream << "\"photonic_identity\": \"" << state.photonic_identity << "\", ";
    stream << "\"spectra_sig9\": " << sig9_to_json(state.spectra_sig9) << ", ";
    stream << "\"telemetry\": {"
           << "\"provider\": \"" << state.telemetry.provider << "\", "
           << "\"graphics_frequency_hz\": " << state.telemetry.graphics_frequency_hz << ", "
           << "\"memory_frequency_hz\": " << state.telemetry.memory_frequency_hz << ", "
           << "\"voltage_v\": " << state.telemetry.voltage_v << ", "
           << "\"amperage_a\": " << state.telemetry.amperage_a << ", "
           << "\"power_w\": " << state.telemetry.power_w << ", "
           << "\"temperature_c\": " << state.telemetry.temperature_c
           << "}, ";
    stream << "\"quartet\": {"
           << "\"frequency_norm\": " << state.quartet.frequency_norm << ", "
           << "\"amplitude_norm\": " << state.quartet.amplitude_norm << ", "
           << "\"amperage_norm\": " << state.quartet.amperage_norm << ", "
           << "\"voltage_norm\": " << state.quartet.voltage_norm
           << "}, ";
    stream << "\"field_dynamics\": {"
           << "\"axis_scale_x\": " << state.axis_scale_x << ", "
           << "\"axis_scale_y\": " << state.axis_scale_y << ", "
           << "\"axis_scale_z\": " << state.axis_scale_z << ", "
           << "\"axis_resonance\": " << state.axis_resonance << ", "
           << "\"vector_energy\": " << state.vector_energy << ", "
           << "\"temporal_coupling_moment\": " << state.temporal_coupling_moment << ", "
           << "\"inertial_mass_proxy\": " << state.inertial_mass_proxy << ", "
           << "\"spin_momentum_score\": " << state.spin_momentum_score << ", "
           << "\"dominant_spin_axis\": \"" << state.dominant_spin_axis << "\""
           << "}, ";
    stream << "\"transport\": {"
           << "\"phase_transport_term\": " << state.phase_transport_term << ", "
           << "\"flux_transport_term\": " << state.flux_transport_term << ", "
           << "\"observer_damping\": " << state.observer_damping << ", "
           << "\"phase_correction_norm\": " << state.phase_correction_norm << ", "
           << "\"flux_correction_norm\": " << state.flux_correction_norm
           << "}, ";
    stream << "\"prediction\": {"
           << "\"predicted_frequency_hz\": " << state.predicted_frequency_hz << ", "
           << "\"predicted_amplitude_norm\": " << state.predicted_amplitude_norm << ", "
           << "\"predicted_voltage_v\": " << state.predicted_voltage_v << ", "
           << "\"predicted_amperage_a\": " << state.predicted_amperage_a
           << "}, ";
    stream << "\"timing\": {"
           << "\"request_to_return_s\": " << state.request_to_return_s << ", "
           << "\"calculation_time_s\": " << state.calculation_time_s << ", "
           << "\"closed_loop_latency_s\": " << state.closed_loop_latency_s << ", "
           << "\"next_feedback_time_s\": " << state.next_feedback_time_s
           << "}, ";
    stream << "\"encoding_words\": {"
           << "\"axis_word\": " << state.axis_word << ", "
           << "\"spin_word\": " << state.spin_word << ", "
           << "\"inertia_word\": " << state.inertia_word
           << "}";
    stream << "}";
    return stream.str();
}

std::vector<PhotonicPacket> PhotonicAPI::watch(const EncodedSubstrateState& state) {
    std::vector<PhotonicPacket> packets;
    for (PhotonicPacketKind kind : {PhotonicPacketKind::Request, PhotonicPacketKind::Feedback}) {
        PhotonicPacket packet;
        packet.kind = kind;
        packet.channel = packet_kind_name(kind);
        packet.photonic_identity = state.photonic_identity;
        packet.payload_json = _decoder.to_json(state, kind);
        packets.push_back(packet);
        std::cout << "[PhotonicAPI] channel=" << packet.channel << " payload=" << packet.payload_json << "\n";
    }
    return packets;
}

DecodedPhotonicFrame PhotonicAPI::decode(const EncodedSubstrateState& state) const {
    return _decoder.decode(state);
}