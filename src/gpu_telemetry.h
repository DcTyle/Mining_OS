// gpu_telemetry.h: GPU Telemetry Interface
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct GPUTelemetrySample {
    std::uint64_t telemetry_sequence = 0;
    double sample_window_start_s = 0.0;
    double sample_window_end_s = 0.0;
    double timestamp_s = 0.0;
    double delta_s = 0.0;
    double graphics_frequency_hz = 0.0;
    double memory_frequency_hz = 0.0;
    double amplitude_norm = 0.0;
    double voltage_v = 0.0;
    double amperage_a = 0.0;
    double power_w = 0.0;
    double temperature_c = 0.0;
    double gpu_util_norm = 0.0;
    double memory_util_norm = 0.0;
    double thermal_interference_norm = 0.0;
    bool live = false;
    std::string provider;
};

class GPUTelemetry {
public:
    GPUTelemetry();
    ~GPUTelemetry();

    GPUTelemetrySample poll();
    void start_stream(std::chrono::milliseconds interval = std::chrono::milliseconds(10));
    void stop_stream();
    [[nodiscard]] bool stream_running() const noexcept;
    [[nodiscard]] std::vector<GPUTelemetrySample> consume_pending_samples();
    [[nodiscard]] std::optional<GPUTelemetrySample> latest_sample() const;

private:
    GPUTelemetrySample poll_once(bool emit_log);
    void stream_loop();
    GPUTelemetrySample _last;
    bool _has_last;
    std::uint64_t _sequence;
    double _phase_seed;
    mutable std::mutex _mutex;
    std::deque<GPUTelemetrySample> _pending_samples;
    std::thread _stream_thread;
    std::chrono::milliseconds _stream_interval {10};
    std::size_t _max_pending_samples = 512U;
    bool _stream_requested = false;

    bool try_poll_nvidia_smi(GPUTelemetrySample& sample);
    GPUTelemetrySample build_proxy_sample(double now_s);

    static double now_seconds();
    static double clamp01(double value);
};
