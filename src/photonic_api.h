// photonic_api.h: External packet bridge for substrate state
#pragma once

#include <string>
#include <vector>

#include "substrate.h"

enum class PhotonicPacketKind {
    Request,
    Feedback,
};

struct PhotonicPacket {
    PhotonicPacketKind kind = PhotonicPacketKind::Request;
    std::string channel;
    std::string photonic_identity;
    std::string payload_json;
};

struct DecodedPhotonicFrame {
    std::string photonic_identity;
    std::string dominant_spin_axis;
    double predicted_frequency_hz = 0.0;
    double predicted_voltage_v = 0.0;
    double predicted_amperage_a = 0.0;
    double inertial_mass_proxy = 0.0;
    double spin_momentum_score = 0.0;
    double request_to_return_s = 0.0;
    double closed_loop_latency_s = 0.0;
};

class PhotonicDecoder {
public:
    DecodedPhotonicFrame decode(const EncodedSubstrateState& state) const;
    std::string to_json(const EncodedSubstrateState& state, PhotonicPacketKind kind) const;
};

class PhotonicAPI {
public:
    std::vector<PhotonicPacket> watch(const EncodedSubstrateState& state);
    DecodedPhotonicFrame decode(const EncodedSubstrateState& state) const;

private:
    PhotonicDecoder _decoder;
};