// gpu_telemetry.cpp: GPU Telemetry Implementation
#include "gpu_telemetry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
#define QBIT_POPEN _popen
#define QBIT_PCLOSE _pclose
#else
#define QBIT_POPEN popen
#define QBIT_PCLOSE pclose
#endif

std::vector<double> parse_csv_numbers(const std::string& line) {
    std::vector<double> values;
    std::stringstream stream(line);
    std::string token;
    while (std::getline(stream, token, ',')) {
        std::stringstream cell(token);
        double value = 0.0;
        cell >> value;
        values.push_back(value);
    }
    return values;
}

}  // namespace

GPUTelemetry::GPUTelemetry()
    : _has_last(false),
      _sequence(0),
      _phase_seed(0.0) {
}

GPUTelemetry::~GPUTelemetry() {
    stop_stream();
}

double GPUTelemetry::now_seconds() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

double GPUTelemetry::clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

bool GPUTelemetry::try_poll_nvidia_smi(GPUTelemetrySample& sample) {
    const char* cmd =
        "nvidia-smi --query-gpu=clocks.current.graphics,clocks.current.memory,power.draw,temperature.gpu,utilization.gpu,utilization.memory --format=csv,noheader,nounits";
    FILE* pipe = QBIT_POPEN(cmd, "r");
    if (pipe == nullptr) {
        return false;
    }

    char buffer[512];
    std::string line;
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = buffer;
    }
    QBIT_PCLOSE(pipe);

    if (line.empty()) {
        return false;
    }

    const std::vector<double> values = parse_csv_numbers(line);
    if (values.size() < 6) {
        return false;
    }

    const double graphics_mhz = values[0];
    const double memory_mhz = values[1];
    const double power_w = values[2];
    const double temperature_c = values[3];
    const double gpu_util_pct = values[4];
    const double memory_util_pct = values[5];

    sample.graphics_frequency_hz = graphics_mhz * 1000000.0;
    sample.memory_frequency_hz = memory_mhz * 1000000.0;
    sample.gpu_util_norm = clamp01(gpu_util_pct / 100.0);
    sample.memory_util_norm = clamp01(memory_util_pct / 100.0);
    sample.power_w = std::max(0.0, power_w);
    sample.temperature_c = std::max(0.0, temperature_c);
    sample.amplitude_norm = clamp01(0.58 * sample.gpu_util_norm + 0.42 * clamp01(memory_mhz / 12000.0));

    // Voltage is treated as a live potential estimate when direct API voltage is unavailable.
    sample.voltage_v = 0.68 + (0.54 * sample.gpu_util_norm) + (0.08 * clamp01(power_w / 350.0));
    sample.amperage_a = sample.power_w / std::max(sample.voltage_v, 0.1);
    sample.thermal_interference_norm = clamp01((sample.temperature_c - 45.0) / 40.0);
    sample.live = true;
    sample.provider = "nvidia-smi";
    return true;
}

GPUTelemetrySample GPUTelemetry::build_proxy_sample(double now_s) {
    _phase_seed += 0.17;

    GPUTelemetrySample sample;
    sample.timestamp_s = now_s;
    sample.graphics_frequency_hz = 1450000000.0 + (140000000.0 * std::sin(_phase_seed));
    sample.memory_frequency_hz = 980000000.0 + (95000000.0 * std::cos(_phase_seed * 0.71));
    sample.gpu_util_norm = clamp01(0.72 + (0.16 * std::sin(_phase_seed * 0.53)));
    sample.memory_util_norm = clamp01(0.61 + (0.18 * std::cos(_phase_seed * 0.41)));
    sample.amplitude_norm = clamp01(0.54 + (0.18 * std::sin(_phase_seed * 0.89)));
    sample.voltage_v = 0.91 + (0.05 * std::cos(_phase_seed * 0.33));
    sample.amperage_a = 42.0 + (6.0 * std::sin(_phase_seed * 0.62));
    sample.power_w = sample.voltage_v * sample.amperage_a;
    sample.temperature_c = 63.0 + (4.0 * std::sin(_phase_seed * 0.24));
    sample.thermal_interference_norm = clamp01((sample.temperature_c - 45.0) / 40.0);
    sample.live = false;
    sample.provider = "deterministic-fallback";
    return sample;
}

GPUTelemetrySample GPUTelemetry::poll() {
    return poll_once(true);
}

GPUTelemetrySample GPUTelemetry::poll_once(bool emit_log) {
    GPUTelemetrySample sample;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const double now_s = now_seconds();
        const std::uint64_t telemetry_sequence = ++_sequence;

        sample.telemetry_sequence = telemetry_sequence;
        sample.timestamp_s = now_s;
        if (!try_poll_nvidia_smi(sample)) {
            sample = build_proxy_sample(now_s);
            sample.telemetry_sequence = telemetry_sequence;
        }

        sample.delta_s = _has_last ? std::max(0.0, sample.timestamp_s - _last.timestamp_s) : 0.02;
        sample.sample_window_end_s = sample.timestamp_s;
        sample.sample_window_start_s = std::max(0.0, sample.timestamp_s - sample.delta_s);
        _last = sample;
        _has_last = true;
    }

    if (emit_log) {
        std::cout
            << "[GPU] provider=" << sample.provider
            << " gfx_hz=" << sample.graphics_frequency_hz
            << " mem_hz=" << sample.memory_frequency_hz
            << " amp_norm=" << sample.amplitude_norm
            << " voltage_v=" << sample.voltage_v
            << " amperage_a=" << sample.amperage_a
            << " power_w=" << sample.power_w
            << " temp_c=" << sample.temperature_c
            << " live=" << (sample.live ? "true" : "false")
            << "\n";
    }

    return sample;
}

void GPUTelemetry::start_stream(std::chrono::milliseconds interval) {
    const auto bounded_interval = std::max(interval, std::chrono::milliseconds(1));
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stream_interval = bounded_interval;
        if (_stream_requested) {
            return;
        }
        _stream_requested = true;
    }

    try {
        _stream_thread = std::thread([this]() { stream_loop(); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(_mutex);
        _stream_requested = false;
        throw;
    }
}

void GPUTelemetry::stop_stream() {
    std::thread stream_thread;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stream_requested = false;
        stream_thread = std::move(_stream_thread);
    }

    if (stream_thread.joinable()) {
        stream_thread.join();
    }
}

bool GPUTelemetry::stream_running() const noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stream_requested;
}

std::vector<GPUTelemetrySample> GPUTelemetry::consume_pending_samples() {
    std::vector<GPUTelemetrySample> pending_samples;
    std::lock_guard<std::mutex> lock(_mutex);
    pending_samples.reserve(_pending_samples.size());
    while (!_pending_samples.empty()) {
        pending_samples.push_back(_pending_samples.front());
        _pending_samples.pop_front();
    }
    return pending_samples;
}

std::optional<GPUTelemetrySample> GPUTelemetry::latest_sample() const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_has_last) {
        return std::nullopt;
    }
    return _last;
}

void GPUTelemetry::stream_loop() {
    while (true) {
        std::chrono::milliseconds interval {10};
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_stream_requested) {
                break;
            }
            interval = _stream_interval;
        }

        const GPUTelemetrySample sample = poll_once(false);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pending_samples.push_back(sample);
            while (_pending_samples.size() > _max_pending_samples) {
                _pending_samples.pop_front();
            }
        }

        std::this_thread::sleep_for(interval);
    }
}
