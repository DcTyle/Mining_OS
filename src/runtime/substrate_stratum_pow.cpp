#include "qbit_miner/runtime/substrate_stratum_pow.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace qbit_miner {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> kSha256InitialState {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

constexpr long double kDifficulty1ExpectedHashes = 4294967296.0L;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr std::array<std::uint8_t, 32> kDifficulty1Target {
    0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0xffU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double clamp_signed(double value, double limit = 1.0) {
    return std::clamp(value, -std::abs(limit), std::abs(limit));
}

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, std::uint32_t shift) {
    return (value >> shift) | (value << (32U - shift));
}

[[nodiscard]] std::uint32_t rotate_left(std::uint32_t value, std::uint32_t shift) {
    const std::uint32_t normalized_shift = shift & 31U;
    if (normalized_shift == 0U) {
        return value;
    }
    return (value << normalized_shift) | (value >> (32U - normalized_shift));
}

[[nodiscard]] std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (~x & z);
}

[[nodiscard]] std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] std::uint32_t big_sigma0(std::uint32_t value) {
    return rotate_right(value, 2U) ^ rotate_right(value, 13U) ^ rotate_right(value, 22U);
}

[[nodiscard]] std::uint32_t big_sigma1(std::uint32_t value) {
    return rotate_right(value, 6U) ^ rotate_right(value, 11U) ^ rotate_right(value, 25U);
}

[[nodiscard]] std::uint32_t small_sigma0(std::uint32_t value) {
    return rotate_right(value, 7U) ^ rotate_right(value, 18U) ^ (value >> 3U);
}

[[nodiscard]] std::uint32_t small_sigma1(std::uint32_t value) {
    return rotate_right(value, 17U) ^ rotate_right(value, 19U) ^ (value >> 10U);
}

[[nodiscard]] int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

[[nodiscard]] std::string normalize_hex(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (normalized.empty()) {
        normalized = "0";
    }
    if ((normalized.size() % 2U) != 0U) {
        normalized.insert(normalized.begin(), '0');
    }
    return normalized;
}

[[nodiscard]] bool try_parse_nonce_hex_impl(const std::string& nonce_hex, std::uint32_t& nonce_value) {
    if (nonce_hex.empty()) {
        return false;
    }

    std::string normalized;
    normalized.reserve(nonce_hex.size());
    for (const char ch : nonce_hex) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if ((normalized.size() % 2U) != 0U) {
        normalized.insert(normalized.begin(), '0');
    }
    if (normalized.size() > 8U) {
        return false;
    }

    std::istringstream stream(normalized);
    stream >> std::hex >> nonce_value;
    return !stream.fail() && stream.eof();
}

[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(const std::string& value) {
    const std::string normalized = normalize_hex(value);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(normalized.size() / 2U);
    for (std::size_t index = 0; index + 1U < normalized.size(); index += 2U) {
        const int hi = hex_value(normalized[index]);
        const int lo = hex_value(normalized[index + 1U]);
        bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

[[nodiscard]] std::string bytes_to_hex(const std::uint8_t* data, std::size_t size) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        stream << std::setw(2) << static_cast<unsigned int>(data[index]);
    }
    return stream.str();
}

[[nodiscard]] std::string reverse_hex_bytes(const std::string& value) {
    std::vector<std::uint8_t> bytes = hex_to_bytes(value);
    std::reverse(bytes.begin(), bytes.end());
    return bytes_to_hex(bytes.data(), bytes.size());
}

[[nodiscard]] std::uint32_t parse_compact_bits(const std::string& nbits_hex) {
    const std::string normalized = normalize_hex(nbits_hex);
    std::uint32_t compact = 0U;
    for (const char ch : normalized) {
        compact = (compact << 4U) | static_cast<std::uint32_t>(std::max(hex_value(ch), 0));
    }
    return compact;
}

[[nodiscard]] std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> message = input;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8ULL;

    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0x00U);
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffULL));
    }

    std::array<std::uint32_t, 8> state = kSha256InitialState;
    std::array<std::uint32_t, 64> schedule {};
    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t base = offset + (index * 4U);
            schedule[index] = (static_cast<std::uint32_t>(message[base]) << 24U)
                | (static_cast<std::uint32_t>(message[base + 1U]) << 16U)
                | (static_cast<std::uint32_t>(message[base + 2U]) << 8U)
                | static_cast<std::uint32_t>(message[base + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            schedule[index] = small_sigma1(schedule[index - 2U]) + schedule[index - 7U]
                + small_sigma0(schedule[index - 15U]) + schedule[index - 16U];
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const std::uint32_t temp1 = h + big_sigma1(e) + choose(e, f, g) + kSha256RoundConstants[index] + schedule[index];
            const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> digest {};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[(index * 4U) + 0U] = static_cast<std::uint8_t>((state[index] >> 24U) & 0xffU);
        digest[(index * 4U) + 1U] = static_cast<std::uint8_t>((state[index] >> 16U) & 0xffU);
        digest[(index * 4U) + 2U] = static_cast<std::uint8_t>((state[index] >> 8U) & 0xffU);
        digest[(index * 4U) + 3U] = static_cast<std::uint8_t>(state[index] & 0xffU);
    }
    return digest;
}

[[nodiscard]] std::vector<std::uint8_t> double_sha256(const std::vector<std::uint8_t>& input) {
    const std::array<std::uint8_t, 32> first_hash = sha256(input);
    const std::vector<std::uint8_t> intermediate(first_hash.begin(), first_hash.end());
    const std::array<std::uint8_t, 32> second_hash = sha256(intermediate);
    return std::vector<std::uint8_t>(second_hash.begin(), second_hash.end());
}

[[nodiscard]] std::array<std::uint8_t, 32> bits_to_target_bytes(const std::string& nbits_hex) {
    std::array<std::uint8_t, 32> target {};
    const std::uint32_t compact = parse_compact_bits(nbits_hex);

    const std::uint32_t exponent = (compact >> 24U) & 0xffU;
    const std::uint32_t mantissa = compact & 0x00ffffffU;

    if (exponent == 0U) {
        return target;
    }
    if (exponent <= 3U) {
        const std::uint32_t value = mantissa >> (8U * (3U - exponent));
        const std::size_t index = 32U - exponent;
        for (std::size_t byte = 0; byte < exponent; ++byte) {
            target[index + byte] = static_cast<std::uint8_t>((value >> (8U * (exponent - byte - 1U))) & 0xffU);
        }
        return target;
    }
    if (exponent > 32U) {
        target.fill(0xffU);
        return target;
    }

    const std::size_t index = 32U - exponent;
    target[index + 0U] = static_cast<std::uint8_t>((mantissa >> 16U) & 0xffU);
    target[index + 1U] = static_cast<std::uint8_t>((mantissa >> 8U) & 0xffU);
    target[index + 2U] = static_cast<std::uint8_t>(mantissa & 0xffU);
    return target;
}

[[nodiscard]] std::vector<std::uint8_t> trim_leading_zero_bytes(std::vector<std::uint8_t> bytes) {
    const auto first_non_zero = std::find_if(bytes.begin(), bytes.end(), [](std::uint8_t value) {
        return value != 0U;
    });
    if (first_non_zero == bytes.end()) {
        return std::vector<std::uint8_t>{0U};
    }
    bytes.erase(bytes.begin(), first_non_zero);
    return bytes;
}

void shift_left_one_bit(std::vector<std::uint8_t>& bytes) {
    std::uint8_t carry = 0U;
    for (std::size_t index = bytes.size(); index > 0U; --index) {
        const std::uint8_t next_carry = static_cast<std::uint8_t>((bytes[index - 1U] >> 7U) & 0x01U);
        bytes[index - 1U] = static_cast<std::uint8_t>((bytes[index - 1U] << 1U) | carry);
        carry = next_carry;
    }
    if (carry != 0U) {
        bytes.insert(bytes.begin(), carry);
    }
}

void shift_right_one_bit(std::vector<std::uint8_t>& bytes) {
    std::uint8_t carry = 0U;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const std::uint8_t next_carry = static_cast<std::uint8_t>(bytes[index] & 0x01U);
        bytes[index] = static_cast<std::uint8_t>((bytes[index] >> 1U) | (carry << 7U));
        carry = next_carry;
    }
    bytes = trim_leading_zero_bytes(std::move(bytes));
}

[[nodiscard]] std::vector<std::uint8_t> divide_bytes_by_u64(
    const std::vector<std::uint8_t>& numerator,
    std::uint64_t divisor
) {
    std::vector<std::uint8_t> quotient(numerator.size(), 0U);
    std::uint64_t remainder = 0U;
    for (std::size_t index = 0; index < numerator.size(); ++index) {
        const std::uint64_t value = (remainder << 8U) | static_cast<std::uint64_t>(numerator[index]);
        quotient[index] = static_cast<std::uint8_t>(value / divisor);
        remainder = value % divisor;
    }
    return trim_leading_zero_bytes(std::move(quotient));
}

[[nodiscard]] std::array<std::uint8_t, 32> difficulty_to_target_bytes(double difficulty) {
    std::array<std::uint8_t, 32> target {};
    if (!std::isfinite(difficulty) || difficulty <= 0.0) {
        target.fill(0xffU);
        return target;
    }

    int exponent = 0;
    const double normalized = std::frexp(difficulty, &exponent);
    const std::uint64_t mantissa = static_cast<std::uint64_t>(std::llround(std::ldexp(normalized, 53)));
    if (mantissa == 0U) {
        target.fill(0xffU);
        return target;
    }

    std::vector<std::uint8_t> numerator(kDifficulty1Target.begin(), kDifficulty1Target.end());
    const int shift = 53 - exponent;
    if (shift > 0) {
        for (int bit = 0; bit < shift; ++bit) {
            shift_left_one_bit(numerator);
        }
    } else if (shift < 0) {
        for (int bit = 0; bit < -shift; ++bit) {
            shift_right_one_bit(numerator);
        }
    }

    const std::vector<std::uint8_t> quotient = divide_bytes_by_u64(numerator, mantissa);
    if (quotient.size() > target.size()) {
        target.fill(0xffU);
        return target;
    }

    const std::size_t offset = target.size() - quotient.size();
    std::copy(quotient.begin(), quotient.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
    return target;
}

[[nodiscard]] double wrap_turns(double turns) {
    double wrapped = std::fmod(turns, 1.0);
    if (wrapped < 0.0) {
        wrapped += 1.0;
    }
    return wrapped;
}

[[nodiscard]] double phase_delta_turns(double lhs, double rhs) {
    double delta = wrap_turns(lhs) - wrap_turns(rhs);
    if (delta > 0.5) {
        delta -= 1.0;
    } else if (delta < -0.5) {
        delta += 1.0;
    }
    return delta;
}

[[nodiscard]] double mean_unit(std::initializer_list<double> values) {
    if (values.size() == 0U) {
        return 0.0;
    }

    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return clamp_unit(total / static_cast<double>(values.size()));
}

[[nodiscard]] double phase_peak_proximity(double phase_turns) {
    const double positive_peak_distance = std::abs(phase_delta_turns(phase_turns, 0.25));
    const double negative_peak_distance = std::abs(phase_delta_turns(phase_turns, 0.75));
    return clamp_unit(1.0 - (std::min(positive_peak_distance, negative_peak_distance) / 0.25));
}

[[nodiscard]] double bytes_to_phase_turns(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t count
) {
    if (bytes.empty() || count == 0U) {
        return 0.0;
    }

    std::uint64_t accumulator = 0U;
    const std::size_t limit = std::min(bytes.size(), offset + count);
    for (std::size_t index = offset; index < limit; ++index) {
        accumulator = (accumulator << 8U) ^ static_cast<std::uint64_t>(bytes[index]);
    }
    const double normalized = static_cast<double>(accumulator & 0xffffffffULL) / 4294967296.0;
    return wrap_turns(normalized);
}

[[nodiscard]] double word_to_phase_turns(std::uint32_t word) {
    return wrap_turns(static_cast<double>(word) / 4294967296.0);
}

struct Sha256FrequencySurface {
    double schedule_phase_turns = 0.0;
    double round_phase_turns = 0.0;
    double digest_phase_turns = 0.0;
    double resonance_norm = 0.0;
    double harmonic_density_norm = 0.0;
    std::uint32_t seed = 0U;
};

struct PowHashFrequencyProfile {
    double schedule_phase_turns = 0.0;
    double round_phase_turns = 0.0;
    double digest_phase_turns = 0.0;
    double frequency_bias_norm = 0.0;
    double harmonic_density_norm = 0.0;
    std::uint32_t seed = 0U;
};

[[nodiscard]] std::vector<std::uint8_t> sha256_pad_message(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> message = input;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8ULL;

    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0x00U);
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffULL));
    }
    return message;
}

[[nodiscard]] Sha256FrequencySurface analyze_sha256_frequency_surface(const std::vector<std::uint8_t>& input) {
    const std::vector<std::uint8_t> message = sha256_pad_message(input);
    std::array<std::uint32_t, 8> state = kSha256InitialState;
    std::array<std::uint32_t, 64> schedule {};

    double schedule_phase_accumulator = 0.0;
    double round_phase_accumulator = 0.0;
    double resonance_accumulator = 0.0;
    double harmonic_accumulator = 0.0;
    std::size_t round_count = 0U;
    std::uint32_t seed = 0U;

    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t base = offset + (index * 4U);
            schedule[index] = (static_cast<std::uint32_t>(message[base]) << 24U)
                | (static_cast<std::uint32_t>(message[base + 1U]) << 16U)
                | (static_cast<std::uint32_t>(message[base + 2U]) << 8U)
                | static_cast<std::uint32_t>(message[base + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            schedule[index] = small_sigma1(schedule[index - 2U]) + schedule[index - 7U]
                + small_sigma0(schedule[index - 15U]) + schedule[index - 16U];
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];

        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const std::uint32_t temp1 =
                h + big_sigma1(e) + choose(e, f, g) + kSha256RoundConstants[index] + schedule[index];
            const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
            const double schedule_turns =
                word_to_phase_turns(schedule[index] ^ rotate_left(kSha256RoundConstants[index], static_cast<std::uint32_t>(index & 31U)));
            const double round_turns =
                word_to_phase_turns(temp1 ^ rotate_left(temp2, static_cast<std::uint32_t>((index % 17U) + 1U)));
            const double constant_turns = word_to_phase_turns(kSha256RoundConstants[index]);
            const double round_alignment = 1.0 - clamp_unit(
                std::abs(phase_delta_turns(schedule_turns, constant_turns)) * 2.0);
            const double state_turns = word_to_phase_turns(a ^ e ^ schedule[index]);

            schedule_phase_accumulator += schedule_turns;
            round_phase_accumulator += round_turns;
            resonance_accumulator += mean_unit({
                round_alignment,
                1.0 - clamp_unit(std::abs(phase_delta_turns(round_turns, state_turns)) * 2.0),
                phase_peak_proximity(schedule_turns),
                phase_peak_proximity(round_turns),
            });
            harmonic_accumulator += mean_unit({
                phase_peak_proximity(schedule_turns),
                phase_peak_proximity(round_turns),
                phase_peak_proximity(state_turns),
            });
            seed ^= rotate_left(
                schedule[index] ^ temp1 ^ temp2 ^ kSha256RoundConstants[index],
                static_cast<std::uint32_t>((index % 29U) + 1U));
            ++round_count;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;

        seed ^= rotate_left(state[0] ^ state[3] ^ state[5] ^ state[7], 7U);
    }

    Sha256FrequencySurface surface;
    if (round_count == 0U) {
        return surface;
    }

    double digest_phase_accumulator = 0.0;
    for (std::uint32_t word : state) {
        digest_phase_accumulator += word_to_phase_turns(word);
        seed ^= rotate_left(word, 3U);
    }

    surface.schedule_phase_turns =
        wrap_turns(schedule_phase_accumulator / static_cast<double>(round_count));
    surface.round_phase_turns =
        wrap_turns(round_phase_accumulator / static_cast<double>(round_count));
    surface.digest_phase_turns =
        wrap_turns(digest_phase_accumulator / static_cast<double>(state.size()));
    surface.resonance_norm = clamp_unit(resonance_accumulator / static_cast<double>(round_count));
    surface.harmonic_density_norm = clamp_unit(harmonic_accumulator / static_cast<double>(round_count));
    surface.seed = seed;
    return surface;
}

[[nodiscard]] PowHashFrequencyProfile build_pow_hash_frequency_profile(
    const std::vector<std::uint8_t>& header,
    const std::array<std::uint8_t, 32>& share_target
) {
    const Sha256FrequencySurface header_surface = analyze_sha256_frequency_surface(header);
    const std::vector<std::uint8_t> header_digest = double_sha256(header);
    const Sha256FrequencySurface digest_surface = analyze_sha256_frequency_surface(header_digest);
    const Sha256FrequencySurface target_surface = analyze_sha256_frequency_surface(
        std::vector<std::uint8_t>(share_target.begin(), share_target.end()));

    const double header_target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(header_surface.digest_phase_turns, target_surface.digest_phase_turns)) * 2.0);
    const double digest_target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(digest_surface.digest_phase_turns, target_surface.digest_phase_turns)) * 2.0);
    const double schedule_round_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(header_surface.schedule_phase_turns, digest_surface.round_phase_turns)) * 2.0);

    PowHashFrequencyProfile profile;
    profile.schedule_phase_turns = wrap_turns(
        (0.50 * header_surface.schedule_phase_turns)
        + (0.25 * digest_surface.schedule_phase_turns)
        + (0.25 * target_surface.schedule_phase_turns));
    profile.round_phase_turns = wrap_turns(
        (0.50 * header_surface.round_phase_turns)
        + (0.25 * digest_surface.round_phase_turns)
        + (0.25 * target_surface.round_phase_turns));
    profile.digest_phase_turns = wrap_turns(
        (0.50 * digest_surface.digest_phase_turns)
        + (0.30 * header_surface.digest_phase_turns)
        + (0.20 * target_surface.digest_phase_turns));
    profile.frequency_bias_norm = mean_unit({
        header_surface.resonance_norm,
        digest_surface.resonance_norm,
        target_surface.resonance_norm,
        header_target_alignment,
        digest_target_alignment,
        schedule_round_alignment,
    });
    profile.harmonic_density_norm = mean_unit({
        header_surface.harmonic_density_norm,
        digest_surface.harmonic_density_norm,
        target_surface.harmonic_density_norm,
        phase_peak_proximity(profile.schedule_phase_turns),
        phase_peak_proximity(profile.round_phase_turns),
    });
    profile.seed =
        header_surface.seed ^ rotate_left(digest_surface.seed, 7U) ^ rotate_left(target_surface.seed, 13U);
    return profile;
}

struct PhaseEncodedSearchParameters {
    double header_phase_turns = 0.0;
    double target_phase_turns = 0.0;
    double pressure_turns = 0.0;
    double sha256_schedule_phase_turns = 0.0;
    double sha256_round_phase_turns = 0.0;
    double sha256_digest_phase_turns = 0.0;
    double sha256_frequency_bias_norm = 0.0;
    double sha256_harmonic_density_norm = 0.0;
    std::size_t lane_count = 4U;
    std::size_t window = 1U;
    std::uint32_t seed = 0U;
    std::uint32_t stride = 1U;
};

[[nodiscard]] PhaseEncodedSearchParameters build_phase_encoded_search_parameters(
    const std::vector<std::uint8_t>& header,
    const std::array<std::uint8_t, 32>& share_target,
    std::size_t max_attempts
) {
    PhaseEncodedSearchParameters parameters;
    const PowHashFrequencyProfile hash_profile = build_pow_hash_frequency_profile(header, share_target);
    parameters.header_phase_turns = bytes_to_phase_turns(header, 0U, 16U);
    parameters.target_phase_turns = bytes_to_phase_turns(
        std::vector<std::uint8_t>(share_target.begin(), share_target.end()),
        0U,
        16U
    );
    parameters.sha256_schedule_phase_turns = hash_profile.schedule_phase_turns;
    parameters.sha256_round_phase_turns = hash_profile.round_phase_turns;
    parameters.sha256_digest_phase_turns = hash_profile.digest_phase_turns;
    parameters.sha256_frequency_bias_norm = hash_profile.frequency_bias_norm;
    parameters.sha256_harmonic_density_norm = hash_profile.harmonic_density_norm;
    parameters.pressure_turns = wrap_turns(
        parameters.target_phase_turns
        + (0.5 * parameters.header_phase_turns)
        + (0.25 * parameters.sha256_schedule_phase_turns)
        + (0.125 * parameters.sha256_round_phase_turns)
        + (0.0625 * parameters.sha256_digest_phase_turns)
    );
    parameters.window = std::max<std::size_t>(1U, max_attempts);
    parameters.lane_count = std::clamp<std::size_t>(
        4U + static_cast<std::size_t>(std::llround(mean_unit({
            parameters.pressure_turns,
            parameters.sha256_frequency_bias_norm,
            parameters.sha256_harmonic_density_norm,
        }) * 8.0)),
        4U,
        12U
    );

    const std::uint32_t header_seed = static_cast<std::uint32_t>(
        std::llround(parameters.header_phase_turns * static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    const std::uint32_t target_seed = static_cast<std::uint32_t>(
        std::llround(parameters.target_phase_turns * static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    parameters.seed =
        header_seed ^ rotate_left(target_seed, 7U) ^ rotate_left(hash_profile.seed, 11U);

    std::uint32_t stride = static_cast<std::uint32_t>(
        1U + ((parameters.seed
            ^ rotate_left(target_seed, 13U)
            ^ rotate_left(hash_profile.seed, 5U))
            % static_cast<std::uint32_t>(parameters.window)));
    if ((stride % 2U) == 0U) {
        ++stride;
    }
    if (stride == 0U) {
        stride = 1U;
    }
    parameters.stride = stride;
    return parameters;
}

[[nodiscard]] SubstrateStratumPhaseFluxMeasurement effective_phase_flux_measurement(
    const PhaseEncodedSearchParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement
) {
    SubstrateStratumPhaseFluxMeasurement effective = measurement;
    const double header_target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(parameters.header_phase_turns, parameters.target_phase_turns)) * 2.0
    );

    if (effective.carrier_phase_turns == 0.0) {
        effective.carrier_phase_turns = parameters.header_phase_turns;
    }
    if (effective.target_phase_turns == 0.0) {
        effective.target_phase_turns = parameters.target_phase_turns;
    }
    if (effective.sha256_schedule_phase_turns == 0.0) {
        effective.sha256_schedule_phase_turns = parameters.sha256_schedule_phase_turns;
    }
    if (effective.sha256_round_phase_turns == 0.0) {
        effective.sha256_round_phase_turns = parameters.sha256_round_phase_turns;
    }
    if (effective.sha256_digest_phase_turns == 0.0) {
        effective.sha256_digest_phase_turns = parameters.sha256_digest_phase_turns;
    }
    if (effective.sha256_frequency_bias_norm == 0.0) {
        effective.sha256_frequency_bias_norm = parameters.sha256_frequency_bias_norm;
    }
    if (effective.sha256_harmonic_density_norm == 0.0) {
        effective.sha256_harmonic_density_norm = parameters.sha256_harmonic_density_norm;
    }
    effective.search_epoch_turns = wrap_turns(effective.search_epoch_turns);
    if (effective.phase_pressure == 0.0) {
        effective.phase_pressure = mean_unit({
            parameters.pressure_turns,
            header_target_alignment,
            effective.sha256_frequency_bias_norm,
            effective.sha256_harmonic_density_norm,
        });
    }
    if (effective.flux_transport_norm == 0.0) {
        effective.flux_transport_norm = mean_unit({
            parameters.header_phase_turns,
            parameters.target_phase_turns,
            parameters.pressure_turns,
            effective.sha256_frequency_bias_norm,
            effective.sha256_harmonic_density_norm,
        });
    }
    if (effective.observer_factor == 0.0) {
        effective.observer_factor = mean_unit({
            effective.phase_pressure,
            header_target_alignment,
            effective.flux_transport_norm,
            effective.sha256_frequency_bias_norm,
        });
    }
    if (effective.zero_point_proximity == 0.0) {
        effective.zero_point_proximity = mean_unit({
            header_target_alignment,
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                effective.sha256_digest_phase_turns,
                parameters.target_phase_turns
            )) * 2.0),
        });
    }
    if (effective.temporal_admissibility == 0.0) {
        effective.temporal_admissibility = mean_unit({
            1.0 - std::abs(phase_delta_turns(parameters.pressure_turns, parameters.target_phase_turns)),
            header_target_alignment,
            effective.zero_point_proximity,
            effective.sha256_harmonic_density_norm,
        });
    }
    if (effective.trajectory_conservation == 0.0) {
        effective.trajectory_conservation = mean_unit({
            header_target_alignment,
            effective.phase_pressure,
            effective.flux_transport_norm,
            effective.sha256_frequency_bias_norm,
        });
    }
    if (effective.rf_phase_position_turns == 0.0) {
        effective.rf_phase_position_turns = effective.carrier_phase_turns;
    }
    if (effective.rf_phase_velocity_turns == 0.0) {
        effective.rf_phase_velocity_turns =
            phase_delta_turns(effective.target_phase_turns, effective.rf_phase_position_turns);
    }
    if (effective.rf_zero_point_displacement_turns == 0.0) {
        effective.rf_zero_point_displacement_turns =
            phase_delta_turns(effective.rf_phase_position_turns, 0.0);
    }
    if (effective.rf_zero_point_distance_norm == 0.0) {
        effective.rf_zero_point_distance_norm =
            clamp_unit(std::abs(effective.rf_zero_point_displacement_turns) * 4.0);
    }
    if (effective.rf_carrier_frequency_norm == 0.0) {
        effective.rf_carrier_frequency_norm = mean_unit({
            parameters.pressure_turns,
            effective.flux_transport_norm,
            1.0 - effective.rf_zero_point_distance_norm,
            effective.sha256_frequency_bias_norm,
            effective.sha256_harmonic_density_norm,
        });
    }
    if (effective.rf_envelope_amplitude_norm == 0.0) {
        effective.rf_envelope_amplitude_norm = mean_unit({
            header_target_alignment,
            effective.zero_point_proximity,
            1.0 - effective.rf_zero_point_distance_norm,
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                effective.sha256_digest_phase_turns,
                effective.target_phase_turns
            )) * 2.0),
        });
    }
    if (effective.rf_spin_drive_signed == 0.0) {
        effective.rf_spin_drive_signed = clamp_signed(std::sin(kTwoPi * (
            effective.carrier_phase_turns
            + effective.flux_transport_norm
            + effective.zero_point_proximity
        )));
    }
    if (effective.rf_rotation_orientation_signed == 0.0) {
        effective.rf_rotation_orientation_signed = clamp_signed(std::cos(kTwoPi * (
            effective.target_phase_turns
            - effective.carrier_phase_turns
            + effective.temporal_admissibility
        )));
    }
    if (effective.rf_temporal_coupling_norm == 0.0) {
        effective.rf_temporal_coupling_norm = mean_unit({
            effective.temporal_admissibility,
            effective.phase_pressure,
            header_target_alignment,
        });
    }
    if (effective.rf_resonance_hold_norm == 0.0) {
        effective.rf_resonance_hold_norm = mean_unit({
            effective.trajectory_conservation,
            1.0 - effective.sideband_energy_norm,
            1.0 - effective.anchor_evm_norm,
            1.0 - effective.phase_lock_error,
        });
    }
    if (effective.rf_sideband_energy_norm == 0.0) {
        effective.rf_sideband_energy_norm = mean_unit({
            effective.sideband_energy_norm,
            effective.interference_projection,
            1.0 - effective.rf_resonance_hold_norm,
        });
    }

    const double zero_point_crossover_norm = mean_unit({
        effective.zero_point_proximity,
        1.0 - effective.rf_zero_point_distance_norm,
        phase_peak_proximity(effective.rf_phase_position_turns),
    });
    if (effective.rf_energy_transfer_norm == 0.0) {
        effective.rf_energy_transfer_norm = mean_unit({
            zero_point_crossover_norm,
            std::abs(effective.rf_spin_drive_signed),
            effective.rf_temporal_coupling_norm,
            1.0 - effective.rf_zero_point_distance_norm,
        });
    }
    if (effective.rf_particle_stability_norm == 0.0) {
        effective.rf_particle_stability_norm = mean_unit({
            effective.rf_resonance_hold_norm,
            effective.trajectory_conservation,
            1.0 - effective.rf_sideband_energy_norm,
        });
    }
    if (effective.spin_alignment_norm == 0.0) {
        effective.spin_alignment_norm = mean_unit({
            std::abs(effective.rf_spin_drive_signed),
            std::abs(effective.rf_rotation_orientation_signed),
            1.0 - effective.interference_projection,
        });
    }

    const double direct_transfer_drive = clamp_unit(
        zero_point_crossover_norm
        * (1.0 - effective.rf_zero_point_distance_norm)
        * effective.rf_temporal_coupling_norm
        * std::abs(effective.rf_spin_drive_signed)
        * 6.0);
    if (effective.transfer_drive_norm == 0.0) {
        effective.transfer_drive_norm = clamp_unit(
            (0.50 * direct_transfer_drive)
            + (0.30 * effective.rf_energy_transfer_norm)
            + (0.20 * std::max(
                phase_peak_proximity(effective.rf_phase_position_turns),
                effective.zero_point_proximity
            )));
    }
    if (effective.stability_gate_norm == 0.0) {
        effective.stability_gate_norm = clamp_unit(
            effective.trajectory_conservation
            * effective.rf_particle_stability_norm
            * effective.rf_resonance_hold_norm
            * (1.0 - effective.phase_lock_error));
    }
    if (effective.damping_norm == 0.0) {
        effective.damping_norm = clamp_unit(
            (0.38 * effective.rf_sideband_energy_norm)
            + (0.32 * effective.interference_projection)
            + (0.30 * effective.anchor_evm_norm));
    }
    if (effective.transport_drive_norm == 0.0) {
        effective.transport_drive_norm = clamp_unit(
            (0.36 * effective.phase_pressure)
            + (0.22 * effective.observer_factor)
            + (0.16 * effective.flux_transport_norm)
            + (0.14 * effective.rf_temporal_coupling_norm)
            + (0.12 * effective.stability_gate_norm)
            + (0.18 * effective.transfer_drive_norm)
            + (0.12 * effective.sha256_frequency_bias_norm)
            + (0.08 * effective.sha256_harmonic_density_norm)
            - (0.12 * effective.damping_norm));
    }
    if (effective.target_resonance_norm == 0.0) {
        effective.target_resonance_norm = mean_unit({
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                effective.carrier_phase_turns,
                effective.target_phase_turns
            )) * 2.0),
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                effective.rf_phase_position_turns,
                effective.target_phase_turns
            )) * 2.0),
            1.0 - clamp_unit(std::abs(phase_delta_turns(
                effective.sha256_digest_phase_turns,
                effective.target_phase_turns
            )) * 2.0),
            effective.zero_point_proximity,
            effective.transfer_drive_norm,
            effective.stability_gate_norm,
            effective.sha256_frequency_bias_norm,
            1.0 - effective.damping_norm,
        });
    }
    if (effective.resonance_activation_norm == 0.0) {
        effective.resonance_activation_norm = mean_unit({
            effective.target_resonance_norm,
            effective.transfer_drive_norm,
            effective.stability_gate_norm,
            effective.transport_drive_norm,
            effective.sha256_frequency_bias_norm,
            effective.sha256_harmonic_density_norm,
            effective.temporal_admissibility,
            1.0 - effective.damping_norm,
        });
    }

    return effective;
}

[[nodiscard]] std::uint32_t phase_encoded_offset(
    const PhaseEncodedSearchParameters& parameters,
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    std::size_t attempt_index
) {
    const std::size_t lane = attempt_index % parameters.lane_count;
    const std::size_t orbit = attempt_index / parameters.lane_count;
    const double carrier_phase_turns = measurement.carrier_phase_turns == 0.0
        ? parameters.header_phase_turns
        : measurement.carrier_phase_turns;
    const double target_phase_turns = measurement.target_phase_turns == 0.0
        ? parameters.target_phase_turns
        : measurement.target_phase_turns;
    const double exploration_turns = wrap_turns(
        measurement.search_epoch_turns
        + (static_cast<double>(attempt_index & 0xffffU) / 65536.0));
    const double pressure_turns = wrap_turns(
        parameters.pressure_turns
        + (0.25 * clamp_unit(measurement.phase_pressure))
        + (0.125 * clamp_unit(measurement.observer_factor))
        + (0.0625 * clamp_unit(measurement.temporal_admissibility))
        + (0.0625 * clamp_unit(measurement.transfer_drive_norm))
        + (0.03125 * clamp_unit(measurement.transport_drive_norm))
        + (0.125 * clamp_unit(measurement.target_resonance_norm))
        + (0.09375 * clamp_unit(measurement.resonance_activation_norm))
        + (0.125 * clamp_unit(measurement.sha256_frequency_bias_norm))
        + (0.0625 * clamp_unit(measurement.sha256_harmonic_density_norm))
        + (0.0625 * exploration_turns)
        - (0.03125 * clamp_unit(measurement.damping_norm))
    );
    const double conservation_turns = wrap_turns(
        clamp_unit(measurement.trajectory_conservation)
        + clamp_unit(measurement.zero_point_proximity)
        + clamp_unit(measurement.observer_factor)
        + clamp_unit(measurement.stability_gate_norm)
        + clamp_unit(measurement.target_resonance_norm)
        + clamp_unit(measurement.resonance_activation_norm)
        + clamp_unit(measurement.sha256_harmonic_density_norm)
        + exploration_turns
    );
    const double rf_orbit_phase = wrap_turns(
        measurement.rf_phase_position_turns
        + measurement.rf_phase_velocity_turns
        + (0.25 * measurement.rf_zero_point_displacement_turns)
        + (0.125 * measurement.rf_rotation_orientation_signed * clamp_unit(measurement.spin_alignment_norm))
        + (0.125 * measurement.sha256_round_phase_turns)
        + (0.125 * exploration_turns)
    );
    const double hash_orbit_phase = wrap_turns(
        measurement.sha256_schedule_phase_turns
        + measurement.sha256_round_phase_turns
        + (0.5 * measurement.sha256_digest_phase_turns)
        + (0.125 * exploration_turns)
    );
    const double lane_phase = wrap_turns(
        carrier_phase_turns
        + (static_cast<double>(lane) / static_cast<double>(parameters.lane_count))
        + (0.5 * target_phase_turns)
        + (0.125 * exploration_turns)
        + (0.25 * std::sin(kTwoPi * (pressure_turns + static_cast<double>(orbit) / static_cast<double>(parameters.window))))
        + (0.125 * std::cos(kTwoPi * (conservation_turns + static_cast<double>(lane) / static_cast<double>(parameters.lane_count))))
        + (0.125 * std::sin(kTwoPi * (
            measurement.target_resonance_norm
            + measurement.resonance_activation_norm
            + target_phase_turns
            + exploration_turns
        )))
        + (0.09375 * std::sin(kTwoPi * (
            rf_orbit_phase
            + measurement.transfer_drive_norm
            + exploration_turns
            + static_cast<double>(lane) / static_cast<double>(parameters.lane_count))))
        + (0.0625 * std::cos(kTwoPi * (
            measurement.transport_drive_norm
            + measurement.rf_carrier_frequency_norm
            + exploration_turns
            + static_cast<double>(orbit) / static_cast<double>(parameters.window)
        )))
        + (0.09375 * std::sin(kTwoPi * (
            hash_orbit_phase
            + measurement.sha256_frequency_bias_norm
            + static_cast<double>(lane) / static_cast<double>(parameters.lane_count)
        )))
        + (0.0625 * std::cos(kTwoPi * (
            measurement.sha256_digest_phase_turns
            + measurement.sha256_harmonic_density_norm
            + static_cast<double>(orbit) / static_cast<double>(parameters.window)
        )))
        - (0.03125 * clamp_unit(measurement.damping_norm))
    );
    const std::uint32_t lane_bias = static_cast<std::uint32_t>(
        std::llround(
            wrap_turns(
                lane_phase
                + (0.0625 * measurement.rf_zero_point_displacement_turns * clamp_unit(measurement.transfer_drive_norm))
                + (0.0625 * phase_delta_turns(hash_orbit_phase, measurement.sha256_digest_phase_turns))
            ) * static_cast<double>(parameters.window - 1U)));
    const std::uint32_t observer_bias = parameters.window == 1U
        ? 0U
        : static_cast<std::uint32_t>(std::llround(
            wrap_turns(
                clamp_unit(measurement.observer_factor)
                + clamp_unit(measurement.temporal_admissibility)
                + clamp_unit(measurement.zero_point_proximity)
                + clamp_unit(measurement.stability_gate_norm)
                + clamp_unit(measurement.spin_alignment_norm)
                + clamp_unit(measurement.target_resonance_norm)
                + clamp_unit(measurement.resonance_activation_norm)
                + clamp_unit(measurement.sha256_frequency_bias_norm)
                + exploration_turns
            ) * static_cast<double>(parameters.window - 1U)));
    return static_cast<std::uint32_t>((
        static_cast<std::uint64_t>(parameters.seed)
        + static_cast<std::uint64_t>(lane_bias)
        + static_cast<std::uint64_t>(observer_bias)
        + (static_cast<std::uint64_t>(orbit) * static_cast<std::uint64_t>(parameters.stride))
    ) % static_cast<std::uint64_t>(parameters.window));
}

[[nodiscard]] bool hash_leq_target(
    const std::array<std::uint8_t, 32>& hash,
    const std::array<std::uint8_t, 32>& target
) {
    for (std::size_t index = 0; index < hash.size(); ++index) {
        if (hash[index] < target[index]) {
            return true;
        }
        if (hash[index] > target[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string format_nonce_hex(std::uint32_t nonce_value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(8) << nonce_value;
    return stream.str();
}

[[nodiscard]] bool collapse_feedback_is_better(
    const SubstrateStratumPowCollapseFeedback& candidate,
    const SubstrateStratumPowCollapseFeedback& incumbent,
    const SubstrateStratumPowEvaluation& candidate_evaluation,
    const SubstrateStratumPowEvaluation& incumbent_evaluation,
    std::uint32_t candidate_nonce,
    std::uint32_t incumbent_nonce
) {
    constexpr double kEpsilon = 1.0e-12;

    if (candidate.nonce_collapse_confidence > incumbent.nonce_collapse_confidence + kEpsilon) {
        return true;
    }
    if (candidate.nonce_collapse_confidence + kEpsilon < incumbent.nonce_collapse_confidence) {
        return false;
    }
    if (candidate.phase_flux_conservation > incumbent.phase_flux_conservation + kEpsilon) {
        return true;
    }
    if (candidate.phase_flux_conservation + kEpsilon < incumbent.phase_flux_conservation) {
        return false;
    }
    if (candidate.observer_collapse_strength > incumbent.observer_collapse_strength + kEpsilon) {
        return true;
    }
    if (candidate.observer_collapse_strength + kEpsilon < incumbent.observer_collapse_strength) {
        return false;
    }
    if (candidate_evaluation.hash_hex < incumbent_evaluation.hash_hex) {
        return true;
    }
    if (candidate_evaluation.hash_hex > incumbent_evaluation.hash_hex) {
        return false;
    }
    return candidate_nonce < incumbent_nonce;
}

}  // namespace

std::string stratum_bits_to_target_hex(const std::string& nbits_hex) {
    const auto target = bits_to_target_bytes(nbits_hex);
    return bytes_to_hex(target.data(), target.size());
}

std::string stratum_difficulty_to_target_hex(double difficulty) {
    const auto target = difficulty_to_target_bytes(difficulty);
    return bytes_to_hex(target.data(), target.size());
}

std::string build_stratum_header_hex(
    const std::string& prevhash_hex,
    const std::string& coinbase1_hex,
    const std::string& extranonce1_hex,
    std::uint32_t extranonce2_size,
    const std::string& coinbase2_hex,
    const std::vector<std::string>& merkle_branches_hex,
    const std::string& version_hex,
    const std::string& nbits_hex,
    const std::string& ntime_hex,
    const std::string& extranonce2_hex
) {
    const std::string normalized_extranonce2 = extranonce2_hex.empty()
        ? std::string(static_cast<std::size_t>(extranonce2_size) * 2U, '0')
        : normalize_hex(extranonce2_hex);
    const std::vector<std::uint8_t> coinbase = hex_to_bytes(
        coinbase1_hex + extranonce1_hex + normalized_extranonce2 + coinbase2_hex);
    std::vector<std::uint8_t> merkle_hash = double_sha256(coinbase);
    for (const auto& merkle_branch_hex : merkle_branches_hex) {
        const std::vector<std::uint8_t> merkle_branch = hex_to_bytes(merkle_branch_hex);
        std::vector<std::uint8_t> branch_concat;
        branch_concat.reserve(merkle_hash.size() + merkle_branch.size());
        branch_concat.insert(branch_concat.end(), merkle_hash.begin(), merkle_hash.end());
        branch_concat.insert(branch_concat.end(), merkle_branch.begin(), merkle_branch.end());
        merkle_hash = double_sha256(branch_concat);
    }

    return reverse_hex_bytes(version_hex)
        + reverse_hex_bytes(prevhash_hex)
        + reverse_hex_bytes(bytes_to_hex(merkle_hash.data(), merkle_hash.size()))
        + reverse_hex_bytes(ntime_hex)
        + reverse_hex_bytes(nbits_hex)
        + "00000000";
}

double stratum_nbits_to_difficulty(const std::string& nbits_hex) {
    const std::uint32_t compact = parse_compact_bits(nbits_hex);
    const std::uint32_t exponent = (compact >> 24U) & 0xffU;
    const std::uint32_t mantissa = compact & 0x00ffffffU;
    if (exponent == 0U || mantissa == 0U) {
        return 0.0;
    }

    long double difficulty = 65535.0L / static_cast<long double>(mantissa);
    int shift = 0x1d - static_cast<int>(exponent);
    while (shift > 0) {
        difficulty *= 256.0L;
        --shift;
    }
    while (shift < 0) {
        difficulty /= 256.0L;
        ++shift;
    }

    return std::max(0.0, static_cast<double>(difficulty));
}

double expected_hashes_for_difficulty(double difficulty) {
    if (!std::isfinite(difficulty) || difficulty <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(static_cast<long double>(difficulty) * kDifficulty1ExpectedHashes);
}

double bitcoin_network_hashrate_from_difficulty(double difficulty, double block_interval_seconds) {
    if (!std::isfinite(block_interval_seconds) || block_interval_seconds <= 0.0) {
        return 0.0;
    }
    return expected_hashes_for_difficulty(difficulty) / block_interval_seconds;
}

BitcoinWorkProjection build_bitcoin_work_projection(
    double share_difficulty,
    const std::string& network_nbits_hex,
    double target_network_share_fraction,
    double block_interval_seconds
) {
    BitcoinWorkProjection projection;
    projection.share_difficulty = std::max(0.0, share_difficulty);
    projection.expected_hashes_per_share = expected_hashes_for_difficulty(projection.share_difficulty);
    projection.network_difficulty = stratum_nbits_to_difficulty(network_nbits_hex);
    projection.network_hashrate_hs = bitcoin_network_hashrate_from_difficulty(
        projection.network_difficulty,
        block_interval_seconds);
    projection.target_network_share_fraction = std::clamp(target_network_share_fraction, 0.0, 1.0);
    projection.required_hashrate_hs = projection.network_hashrate_hs * projection.target_network_share_fraction;
    projection.required_share_submissions_per_s = projection.expected_hashes_per_share > 0.0
        ? (projection.required_hashrate_hs / projection.expected_hashes_per_share)
        : 0.0;
    return projection;
}

namespace {

struct PreparedPowEvaluationContext {
    std::array<std::uint8_t, 80> header_template {};
    std::string normalized_nbits_hex;
    std::array<std::uint8_t, 32> share_target {};
    std::array<std::uint8_t, 32> block_target {};
    std::string share_target_hex;
    std::string block_target_hex;
    double share_difficulty = 0.0;
    double expected_hashes_for_share = 0.0;
};

void write_nonce_to_header(std::array<std::uint8_t, 80>& header, std::uint32_t nonce_value) {
    header[76U] = static_cast<std::uint8_t>(nonce_value & 0xffU);
    header[77U] = static_cast<std::uint8_t>((nonce_value >> 8U) & 0xffU);
    header[78U] = static_cast<std::uint8_t>((nonce_value >> 16U) & 0xffU);
    header[79U] = static_cast<std::uint8_t>((nonce_value >> 24U) & 0xffU);
}

[[nodiscard]] PreparedPowEvaluationContext prepare_pow_evaluation_context(
    const std::string& header_hex,
    const std::string& nbits_hex,
    double share_difficulty
) {
    PreparedPowEvaluationContext context;
    const std::vector<std::uint8_t> header_bytes = hex_to_bytes(header_hex);
    const std::size_t header_copy_count = std::min(header_bytes.size(), context.header_template.size());
    std::copy_n(header_bytes.begin(), header_copy_count, context.header_template.begin());
    context.normalized_nbits_hex = normalize_hex(nbits_hex);
    context.share_difficulty = share_difficulty > 0.0
        ? share_difficulty
        : stratum_nbits_to_difficulty(context.normalized_nbits_hex);
    context.expected_hashes_for_share = expected_hashes_for_difficulty(context.share_difficulty);
    context.share_target = difficulty_to_target_bytes(context.share_difficulty);
    context.block_target = bits_to_target_bytes(context.normalized_nbits_hex);
    context.share_target_hex = bytes_to_hex(context.share_target.data(), context.share_target.size());
    context.block_target_hex = bytes_to_hex(context.block_target.data(), context.block_target.size());
    return context;
}

[[nodiscard]] std::string build_header_hex_for_nonce(
    const PreparedPowEvaluationContext& context,
    std::uint32_t nonce_value
) {
    std::array<std::uint8_t, 80> header = context.header_template;
    write_nonce_to_header(header, nonce_value);
    return bytes_to_hex(header.data(), header.size());
}

[[nodiscard]] SubstrateStratumPowEvaluation evaluate_prepared_stratum_pow(
    const PreparedPowEvaluationContext& context,
    std::uint32_t nonce_value,
    bool include_header_hex = false
) {
    std::array<std::uint8_t, 80> header = context.header_template;
    write_nonce_to_header(header, nonce_value);

    const auto first_hash = sha256(std::vector<std::uint8_t>(header.begin(), header.end()));
    const std::vector<std::uint8_t> first_hash_vector(first_hash.begin(), first_hash.end());
    const auto hash = sha256(first_hash_vector);
    auto display_hash = hash;
    std::reverse(display_hash.begin(), display_hash.end());

    SubstrateStratumPowEvaluation evaluation;
    if (include_header_hex) {
        evaluation.header_hex = bytes_to_hex(header.data(), header.size());
    }
    evaluation.hash_hex = bytes_to_hex(display_hash.data(), display_hash.size());
    evaluation.target_hex = context.share_target_hex;
    evaluation.share_target_hex = context.share_target_hex;
    evaluation.block_target_hex = context.block_target_hex;
    evaluation.nbits_hex = context.normalized_nbits_hex;
    evaluation.nonce_hex = format_nonce_hex(nonce_value);
    evaluation.share_difficulty = context.share_difficulty;
    evaluation.expected_hashes_for_share = context.expected_hashes_for_share;
    evaluation.valid_share = hash_leq_target(display_hash, context.share_target);
    evaluation.valid_block = hash_leq_target(display_hash, context.block_target);
    return evaluation;
}

}  // namespace

bool try_parse_stratum_nonce_hex(const std::string& nonce_hex, std::uint32_t& nonce_value) {
    return try_parse_nonce_hex_impl(nonce_hex, nonce_value);
}

SubstrateStratumPowEvaluation evaluate_stratum_pow(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_value,
    double share_difficulty
) {
    const PreparedPowEvaluationContext context =
        prepare_pow_evaluation_context(header_hex, nbits_hex, share_difficulty);
    return evaluate_prepared_stratum_pow(context, nonce_value, true);
}

SubstrateStratumPhaseFluxMeasurement bias_phase_flux_measurement_with_sha256_frequency(
    const std::string& header_hex,
    const std::string& nbits_hex,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& measurement
) {
    const PreparedPowEvaluationContext context =
        prepare_pow_evaluation_context(header_hex, nbits_hex, share_difficulty);
    const std::vector<std::uint8_t> header(
        context.header_template.begin(),
        context.header_template.end());
    const PowHashFrequencyProfile hash_profile =
        build_pow_hash_frequency_profile(header, context.share_target);

    SubstrateStratumPhaseFluxMeasurement biased = measurement;
    biased.sha256_schedule_phase_turns = biased.sha256_schedule_phase_turns == 0.0
        ? hash_profile.schedule_phase_turns
        : wrap_turns((0.75 * biased.sha256_schedule_phase_turns) + (0.25 * hash_profile.schedule_phase_turns));
    biased.sha256_round_phase_turns = biased.sha256_round_phase_turns == 0.0
        ? hash_profile.round_phase_turns
        : wrap_turns((0.75 * biased.sha256_round_phase_turns) + (0.25 * hash_profile.round_phase_turns));
    biased.sha256_digest_phase_turns = biased.sha256_digest_phase_turns == 0.0
        ? hash_profile.digest_phase_turns
        : wrap_turns((0.70 * biased.sha256_digest_phase_turns) + (0.30 * hash_profile.digest_phase_turns));
    biased.sha256_frequency_bias_norm = mean_unit({
        biased.sha256_frequency_bias_norm,
        hash_profile.frequency_bias_norm,
    });
    biased.sha256_harmonic_density_norm = mean_unit({
        biased.sha256_harmonic_density_norm,
        hash_profile.harmonic_density_norm,
    });

    const double digest_target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(biased.sha256_digest_phase_turns, bytes_to_phase_turns(
            std::vector<std::uint8_t>(context.share_target.begin(), context.share_target.end()),
            0U,
            16U
        ))) * 2.0);
    biased.search_epoch_turns = wrap_turns(
        biased.search_epoch_turns
        + (0.125 * biased.sha256_round_phase_turns)
        + (0.0625 * biased.sha256_schedule_phase_turns)
    );
    biased.phase_pressure = mean_unit({
        biased.phase_pressure,
        biased.sha256_frequency_bias_norm,
        biased.sha256_harmonic_density_norm,
        digest_target_alignment,
    });
    biased.flux_transport_norm = mean_unit({
        biased.flux_transport_norm,
        biased.sha256_frequency_bias_norm,
        biased.sha256_harmonic_density_norm,
    });
    biased.observer_factor = mean_unit({
        biased.observer_factor,
        biased.sha256_frequency_bias_norm,
        digest_target_alignment,
    });
    biased.zero_point_proximity = mean_unit({
        biased.zero_point_proximity,
        digest_target_alignment,
        phase_peak_proximity(biased.sha256_digest_phase_turns),
    });
    biased.temporal_admissibility = mean_unit({
        biased.temporal_admissibility,
        biased.sha256_harmonic_density_norm,
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            biased.sha256_schedule_phase_turns,
            biased.sha256_round_phase_turns
        )) * 2.0),
    });
    biased.trajectory_conservation = mean_unit({
        biased.trajectory_conservation,
        biased.sha256_frequency_bias_norm,
        biased.sha256_harmonic_density_norm,
    });
    biased.rf_carrier_frequency_norm = mean_unit({
        biased.rf_carrier_frequency_norm,
        biased.sha256_frequency_bias_norm,
        biased.sha256_harmonic_density_norm,
    });
    biased.rf_envelope_amplitude_norm = mean_unit({
        biased.rf_envelope_amplitude_norm,
        digest_target_alignment,
        phase_peak_proximity(biased.sha256_digest_phase_turns),
    });
    biased.transfer_drive_norm = mean_unit({
        biased.transfer_drive_norm,
        biased.sha256_frequency_bias_norm,
        digest_target_alignment,
    });
    biased.stability_gate_norm = mean_unit({
        biased.stability_gate_norm,
        biased.sha256_harmonic_density_norm,
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            biased.sha256_digest_phase_turns,
            biased.sha256_round_phase_turns
        )) * 2.0),
    });
    biased.transport_drive_norm = clamp_unit(
        (0.74 * biased.transport_drive_norm)
        + (0.16 * biased.sha256_frequency_bias_norm)
        + (0.10 * biased.sha256_harmonic_density_norm));
    biased.target_resonance_norm = mean_unit({
        biased.target_resonance_norm,
        digest_target_alignment,
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            biased.carrier_phase_turns,
            biased.target_phase_turns
        )) * 2.0),
        1.0 - clamp_unit(std::abs(phase_delta_turns(
            biased.rf_phase_position_turns,
            biased.target_phase_turns
        )) * 2.0),
        biased.transfer_drive_norm,
        biased.stability_gate_norm,
        biased.sha256_frequency_bias_norm,
        1.0 - biased.damping_norm,
    });
    biased.resonance_activation_norm = mean_unit({
        biased.resonance_activation_norm,
        biased.target_resonance_norm,
        biased.transfer_drive_norm,
        biased.stability_gate_norm,
        biased.transport_drive_norm,
        biased.sha256_frequency_bias_norm,
        biased.sha256_harmonic_density_norm,
        1.0 - biased.damping_norm,
    });
    return biased;
}

SubstrateStratumPowCollapseFeedback measure_stratum_pow_collapse(
    const SubstrateStratumPowEvaluation& evaluation,
    const SubstrateStratumPhaseFluxMeasurement& measurement
) {
    SubstrateStratumPowCollapseFeedback feedback;

    const std::vector<std::uint8_t> hash_bytes = hex_to_bytes(evaluation.hash_hex);
    const std::vector<std::uint8_t> nonce_bytes = hex_to_bytes(evaluation.nonce_hex);
    feedback.measured_hash_phase_turns = bytes_to_phase_turns(hash_bytes, 0U, 16U);
    feedback.measured_nonce_phase_turns = bytes_to_phase_turns(nonce_bytes, 0U, 4U);

    const double target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_hash_phase_turns, measurement.target_phase_turns)) * 2.0
    );
    const double carrier_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(feedback.measured_nonce_phase_turns, measurement.carrier_phase_turns)) * 2.0
    );
    const double rf_phase_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_nonce_phase_turns,
            wrap_turns(measurement.rf_phase_position_turns + measurement.rf_phase_velocity_turns)
        )) * 2.0
    );
    const double sha256_digest_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_hash_phase_turns,
            measurement.sha256_digest_phase_turns
        )) * 2.0
    );
    const double sha256_round_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(
            feedback.measured_nonce_phase_turns,
            measurement.sha256_round_phase_turns
        )) * 2.0
    );
    const double phase_difference_turns =
        phase_delta_turns(feedback.measured_hash_phase_turns, measurement.target_phase_turns);
    const double base_intensity = clamp_unit(std::pow(std::cos(kPi * phase_difference_turns), 2.0));
    const double modulation_alpha = 0.01 + (0.04 * clamp_unit(measurement.phase_pressure));
    const double modulation_beta = kTwoPi * (0.5 + clamp_unit(measurement.flux_transport_norm));
    const double modulation_gamma = kTwoPi * (0.5 + clamp_unit(measurement.temporal_admissibility));
    const double dmt_modulation = 1.0 + (
        modulation_alpha * std::sin(
            (modulation_beta * feedback.measured_nonce_phase_turns)
            + (modulation_gamma * clamp_unit(measurement.phase_pressure))
        )
    );
    const double dmt_intensity = clamp_unit(base_intensity * dmt_modulation);
    const double measurement_probability = mean_unit({
        clamp_unit(measurement.observer_factor),
        clamp_unit(measurement.temporal_admissibility),
        clamp_unit(measurement.zero_point_proximity),
        clamp_unit(measurement.target_resonance_norm),
        clamp_unit(measurement.resonance_activation_norm),
        1.0 - clamp_unit(measurement.interference_projection),
    });
    const double observer_damping = 0.01 + (0.01 * clamp_unit(measurement.observer_factor));
    const double damped_intensity = clamp_unit(
        dmt_intensity * (1.0 - (observer_damping * measurement_probability))
    );
    const double decoherence_sigma = 0.08 + (0.24 * (1.0 - clamp_unit(measurement.temporal_admissibility)));
    const double decoherence_smoothing = std::exp(
        -((phase_difference_turns * phase_difference_turns) / (2.0 * decoherence_sigma * decoherence_sigma))
    );
    const double smoothed_intensity = clamp_unit(damped_intensity * decoherence_smoothing);

    feedback.feedback_phase_turns = wrap_turns(
        measurement.carrier_phase_turns
        + (smoothed_intensity * phase_delta_turns(measurement.target_phase_turns, feedback.measured_hash_phase_turns))
        + (measurement_probability * phase_delta_turns(feedback.measured_hash_phase_turns, feedback.measured_nonce_phase_turns))
        + (0.125 * clamp_unit(measurement.phase_pressure))
        + (0.0625 * clamp_unit(measurement.flux_transport_norm))
        + (0.0625 * clamp_unit(measurement.transfer_drive_norm))
        + (0.0625 * clamp_unit(measurement.transport_drive_norm))
        + (0.046875 * clamp_unit(measurement.resonance_activation_norm))
        + (0.0625 * clamp_unit(measurement.sha256_frequency_bias_norm))
        + (0.03125 * clamp_unit(measurement.sha256_harmonic_density_norm))
        + (0.03125 * measurement.rf_phase_velocity_turns)
        + (0.03125 * measurement.rf_zero_point_displacement_turns)
        - (0.03125 * clamp_unit(measurement.damping_norm))
    );
    feedback.relock_error_turns = std::abs(
        phase_delta_turns(feedback.feedback_phase_turns, measurement.target_phase_turns)
    );
    feedback.phase_flux_conservation = mean_unit({
        smoothed_intensity,
        target_alignment,
        carrier_alignment,
        rf_phase_alignment,
        sha256_digest_alignment,
        sha256_round_alignment,
        clamp_unit(measurement.trajectory_conservation),
        clamp_unit(measurement.temporal_admissibility),
        clamp_unit(measurement.zero_point_proximity),
        clamp_unit(measurement.stability_gate_norm),
        clamp_unit(measurement.rf_particle_stability_norm),
        clamp_unit(measurement.target_resonance_norm),
        clamp_unit(measurement.resonance_activation_norm),
        clamp_unit(measurement.sha256_frequency_bias_norm),
        clamp_unit(measurement.sha256_harmonic_density_norm),
        1.0 - clamp_unit(measurement.damping_norm),
        1.0 - clamp_unit(measurement.interference_projection),
        1.0 - clamp_unit(measurement.anchor_evm_norm),
        1.0 - clamp_unit(measurement.sideband_energy_norm),
    });
    feedback.observer_collapse_strength = mean_unit({
        measurement_probability,
        1.0 - observer_damping,
        smoothed_intensity,
        feedback.phase_flux_conservation,
        1.0 - clamp_unit(measurement.phase_lock_error),
        clamp_unit(measurement.spin_alignment_norm),
        clamp_unit(measurement.transfer_drive_norm),
        sha256_digest_alignment,
        clamp_unit(measurement.resonance_activation_norm),
        clamp_unit(measurement.sha256_frequency_bias_norm),
    });
    feedback.nonce_collapse_confidence = mean_unit({
        feedback.observer_collapse_strength,
        feedback.phase_flux_conservation,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
        target_alignment,
        carrier_alignment,
        sha256_round_alignment,
        clamp_unit(measurement.stability_gate_norm),
        clamp_unit(measurement.resonance_activation_norm),
        1.0 - clamp_unit(measurement.damping_norm),
    });
    return feedback;
}

SubstrateStratumPhaseFluxMeasurement apply_stratum_pow_collapse_feedback(
    const SubstrateStratumPhaseFluxMeasurement& measurement,
    const SubstrateStratumPowCollapseFeedback& feedback,
    bool valid_share
) {
    SubstrateStratumPhaseFluxMeasurement updated = measurement;
    const double feedback_weight = valid_share ? 0.625 : 0.375;
    const double target_weight = valid_share ? 0.25 : 0.125;
    updated.carrier_phase_turns = wrap_turns(
        ((1.0 - feedback_weight) * updated.carrier_phase_turns)
        + (feedback_weight * feedback.feedback_phase_turns)
    );
    updated.target_phase_turns = wrap_turns(
        ((1.0 - target_weight) * updated.target_phase_turns)
        + (target_weight * feedback.measured_hash_phase_turns)
    );
    updated.phase_pressure = mean_unit({
        updated.phase_pressure,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
    });
    updated.flux_transport_norm = mean_unit({
        updated.flux_transport_norm,
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
    });
    updated.observer_factor = mean_unit({
        updated.observer_factor,
        feedback.observer_collapse_strength,
        valid_share ? 1.0 : 0.5,
    });
    updated.zero_point_proximity = mean_unit({
        updated.zero_point_proximity,
        feedback.nonce_collapse_confidence,
        feedback.phase_flux_conservation,
    });
    updated.temporal_admissibility = mean_unit({
        updated.temporal_admissibility,
        feedback.phase_flux_conservation,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
    });
    updated.trajectory_conservation = mean_unit({
        updated.trajectory_conservation,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
    });
    updated.phase_lock_error = mean_unit({
        updated.phase_lock_error,
        clamp_unit(feedback.relock_error_turns * 2.0),
    });
    updated.rf_phase_position_turns = wrap_turns(
        ((1.0 - feedback_weight) * updated.rf_phase_position_turns)
        + (feedback_weight * feedback.measured_nonce_phase_turns)
    );
    updated.rf_phase_velocity_turns = phase_delta_turns(
        feedback.feedback_phase_turns,
        updated.rf_phase_position_turns
    );
    updated.rf_zero_point_displacement_turns = phase_delta_turns(updated.rf_phase_position_turns, 0.0);
    updated.rf_zero_point_distance_norm = clamp_unit(
        std::abs(updated.rf_zero_point_displacement_turns) * 4.0
    );
    updated.rf_carrier_frequency_norm = mean_unit({
        updated.rf_carrier_frequency_norm,
        updated.phase_pressure,
        feedback.nonce_collapse_confidence,
    });
    updated.rf_envelope_amplitude_norm = mean_unit({
        updated.rf_envelope_amplitude_norm,
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
    });
    updated.rf_spin_drive_signed = clamp_signed(
        (0.75 * updated.rf_spin_drive_signed)
        + (0.25 * std::sin(kTwoPi * updated.rf_phase_position_turns) * feedback.phase_flux_conservation)
    );
    updated.rf_rotation_orientation_signed = clamp_signed(
        (0.75 * updated.rf_rotation_orientation_signed)
        + (0.25 * std::cos(kTwoPi * updated.target_phase_turns) * feedback.observer_collapse_strength)
    );
    updated.rf_temporal_coupling_norm = mean_unit({
        updated.rf_temporal_coupling_norm,
        feedback.phase_flux_conservation,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
    });
    updated.rf_resonance_hold_norm = mean_unit({
        updated.rf_resonance_hold_norm,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
    });
    updated.rf_sideband_energy_norm = clamp_unit(
        (0.75 * updated.rf_sideband_energy_norm)
        + (0.25 * (1.0 - feedback.observer_collapse_strength))
    );
    updated.rf_energy_transfer_norm = mean_unit({
        updated.rf_energy_transfer_norm,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
    });
    updated.rf_particle_stability_norm = mean_unit({
        updated.rf_particle_stability_norm,
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
    });
    updated.sha256_schedule_phase_turns = wrap_turns(
        (0.75 * updated.sha256_schedule_phase_turns)
        + (0.125 * feedback.feedback_phase_turns)
        + (0.125 * feedback.measured_nonce_phase_turns)
    );
    updated.sha256_round_phase_turns = wrap_turns(
        (0.70 * updated.sha256_round_phase_turns)
        + (0.30 * feedback.measured_nonce_phase_turns)
    );
    updated.sha256_digest_phase_turns = wrap_turns(
        (0.70 * updated.sha256_digest_phase_turns)
        + (0.30 * feedback.measured_hash_phase_turns)
    );
    updated.sha256_frequency_bias_norm = mean_unit({
        updated.sha256_frequency_bias_norm,
        feedback.phase_flux_conservation,
        feedback.observer_collapse_strength,
    });
    updated.sha256_harmonic_density_norm = mean_unit({
        updated.sha256_harmonic_density_norm,
        feedback.nonce_collapse_confidence,
        feedback.phase_flux_conservation,
    });
    updated.target_resonance_norm = mean_unit({
        updated.target_resonance_norm,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
    });
    updated.resonance_activation_norm = mean_unit({
        updated.resonance_activation_norm,
        updated.target_resonance_norm,
        feedback.phase_flux_conservation,
        feedback.nonce_collapse_confidence,
        feedback.observer_collapse_strength,
        updated.transfer_drive_norm,
        updated.stability_gate_norm,
        1.0 - clamp_unit(feedback.relock_error_turns * 2.0),
    });
    updated.spin_alignment_norm = mean_unit({
        updated.spin_alignment_norm,
        std::abs(updated.rf_spin_drive_signed),
        std::abs(updated.rf_rotation_orientation_signed),
        1.0 - updated.interference_projection,
    });
    const double zero_point_crossover_norm = mean_unit({
        updated.zero_point_proximity,
        1.0 - updated.rf_zero_point_distance_norm,
        phase_peak_proximity(updated.rf_phase_position_turns),
    });
    updated.transfer_drive_norm = clamp_unit(
        (0.50 * clamp_unit(
            zero_point_crossover_norm
            * (1.0 - updated.rf_zero_point_distance_norm)
            * updated.rf_temporal_coupling_norm
            * std::abs(updated.rf_spin_drive_signed)
            * 6.0))
        + (0.30 * updated.rf_energy_transfer_norm)
        + (0.20 * std::max(
            phase_peak_proximity(updated.rf_phase_position_turns),
            updated.zero_point_proximity
        )));
    updated.stability_gate_norm = clamp_unit(
        updated.trajectory_conservation
        * updated.rf_particle_stability_norm
        * updated.rf_resonance_hold_norm
        * (1.0 - updated.phase_lock_error));
    updated.damping_norm = clamp_unit(
        (0.38 * updated.rf_sideband_energy_norm)
        + (0.32 * updated.interference_projection)
        + (0.30 * updated.anchor_evm_norm));
    updated.transport_drive_norm = clamp_unit(
        (0.36 * updated.phase_pressure)
        + (0.22 * updated.observer_factor)
        + (0.16 * updated.flux_transport_norm)
        + (0.14 * updated.rf_temporal_coupling_norm)
        + (0.12 * updated.stability_gate_norm)
        + (0.18 * updated.transfer_drive_norm)
        + (0.12 * updated.target_resonance_norm)
        + (0.10 * updated.resonance_activation_norm)
        + (0.12 * updated.sha256_frequency_bias_norm)
        + (0.08 * updated.sha256_harmonic_density_norm)
        - (0.12 * updated.damping_norm));
    updated.anchor_evm_norm = clamp_unit(
        (0.75 * updated.anchor_evm_norm) + (0.25 * (1.0 - feedback.phase_flux_conservation))
    );
    updated.sideband_energy_norm = clamp_unit(
        (0.75 * updated.sideband_energy_norm) + (0.25 * (1.0 - feedback.observer_collapse_strength))
    );
    updated.interference_projection = clamp_unit(
        (0.75 * updated.interference_projection) + (0.25 * (1.0 - feedback.nonce_collapse_confidence))
    );
    updated.rf_sideband_energy_norm = mean_unit({
        updated.rf_sideband_energy_norm,
        updated.sideband_energy_norm,
        updated.interference_projection,
    });
    updated.damping_norm = clamp_unit(
        (0.38 * updated.rf_sideband_energy_norm)
        + (0.32 * updated.interference_projection)
        + (0.30 * updated.anchor_evm_norm));
    updated.transport_drive_norm = clamp_unit(
        (0.36 * updated.phase_pressure)
        + (0.22 * updated.observer_factor)
        + (0.16 * updated.flux_transport_norm)
        + (0.14 * updated.rf_temporal_coupling_norm)
        + (0.12 * updated.stability_gate_norm)
        + (0.18 * updated.transfer_drive_norm)
        + (0.12 * updated.target_resonance_norm)
        + (0.10 * updated.resonance_activation_norm)
        + (0.12 * updated.sha256_frequency_bias_norm)
        + (0.08 * updated.sha256_harmonic_density_norm)
        - (0.12 * updated.damping_norm));
    updated.resonance_activation_norm = mean_unit({
        updated.resonance_activation_norm,
        updated.target_resonance_norm,
        updated.transfer_drive_norm,
        updated.stability_gate_norm,
        updated.transport_drive_norm,
        updated.sha256_frequency_bias_norm,
        updated.sha256_harmonic_density_norm,
        1.0 - updated.damping_norm,
    });
    return updated;
}

SubstrateStratumPowPhaseTrace trace_stratum_pow_phase(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_value,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& measurement
) {
    SubstrateStratumPowPhaseTrace trace;
    const PreparedPowEvaluationContext context =
        prepare_pow_evaluation_context(header_hex, nbits_hex, share_difficulty);
    const std::vector<std::uint8_t> header(
        context.header_template.begin(),
        context.header_template.end());
    const PhaseEncodedSearchParameters parameters = build_phase_encoded_search_parameters(
        header,
        context.share_target,
        1U
    );

    trace.nonce_seed_phase_turns = word_to_phase_turns(nonce_value);
    trace.header_phase_turns = parameters.header_phase_turns;
    trace.share_target_phase_turns = parameters.target_phase_turns;
    trace.block_target_phase_turns = bytes_to_phase_turns(
        std::vector<std::uint8_t>(context.block_target.begin(), context.block_target.end()),
        0U,
        16U
    );

    trace.initial_measurement = effective_phase_flux_measurement(parameters, measurement);
    trace.initial_measurement.search_epoch_turns = wrap_turns(
        (0.75 * trace.initial_measurement.search_epoch_turns)
        + (0.25 * trace.nonce_seed_phase_turns)
    );
    trace.initial_measurement.carrier_phase_turns = wrap_turns(
        (0.40 * trace.initial_measurement.carrier_phase_turns)
        + (0.35 * trace.nonce_seed_phase_turns)
        + (0.25 * trace.header_phase_turns)
    );
    trace.initial_measurement.target_phase_turns = wrap_turns(
        (0.60 * trace.initial_measurement.target_phase_turns)
        + (0.40 * trace.share_target_phase_turns)
    );
    trace.initial_measurement.rf_phase_position_turns = wrap_turns(
        trace.initial_measurement.rf_phase_position_turns == 0.0
            ? trace.nonce_seed_phase_turns
            : ((0.50 * trace.initial_measurement.rf_phase_position_turns)
                + (0.50 * trace.nonce_seed_phase_turns))
    );
    trace.initial_measurement.rf_phase_velocity_turns = phase_delta_turns(
        trace.share_target_phase_turns,
        trace.nonce_seed_phase_turns
    );
    trace.initial_measurement.rf_zero_point_displacement_turns =
        phase_delta_turns(trace.nonce_seed_phase_turns, 0.0);
    trace.initial_measurement.rf_zero_point_distance_norm = clamp_unit(
        std::abs(trace.initial_measurement.rf_zero_point_displacement_turns) * 4.0
    );
    trace.initial_measurement.zero_point_proximity = mean_unit({
        trace.initial_measurement.zero_point_proximity,
        1.0 - trace.initial_measurement.rf_zero_point_distance_norm,
        phase_peak_proximity(trace.nonce_seed_phase_turns),
    });
    const double nonce_header_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(trace.nonce_seed_phase_turns, trace.header_phase_turns)) * 2.0
    );
    const double nonce_target_alignment = 1.0 - clamp_unit(
        std::abs(phase_delta_turns(trace.nonce_seed_phase_turns, trace.share_target_phase_turns)) * 2.0
    );
    trace.initial_measurement.target_resonance_norm = mean_unit({
        trace.initial_measurement.target_resonance_norm,
        nonce_target_alignment,
        nonce_header_alignment,
        trace.initial_measurement.zero_point_proximity,
        trace.initial_measurement.transfer_drive_norm,
        trace.initial_measurement.stability_gate_norm,
    });
    trace.initial_measurement.resonance_activation_norm = mean_unit({
        trace.initial_measurement.resonance_activation_norm,
        trace.initial_measurement.target_resonance_norm,
        trace.initial_measurement.transfer_drive_norm,
        trace.initial_measurement.stability_gate_norm,
        trace.initial_measurement.transport_drive_norm,
        1.0 - trace.initial_measurement.damping_norm,
    });

    trace.resonant_measurement = bias_phase_flux_measurement_with_sha256_frequency(
        header_hex,
        nbits_hex,
        share_difficulty,
        trace.initial_measurement
    );
    trace.evaluation = evaluate_prepared_stratum_pow(context, nonce_value, true);
    trace.collapse_feedback = measure_stratum_pow_collapse(trace.evaluation, trace.resonant_measurement);
    trace.collapsed_measurement = apply_stratum_pow_collapse_feedback(
        trace.resonant_measurement,
        trace.collapse_feedback,
        trace.evaluation.valid_share
    );

    std::ostringstream sequence;
    sequence << std::fixed << std::setprecision(6)
             << "nonce_seed:" << trace.nonce_seed_phase_turns
             << "|header:" << trace.header_phase_turns
             << "|share_target:" << trace.share_target_phase_turns
             << "|block_target:" << trace.block_target_phase_turns
             << "|sha_schedule:" << trace.resonant_measurement.sha256_schedule_phase_turns
             << "|sha_round:" << trace.resonant_measurement.sha256_round_phase_turns
             << "|sha_digest:" << trace.resonant_measurement.sha256_digest_phase_turns
             << "|target_resonance:" << trace.resonant_measurement.target_resonance_norm
             << "|activation:" << trace.resonant_measurement.resonance_activation_norm
             << "|collapse_hash:" << trace.collapse_feedback.measured_hash_phase_turns
             << "|collapse_nonce:" << trace.collapse_feedback.measured_nonce_phase_turns
             << "|collapse_feedback:" << trace.collapse_feedback.feedback_phase_turns
             << "|conservation:" << trace.collapse_feedback.phase_flux_conservation
             << "|confidence:" << trace.collapse_feedback.nonce_collapse_confidence;
    trace.temporal_sequence = sequence.str();
    trace.performed = true;
    return trace;
}

SubstrateStratumPowSearchResult find_valid_stratum_nonce(
    const std::string& header_hex,
    const std::string& nbits_hex,
    std::uint32_t nonce_start,
    std::size_t max_attempts,
    double share_difficulty,
    const SubstrateStratumPhaseFluxMeasurement& measurement
) {
    SubstrateStratumPowSearchResult result;
    const std::size_t attempts = std::max<std::size_t>(1U, max_attempts);
    const PreparedPowEvaluationContext evaluation_context =
        prepare_pow_evaluation_context(header_hex, nbits_hex, share_difficulty);
    const std::vector<std::uint8_t> header(
        evaluation_context.header_template.begin(),
        evaluation_context.header_template.end());
    const PhaseEncodedSearchParameters parameters = build_phase_encoded_search_parameters(
        header,
        evaluation_context.share_target,
        attempts
    );
    SubstrateStratumPhaseFluxMeasurement rolling_measurement = bias_phase_flux_measurement_with_sha256_frequency(
        header_hex,
        nbits_hex,
        share_difficulty,
        effective_phase_flux_measurement(parameters, measurement));
    std::unordered_set<std::uint32_t> visited_nonces;
    visited_nonces.reserve(attempts);

    for (std::size_t index = 0; index < attempts; ++index) {
        const std::uint32_t offset = phase_encoded_offset(parameters, rolling_measurement, index);
        const std::uint32_t nonce_value = nonce_start + offset;
        if (!visited_nonces.insert(nonce_value).second) {
            continue;
        }
        const SubstrateStratumPowEvaluation candidate_evaluation =
            evaluate_prepared_stratum_pow(evaluation_context, nonce_value);
        const SubstrateStratumPowCollapseFeedback feedback =
            measure_stratum_pow_collapse(candidate_evaluation, rolling_measurement);
        rolling_measurement = apply_stratum_pow_collapse_feedback(
            rolling_measurement,
            feedback,
            candidate_evaluation.valid_share
        );
        if (!result.found) {
            result.evaluation = candidate_evaluation;
            result.evaluation.header_hex = build_header_hex_for_nonce(evaluation_context, nonce_value);
            result.collapse_feedback = feedback;
            result.nonce_value = nonce_value;
        }
        result.attempts = visited_nonces.size();
        if (candidate_evaluation.valid_share) {
            if (!result.found || collapse_feedback_is_better(
                feedback,
                result.collapse_feedback,
                candidate_evaluation,
                result.evaluation,
                nonce_value,
                result.nonce_value
            )) {
                result.evaluation = candidate_evaluation;
                result.evaluation.header_hex = build_header_hex_for_nonce(evaluation_context, nonce_value);
                result.collapse_feedback = feedback;
                result.nonce_value = nonce_value;
                result.found = true;
            }
        }
    }
    return result;
}

}  // namespace qbit_miner
