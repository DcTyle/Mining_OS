#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(QBIT_MINER_HAS_VULKAN)
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#endif

#include "qbit_miner/control_center/substrate_compute_runtime.hpp"
#include "qbit_miner/control_center/live_control_center.hpp"

namespace {

constexpr UINT_PTR kUiTimerId = 1;
constexpr int kTopBarHeight = 60;
constexpr int kLeftPaneWidth = 500;
constexpr int kWindowWidth = 1680;
constexpr int kWindowHeight = 980;
constexpr int kViewportMinWidth = 540;

constexpr UINT kCommandFileExportCalibration = 1001;
constexpr UINT kCommandFileExportValidation = 1002;
constexpr UINT kCommandFileSaveViewport = 1003;
constexpr UINT kCommandFileExit = 1004;
constexpr UINT kCommandEditCopyMetrics = 1101;
constexpr UINT kCommandEditToggleAudio = 1102;
constexpr UINT kCommandEditTogglePause = 1103;
constexpr UINT kCommandEditUserSettings = 1104;
constexpr UINT kCommandWindowResetLayout = 1201;
constexpr UINT kCommandWindowToggleCompact = 1202;
constexpr UINT kCommandWindowAlwaysOnTop = 1203;
constexpr int kOperatorPanelHeight = 150;
constexpr int kConnectionCardHeight = 224;
constexpr int kShareCardHeight = 224;

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (wide_length <= 0) {
        return std::wstring(text.begin(), text.end());
    }

    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), wide_length);
    return wide;
}

std::string narrow_utf16(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int narrow_length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (narrow_length <= 0) {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t value : text) {
            fallback.push_back(value >= 0 && value <= 0x7F ? static_cast<char>(value) : '?');
        }
        return fallback;
    }

    std::string narrow(static_cast<std::size_t>(narrow_length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        narrow.data(),
        narrow_length,
        nullptr,
        nullptr
    );
    return narrow;
}

struct StartupMiningProfile {
    qbit_miner::MiningConnectionSettings settings;
    bool auto_connect = false;
    bool auto_start_run = false;
};

std::string trim_ascii_copy(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    if (first == text.end()) {
        return {};
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    return std::string(first, last);
}

std::string lowercase_ascii_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

bool parse_bool_value(const std::string& text, bool fallback = false) {
    const std::string normalized = lowercase_ascii_copy(trim_ascii_copy(text));
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return fallback;
}

std::optional<unsigned long long> parse_unsigned_value(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(trim_ascii_copy(text), &consumed, 10);
        if (consumed == trim_ascii_copy(text).size()) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<double> parse_double_value(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stod(trim_ascii_copy(text), &consumed);
        if (consumed == trim_ascii_copy(text).size()) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::filesystem::path control_center_startup_profile_path() {
    return std::filesystem::path("runtime_logs") / "control_center_startup.ini";
}

std::optional<StartupMiningProfile> load_startup_mining_profile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }

    StartupMiningProfile profile;
    bool has_any_value = false;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        const std::size_t delimiter = line.find('=');
        if (delimiter == std::string::npos) {
            continue;
        }

        const std::string key = lowercase_ascii_copy(trim_ascii_copy(line.substr(0, delimiter)));
        const std::string value = trim_ascii_copy(line.substr(delimiter + 1U));
        if (key.empty()) {
            continue;
        }

        has_any_value = true;
        if (key == "pool_policy") {
            profile.settings.pool_policy =
                lowercase_ascii_copy(value) == "f2pool"
                ? qbit_miner::MiningPoolPolicy::F2Pool
                : qbit_miner::MiningPoolPolicy::TwoMiners;
        } else if (key == "pool_host") {
            profile.settings.pool_host = value;
        } else if (key == "pool_port") {
            if (const auto parsed = parse_unsigned_value(value)) {
                profile.settings.pool_port = static_cast<std::uint16_t>(std::min<unsigned long long>(*parsed, 65535ULL));
            }
        } else if (key == "payout_address") {
            profile.settings.payout_address = value;
        } else if (key == "worker_id") {
            profile.settings.worker_id = value;
        } else if (key == "worker_password") {
            profile.settings.worker_password = value;
        } else if (key == "allow_live_submit") {
            profile.settings.allow_live_submit = parse_bool_value(value, profile.settings.allow_live_submit);
        } else if (key == "phase_guided_preview_test_mode") {
            profile.settings.phase_guided_preview_test_mode =
                parse_bool_value(value, profile.settings.phase_guided_preview_test_mode);
        } else if (key == "mode") {
            const std::string normalized = lowercase_ascii_copy(value);
            if (normalized == "live" || normalized == "livemode") {
                profile.settings.allow_live_submit = true;
                profile.settings.phase_guided_preview_test_mode = false;
            } else if (normalized == "validate" || normalized == "test" || normalized == "testmode") {
                profile.settings.allow_live_submit = false;
                profile.settings.phase_guided_preview_test_mode = true;
            } else if (normalized == "preview") {
                profile.settings.allow_live_submit = false;
                profile.settings.phase_guided_preview_test_mode = false;
            }
        } else if (key == "auto_promote_to_live_mode") {
            profile.settings.auto_promote_to_live_mode =
                parse_bool_value(value, profile.settings.auto_promote_to_live_mode);
        } else if (key == "max_requests_per_second") {
            if (const auto parsed = parse_double_value(value)) {
                profile.settings.max_requests_per_second = *parsed;
            }
        } else if (key == "target_network_share_fraction") {
            if (const auto parsed = parse_double_value(value)) {
                profile.settings.target_network_share_fraction = *parsed;
            }
        } else if (key == "target_hashrate_hs") {
            if (const auto parsed = parse_double_value(value)) {
                profile.settings.target_hashrate_hs = *parsed;
            }
        } else if (key == "max_invalid_pool_submissions") {
            if (const auto parsed = parse_unsigned_value(value)) {
                profile.settings.max_invalid_pool_submissions = static_cast<std::size_t>(*parsed);
            }
        } else if (key == "allowed_worker_count") {
            if (const auto parsed = parse_unsigned_value(value)) {
                profile.settings.allowed_worker_count = static_cast<std::size_t>(*parsed);
            }
        } else if (key == "validation_jitter_window_seconds") {
            if (const auto parsed = parse_double_value(value)) {
                profile.settings.validation_jitter_window_seconds = *parsed;
            }
        } else if (key == "min_validation_jitter_samples") {
            if (const auto parsed = parse_unsigned_value(value)) {
                profile.settings.min_validation_jitter_samples = static_cast<std::size_t>(*parsed);
            }
        } else if (key == "validation_log_csv_path") {
            profile.settings.validation_log_csv_path = value;
        } else if (key == "run_indefinitely") {
            profile.settings.run_indefinitely = parse_bool_value(value, profile.settings.run_indefinitely);
        } else if (key == "run_duration_minutes") {
            if (const auto parsed = parse_double_value(value)) {
                profile.settings.run_duration_minutes = *parsed;
            }
        } else if (key == "auto_connect") {
            profile.auto_connect = parse_bool_value(value, profile.auto_connect);
        } else if (key == "auto_start_run") {
            profile.auto_start_run = parse_bool_value(value, profile.auto_start_run);
        }
    }

    if (!has_any_value) {
        return std::nullopt;
    }
    return profile;
}

std::wstring trim_copy(const std::wstring& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](wchar_t value) {
        return std::iswspace(value) != 0;
    });
    if (first == text.end()) {
        return {};
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](wchar_t value) {
        return std::iswspace(value) != 0;
    }).base();
    return std::wstring(first, last);
}

std::wstring pool_policy_display_name(qbit_miner::MiningPoolPolicy policy) {
    switch (policy) {
    case qbit_miner::MiningPoolPolicy::TwoMiners:
        return L"2Miners";
    case qbit_miner::MiningPoolPolicy::F2Pool:
        return L"F2Pool";
    }
    return L"2Miners";
}

std::wstring pool_policy_requirements_text(qbit_miner::MiningPoolPolicy policy) {
    switch (policy) {
    case qbit_miner::MiningPoolPolicy::TwoMiners:
        return L"2Miners policy requires the coin-specific pool host and port from your 2Miners page, plus a payout wallet and worker id. The authorize login resolves as wallet.worker and the password usually remains x.";
    case qbit_miner::MiningPoolPolicy::F2Pool:
        return L"F2Pool policy requires the pool host and port from the matching F2Pool stratum page, plus the payout account or wallet and a worker id. The authorize login resolves as wallet.worker and the password usually remains x. The worker id itself should use letters and digits only with no dots or underscores.";
    }
    return L"Select a pool policy to review the required connection fields.";
}

bool is_f2pool_worker_id_valid(const std::wstring& worker_id) {
    return !worker_id.empty() && std::all_of(worker_id.begin(), worker_id.end(), [](wchar_t value) {
        return std::iswalnum(value) != 0;
    });
}

std::wstring build_worker_login_preview(
    qbit_miner::MiningPoolPolicy policy,
    const std::wstring& payout_address,
    const std::wstring& worker_id
) {
    (void)policy;
    const std::wstring trimmed_payout = trim_copy(payout_address);
    const std::wstring trimmed_worker = trim_copy(worker_id);
    if (trimmed_payout.empty() || trimmed_worker.empty()) {
        return L"(enter payout/account and worker id to resolve the authorize login)";
    }
    return trimmed_payout + L"." + trimmed_worker;
}

std::wstring mining_session_mode_text(const qbit_miner::MiningConnectionSettings& settings) {
    if (settings.allow_live_submit) {
        return L"Live Submit";
    }
    if (settings.phase_guided_preview_test_mode) {
        return L"Online Header Resonance Validation";
    }
    return L"Preview Only";
}

std::wstring runtime_reward_gate_text(const qbit_miner::MiningConnectionSettings& settings) {
    if (settings.allow_live_submit) {
        return L"Live TCP Stratum transport is enabled on Windows. Share submits can be sent to the selected pool, and reward credit depends on accepted shares landing at the configured payout wallet/account.";
    }
    if (settings.phase_guided_preview_test_mode) {
        return L"Online header-resonance validation keeps network share submits disabled while the runtime conventionally verifies header-resonant share projections against live pool work.";
    }
    return L"Preview mode keeps live share submission disabled. Enable Allow Live Pool Submit when you want the operator controls to send real Stratum share submits.";
}

std::wstring format_metric_history_value(const qbit_miner::MetricHistorySeries& series);

std::wstring prettify_identifier(const std::string& value) {
    if (value.empty()) {
        return L"-";
    }

    std::wstring text = widen_utf8(value);
    bool capitalize_next = true;
    for (wchar_t& ch : text) {
        if (ch == L'_' || ch == L'.' || ch == L':') {
            ch = L' ';
            capitalize_next = true;
            continue;
        }
        if (capitalize_next && ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
        }
        capitalize_next = ch == L' ';
    }
    return text;
}

std::wstring shorten_text(const std::wstring& text, std::size_t max_chars) {
    if (text.size() <= max_chars || max_chars < 7U) {
        return text;
    }

    const std::size_t head = max_chars / 2U;
    const std::size_t tail = max_chars - head - 3U;
    return text.substr(0, head) + L"..." + text.substr(text.size() - tail);
}

std::wstring make_timestamp_string() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::wostringstream out;
    out << std::put_time(&local_tm, L"%Y%m%d_%H%M%S");
    return out.str();
}

std::filesystem::path export_root() {
    return std::filesystem::current_path() / "control_center_exports";
}

std::filesystem::path make_export_directory(const std::wstring& stem) {
    const std::filesystem::path root = export_root();
    std::filesystem::create_directories(root);
    return root / std::filesystem::path(stem + L"_" + make_timestamp_string());
}

std::filesystem::path make_export_file(const std::wstring& stem, const std::wstring& extension) {
    const std::filesystem::path root = export_root();
    std::filesystem::create_directories(root);
    return root / std::filesystem::path(stem + L"_" + make_timestamp_string() + extension);
}

std::wstring format_scaled_value(double value, const std::array<const wchar_t*, 7>& suffixes) {
    double scaled = value;
    std::size_t suffix_index = 0;
    while (std::abs(scaled) >= 1000.0 && suffix_index + 1U < suffixes.size()) {
        scaled /= 1000.0;
        ++suffix_index;
    }
    std::wostringstream out;
    out << std::fixed << std::setprecision(scaled >= 100.0 ? 0 : 2) << scaled << L' ' << suffixes[suffix_index];
    return out.str();
}

std::wstring format_hashrate(double hashrate_hs) {
    return format_scaled_value(hashrate_hs, {L"H/s", L"kH/s", L"MH/s", L"GH/s", L"TH/s", L"PH/s", L"EH/s"});
}

std::wstring format_rate(double value, const wchar_t* suffix) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value << L' ' << suffix;
    return out.str();
}

std::wstring format_duration_compact(double seconds) {
    const auto rounded_seconds = static_cast<long long>(std::llround(std::max(0.0, seconds)));
    const long long hours = rounded_seconds / 3600LL;
    const long long minutes = (rounded_seconds % 3600LL) / 60LL;
    const long long remaining_seconds = rounded_seconds % 60LL;

    std::wostringstream out;
    out << std::setfill(L'0');
    if (hours > 0LL) {
        out << std::setw(2) << hours << L':';
    }
    out << std::setw(2) << minutes << L':' << std::setw(2) << remaining_seconds;
    return out.str();
}

std::wstring format_minutes_label(double minutes) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(minutes >= 10.0 ? 1 : 2) << minutes << L" min";
    return out.str();
}

std::wstring format_percent(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << (value * 100.0) << L"%";
    return out.str();
}

std::wstring format_difficulty(double value) {
    return format_scaled_value(value, {L"", L"K", L"M", L"G", L"T", L"P", L"E"});
}

std::wstring format_currency(double value) {
    std::wostringstream out;
    if (std::abs(value) >= 1000.0) {
        out << L'$' << format_scaled_value(value, {L"", L"K", L"M", L"B", L"T", L"Q", L"QQ"});
        return out.str();
    }
    out << L'$' << std::fixed << std::setprecision(value >= 10.0 ? 2 : 4) << value;
    return out.str();
}

std::wstring format_coin(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(value >= 1.0 ? 4 : 8) << value << L" BTC";
    return out.str();
}

std::wstring format_compact_quantity(double value) {
    static constexpr std::array<const wchar_t*, 7> suffixes {L"", L"K", L"M", L"B", L"T", L"P", L"E"};

    double scaled = value;
    std::size_t suffix_index = 0;
    while (std::abs(scaled) >= 1000.0 && suffix_index + 1U < suffixes.size()) {
        scaled /= 1000.0;
        ++suffix_index;
    }

    std::wostringstream out;
    out << std::fixed << std::setprecision(scaled >= 100.0 ? 0 : 2) << scaled;
    if (suffix_index > 0U) {
        out << L' ' << suffixes[suffix_index];
    }
    return out.str();
}

std::wstring format_window_seconds_label(double seconds) {
    if (!(seconds > 0.0)) {
        return L"-";
    }

    std::wostringstream out;
    out << std::fixed << std::setprecision(seconds >= 10.0 ? 0 : 1) << seconds << L"s";
    return out.str();
}

std::wstring format_window_target(double count, double window_seconds) {
    if (!(window_seconds > 0.0)) {
        return L"-";
    }
    return format_compact_quantity(count) + L" / " + format_window_seconds_label(window_seconds);
}

std::wstring format_metric_history_value(const qbit_miner::MetricHistorySeries& series) {
    switch (series.format) {
    case qbit_miner::MetricValueFormat::Hashrate:
        return format_hashrate(series.current_value);
    case qbit_miner::MetricValueFormat::Rate:
        return format_rate(series.current_value, L"/s");
    case qbit_miner::MetricValueFormat::Percent: {
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << series.current_value << L"%";
        return out.str();
    }
    case qbit_miner::MetricValueFormat::Difficulty:
        return format_difficulty(series.current_value);
    case qbit_miner::MetricValueFormat::Currency:
        return format_currency(series.current_value);
    case qbit_miner::MetricValueFormat::Coin:
        return format_coin(series.current_value);
    case qbit_miner::MetricValueFormat::Temperature: {
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << series.current_value << L" C";
        return out.str();
    }
    case qbit_miner::MetricValueFormat::Power: {
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << series.current_value << L" W";
        return out.str();
    }
    case qbit_miner::MetricValueFormat::FrequencyGHz: {
        std::wostringstream out;
        out << std::fixed << std::setprecision(2) << series.current_value << L" GHz";
        return out.str();
    }
    case qbit_miner::MetricValueFormat::Count:
    case qbit_miner::MetricValueFormat::Number:
    default: {
        std::wostringstream out;
        out << std::fixed << std::setprecision(series.current_value >= 10.0 ? 0 : 2) << series.current_value;
        return out.str();
    }
    }
}

COLORREF mix_color(COLORREF a, COLORREF b, double t) {
    const auto blend = [t](int low, int high) -> BYTE {
        return static_cast<BYTE>(std::clamp(low + ((high - low) * t), 0.0, 255.0));
    };
    return RGB(
        blend(GetRValue(a), GetRValue(b)),
        blend(GetGValue(a), GetGValue(b)),
        blend(GetBValue(a), GetBValue(b))
    );
}

void fill_vertical_gradient(HDC hdc, const RECT& rect, COLORREF top, COLORREF bottom) {
    const int height = std::max<int>(1, static_cast<int>(rect.bottom - rect.top));
    for (int y = 0; y < height; ++y) {
        RECT line {rect.left, rect.top + y, rect.right, rect.top + y + 1};
        const double t = static_cast<double>(y) / static_cast<double>(height);
        HBRUSH brush = CreateSolidBrush(mix_color(top, bottom, t));
        FillRect(hdc, &line, brush);
        DeleteObject(brush);
    }
}

void fill_rect(HDC hdc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void stroke_rect(HDC hdc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
}

void draw_inset_text(
    HDC hdc,
    const std::wstring& text,
    const RECT& rect,
    UINT format,
    COLORREF top_highlight,
    COLORREF shadow,
    COLORREF foreground
) {
    RECT highlight = rect;
    OffsetRect(&highlight, -1, -1);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, top_highlight);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &highlight, format);

    RECT lowlight = rect;
    OffsetRect(&lowlight, 1, 1);
    SetTextColor(hdc, shadow);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &lowlight, format);

    SetTextColor(hdc, foreground);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), const_cast<RECT*>(&rect), format);
}

bool point_in_rect(const POINT& point, const RECT& rect) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

struct SoftwareImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

bool write_bitmap_file(const std::filesystem::path& output_path, const SoftwareImage& image) {
    if (image.width == 0 || image.height == 0 || image.rgba.empty()) {
        return false;
    }

    BITMAPFILEHEADER file_header {};
    BITMAPINFOHEADER info_header {};
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = static_cast<LONG>(image.width);
    info_header.biHeight = -static_cast<LONG>(image.height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(image.width * image.height * 4U);

    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + info_header.biSizeImage;

    std::vector<std::uint8_t> bgra(image.rgba.size(), 0U);
    for (std::size_t index = 0; index + 3U < image.rgba.size(); index += 4U) {
        bgra[index] = image.rgba[index + 2U];
        bgra[index + 1U] = image.rgba[index + 1U];
        bgra[index + 2U] = image.rgba[index];
        bgra[index + 3U] = 255U;
    }

    HANDLE file = CreateFileW(output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL ok = WriteFile(file, &file_header, sizeof(file_header), &bytes_written, nullptr)
        && WriteFile(file, &info_header, sizeof(info_header), &bytes_written, nullptr)
        && WriteFile(file, bgra.data(), static_cast<DWORD>(bgra.size()), &bytes_written, nullptr);
    CloseHandle(file);
    return ok == TRUE;
}

class FieldViewportRasterizer {
public:
    static SoftwareImage render(const qbit_miner::FieldViewportFrame& frame, std::uint32_t width, std::uint32_t height) {
        SoftwareImage image;
        image.width = std::max<std::uint32_t>(width, 32U);
        image.height = std::max<std::uint32_t>(height, 32U);
        image.rgba.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4U, 0U);

        fill_background(image);
        draw_lattice_glow(image, frame);
        draw_voxels(image, frame);
        return image;
    }

private:
    struct ProjectedVoxel {
        float depth = 0.0f;
        int center_x = 0;
        int center_y = 0;
        int radius = 1;
        std::array<std::uint8_t, 4> color {0, 0, 0, 255};
        float highlight = 0.0f;
    };

    static void fill_background(SoftwareImage& image) {
        for (std::uint32_t y = 0; y < image.height; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>(std::max(1U, image.height - 1U));
            const std::uint8_t r = static_cast<std::uint8_t>(18 + (34 * t));
            const std::uint8_t g = static_cast<std::uint8_t>(20 + (28 * t));
            const std::uint8_t b = static_cast<std::uint8_t>(24 + (42 * t));
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
                image.rgba[offset] = r;
                image.rgba[offset + 1U] = g;
                image.rgba[offset + 2U] = b;
                image.rgba[offset + 3U] = 255U;
            }
        }
    }

    static void draw_lattice_glow(SoftwareImage& image, const qbit_miner::FieldViewportFrame& frame) {
        const float center_x = static_cast<float>(image.width) * 0.5f;
        const float center_y = static_cast<float>(image.height) * 0.5f;
        const float base_radius = static_cast<float>(std::min(image.width, image.height)) * 0.34f;
        const float glow_strength = 0.35f + (frame.aggregate_material_pbr[2] * 0.65f);

        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const float dx = (static_cast<float>(x) - center_x) / base_radius;
                const float dy = (static_cast<float>(y) - center_y) / base_radius;
                const float radial = std::sqrt((dx * dx) + (dy * dy));
                const float glow = std::clamp((1.0f - radial) * glow_strength, 0.0f, 1.0f);
                const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
                image.rgba[offset] = static_cast<std::uint8_t>(std::min(255.0f, image.rgba[offset] + (glow * 48.0f)));
                image.rgba[offset + 1U] = static_cast<std::uint8_t>(std::min(255.0f, image.rgba[offset + 1U] + (glow * 56.0f)));
                image.rgba[offset + 2U] = static_cast<std::uint8_t>(std::min(255.0f, image.rgba[offset + 2U] + (glow * 82.0f)));
            }
        }
    }

    static void draw_voxels(SoftwareImage& image, const qbit_miner::FieldViewportFrame& frame) {
        std::vector<ProjectedVoxel> voxels;
        voxels.reserve(frame.voxels.size());

        const float yaw = static_cast<float>(frame.time_s * 0.35);
        const float pitch = -0.52f;
        const float cos_yaw = std::cos(yaw);
        const float sin_yaw = std::sin(yaw);
        const float cos_pitch = std::cos(pitch);
        const float sin_pitch = std::sin(pitch);
        const float viewport_scale = static_cast<float>(std::min(image.width, image.height)) * 0.54f;
        const std::array<float, 3> light_dir = {0.34f, -0.48f, -0.81f};

        for (const auto& voxel : frame.voxels) {
            const float x1 = (voxel.position_xyz[0] * cos_yaw) - (voxel.position_xyz[2] * sin_yaw);
            const float z1 = (voxel.position_xyz[0] * sin_yaw) + (voxel.position_xyz[2] * cos_yaw);
            const float y1 = voxel.position_xyz[1];

            const float y2 = (y1 * cos_pitch) - (z1 * sin_pitch);
            const float z2 = (y1 * sin_pitch) + (z1 * cos_pitch) + 3.6f;
            if (z2 <= 0.2f) {
                continue;
            }

            const float inv_z = 1.0f / z2;
            const int screen_x = static_cast<int>((x1 * inv_z * viewport_scale) + (static_cast<float>(image.width) * 0.5f));
            const int screen_y = static_cast<int>((y2 * inv_z * viewport_scale) + (static_cast<float>(image.height) * 0.52f));
            const int radius = std::max(1, static_cast<int>((voxel.size * 10.0f * inv_z) + 1.5f));

            const float ndotl = std::max(0.0f,
                (voxel.material.normal_xyz[0] * -light_dir[0])
                + (voxel.material.normal_xyz[1] * -light_dir[1])
                + (voxel.material.normal_xyz[2] * -light_dir[2]));
            const float specular = std::pow(std::max(0.0f, ndotl), 4.0f + ((1.0f - voxel.material.roughness) * 18.0f))
                * (0.18f + (0.82f * voxel.material.metallic));
            const float shading = 0.18f + (0.76f * ndotl) + (0.36f * voxel.material.conductivity);

            ProjectedVoxel projected;
            projected.depth = z2;
            projected.center_x = screen_x;
            projected.center_y = screen_y;
            projected.radius = radius;
            projected.highlight = specular + (voxel.material.emissive * 0.85f);
            projected.color = {
                static_cast<std::uint8_t>(std::clamp((voxel.material.base_color_rgba[0] * shading + (voxel.material.emissive * 0.34f) + specular) * 255.0f, 0.0f, 255.0f)),
                static_cast<std::uint8_t>(std::clamp((voxel.material.base_color_rgba[1] * shading + (voxel.material.emissive * 0.42f) + specular) * 255.0f, 0.0f, 255.0f)),
                static_cast<std::uint8_t>(std::clamp((voxel.material.base_color_rgba[2] * shading + (voxel.material.emissive * 0.52f) + specular) * 255.0f, 0.0f, 255.0f)),
                static_cast<std::uint8_t>(std::clamp((0.48f + (voxel.density * 0.50f)) * 255.0f, 0.0f, 255.0f)),
            };
            voxels.push_back(projected);
        }

        std::sort(voxels.begin(), voxels.end(), [](const ProjectedVoxel& lhs, const ProjectedVoxel& rhs) {
            return lhs.depth > rhs.depth;
        });

        for (const auto& projected : voxels) {
            draw_square(image, projected);
        }
    }

    static void draw_square(SoftwareImage& image, const ProjectedVoxel& voxel) {
        const int left = std::max(0, voxel.center_x - voxel.radius);
        const int right = std::min(static_cast<int>(image.width) - 1, voxel.center_x + voxel.radius);
        const int top = std::max(0, voxel.center_y - voxel.radius);
        const int bottom = std::min(static_cast<int>(image.height) - 1, voxel.center_y + voxel.radius);

        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const float dx = static_cast<float>(x - voxel.center_x) / static_cast<float>(std::max(1, voxel.radius));
                const float dy = static_cast<float>(y - voxel.center_y) / static_cast<float>(std::max(1, voxel.radius));
                const float radial = std::sqrt((dx * dx) + (dy * dy));
                if (radial > 1.12f) {
                    continue;
                }

                const float edge = std::clamp(1.0f - radial, 0.0f, 1.0f);
                const float alpha = (voxel.color[3] / 255.0f) * (0.35f + (0.65f * edge));
                const float highlight = std::clamp(voxel.highlight * edge, 0.0f, 1.0f);

                const std::size_t offset = (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * 4U;
                const float dst_r = image.rgba[offset] / 255.0f;
                const float dst_g = image.rgba[offset + 1U] / 255.0f;
                const float dst_b = image.rgba[offset + 2U] / 255.0f;

                const float src_r = std::clamp((voxel.color[0] / 255.0f) + (highlight * 0.35f), 0.0f, 1.0f);
                const float src_g = std::clamp((voxel.color[1] / 255.0f) + (highlight * 0.25f), 0.0f, 1.0f);
                const float src_b = std::clamp((voxel.color[2] / 255.0f) + (highlight * 0.40f), 0.0f, 1.0f);

                const float out_r = (src_r * alpha) + (dst_r * (1.0f - alpha));
                const float out_g = (src_g * alpha) + (dst_g * (1.0f - alpha));
                const float out_b = (src_b * alpha) + (dst_b * (1.0f - alpha));

                image.rgba[offset] = static_cast<std::uint8_t>(std::clamp(out_r * 255.0f, 0.0f, 255.0f));
                image.rgba[offset + 1U] = static_cast<std::uint8_t>(std::clamp(out_g * 255.0f, 0.0f, 255.0f));
                image.rgba[offset + 2U] = static_cast<std::uint8_t>(std::clamp(out_b * 255.0f, 0.0f, 255.0f));
                image.rgba[offset + 3U] = 255U;
            }
        }
    }
};

class FieldAudioOutput {
public:
    ~FieldAudioOutput() {
        shutdown();
    }

    bool initialize() {
        if (device_ != nullptr) {
            return true;
        }

        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8U);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        return waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }

    void shutdown() {
        if (device_ == nullptr) {
            return;
        }

        waveOutReset(device_);
        for (std::size_t index = 0; index < headers_.size(); ++index) {
            if ((headers_[index].dwFlags & WHDR_PREPARED) != 0U) {
                waveOutUnprepareHeader(device_, &headers_[index], sizeof(WAVEHDR));
            }
        }
        waveOutClose(device_);
        device_ = nullptr;
        for (auto& header : headers_) {
            std::memset(&header, 0, sizeof(header));
        }
    }

    void submit(const qbit_miner::StereoPcmFrame& pcm, bool enabled) {
        if (!enabled || device_ == nullptr || pcm.interleaved_samples.empty()) {
            return;
        }

        WAVEHDR& header = headers_[next_header_];
        if ((header.dwFlags & WHDR_INQUEUE) != 0U) {
            next_header_ = (next_header_ + 1U) % headers_.size();
            return;
        }

        if ((header.dwFlags & WHDR_PREPARED) != 0U) {
            waveOutUnprepareHeader(device_, &header, sizeof(WAVEHDR));
        }

        buffers_[next_header_].resize(pcm.interleaved_samples.size() * sizeof(std::int16_t));
        std::memcpy(buffers_[next_header_].data(), pcm.interleaved_samples.data(), buffers_[next_header_].size());

        std::memset(&header, 0, sizeof(header));
        header.lpData = reinterpret_cast<LPSTR>(buffers_[next_header_].data());
        header.dwBufferLength = static_cast<DWORD>(buffers_[next_header_].size());
        if (waveOutPrepareHeader(device_, &header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            waveOutWrite(device_, &header, sizeof(WAVEHDR));
        }

        next_header_ = (next_header_ + 1U) % headers_.size();
    }

private:
    HWAVEOUT device_ = nullptr;
    std::array<WAVEHDR, 3> headers_ {};
    std::array<std::vector<std::uint8_t>, 3> buffers_;
    std::size_t next_header_ = 0;
};

class ViewportPresenter {
public:
    ViewportPresenter() = default;

    ~ViewportPresenter() {
        destroy();
    }

    bool create(HINSTANCE instance, HWND parent, const RECT& bounds) {
        register_window_class(instance);
        hwnd_ = CreateWindowExW(
            0,
            kClassName,
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            bounds.left,
            bounds.top,
            std::max(8L, bounds.right - bounds.left),
            std::max(8L, bounds.bottom - bounds.top),
            parent,
            nullptr,
            instance,
            this);
        if (hwnd_ == nullptr) {
            return false;
        }

        initialize_backend();
        return true;
    }

    void destroy() {
#if defined(QBIT_MINER_HAS_VULKAN)
        cleanup_vulkan();
#endif
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void resize(const RECT& bounds) {
        if (hwnd_ == nullptr) {
            return;
        }
        MoveWindow(
            hwnd_,
            bounds.left,
            bounds.top,
            std::max(8L, bounds.right - bounds.left),
            std::max(8L, bounds.bottom - bounds.top),
            TRUE);
        needs_resize_ = true;
    }

    void present(const qbit_miner::FieldViewportFrame& frame) {
        if (hwnd_ == nullptr) {
            return;
        }

        RECT client_rect {};
        GetClientRect(hwnd_, &client_rect);
        const std::uint32_t width = std::max<LONG>(32, client_rect.right - client_rect.left);
        const std::uint32_t height = std::max<LONG>(32, client_rect.bottom - client_rect.top);

        bool used_compute_surface = false;
        if (compute_runtime_.is_available() && compute_runtime_.update(frame, width, height)) {
            last_image_.width = compute_runtime_.preview_width();
            last_image_.height = compute_runtime_.preview_height();
            last_image_.rgba = compute_runtime_.preview_rgba();
            last_audio_frame_ = compute_runtime_.last_audio_frame();
            has_compute_audio_ = true;
            device_label_ = compute_runtime_.device_label();
            used_compute_surface = true;
        }

        if (!used_compute_surface) {
            last_image_ = FieldViewportRasterizer::render(frame, width, height);
            last_audio_frame_ = {};
            has_compute_audio_ = false;
            if (!vulkan_enabled_) {
                device_label_ = L"Software viewport fallback";
            }
        }

#if defined(QBIT_MINER_HAS_VULKAN)
        if (vulkan_enabled_) {
            if (!present_vulkan()) {
                vulkan_enabled_ = false;
                device_label_ = L"Software viewport fallback";
            }
        }
#endif
        if (!vulkan_enabled_) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    [[nodiscard]] std::wstring device_label() const {
        return device_label_;
    }

    [[nodiscard]] bool has_compute_audio() const {
        return has_compute_audio_;
    }

    [[nodiscard]] qbit_miner::StereoPcmFrame audio_frame() const {
        return last_audio_frame_;
    }

    void set_kernel_iteration_observer(std::function<void(const qbit_miner::GpuKernelIterationEvent&)> observer) {
        compute_runtime_.set_kernel_iteration_observer(std::move(observer));
    }

    void set_mining_validation_observer(std::function<void(const qbit_miner::MiningValidationSnapshot&)> observer) {
        compute_runtime_.set_mining_validation_observer(std::move(observer));
    }

    [[nodiscard]] bool save_snapshot(const std::filesystem::path& output_path) const {
        return write_bitmap_file(output_path, last_image_);
    }

private:
    static constexpr const wchar_t* kClassName = L"QBitMinerViewportWindow";

    static void register_window_class(HINSTANCE instance) {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSW window_class {};
        window_class.lpfnWndProc = &ViewportPresenter::WindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kClassName;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&window_class);
        registered = true;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<ViewportPresenter*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }

        auto* self = reinterpret_cast<ViewportPresenter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->handle_message(message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            needs_resize_ = true;
            return 0;
        case WM_PAINT:
            paint_software();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    void paint_software() {
        PAINTSTRUCT paint {};
        HDC hdc = BeginPaint(hwnd_, &paint);
        if (hdc != nullptr && last_image_.width != 0 && last_image_.height != 0 && !last_image_.rgba.empty()) {
            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(last_image_.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(last_image_.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            std::vector<std::uint8_t> bgra(last_image_.rgba.size(), 0U);
            for (std::size_t index = 0; index + 3U < last_image_.rgba.size(); index += 4U) {
                bgra[index] = last_image_.rgba[index + 2U];
                bgra[index + 1U] = last_image_.rgba[index + 1U];
                bgra[index + 2U] = last_image_.rgba[index];
                bgra[index + 3U] = 255U;
            }

            StretchDIBits(
                hdc,
                0,
                0,
                static_cast<int>(last_image_.width),
                static_cast<int>(last_image_.height),
                0,
                0,
                static_cast<int>(last_image_.width),
                static_cast<int>(last_image_.height),
                bgra.data(),
                &info,
                DIB_RGB_COLORS,
                SRCCOPY);
        }
        EndPaint(hwnd_, &paint);
    }

    void initialize_backend() {
        device_label_ = L"Software viewport fallback";
        if (compute_runtime_.initialize()) {
            device_label_ = compute_runtime_.device_label();
        }
#if defined(QBIT_MINER_HAS_VULKAN)
        try {
            create_vulkan_instance();
            create_vulkan_surface();
            select_vulkan_device();
            create_vulkan_device();
            create_command_objects();
            create_sync_objects();
            vulkan_enabled_ = true;
            needs_resize_ = true;
        } catch (...) {
            cleanup_vulkan();
            vulkan_enabled_ = false;
            device_label_ = L"Software viewport fallback";
        }
#endif
    }

#if defined(QBIT_MINER_HAS_VULKAN)
    struct BufferHandle {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    [[nodiscard]] bool present_vulkan() {
        if (device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE || last_image_.width == 0 || last_image_.height == 0) {
            return false;
        }

        if ((swapchain_ == VK_NULL_HANDLE || needs_resize_) && !recreate_swapchain()) {
            return false;
        }

        if (swapchain_ == VK_NULL_HANDLE) {
            return false;
        }

        vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &in_flight_fence_);

        std::uint32_t image_index = 0;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_semaphore_, VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return recreate_swapchain();
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device_, staging_buffer_.memory, 0, staging_buffer_.size, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        std::memcpy(mapped, last_image_.rgba.data(), static_cast<std::size_t>(staging_buffer_.size));
        vkUnmapMemory(device_, staging_buffer_.memory);

        vkResetCommandPool(device_, command_pool_, 0);

        VkCommandBufferBeginInfo begin_info {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command_buffer_, &begin_info) != VK_SUCCESS) {
            return false;
        }

        VkImageSubresourceRange range {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        VkImageMemoryBarrier to_transfer {};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.oldLayout = image_initialized_[image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = swapchain_images_[image_index];
        to_transfer.subresourceRange = range;
        to_transfer.srcAccessMask = 0;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &to_transfer);

        VkBufferImageCopy copy_region {};
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.mipLevel = 0;
        copy_region.imageSubresource.baseArrayLayer = 0;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent = {swapchain_extent_.width, swapchain_extent_.height, 1};
        vkCmdCopyBufferToImage(
            command_buffer_,
            staging_buffer_.buffer,
            swapchain_images_[image_index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy_region);

        VkImageMemoryBarrier to_present {};
        to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.image = swapchain_images_[image_index];
        to_present.subresourceRange = range;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &to_present);

        if (vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
            return false;
        }

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit_info {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available_semaphore_;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished_semaphore_;
        if (vkQueueSubmit(queue_, 1, &submit_info, in_flight_fence_) != VK_SUCCESS) {
            return false;
        }

        VkPresentInfoKHR present_info {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished_semaphore_;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_;
        present_info.pImageIndices = &image_index;
        result = vkQueuePresentKHR(queue_, &present_info);
        image_initialized_[image_index] = true;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            needs_resize_ = true;
            return recreate_swapchain();
        }
        return result == VK_SUCCESS;
    }

    void create_vulkan_instance() {
        VkApplicationInfo app_info {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "Quantum Miner Control Center";
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "QBitMinerControlCenter";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_0;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo create_info {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = 2;
        create_info.ppEnabledExtensionNames = extensions;
        if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan instance");
        }
    }

    void create_vulkan_surface() {
        VkWin32SurfaceCreateInfoKHR create_info {};
        create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        create_info.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
        create_info.hwnd = hwnd_;
        if (vkCreateWin32SurfaceKHR(instance_, &create_info, nullptr, &surface_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Win32 Vulkan surface");
        }
    }

    void select_vulkan_device() {
        std::uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
        if (device_count == 0) {
            throw std::runtime_error("No Vulkan physical device available");
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

        for (VkPhysicalDevice candidate : devices) {
            std::uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());

            for (std::uint32_t index = 0; index < queue_count; ++index) {
                VkBool32 present_supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &present_supported);
                if (present_supported == VK_TRUE && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                    physical_device_ = candidate;
                    queue_family_index_ = index;

                    VkPhysicalDeviceProperties properties {};
                    vkGetPhysicalDeviceProperties(candidate, &properties);
                    device_label_ = widen_utf8(properties.deviceName);
                    return;
                }
            }
        }

        throw std::runtime_error("No Vulkan queue family with graphics and present support found");
    }

    void create_vulkan_device() {
        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_index_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        VkDeviceCreateInfo create_info {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan logical device");
        }
        vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
    }

    void create_command_objects() {
        VkCommandPoolCreateInfo pool_info {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family_index_;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan command pool");
        }

        VkCommandBufferAllocateInfo alloc_info {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool_;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &alloc_info, &command_buffer_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to allocate Vulkan command buffer");
        }
    }

    void create_sync_objects() {
        VkSemaphoreCreateInfo semaphore_info {};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_available_semaphore_) != VK_SUCCESS
            || vkCreateSemaphore(device_, &semaphore_info, nullptr, &render_finished_semaphore_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan semaphores");
        }

        VkFenceCreateInfo fence_info {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fence_info, nullptr, &in_flight_fence_) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan fence");
        }
    }

    [[nodiscard]] bool recreate_swapchain() {
        if (device_ == VK_NULL_HANDLE) {
            return false;
        }

        RECT client_rect {};
        GetClientRect(hwnd_, &client_rect);
        const std::uint32_t width = std::max<LONG>(32, client_rect.right - client_rect.left);
        const std::uint32_t height = std::max<LONG>(32, client_rect.bottom - client_rect.top);

        vkDeviceWaitIdle(device_);
        destroy_swapchain_resources();

        VkSurfaceCapabilitiesKHR capabilities {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0U) {
            return false;
        }

        std::uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

        VkSurfaceFormatKHR chosen_format = formats.empty()
            ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
            : formats.front();
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosen_format = format;
                break;
            }
        }

        std::uint32_t image_count = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0U && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        if (capabilities.currentExtent.width != UINT32_MAX) {
            swapchain_extent_ = capabilities.currentExtent;
        } else {
            swapchain_extent_.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            swapchain_extent_.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        swapchain_format_ = chosen_format.format;

        VkSwapchainCreateInfoKHR create_info {};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface_;
        create_info.minImageCount = image_count;
        create_info.imageFormat = chosen_format.format;
        create_info.imageColorSpace = chosen_format.colorSpace;
        create_info.imageExtent = swapchain_extent_;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create_info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
            return false;
        }

        std::uint32_t swapchain_image_count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, nullptr);
        swapchain_images_.resize(swapchain_image_count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, swapchain_images_.data());
        image_initialized_.assign(swapchain_images_.size(), false);

        const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(swapchain_extent_.width) * swapchain_extent_.height * 4U;
        if (!create_staging_buffer(buffer_size)) {
            return false;
        }

        needs_resize_ = false;
        return true;
    }

    [[nodiscard]] bool create_staging_buffer(VkDeviceSize size) {
        VkBufferCreateInfo create_info {};
        create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create_info.size = size;
        create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &create_info, nullptr, &staging_buffer_.buffer) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements memory_requirements {};
        vkGetBufferMemoryRequirements(device_, staging_buffer_.buffer, &memory_requirements);

        VkPhysicalDeviceMemoryProperties memory_properties {};
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        std::optional<std::uint32_t> selected_type;
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            const bool supported = (memory_requirements.memoryTypeBits & (1U << index)) != 0U;
            const bool host_visible = (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
            const bool host_coherent = (memory_properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
            if (supported && host_visible && host_coherent) {
                selected_type = index;
                break;
            }
        }
        if (!selected_type.has_value()) {
            return false;
        }

        VkMemoryAllocateInfo alloc_info {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = memory_requirements.size;
        alloc_info.memoryTypeIndex = selected_type.value();
        if (vkAllocateMemory(device_, &alloc_info, nullptr, &staging_buffer_.memory) != VK_SUCCESS) {
            return false;
        }
        if (vkBindBufferMemory(device_, staging_buffer_.buffer, staging_buffer_.memory, 0) != VK_SUCCESS) {
            return false;
        }
        staging_buffer_.size = size;
        return true;
    }

    void destroy_swapchain_resources() {
        if (staging_buffer_.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, staging_buffer_.memory, nullptr);
            staging_buffer_.memory = VK_NULL_HANDLE;
        }
        if (staging_buffer_.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, staging_buffer_.buffer, nullptr);
            staging_buffer_.buffer = VK_NULL_HANDLE;
        }
        staging_buffer_.size = 0;
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapchain_images_.clear();
        image_initialized_.clear();
    }

    void cleanup_vulkan() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            destroy_swapchain_resources();
            if (image_available_semaphore_ != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, image_available_semaphore_, nullptr);
                image_available_semaphore_ = VK_NULL_HANDLE;
            }
            if (render_finished_semaphore_ != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, render_finished_semaphore_, nullptr);
                render_finished_semaphore_ = VK_NULL_HANDLE;
            }
            if (in_flight_fence_ != VK_NULL_HANDLE) {
                vkDestroyFence(device_, in_flight_fence_, nullptr);
                in_flight_fence_ = VK_NULL_HANDLE;
            }
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
                command_pool_ = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        physical_device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        queue_family_index_ = 0;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_index_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_ {0U, 0U};
    std::vector<VkImage> swapchain_images_;
    std::vector<bool> image_initialized_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkSemaphore image_available_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_semaphore_ = VK_NULL_HANDLE;
    VkFence in_flight_fence_ = VK_NULL_HANDLE;
    BufferHandle staging_buffer_;
#endif

    HWND hwnd_ = nullptr;
    SoftwareImage last_image_;
    qbit_miner::StereoPcmFrame last_audio_frame_;
    qbit_miner::SubstrateComputeRuntime compute_runtime_;
    std::wstring device_label_;
    bool has_compute_audio_ = false;
    bool vulkan_enabled_ = false;
    bool needs_resize_ = false;
};

struct MetricRow {
    std::wstring label;
    std::wstring value;
};

class UserSettingsDialog {
public:
    bool show_modal(
        HINSTANCE instance,
        HWND owner,
        const qbit_miner::MiningConnectionSettings& initial_settings,
        const qbit_miner::MiningConnectionStatus& initial_status,
        qbit_miner::MiningConnectionSettings* updated_settings
    ) {
        register_window_class(instance);
        instance_ = instance;
        owner_ = owner;
        initial_settings_ = initial_settings;
        initial_status_ = initial_status;
        result_ = initial_settings;

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            kWindowClassName,
            L"User Settings",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kDialogWidth,
            kDialogHeight,
            owner,
            nullptr,
            instance,
            this
        );
        if (hwnd_ == nullptr) {
            return false;
        }

        if (owner_ != nullptr) {
            EnableWindow(owner_, FALSE);
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG message {};
        while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (owner_ != nullptr) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }

        if (accepted_ && updated_settings != nullptr) {
            *updated_settings = result_;
        }
        return accepted_;
    }

private:
    static constexpr const wchar_t* kWindowClassName = L"QBitMinerUserSettingsDialog";
    static constexpr int kDialogWidth = 560;
    static constexpr int kDialogHeight = 748;

    static constexpr int kPolicyComboId = 3001;
    static constexpr int kHostEditId = 3002;
    static constexpr int kPortEditId = 3003;
    static constexpr int kPayoutEditId = 3004;
    static constexpr int kWorkerEditId = 3005;
    static constexpr int kPasswordEditId = 3006;
    static constexpr int kRateEditId = 3007;
    static constexpr int kDerivedLoginValueId = 3008;
    static constexpr int kPolicyNoteValueId = 3009;
    static constexpr int kRuntimeNoteValueId = 3010;
    static constexpr int kCurrentStateValueId = 3011;
    static constexpr int kPayoutLabelId = 3012;
    static constexpr int kLiveSubmitCheckId = 3013;
    static constexpr int kPreviewValidationCheckId = 3014;
    static constexpr int kRunMinutesEditId = 3015;
    static constexpr int kRunIndefiniteCheckId = 3016;
    static constexpr int kWorkerCountEditId = 3017;

    static void register_window_class(HINSTANCE instance) {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSW window_class {};
        window_class.lpfnWndProc = &UserSettingsDialog::WindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClassName;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&window_class);
        registered = true;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<UserSettingsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }

        auto* self = reinterpret_cast<UserSettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->handle_message(message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    void on_create() {
        dialog_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        create_controls();
        populate_controls();
        center_over_owner();
    }

    void on_command(UINT control_id, UINT notify_code, HWND) {
        if (control_id == IDOK) {
            save_and_close();
            return;
        }
        if (control_id == IDCANCEL) {
            DestroyWindow(hwnd_);
            return;
        }

        if (control_id == kPolicyComboId && notify_code == CBN_SELCHANGE) {
            update_policy_copy();
            update_derived_login_preview();
            return;
        }

        if ((control_id == kPayoutEditId || control_id == kWorkerEditId) && notify_code == EN_CHANGE) {
            update_derived_login_preview();
        }
        if (control_id == kLiveSubmitCheckId && notify_code == BN_CLICKED) {
            if (live_submit_enabled()) {
                SendMessageW(preview_validation_check_, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            update_runtime_note();
            return;
        }
        if (control_id == kRunIndefiniteCheckId && notify_code == BN_CLICKED) {
            update_run_duration_enabled_state();
            return;
        }
        if (control_id == kPreviewValidationCheckId && notify_code == BN_CLICKED) {
            if (preview_validation_enabled()) {
                SendMessageW(live_submit_check_, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            update_runtime_note();
        }
    }

    HWND create_control(
        DWORD ex_style,
        const wchar_t* class_name,
        const wchar_t* text,
        DWORD style,
        int x,
        int y,
        int width,
        int height,
        int control_id
    ) {
        HWND control = CreateWindowExW(
            ex_style,
            class_name,
            text,
            WS_CHILD | WS_VISIBLE | style,
            x,
            y,
            width,
            height,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
            instance_,
            nullptr
        );
        if (control != nullptr && dialog_font_ != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog_font_), TRUE);
        }
        return control;
    }

    void create_controls() {
        const int label_left = 20;
        const int input_left = 188;
        const int input_width = 320;

        create_control(0, L"STATIC", L"Pool Policy", SS_LEFT, label_left, 20, 150, 20, -1);
        policy_combo_ = create_control(0, L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, input_left, 16, 220, 240, kPolicyComboId);

        create_control(0, L"STATIC", L"Pool Host", SS_LEFT, label_left, 58, 150, 20, -1);
        host_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 54, input_width, 24, kHostEditId);

        create_control(0, L"STATIC", L"Pool Port", SS_LEFT, label_left, 96, 150, 20, -1);
        port_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 92, 120, 24, kPortEditId);

        payout_label_ = create_control(0, L"STATIC", L"Payout Wallet", SS_LEFT, label_left, 134, 150, 20, kPayoutLabelId);
        payout_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 130, input_width, 24, kPayoutEditId);

        create_control(0, L"STATIC", L"Rig / Worker ID", SS_LEFT, label_left, 172, 150, 20, -1);
        worker_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 168, input_width, 24, kWorkerEditId);

        create_control(0, L"STATIC", L"Worker Password", SS_LEFT, label_left, 210, 150, 20, -1);
        password_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 206, input_width, 24, kPasswordEditId);

        create_control(0, L"STATIC", L"Max Requests / Second", SS_LEFT, label_left, 248, 160, 20, -1);
        rate_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 244, 120, 24, kRateEditId);

        create_control(0, L"STATIC", L"Active Workers", SS_LEFT, label_left, 286, 150, 20, -1);
        worker_count_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 282, 120, 24, kWorkerCountEditId);

        create_control(0, L"STATIC", L"Run Minutes", SS_LEFT, label_left, 324, 150, 20, -1);
        run_minutes_edit_ = create_control(WS_EX_CLIENTEDGE, L"EDIT", nullptr, ES_AUTOHSCROLL | WS_TABSTOP, input_left, 320, 120, 24, kRunMinutesEditId);

        run_indefinite_check_ = create_control(
            0,
            L"BUTTON",
            L"Run Indefinitely",
            BS_AUTOCHECKBOX | WS_TABSTOP,
            input_left,
            348,
            input_width,
            24,
            kRunIndefiniteCheckId
        );

        live_submit_check_ = create_control(
            0,
            L"BUTTON",
            L"Allow Live Pool Submit",
            BS_AUTOCHECKBOX | WS_TABSTOP,
            input_left,
            386,
            input_width,
            24,
            kLiveSubmitCheckId
        );

        preview_validation_check_ = create_control(
            0,
            L"BUTTON",
            L"Online Phase-Guided Header Resonance",
            BS_AUTOCHECKBOX | WS_TABSTOP,
            input_left,
            414,
            input_width,
            24,
            kPreviewValidationCheckId
        );

        create_control(0, L"STATIC", L"Derived Authorize Login", SS_LEFT, label_left, 452, 160, 20, -1);
        derived_login_value_ = create_control(0, L"STATIC", nullptr, SS_LEFT | SS_NOPREFIX, input_left, 450, input_width, 36, kDerivedLoginValueId);

        create_control(0, L"STATIC", L"Current Runtime State", SS_LEFT, label_left, 498, 160, 20, -1);
        current_state_value_ = create_control(0, L"STATIC", nullptr, SS_LEFT, input_left, 496, input_width, 44, kCurrentStateValueId);

        create_control(0, L"STATIC", L"Pool Requirements", SS_LEFT, label_left, 552, 160, 20, -1);
        policy_note_value_ = create_control(0, L"STATIC", nullptr, SS_LEFT, input_left, 550, input_width, 78, kPolicyNoteValueId);

        create_control(0, L"STATIC", L"Reward Path Gate", SS_LEFT, label_left, 636, 160, 20, -1);
        runtime_note_value_ = create_control(0, L"STATIC", nullptr, SS_LEFT, input_left, 634, input_width, 78, kRuntimeNoteValueId);

        create_control(0, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, input_left + 140, 706, 90, 28, IDOK);
        create_control(0, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, input_left + 238, 706, 90, 28, IDCANCEL);
    }

    void populate_controls() {
        SendMessageW(policy_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2Miners"));
        SendMessageW(policy_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"F2Pool"));
        SendMessageW(
            policy_combo_,
            CB_SETCURSEL,
            initial_settings_.pool_policy == qbit_miner::MiningPoolPolicy::F2Pool ? 1 : 0,
            0
        );

        SetWindowTextW(host_edit_, widen_utf8(initial_settings_.pool_host).c_str());
        SetWindowTextW(port_edit_, std::to_wstring(initial_settings_.pool_port).c_str());
        SetWindowTextW(payout_edit_, widen_utf8(initial_settings_.payout_address).c_str());
        SetWindowTextW(worker_edit_, widen_utf8(initial_settings_.worker_id).c_str());
        SetWindowTextW(password_edit_, widen_utf8(initial_settings_.worker_password).c_str());
        SendMessageW(live_submit_check_, BM_SETCHECK, initial_settings_.allow_live_submit ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(
            preview_validation_check_,
            BM_SETCHECK,
            (!initial_settings_.allow_live_submit && initial_settings_.phase_guided_preview_test_mode) ? BST_CHECKED : BST_UNCHECKED,
            0
        );

        std::wostringstream rate_text;
        rate_text << std::fixed << std::setprecision(2) << initial_settings_.max_requests_per_second;
        SetWindowTextW(rate_edit_, rate_text.str().c_str());
        SetWindowTextW(worker_count_edit_, std::to_wstring(initial_settings_.allowed_worker_count).c_str());
        std::wostringstream run_duration_text;
        run_duration_text << std::fixed << std::setprecision(initial_settings_.run_duration_minutes >= 10.0 ? 1 : 2)
                          << initial_settings_.run_duration_minutes;
        SetWindowTextW(run_minutes_edit_, run_duration_text.str().c_str());
        SendMessageW(
            run_indefinite_check_,
            BM_SETCHECK,
            initial_settings_.run_indefinitely ? BST_CHECKED : BST_UNCHECKED,
            0
        );

        const std::wstring state_text = L"State: " + widen_utf8(initial_status_.connection_state)
            + L"\r\n" + widen_utf8(initial_status_.status_message);
        SetWindowTextW(current_state_value_, state_text.c_str());

        update_policy_copy();
        update_runtime_note();
        update_derived_login_preview();
        update_run_duration_enabled_state();
    }

    void update_policy_copy() {
        const qbit_miner::MiningPoolPolicy policy = selected_policy();
        SetWindowTextW(
            payout_label_,
            policy == qbit_miner::MiningPoolPolicy::F2Pool ? L"Wallet / F2Pool Account" : L"Payout Wallet"
        );
        SetWindowTextW(policy_note_value_, pool_policy_requirements_text(policy).c_str());
    }

    void update_derived_login_preview() {
        SetWindowTextW(
            derived_login_value_,
            build_worker_login_preview(selected_policy(), read_control_text(payout_edit_), read_control_text(worker_edit_)).c_str()
        );
    }

    void update_runtime_note() {
        qbit_miner::MiningConnectionSettings preview_settings = initial_settings_;
        preview_settings.allow_live_submit = live_submit_enabled();
        preview_settings.phase_guided_preview_test_mode = preview_validation_enabled() && !preview_settings.allow_live_submit;
        SetWindowTextW(runtime_note_value_, runtime_reward_gate_text(preview_settings).c_str());
    }

    void update_run_duration_enabled_state() {
        EnableWindow(run_minutes_edit_, !run_indefinite_enabled());
    }

    void save_and_close() {
        const std::wstring host = trim_copy(read_control_text(host_edit_));
        const std::wstring payout_address = trim_copy(read_control_text(payout_edit_));
        const std::wstring worker_id = trim_copy(read_control_text(worker_edit_));
        std::wstring worker_password = trim_copy(read_control_text(password_edit_));
        const std::wstring port_text = trim_copy(read_control_text(port_edit_));
        const std::wstring rate_text = trim_copy(read_control_text(rate_edit_));
        const std::wstring worker_count_text = trim_copy(read_control_text(worker_count_edit_));
        const std::wstring run_minutes_text = trim_copy(read_control_text(run_minutes_edit_));

        if (host.empty()) {
            show_validation_error(L"Pool host is required.");
            return;
        }
        if (payout_address.empty()) {
            show_validation_error(L"A payout wallet or pool account is required.");
            return;
        }
        if (worker_id.empty()) {
            show_validation_error(L"A rig / worker id is required.");
            return;
        }
        if (selected_policy() == qbit_miner::MiningPoolPolicy::F2Pool && !is_f2pool_worker_id_valid(worker_id)) {
            show_validation_error(L"For F2Pool, the worker id must use letters and digits only. Do not include dots or underscores; the control center adds the account.worker separator automatically.");
            return;
        }

        const std::optional<std::uint16_t> pool_port = parse_port(port_text);
        if (!pool_port.has_value()) {
            show_validation_error(L"Pool port must be a number between 1 and 65535.");
            return;
        }

        const std::optional<double> max_requests = parse_rate(rate_text);
        if (!max_requests.has_value()) {
            show_validation_error(L"Max requests / second must be a positive number.");
            return;
        }
        const std::optional<std::size_t> worker_count = parse_count(worker_count_text);
        if (!worker_count.has_value()) {
            show_validation_error(L"Active workers must be a number between 1 and 4.");
            return;
        }

        const std::optional<double> run_minutes = parse_positive_number(run_minutes_text);
        if (!run_indefinite_enabled() && !run_minutes.has_value()) {
            show_validation_error(L"Run minutes must be a positive number when timed run mode is enabled.");
            return;
        }

        if (worker_password.empty()) {
            worker_password = L"x";
        }

        result_.pool_policy = selected_policy();
        result_.pool_host = narrow_utf16(host);
        result_.pool_port = pool_port.value();
        result_.payout_address = narrow_utf16(payout_address);
        result_.worker_id = narrow_utf16(worker_id);
        result_.worker_password = narrow_utf16(worker_password);
        result_.allow_live_submit = live_submit_enabled();
        result_.phase_guided_preview_test_mode = preview_validation_enabled() && !result_.allow_live_submit;
        result_.max_requests_per_second = std::clamp(max_requests.value(), 0.05, 2.0);
        result_.allowed_worker_count = std::clamp<std::size_t>(worker_count.value(), 1U, qbit_miner::kStratumWorkerSlotCount);
        result_.run_indefinitely = run_indefinite_enabled();
        result_.run_duration_minutes = run_minutes.has_value() ? run_minutes.value() : initial_settings_.run_duration_minutes;

        accepted_ = true;
        DestroyWindow(hwnd_);
    }

    qbit_miner::MiningPoolPolicy selected_policy() const {
        const LRESULT selection = SendMessageW(policy_combo_, CB_GETCURSEL, 0, 0);
        return selection == 1 ? qbit_miner::MiningPoolPolicy::F2Pool : qbit_miner::MiningPoolPolicy::TwoMiners;
    }

    bool live_submit_enabled() const {
        return SendMessageW(live_submit_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    bool preview_validation_enabled() const {
        return SendMessageW(preview_validation_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    bool run_indefinite_enabled() const {
        return SendMessageW(run_indefinite_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    std::wstring read_control_text(HWND control) const {
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
        if (length > 0) {
            GetWindowTextW(control, text.data(), length + 1);
        }
        text.resize(static_cast<std::size_t>(length));
        return text;
    }

    std::optional<std::uint16_t> parse_port(const std::wstring& text) const {
        try {
            const unsigned long value = std::stoul(narrow_utf16(text));
            if (value == 0UL || value > 65535UL) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<double> parse_rate(const std::wstring& text) const {
        try {
            const double value = std::stod(narrow_utf16(text));
            if (!(value > 0.0)) {
                return std::nullopt;
            }
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<double> parse_positive_number(const std::wstring& text) const {
        try {
            const double value = std::stod(narrow_utf16(text));
            if (!(value > 0.0)) {
                return std::nullopt;
            }
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::size_t> parse_count(const std::wstring& text) const {
        try {
            const unsigned long value = std::stoul(narrow_utf16(text));
            if (value == 0UL || value > static_cast<unsigned long>(qbit_miner::kStratumWorkerSlotCount)) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    void show_validation_error(const std::wstring& text) const {
        MessageBoxW(hwnd_, text.c_str(), L"User Settings", MB_OK | MB_ICONWARNING);
    }

    void center_over_owner() const {
        RECT owner_rect {};
        if (owner_ != nullptr) {
            GetWindowRect(owner_, &owner_rect);
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
        }

        const int owner_width = owner_rect.right - owner_rect.left;
        const int owner_height = owner_rect.bottom - owner_rect.top;
        const int x = owner_rect.left + std::max(0, (owner_width - kDialogWidth) / 2);
        const int y = owner_rect.top + std::max(0, (owner_height - kDialogHeight) / 2);
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    }

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT dialog_font_ = nullptr;
    HWND policy_combo_ = nullptr;
    HWND host_edit_ = nullptr;
    HWND port_edit_ = nullptr;
    HWND payout_label_ = nullptr;
    HWND payout_edit_ = nullptr;
    HWND worker_edit_ = nullptr;
    HWND password_edit_ = nullptr;
    HWND rate_edit_ = nullptr;
    HWND worker_count_edit_ = nullptr;
    HWND run_minutes_edit_ = nullptr;
    HWND run_indefinite_check_ = nullptr;
    HWND live_submit_check_ = nullptr;
    HWND preview_validation_check_ = nullptr;
    HWND derived_login_value_ = nullptr;
    HWND policy_note_value_ = nullptr;
    HWND runtime_note_value_ = nullptr;
    HWND current_state_value_ = nullptr;
    bool accepted_ = false;
    qbit_miner::MiningConnectionSettings initial_settings_;
    qbit_miner::MiningConnectionStatus initial_status_;
    qbit_miner::MiningConnectionSettings result_;
};

class ControlCenterWindow {
public:
    int run(HINSTANCE instance, int command_show) {
        register_window_class(instance);
        if (!create_window(instance, command_show)) {
            return 1;
        }

        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static constexpr const wchar_t* kWindowClassName = L"QBitMinerControlCenterWindow";
    static constexpr const wchar_t* kMetricDockWindowClassName = L"QBitMinerMetricDockWindow";

    struct MetricDockWindowState {
        ControlCenterWindow* owner = nullptr;
        std::size_t panel_index = 0;
        HWND hwnd = nullptr;
        RECT dock_back_rect {};
    };

    static void register_window_class(HINSTANCE instance) {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSW window_class {};
        window_class.lpfnWndProc = &ControlCenterWindow::WindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClassName;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&window_class);
        registered = true;
    }

    static void register_metric_dock_window_class(HINSTANCE instance) {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSW window_class {};
        window_class.lpfnWndProc = &ControlCenterWindow::MetricDockWindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kMetricDockWindowClassName;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&window_class);
        registered = true;
    }

    bool create_window(HINSTANCE instance, int command_show) {
        instance_ = instance;
        hwnd_ = CreateWindowExW(
            0,
            kWindowClassName,
            L"Quantum Miner Control Center",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kWindowWidth,
            kWindowHeight,
            nullptr,
            nullptr,
            instance,
            this);
        if (hwnd_ == nullptr) {
            return false;
        }

        ShowWindow(hwnd_, command_show);
        UpdateWindow(hwnd_);
        return true;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<ControlCenterWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }

        auto* self = reinterpret_cast<ControlCenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->handle_message(message, wparam, lparam);
    }

    static LRESULT CALLBACK MetricDockWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* state = static_cast<MetricDockWindowState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            if (state != nullptr) {
                state->hwnd = hwnd;
            }
        }

        auto* state = reinterpret_cast<MetricDockWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state == nullptr || state->owner == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return state->owner->handle_metric_dock_message(*state, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_TIMER:
            on_timer();
            return 0;
        case WM_LBUTTONUP:
            on_left_button_up(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wparam));
            return 0;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_DESTROY:
            on_destroy();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    void on_create() {
        register_metric_dock_window_class(instance_);
        viewport_.set_kernel_iteration_observer([this](const qbit_miner::GpuKernelIterationEvent& event) {
            service_.ingest_gpu_kernel_iteration(event);
        });
        viewport_.set_mining_validation_observer([this](const qbit_miner::MiningValidationSnapshot& snapshot) {
            service_.ingest_gpu_mining_validation(snapshot);
        });
        service_.start();
        snapshot_ = service_.snapshot();
        if (const auto startup_profile = load_startup_mining_profile(control_center_startup_profile_path())) {
            service_.set_mining_settings(startup_profile->settings);
            if (startup_profile->auto_connect || startup_profile->auto_start_run) {
                service_.connect_pool();
            }
            if (startup_profile->auto_start_run) {
                service_.start_mining_session();
            }
            snapshot_ = service_.snapshot();
        }
        audio_output_.initialize();
        create_fonts();
        layout();
        SetTimer(hwnd_, kUiTimerId, 100, nullptr);
    }

    void on_destroy() {
        KillTimer(hwnd_, kUiTimerId);
        for (auto& dock_window : metric_dock_windows_) {
            if (dock_window.hwnd != nullptr) {
                DestroyWindow(dock_window.hwnd);
                dock_window.hwnd = nullptr;
            }
        }
        service_.stop();
        audio_output_.shutdown();
        destroy_fonts();
        PostQuitMessage(0);
    }

    void on_timer() {
        snapshot_ = service_.snapshot();
        viewport_.present(snapshot_.viewport);
        qbit_miner::StereoPcmFrame audio_frame = viewport_.has_compute_audio()
            ? viewport_.audio_frame()
            : qbit_miner::synthesize_field_audio(snapshot_.viewport);
        audio_output_.submit(audio_frame, snapshot_.audio_enabled);
        for (const auto& dock_window : metric_dock_windows_) {
            if (dock_window.hwnd != nullptr) {
                InvalidateRect(dock_window.hwnd, nullptr, FALSE);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void layout() {
        if (hwnd_ == nullptr) {
            return;
        }

        RECT client {};
        GetClientRect(hwnd_, &client);
        file_rect_ = {16, 12, 82, 40};
        edit_rect_ = {90, 12, 156, 40};
        window_rect_ = {164, 12, 248, 40};

        operator_panel_rect_ = {16, kTopBarHeight + 16, kLeftPaneWidth - 16, kTopBarHeight + 16 + kOperatorPanelHeight};
        const int panel_inner_left = operator_panel_rect_.left + 12;
        const int panel_inner_right = operator_panel_rect_.right - 12;
        const int panel_inner_width = panel_inner_right - panel_inner_left;
        const int button_gap = 10;
        const int button_left = panel_inner_left;
        const int button_width = (panel_inner_width - button_gap) / 2;
        const int first_row_top = operator_panel_rect_.top + 30;
        const int second_row_top = first_row_top + 34;
        const int third_row_top = second_row_top + 34;
        operator_button_rects_[0] = {button_left, first_row_top, button_left + button_width, first_row_top + 28};
        operator_button_rects_[1] = {button_left + button_width + button_gap, first_row_top, panel_inner_right, first_row_top + 28};
        operator_button_rects_[2] = {button_left, second_row_top, button_left + button_width, second_row_top + 28};
        operator_button_rects_[3] = {button_left + button_width + button_gap, second_row_top, panel_inner_right, second_row_top + 28};

        const int mode_gap = 8;
        const int mode_width = (panel_inner_width - (mode_gap * 2)) / 3;
        session_mode_button_rects_[0] = {panel_inner_left, third_row_top, panel_inner_left + mode_width, third_row_top + 24};
        session_mode_button_rects_[1] = {
            panel_inner_left + mode_width + mode_gap,
            third_row_top,
            panel_inner_left + (mode_width * 2) + mode_gap,
            third_row_top + 24,
        };
        session_mode_button_rects_[2] = {
            panel_inner_left + (mode_width * 2) + (mode_gap * 2),
            third_row_top,
            panel_inner_right,
            third_row_top + 24,
        };

        metric_workspace_rect_ = {
            16,
            operator_panel_rect_.bottom + 14,
            kLeftPaneWidth - 16,
            client.bottom - 16,
        };
        const int workspace_inner_width = (metric_workspace_rect_.right - metric_workspace_rect_.left) - 24;
        const std::size_t tab_count = std::min<std::size_t>(metric_tab_rects_.size(), std::max<std::size_t>(1U, snapshot_.metric_panels.size()));
        const int tab_width = workspace_inner_width / static_cast<int>(std::max<std::size_t>(1U, tab_count));
        for (std::size_t index = 0; index < metric_tab_rects_.size(); ++index) {
            const int left = metric_workspace_rect_.left + 12 + static_cast<int>(index) * tab_width;
            const int right = index + 1U == tab_count
                ? metric_workspace_rect_.right - 12
                : left + tab_width - 6;
            metric_tab_rects_[index] = {left, metric_workspace_rect_.top + 42, right, metric_workspace_rect_.top + 72};
        }
        metric_panel_action_rect_ = {
            metric_workspace_rect_.right - 114,
            metric_workspace_rect_.top + 10,
            metric_workspace_rect_.right - 12,
            metric_workspace_rect_.top + 36,
        };

        const int right_edge = client.right - 16;
        reward_rects_[0] = {right_edge - 330, 12, right_edge - 226, 40};
        reward_rects_[1] = {right_edge - 220, 12, right_edge - 116, 40};
        reward_rects_[2] = {right_edge - 110, 12, right_edge - 6, 40};

        const int viewport_left = std::max<int>(kLeftPaneWidth + 24, static_cast<int>(client.right - kViewportMinWidth - 16));
        connection_card_rect_ = {
            viewport_left,
            kTopBarHeight + 16,
            client.right - 16,
            kTopBarHeight + 16 + kConnectionCardHeight,
        };
        share_card_rect_ = {
            viewport_left,
            connection_card_rect_.bottom + 16,
            client.right - 16,
            connection_card_rect_.bottom + 16 + kShareCardHeight,
        };
        viewport_rect_ = {
            viewport_left,
            share_card_rect_.bottom + 16,
            client.right - 16,
            client.bottom - 16,
        };

        if (viewport_created_) {
            viewport_.resize(viewport_rect_);
        } else if (client.right > viewport_left && client.bottom > (kTopBarHeight + 16)) {
            viewport_created_ = viewport_.create(instance_, hwnd_, viewport_rect_);
        }
    }

    void on_left_button_up(int x, int y) {
        const POINT point {x, y};
        if (point_in_rect(point, file_rect_)) {
            show_file_menu();
            return;
        }
        if (point_in_rect(point, edit_rect_)) {
            show_edit_menu();
            return;
        }
        if (point_in_rect(point, window_rect_)) {
            show_window_menu();
            return;
        }
        if (point_in_rect(point, operator_button_rects_[0])) {
            service_.start_mining_session();
            snapshot_ = service_.snapshot();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (point_in_rect(point, operator_button_rects_[1])) {
            service_.stop_mining_session();
            snapshot_ = service_.snapshot();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (point_in_rect(point, operator_button_rects_[2])) {
            service_.connect_pool();
            snapshot_ = service_.snapshot();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (point_in_rect(point, operator_button_rects_[3])) {
            service_.disconnect_pool();
            snapshot_ = service_.snapshot();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (point_in_rect(point, session_mode_button_rects_[0])) {
            apply_session_mode(false, false);
            return;
        }
        if (point_in_rect(point, session_mode_button_rects_[1])) {
            apply_session_mode(false, true);
            return;
        }
        if (point_in_rect(point, session_mode_button_rects_[2])) {
            apply_session_mode(true, false);
            return;
        }
        if (point_in_rect(point, reward_rects_[0])) {
            service_.set_reward_unit(qbit_miner::RewardIntervalUnit::PerMinute);
            return;
        }
        if (point_in_rect(point, reward_rects_[1])) {
            service_.set_reward_unit(qbit_miner::RewardIntervalUnit::PerHour);
            return;
        }
        if (point_in_rect(point, reward_rects_[2])) {
            service_.set_reward_unit(qbit_miner::RewardIntervalUnit::PerDay);
            return;
        }

        for (std::size_t index = 0; index < snapshot_.metric_panels.size() && index < metric_tab_rects_.size(); ++index) {
            if (!point_in_rect(point, metric_tab_rects_[index])) {
                continue;
            }
            if (metric_panel_is_undocked(index)) {
                focus_metric_panel_window(index);
            } else {
                active_metric_panel_index_ = index;
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return;
        }

        if (point_in_rect(point, metric_panel_action_rect_) && !snapshot_.metric_panels.empty()) {
            if (metric_panel_is_undocked(active_metric_panel_index_)) {
                focus_metric_panel_window(active_metric_panel_index_);
            } else {
                undock_metric_panel(active_metric_panel_index_);
            }
            return;
        }
    }

    void on_command(UINT command_id) {
        switch (command_id) {
        case kCommandFileExportCalibration:
            handle_export_calibration();
            break;
        case kCommandFileExportValidation:
            handle_export_validation();
            break;
        case kCommandFileSaveViewport:
            handle_save_viewport();
            break;
        case kCommandFileExit:
            DestroyWindow(hwnd_);
            break;
        case kCommandEditCopyMetrics:
            copy_metrics_to_clipboard();
            break;
        case kCommandEditUserSettings:
            handle_user_settings();
            break;
        case kCommandEditToggleAudio:
            service_.set_audio_enabled(!service_.audio_enabled());
            break;
        case kCommandEditTogglePause:
            service_.set_paused(!service_.paused());
            break;
        case kCommandWindowResetLayout:
            service_.set_compact_layout(false);
            service_.set_always_on_top(false);
            snapshot_ = service_.snapshot();
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            layout();
            break;
        case kCommandWindowToggleCompact:
            service_.set_compact_layout(!service_.compact_layout());
            snapshot_ = service_.snapshot();
            InvalidateRect(hwnd_, nullptr, TRUE);
            break;
        case kCommandWindowAlwaysOnTop:
            service_.set_always_on_top(!service_.always_on_top());
            snapshot_ = service_.snapshot();
            SetWindowPos(hwnd_, snapshot_.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            break;
        default:
            break;
        }
    }

    void on_paint() {
        PAINTSTRUCT paint {};
        HDC window_dc = BeginPaint(hwnd_, &paint);

        RECT client {};
        GetClientRect(hwnd_, &client);
        HDC back_buffer_dc = CreateCompatibleDC(window_dc);
        HBITMAP back_bitmap = CreateCompatibleBitmap(window_dc, std::max(1L, client.right), std::max(1L, client.bottom));
        HGDIOBJ old_bitmap = SelectObject(back_buffer_dc, back_bitmap);

        fill_vertical_gradient(back_buffer_dc, client, RGB(14, 16, 20), RGB(8, 10, 14));
        draw_top_bar(back_buffer_dc, client);
        draw_metric_sections(back_buffer_dc, client);
        draw_connection_status_card(back_buffer_dc);
        draw_share_status_card(back_buffer_dc);
        draw_viewport_frame(back_buffer_dc, client);

        BitBlt(window_dc, 0, 0, client.right, client.bottom, back_buffer_dc, 0, 0, SRCCOPY);

        SelectObject(back_buffer_dc, old_bitmap);
        DeleteObject(back_bitmap);
        DeleteDC(back_buffer_dc);
        EndPaint(hwnd_, &paint);
    }

    void draw_top_bar(HDC hdc, const RECT& client) {
        RECT top_bar {0, 0, client.right, kTopBarHeight};
        fill_vertical_gradient(hdc, top_bar, RGB(82, 83, 86), RGB(44, 47, 52));
        RECT underbar {0, kTopBarHeight - 1, client.right, kTopBarHeight};
        fill_rect(hdc, underbar, RGB(156, 161, 169));

        draw_button(hdc, file_rect_, L"File", false);
        draw_button(hdc, edit_rect_, L"Edit", false);
        draw_button(hdc, window_rect_, L"Window", false);

        const auto current_unit = service_.reward_unit();
        draw_button(hdc, reward_rects_[0], L"Per Minute", current_unit == qbit_miner::RewardIntervalUnit::PerMinute);
        draw_button(hdc, reward_rects_[1], L"Per Hour", current_unit == qbit_miner::RewardIntervalUnit::PerHour);
        draw_button(hdc, reward_rects_[2], L"Per Day", current_unit == qbit_miner::RewardIntervalUnit::PerDay);

        SelectObject(hdc, title_font_);
        RECT title_rect {270, 6, client.right - 360, 34};
        draw_inset_text(
            hdc,
            L"Quantum Miner Control Center",
            title_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE,
            RGB(232, 235, 240),
            RGB(12, 12, 14),
            RGB(16, 16, 18));

        SelectObject(hdc, body_font_);
        RECT status_rect {270, 30, client.right - 360, 54};
        const std::wstring live_tag = snapshot_.live ? L"Live" : L"Deterministic";
        const std::wstring pool_policy = widen_utf8(snapshot_.mining_status.pool_policy_label);
        const std::wstring worker_login = snapshot_.mining_status.derived_worker_name.empty()
            ? L"unconfigured"
            : widen_utf8(snapshot_.mining_status.derived_worker_name);
        const std::wstring stratum_state = widen_utf8(snapshot_.mining_status.connection_state);
        const std::wstring runtime_state = snapshot_.mining_runtime_running ? L"Run Active" : L"Run Idle";
        const std::wstring accepted_shares = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.accepted_submit_count)
            : L"0";
        const std::wstring run_timer = run_timer_status_text();
        const std::wstring status_text = live_tag
            + L" | " + widen_utf8(snapshot_.metrics.status_line)
            + L" | Mining=" + runtime_state
            + L" | Pool=" + pool_policy
            + L" | Login=" + worker_login
            + L" | Stratum=" + stratum_state
            + L" | Timer=" + run_timer
            + L" | Shares=" + accepted_shares
            + L" | Control Source=control_surface_firmware.freq.md"
            + L" | Viewport Source=runtime_substrate_viewport.freq.md"
            + L" | Viewport=" + viewport_.device_label();
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(223, 228, 235));
        DrawTextW(hdc, status_text.c_str(), static_cast<int>(status_text.size()), &status_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void draw_metric_sections(HDC hdc, const RECT& client) {
        (void)client;
        draw_operator_panel(hdc, operator_panel_rect_);
        draw_metric_workspace(hdc, metric_workspace_rect_);
    }

    bool metric_panel_is_undocked(std::size_t panel_index) const {
        return panel_index < metric_dock_windows_.size() && metric_dock_windows_[panel_index].hwnd != nullptr;
    }

    std::optional<std::size_t> first_docked_metric_panel(std::optional<std::size_t> skip = std::nullopt) const {
        for (std::size_t index = 0; index < snapshot_.metric_panels.size(); ++index) {
            if (skip.has_value() && skip.value() == index) {
                continue;
            }
            if (!metric_panel_is_undocked(index)) {
                return index;
            }
        }
        return std::nullopt;
    }

    const qbit_miner::MetricHistoryPanel* metric_panel_at(std::size_t panel_index) const {
        if (panel_index >= snapshot_.metric_panels.size()) {
            return nullptr;
        }
        return &snapshot_.metric_panels[panel_index];
    }

    COLORREF metric_panel_accent(const qbit_miner::MetricHistoryPanel& panel) const {
        if (panel.panel_id == "system") {
            return RGB(150, 196, 214);
        }
        if (panel.panel_id == "mining") {
            return RGB(206, 188, 144);
        }
        if (panel.panel_id == "network") {
            return RGB(176, 190, 212);
        }
        if (panel.panel_id == "payout") {
            return RGB(168, 208, 178);
        }
        return RGB(182, 194, 209);
    }

    COLORREF metric_series_color(std::size_t series_index) const {
        static constexpr std::array<COLORREF, 6> kSeriesPalette {
            RGB(83, 192, 255),
            RGB(255, 193, 79),
            RGB(114, 224, 151),
            RGB(255, 120, 130),
            RGB(192, 153, 255),
            RGB(118, 224, 214),
        };
        return kSeriesPalette[series_index % kSeriesPalette.size()];
    }

    void draw_metric_value_block(
        HDC hdc,
        const RECT& rect,
        const qbit_miner::MetricHistorySeries& series,
        COLORREF accent
    ) {
        fill_vertical_gradient(hdc, rect, mix_color(accent, RGB(92, 94, 98), 0.20), RGB(28, 31, 36));
        stroke_rect(hdc, rect, RGB(148, 154, 164));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(14, 16, 20));

        RECT label_rect {inner.left + 10, inner.top + 8, inner.right - 10, inner.top + 26};
        SelectObject(hdc, body_font_);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(196, 202, 212));
        const std::wstring label = widen_utf8(series.label);
        DrawTextW(label_rect.right > label_rect.left ? hdc : hdc, label.c_str(), static_cast<int>(label.size()), &label_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT value_rect {inner.left + 10, inner.top + 28, inner.right - 10, inner.bottom - 8};
        SelectObject(hdc, accent_font_);
        draw_inset_text(
            hdc,
            format_metric_history_value(series),
            value_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
            RGB(234, 238, 244),
            RGB(8, 8, 10),
            RGB(22, 22, 24));
    }

    void draw_metric_graph(
        HDC hdc,
        const RECT& rect,
        const std::wstring& title,
        const std::vector<const qbit_miner::MetricHistorySeries*>& series_group
    ) {
        fill_vertical_gradient(hdc, rect, RGB(54, 57, 62), RGB(22, 24, 28));
        stroke_rect(hdc, rect, RGB(144, 150, 158));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(10, 12, 16));

        RECT title_rect {inner.left + 10, inner.top + 8, inner.right - 10, inner.top + 26};
        SelectObject(hdc, body_font_);
        draw_inset_text(hdc, title, title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(230, 234, 240), RGB(8, 8, 10), RGB(18, 18, 20));

        RECT plot_rect {inner.left + 12, inner.top + 34, inner.right - 12, inner.bottom - 16};
        if (plot_rect.right <= plot_rect.left || plot_rect.bottom <= plot_rect.top) {
            return;
        }

        for (int step = 1; step < 4; ++step) {
            const int y = plot_rect.top + ((plot_rect.bottom - plot_rect.top) * step) / 4;
            HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(38, 42, 48));
            HGDIOBJ old_pen = SelectObject(hdc, grid_pen);
            MoveToEx(hdc, plot_rect.left, y, nullptr);
            LineTo(hdc, plot_rect.right, y);
            SelectObject(hdc, old_pen);
            DeleteObject(grid_pen);
        }

        double min_value = std::numeric_limits<double>::max();
        double max_value = std::numeric_limits<double>::lowest();
        for (const auto* series : series_group) {
            if (series == nullptr) {
                continue;
            }
            if (series->samples.empty()) {
                min_value = std::min(min_value, series->current_value);
                max_value = std::max(max_value, series->current_value);
                continue;
            }
            for (const double sample : series->samples) {
                min_value = std::min(min_value, sample);
                max_value = std::max(max_value, sample);
            }
        }

        if (!(min_value <= max_value)) {
            min_value = 0.0;
            max_value = 1.0;
        }
        if (std::abs(max_value - min_value) < 1.0e-9) {
            const double pad = std::abs(max_value) > 1.0 ? std::abs(max_value) * 0.1 : 1.0;
            min_value -= pad;
            max_value += pad;
        } else {
            const double pad = (max_value - min_value) * 0.08;
            min_value -= pad;
            max_value += pad;
        }

        for (std::size_t index = 0; index < series_group.size(); ++index) {
            const auto* series = series_group[index];
            if (series == nullptr) {
                continue;
            }

            const COLORREF color = metric_series_color(index);
            HPEN pen = CreatePen(PS_SOLID, 2, color);
            HGDIOBJ old_pen = SelectObject(hdc, pen);

            const std::vector<double>& samples = series->samples.empty()
                ? std::vector<double>{series->current_value}
                : series->samples;

            for (std::size_t sample_index = 0; sample_index < samples.size(); ++sample_index) {
                const double normalized = (samples[sample_index] - min_value) / std::max(1.0e-9, max_value - min_value);
                const int x = plot_rect.left + static_cast<int>((static_cast<double>(sample_index) * (plot_rect.right - plot_rect.left - 1)) / std::max<std::size_t>(1U, samples.size() - 1U));
                const int y = plot_rect.bottom - 1 - static_cast<int>(normalized * (plot_rect.bottom - plot_rect.top - 1));
                if (sample_index == 0U) {
                    MoveToEx(hdc, x, y, nullptr);
                } else {
                    LineTo(hdc, x, y);
                }
            }

            SelectObject(hdc, old_pen);
            DeleteObject(pen);

            RECT legend_rect {plot_rect.left + 8, plot_rect.top + 4 + static_cast<int>(index) * 18, plot_rect.right - 8, plot_rect.top + 22 + static_cast<int>(index) * 18};
            HBRUSH swatch = CreateSolidBrush(color);
            RECT swatch_rect {legend_rect.left, legend_rect.top + 4, legend_rect.left + 10, legend_rect.top + 14};
            FillRect(hdc, &swatch_rect, swatch);
            DeleteObject(swatch);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(212, 218, 226));
            const std::wstring legend = widen_utf8(series->label) + L": " + format_metric_history_value(*series);
            RECT legend_text_rect {legend_rect.left + 16, legend_rect.top, legend_rect.right, legend_rect.bottom};
            DrawTextW(hdc, legend.c_str(), static_cast<int>(legend.size()), &legend_text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    void draw_metric_panel_surface(
        HDC hdc,
        const RECT& rect,
        const qbit_miner::MetricHistoryPanel& panel,
        const std::wstring& action_label,
        RECT* action_rect
    ) {
        const COLORREF accent = metric_panel_accent(panel);
        fill_vertical_gradient(hdc, rect, mix_color(accent, RGB(72, 74, 80), 0.28), RGB(30, 33, 38));
        stroke_rect(hdc, rect, RGB(178, 184, 192));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(16, 18, 22));

        RECT title_rect {inner.left + 12, inner.top + 8, inner.right - 128, inner.top + 34};
        SelectObject(hdc, accent_font_);
        draw_inset_text(hdc, widen_utf8(panel.title), title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(236, 239, 244), RGB(8, 8, 10), RGB(18, 18, 20));

        if (action_rect != nullptr) {
            *action_rect = {inner.right - 100, inner.top + 8, inner.right - 10, inner.top + 34};
            draw_button(hdc, *action_rect, action_label, false);
        }

        const int gap = 10;
        const int block_height = snapshot_.compact_layout ? 54 : 62;
        const int block_area_top = inner.top + 42;
        const int block_area_height = (block_height * 3) + (gap * 2);
        const int block_width = ((inner.right - inner.left) - 24 - gap) / 2;
        for (std::size_t index = 0; index < panel.series.size() && index < 6U; ++index) {
            const int column = static_cast<int>(index % 2U);
            const int row = static_cast<int>(index / 2U);
            const RECT block_rect {
                inner.left + 12 + column * (block_width + gap),
                block_area_top + row * (block_height + gap),
                inner.left + 12 + column * (block_width + gap) + block_width,
                block_area_top + row * (block_height + gap) + block_height,
            };
            draw_metric_value_block(hdc, block_rect, panel.series[index], accent);
        }

        RECT graph_rect_1 {inner.left + 12, block_area_top + block_area_height + 14, inner.right - 12, inner.top + ((inner.bottom - inner.top) / 2) + 36};
        RECT graph_rect_2 {inner.left + 12, graph_rect_1.bottom + 12, inner.right - 12, inner.bottom - 12};

        std::vector<const qbit_miner::MetricHistorySeries*> first_graph_series;
        std::vector<const qbit_miner::MetricHistorySeries*> second_graph_series;
        for (std::size_t index = 0; index < panel.series.size(); ++index) {
            if (index < 3U) {
                first_graph_series.push_back(&panel.series[index]);
            } else if (index < 6U) {
                second_graph_series.push_back(&panel.series[index]);
            }
        }
        draw_metric_graph(hdc, graph_rect_1, L"Trend Window A", first_graph_series);
        draw_metric_graph(hdc, graph_rect_2, L"Trend Window B", second_graph_series);
    }

    void draw_metric_workspace(HDC hdc, const RECT& rect) {
        fill_vertical_gradient(hdc, rect, RGB(72, 74, 78), RGB(26, 29, 34));
        stroke_rect(hdc, rect, RGB(178, 184, 192));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(14, 16, 20));

        RECT title_rect {inner.left + 12, inner.top + 8, inner.right - 120, inner.top + 34};
        SelectObject(hdc, accent_font_);
        draw_inset_text(hdc, L"Metric Workspaces", title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(236, 239, 244), RGB(8, 8, 10), RGB(18, 18, 20));

        for (std::size_t index = 0; index < snapshot_.metric_panels.size() && index < metric_tab_rects_.size(); ++index) {
            const qbit_miner::MetricHistoryPanel& panel = snapshot_.metric_panels[index];
            std::wstring tab_label = widen_utf8(panel.title);
            if (metric_panel_is_undocked(index)) {
                tab_label += L" *";
            }
            draw_button(hdc, metric_tab_rects_[index], tab_label, index == active_metric_panel_index_ && !metric_panel_is_undocked(index));
        }

        if (snapshot_.metric_panels.empty()) {
            RECT empty_rect {inner.left + 12, inner.top + 82, inner.right - 12, inner.bottom - 12};
            SelectObject(hdc, body_font_);
            draw_inset_text(hdc, L"Waiting for metric history surfaces.", empty_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(228, 232, 238), RGB(8, 8, 10), RGB(18, 18, 20));
            return;
        }

        std::size_t visible_index = std::min(active_metric_panel_index_, snapshot_.metric_panels.size() - 1U);
        if (metric_panel_is_undocked(visible_index)) {
            const std::optional<std::size_t> fallback_index = first_docked_metric_panel();
            if (fallback_index.has_value()) {
                visible_index = fallback_index.value();
            }
        }

        const qbit_miner::MetricHistoryPanel* panel = metric_panel_at(visible_index);
        if (panel == nullptr) {
            return;
        }

        const std::wstring action_label = metric_panel_is_undocked(active_metric_panel_index_)
            ? L"Focus"
            : L"Undock";
        draw_button(hdc, metric_panel_action_rect_, action_label, false);

        RECT content_rect {inner.left + 12, inner.top + 78, inner.right - 12, inner.bottom - 12};
        draw_metric_panel_surface(hdc, content_rect, *panel, L"", nullptr);

        if (metric_panel_is_undocked(active_metric_panel_index_) && visible_index != active_metric_panel_index_) {
            RECT notice_rect {content_rect.left + 12, content_rect.top + 10, content_rect.right - 12, content_rect.top + 28};
            SelectObject(hdc, body_font_);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(206, 212, 220));
            const std::wstring notice = L"The selected tab is undocked. Close its window to dock it back, or click Focus.";
            DrawTextW(hdc, notice.c_str(), static_cast<int>(notice.size()), &notice_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    void undock_metric_panel(std::size_t panel_index) {
        if (panel_index >= snapshot_.metric_panels.size() || metric_panel_is_undocked(panel_index)) {
            focus_metric_panel_window(panel_index);
            return;
        }

        MetricDockWindowState& state = metric_dock_windows_[panel_index];
        state.owner = this;
        state.panel_index = panel_index;

        RECT owner_rect {};
        GetWindowRect(hwnd_, &owner_rect);
        const std::wstring title = widen_utf8(snapshot_.metric_panels[panel_index].title) + L" | Docked Metric Surface";
        state.hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kMetricDockWindowClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            owner_rect.left + 28 + static_cast<int>(panel_index) * 24,
            owner_rect.top + 72 + static_cast<int>(panel_index) * 20,
            560,
            760,
            hwnd_,
            nullptr,
            instance_,
            &state
        );

        if (state.hwnd != nullptr && active_metric_panel_index_ == panel_index) {
            if (const auto fallback_index = first_docked_metric_panel(panel_index); fallback_index.has_value()) {
                active_metric_panel_index_ = fallback_index.value();
            }
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void focus_metric_panel_window(std::size_t panel_index) {
        if (panel_index >= metric_dock_windows_.size()) {
            return;
        }
        HWND dock_window = metric_dock_windows_[panel_index].hwnd;
        if (dock_window == nullptr) {
            return;
        }
        ShowWindow(dock_window, SW_SHOW);
        SetForegroundWindow(dock_window);
    }

    void dock_metric_panel(std::size_t panel_index) {
        if (panel_index >= metric_dock_windows_.size()) {
            return;
        }
        MetricDockWindowState& state = metric_dock_windows_[panel_index];
        HWND dock_window = state.hwnd;
        state.hwnd = nullptr;
        state.dock_back_rect = {};
        if (active_metric_panel_index_ >= snapshot_.metric_panels.size()) {
            active_metric_panel_index_ = panel_index;
        }
        if (dock_window != nullptr) {
            DestroyWindow(dock_window);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void paint_metric_dock_window(MetricDockWindowState& state) {
        PAINTSTRUCT paint {};
        HDC window_dc = BeginPaint(state.hwnd, &paint);

        RECT client {};
        GetClientRect(state.hwnd, &client);
        HDC back_buffer_dc = CreateCompatibleDC(window_dc);
        HBITMAP back_bitmap = CreateCompatibleBitmap(window_dc, std::max(1L, client.right), std::max(1L, client.bottom));
        HGDIOBJ old_bitmap = SelectObject(back_buffer_dc, back_bitmap);

        fill_vertical_gradient(back_buffer_dc, client, RGB(14, 16, 20), RGB(8, 10, 14));
        if (const qbit_miner::MetricHistoryPanel* panel = metric_panel_at(state.panel_index); panel != nullptr) {
            RECT content_rect {12, 12, client.right - 12, client.bottom - 12};
            draw_metric_panel_surface(back_buffer_dc, content_rect, *panel, L"Dock Back", &state.dock_back_rect);
        }

        BitBlt(window_dc, 0, 0, client.right, client.bottom, back_buffer_dc, 0, 0, SRCCOPY);

        SelectObject(back_buffer_dc, old_bitmap);
        DeleteObject(back_bitmap);
        DeleteDC(back_buffer_dc);
        EndPaint(state.hwnd, &paint);
    }

    LRESULT handle_metric_dock_message(MetricDockWindowState& state, UINT message, WPARAM, LPARAM lparam) {
        switch (message) {
        case WM_LBUTTONUP: {
            const POINT point {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point_in_rect(point, state.dock_back_rect)) {
                DestroyWindow(state.hwnd);
                return 0;
            }
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(state.hwnd, nullptr, TRUE);
            return 0;
        case WM_PAINT:
            paint_metric_dock_window(state);
            return 0;
        case WM_CLOSE:
            DestroyWindow(state.hwnd);
            return 0;
        case WM_DESTROY:
            state.hwnd = nullptr;
            state.dock_back_rect = {};
            if (active_metric_panel_index_ >= snapshot_.metric_panels.size()) {
                active_metric_panel_index_ = state.panel_index;
            }
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(state.hwnd, message, 0, lparam);
    }

    void draw_operator_panel(HDC hdc, const RECT& rect) {
        fill_vertical_gradient(hdc, rect, RGB(74, 76, 82), RGB(34, 37, 42));
        stroke_rect(hdc, rect, RGB(180, 186, 194));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(18, 20, 25));

        RECT title_rect {inner.left + 12, inner.top + 8, inner.right - 12, inner.top + 28};
        SelectObject(hdc, accent_font_);
        draw_inset_text(hdc, L"Operator Controls", title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(236, 239, 244), RGB(8, 8, 10), RGB(18, 18, 20));

        draw_button(hdc, operator_button_rects_[0], L"Start Run", snapshot_.mining_runtime_running);
        draw_button(hdc, operator_button_rects_[1], L"Stop Run", !snapshot_.mining_runtime_running);
        draw_button(hdc, operator_button_rects_[2], L"Connect Pool", snapshot_.pool_connection_requested);
        draw_button(hdc, operator_button_rects_[3], L"Disconnect", !snapshot_.pool_connection_requested);

        const bool preview_only_mode = !snapshot_.mining_settings.allow_live_submit && !snapshot_.mining_settings.phase_guided_preview_test_mode;
        draw_button(hdc, session_mode_button_rects_[0], L"Preview", preview_only_mode);
        draw_button(hdc, session_mode_button_rects_[1], L"Validate", snapshot_.mining_settings.phase_guided_preview_test_mode);
        draw_button(hdc, session_mode_button_rects_[2], L"Live Submit", snapshot_.mining_settings.allow_live_submit);

        const std::wstring runtime_state = snapshot_.mining_runtime_running ? L"Run Active" : L"Run Idle";
        const std::wstring pool_state = snapshot_.pool_connection_requested
            ? prettify_identifier(snapshot_.mining_status.connection_state)
            : L"Manual";
        const std::wstring submit_mode = mining_session_mode_text(snapshot_.mining_settings);
        const std::wstring share_path = share_path_status_text();
        const std::wstring accepted_shares = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.accepted_submit_count)
            : L"0";
        const std::wstring valid_candidates = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.offline_valid_submit_preview_count)
                + L" / " + std::to_wstring(snapshot_.stratum_authority.submit_preview_count)
            : L"0 / 0";
        const std::wstring run_window = snapshot_.mining_session_run_indefinitely
            ? L"Indefinite"
            : format_minutes_label(snapshot_.mining_session_duration_minutes);
        const std::wstring run_timer = run_timer_status_text();
        const std::wstring rate_status = validation_rate_status_text();
        const std::wstring summary = L"Runtime: " + runtime_state + L" | Pool: " + pool_state + L" | Mode: " + submit_mode
            + L"\r\nRun Window: " + run_window + L" | Timer: " + run_timer + L" | Rate: " + rate_status
            + L"\r\nShare Path: " + share_path + L" | Accepted: " + accepted_shares + L" | Preview Valid: " + valid_candidates;

        RECT summary_rect {inner.left + 12, session_mode_button_rects_[0].bottom + 10, inner.right - 12, inner.bottom - 8};
        SelectObject(hdc, body_font_);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(204, 210, 219));
        DrawTextW(hdc, summary.c_str(), static_cast<int>(summary.size()), &summary_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    std::wstring share_path_status_text() const {
        if (snapshot_.mining_settings.allow_live_submit) {
            if (!snapshot_.has_stratum_authority) {
                return L"Live Submit | Awaiting Connect";
            }
            return snapshot_.stratum_authority.submit_path_ready
                ? L"Live Submit | Ready"
                : L"Live Submit | " + prettify_identifier(snapshot_.stratum_authority.submit_gate_reason);
        }
        if (snapshot_.mining_settings.phase_guided_preview_test_mode) {
            if (!snapshot_.has_stratum_authority) {
                return L"Preview Validate | Awaiting Connect";
            }
            return snapshot_.stratum_authority.submit_preview_count > 0U
                ? L"Preview Validate | " + prettify_identifier(snapshot_.stratum_authority.submit_gate_reason)
                : L"Preview Validate | Awaiting Candidate";
        }
        return snapshot_.pool_connection_requested ? L"Preview Only | No Network Send" : L"Preview Only | Awaiting Connect";
    }

    std::wstring run_timer_status_text() const {
        if (snapshot_.mining_runtime_running) {
            if (snapshot_.mining_session_run_indefinitely) {
                return L"Elapsed " + format_duration_compact(snapshot_.mining_session_elapsed_seconds);
            }
            return L"Remaining " + format_duration_compact(snapshot_.mining_session_remaining_seconds);
        }
        if (snapshot_.mining_session_run_indefinitely) {
            return L"Configured Indefinite";
        }
        return L"Configured " + format_minutes_label(snapshot_.mining_session_duration_minutes);
    }

    std::wstring validation_rate_status_text() const {
        if (!snapshot_.has_stratum_authority) {
            return L"Awaiting Authority";
        }
        return format_rate(snapshot_.stratum_authority.measured_validation_share_rate_per_s, L"/s")
            + L" / "
            + format_rate(snapshot_.stratum_authority.required_share_submissions_per_s, L"/s");
    }

    double active_pool_window_seconds() const {
        if (snapshot_.has_stratum_authority) {
            if (snapshot_.stratum_authority.pool_policy.hashrate_window_seconds > 0.0) {
                return snapshot_.stratum_authority.pool_policy.hashrate_window_seconds;
            }
            if (snapshot_.stratum_authority.validation_jitter_window_seconds > 0.0) {
                return snapshot_.stratum_authority.validation_jitter_window_seconds;
            }
        }

        if (snapshot_.mining_settings.pool_policy == qbit_miner::MiningPoolPolicy::F2Pool) {
            return 900.0;
        }
        return snapshot_.mining_settings.validation_jitter_window_seconds;
    }

    std::wstring pool_policy_window_text() const {
        const std::wstring policy_name = snapshot_.has_stratum_authority && !snapshot_.stratum_authority.pool_policy.pool_name.empty()
            ? prettify_identifier(snapshot_.stratum_authority.pool_policy.pool_name)
            : pool_policy_display_name(snapshot_.mining_settings.pool_policy);
        const double window_seconds = active_pool_window_seconds();
        return window_seconds > 0.0
            ? policy_name + L" | " + format_window_seconds_label(window_seconds)
            : policy_name;
    }

    std::wstring session_budget_text() const {
        if (snapshot_.has_stratum_authority) {
            return format_rate(snapshot_.stratum_authority.effective_request_budget_per_s, L"req/s");
        }
        return format_rate(snapshot_.mining_settings.max_requests_per_second, L"req/s") + L" cfg";
    }

    std::wstring pool_window_target_text() const {
        if (!snapshot_.has_stratum_authority || !snapshot_.stratum_authority.has_difficulty) {
            return L"Awaiting Difficulty";
        }
        return format_window_target(
            snapshot_.stratum_authority.required_share_submissions_per_pool_window,
            active_pool_window_seconds());
    }

    std::wstring worker_window_target_text() const {
        if (!snapshot_.has_stratum_authority
            || !snapshot_.stratum_authority.has_difficulty
            || snapshot_.stratum_authority.active_worker_count == 0U) {
            return L"Awaiting Difficulty";
        }
        return format_window_target(
            snapshot_.stratum_authority.required_share_submissions_per_worker_pool_window,
            active_pool_window_seconds());
    }

    void apply_session_mode(bool allow_live_submit, bool preview_validation) {
        qbit_miner::MiningConnectionSettings settings = service_.mining_settings();
        const bool normalized_preview_validation = preview_validation && !allow_live_submit;
        if (settings.allow_live_submit == allow_live_submit
            && settings.phase_guided_preview_test_mode == normalized_preview_validation) {
            return;
        }

        settings.allow_live_submit = allow_live_submit;
        settings.phase_guided_preview_test_mode = normalized_preview_validation;
        service_.set_mining_settings(settings);
        snapshot_ = service_.snapshot();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void draw_connection_status_card(HDC hdc) {
        draw_metric_card(hdc, connection_card_rect_, L"Connection Path", build_connection_rows(), RGB(176, 190, 212));
    }

    void draw_share_status_card(HDC hdc) {
        draw_metric_card(hdc, share_card_rect_, L"Share Path", build_share_rows(), RGB(192, 182, 156));
    }

    void draw_viewport_frame(HDC hdc, const RECT&) {
        RECT outer = viewport_rect_;
        outer.top -= 32;
        fill_vertical_gradient(hdc, outer, RGB(62, 65, 70), RGB(28, 31, 36));
        stroke_rect(hdc, outer, RGB(178, 183, 191));

        RECT inner = outer;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(12, 14, 18));

        RECT label_rect {outer.left + 12, outer.top + 6, outer.right - 12, outer.top + 28};
        SelectObject(hdc, accent_font_);
        draw_inset_text(
            hdc,
            L"Substrate Viewport | 9D Save-State -> Voxels -> PBR -> Audio",
            label_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE,
            RGB(228, 232, 238),
            RGB(12, 12, 14),
            RGB(22, 22, 24));
    }

    void draw_metric_card(HDC hdc, const RECT& rect, const std::wstring& title, const std::vector<MetricRow>& rows, COLORREF accent) {
        fill_vertical_gradient(hdc, rect, mix_color(accent, RGB(82, 84, 88), 0.30), RGB(40, 43, 48));
        stroke_rect(hdc, rect, RGB(184, 188, 195));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(18, 20, 25));

        RECT title_rect {inner.left + 12, inner.top + 10, inner.right - 12, inner.top + 36};
        SelectObject(hdc, accent_font_);
        draw_inset_text(hdc, title, title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(236, 239, 244), RGB(8, 8, 10), RGB(18, 18, 20));

        SelectObject(hdc, body_font_);
        int row_top = title_rect.bottom + 8;
        for (const auto& row : rows) {
            RECT label_rect {inner.left + 12, row_top, inner.left + 180, row_top + 22};
            RECT value_rect {inner.left + 188, row_top, inner.right - 12, row_top + 22};
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(198, 205, 214));
            DrawTextW(hdc, row.label.c_str(), static_cast<int>(row.label.size()), &label_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_inset_text(hdc, row.value, value_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, RGB(222, 227, 235), RGB(8, 8, 10), RGB(18, 18, 20));
            row_top += snapshot_.compact_layout ? 18 : 22;
        }
    }

    void draw_event_card(HDC hdc, const RECT& rect) {
        fill_vertical_gradient(hdc, rect, RGB(70, 72, 76), RGB(32, 35, 40));
        stroke_rect(hdc, rect, RGB(176, 181, 188));

        RECT inner = rect;
        InflateRect(&inner, -2, -2);
        fill_rect(hdc, inner, RGB(16, 18, 22));

        RECT title_rect {inner.left + 12, inner.top + 10, inner.right - 12, inner.top + 34};
        SelectObject(hdc, accent_font_);
        draw_inset_text(hdc, L"Live Event Rail", title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(236, 239, 244), RGB(8, 8, 10), RGB(18, 18, 20));

        SelectObject(hdc, mono_font_);
        int row_top = title_rect.bottom + 6;
        for (const auto& event_text : snapshot_.recent_events) {
            RECT line_rect {inner.left + 12, row_top, inner.right - 12, row_top + 20};
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(204, 210, 219));
            const std::wstring wide_event = widen_utf8(event_text);
            DrawTextW(hdc, wide_event.c_str(), static_cast<int>(wide_event.size()), &line_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            row_top += 20;
            if (row_top + 20 > inner.bottom) {
                break;
            }
        }
    }

    void draw_button(HDC hdc, const RECT& rect, const std::wstring& label, bool active) {
        const COLORREF top = active ? RGB(210, 215, 224) : RGB(166, 171, 178);
        const COLORREF bottom = active ? RGB(108, 113, 120) : RGB(82, 87, 94);
        fill_vertical_gradient(hdc, rect, top, bottom);
        stroke_rect(hdc, rect, RGB(230, 233, 238));

        RECT inner = rect;
        InflateRect(&inner, -1, -1);
        stroke_rect(hdc, inner, RGB(58, 60, 66));

        SelectObject(hdc, body_font_);
        draw_inset_text(hdc, label, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(246, 247, 250), RGB(8, 8, 10), RGB(18, 18, 20));
    }

    std::vector<MetricRow> build_rig_rows() const {
        return {
            {L"Hashrate", format_hashrate(snapshot_.metrics.rig.hashrate_hs)},
            {L"Submission Rate", format_rate(snapshot_.metrics.rig.submission_rate_per_s, L"/s")},
            {L"Acceptance", format_percent(snapshot_.metrics.rig.submission_acceptance_rate)},
            {L"Share Valid", format_percent(snapshot_.metrics.rig.share_valid_rate)},
            {L"Difficulty", format_difficulty(snapshot_.metrics.rig.difficulty)},
            {L"Reward Coin", format_coin(snapshot_.metrics.rig.reward.coin)},
            {L"Reward Value", format_currency(snapshot_.metrics.rig.reward.value_usd)},
            {L"Power / Temp", format_rate(snapshot_.metrics.rig.device_power_w, L"W") + L" | " + format_rate(snapshot_.metrics.rig.device_temperature_c, L"C")},
        };
    }

    std::vector<MetricRow> build_pool_rows() const {
        return {
            {L"Pool Hashrate", format_hashrate(snapshot_.metrics.pool.pool_hashrate_hs)},
            {L"Pool Difficulty", format_difficulty(snapshot_.metrics.pool.pool_difficulty)},
            {L"Pool Effort", format_percent(snapshot_.metrics.pool.pool_effort_norm)},
            {L"Worker Share", format_percent(snapshot_.metrics.pool.worker_pool_share_norm)},
            {L"Submission Rate", format_rate(snapshot_.metrics.pool.submission_rate_per_s, L"/s")},
            {L"Acceptance", format_percent(snapshot_.metrics.pool.acceptance_rate)},
            {L"Reward Coin", format_coin(snapshot_.metrics.pool.reward.coin)},
            {L"Reward Value", format_currency(snapshot_.metrics.pool.reward.value_usd)},
        };
    }

    std::vector<MetricRow> build_blockchain_rows() const {
        return {
            {L"Network Difficulty", format_difficulty(snapshot_.metrics.blockchain.network_difficulty)},
            {L"Network Hashrate", format_hashrate(snapshot_.metrics.blockchain.network_hashrate_hs)},
            {L"Block Reward", format_coin(snapshot_.metrics.blockchain.block_reward_coin)},
            {L"Halving Progress", format_percent(snapshot_.metrics.blockchain.next_halving_progress_norm)},
            {L"Reward Coin", format_coin(snapshot_.metrics.blockchain.reward.coin)},
            {L"Reward Value", format_currency(snapshot_.metrics.blockchain.reward.value_usd)},
            {L"Trace ID", widen_utf8(snapshot_.metrics.trace_id)},
            {L"Provider", widen_utf8(snapshot_.metrics.provider)},
        };
    }

    std::vector<MetricRow> build_connection_rows() const {
        const std::wstring pool_session = snapshot_.pool_connection_requested
            ? prettify_identifier(snapshot_.mining_status.connection_state)
            : L"Manual";
        const std::wstring session_mode = mining_session_mode_text(snapshot_.mining_settings);
        const std::wstring worker_login = snapshot_.mining_status.derived_worker_name.empty()
            ? L"-"
            : shorten_text(widen_utf8(snapshot_.mining_status.derived_worker_name), 30);
        const std::wstring tcp_adapter = snapshot_.tcp_adapter_state.winsock_ready
            ? L"Ready | Sessions " + std::to_wstring(snapshot_.tcp_adapter_state.active_session_count)
            : L"Not Ready";
        const std::wstring dispatch_response = std::to_wstring(snapshot_.tcp_adapter_state.live_dispatch_count)
            + L" / " + std::to_wstring(snapshot_.tcp_adapter_state.response_count);
        const std::wstring last_disconnect = snapshot_.tcp_adapter_state.last_disconnect_reason.empty()
            ? L"-"
            : shorten_text(widen_utf8(snapshot_.tcp_adapter_state.last_disconnect_reason), 28);

        return {
            {L"Pool Session", pool_session},
            {L"Session Mode", session_mode},
            {L"Worker Login", worker_login},
            {L"Policy / Window", pool_policy_window_text()},
            {L"TCP Adapter", tcp_adapter},
            {L"Dispatch / Resp", dispatch_response},
            {L"Session Budget", session_budget_text()},
            {L"Last Disconnect", last_disconnect},
        };
    }

    std::vector<MetricRow> build_share_rows() const {
        const std::wstring shares_accepted = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.submit_dispatch_count)
                + L" / " + std::to_wstring(snapshot_.stratum_authority.accepted_submit_count)
            : L"0 / 0";
        const std::wstring candidates = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.offline_valid_submit_preview_count)
                + L" / " + std::to_wstring(snapshot_.stratum_authority.submit_preview_count)
            : L"0 / 0";
        const std::wstring active_job = snapshot_.has_stratum_authority && !snapshot_.stratum_authority.active_job_id.empty()
            ? shorten_text(widen_utf8(snapshot_.stratum_authority.active_job_id), 28)
            : L"-";
        const std::wstring last_nonce = snapshot_.has_stratum_authority && !snapshot_.stratum_authority.last_submit_nonce.empty()
            ? shorten_text(widen_utf8(snapshot_.stratum_authority.last_submit_nonce), 22)
            : L"-";
        const std::wstring last_hash = snapshot_.has_stratum_authority && !snapshot_.stratum_authority.last_submit_hash_hex.empty()
            ? shorten_text(widen_utf8(snapshot_.stratum_authority.last_submit_hash_hex), 22)
            : L"-";

        return {
            {L"Share Path", share_path_status_text()},
            {L"Shares / Accepted", shares_accepted},
            {L"Valid Candidates", candidates},
            {L"Pool Window Goal", pool_window_target_text()},
            {L"Worker Window Goal", worker_window_target_text()},
            {L"Active Job", active_job},
            {L"Last Nonce", last_nonce},
            {L"Last Hash", last_hash},
        };
    }

    void show_file_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCommandFileExportCalibration, L"Export Calibration Bundle");
        AppendMenuW(menu, MF_STRING, kCommandFileExportValidation, L"Export Validation Bundle");
        AppendMenuW(menu, MF_STRING, kCommandFileSaveViewport, L"Save Viewport Snapshot");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandFileExit, L"Exit");
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, file_rect_.left, file_rect_.bottom, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void show_edit_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCommandEditCopyMetrics, L"Copy Metrics Snapshot");
        AppendMenuW(menu, MF_STRING, kCommandEditUserSettings, L"User Settings...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandEditToggleAudio, snapshot_.audio_enabled ? L"Disable Audio Output" : L"Enable Audio Output");
        AppendMenuW(menu, MF_STRING, kCommandEditTogglePause, snapshot_.paused ? L"Resume Live Feed" : L"Pause Live Feed");
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, edit_rect_.left, edit_rect_.bottom, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void show_window_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCommandWindowResetLayout, L"Reset Layout");
        AppendMenuW(menu, MF_STRING, kCommandWindowToggleCompact, snapshot_.compact_layout ? L"Expanded Metrics" : L"Compact Metrics");
        AppendMenuW(menu, MF_STRING, kCommandWindowAlwaysOnTop, snapshot_.always_on_top ? L"Disable Always On Top" : L"Enable Always On Top");
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, window_rect_.left, window_rect_.bottom, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void handle_export_calibration() {
        const auto output_dir = make_export_directory(L"calibration");
        if (service_.export_calibration_bundle(output_dir)) {
            show_message(L"Calibration export written to\n" + output_dir.wstring());
        } else {
            show_message(L"Calibration export failed.\n" + service_.last_error_message());
        }
    }

    void handle_export_validation() {
        const auto output_dir = make_export_directory(L"validation");
        if (service_.export_device_validation_bundle(output_dir)) {
            show_message(L"Validation export written to\n" + output_dir.wstring());
        } else {
            show_message(L"Validation export failed.\n" + service_.last_error_message());
        }
    }

    void handle_save_viewport() {
        const auto output_path = make_export_file(L"viewport_snapshot", L".bmp");
        if (viewport_.save_snapshot(output_path)) {
            show_message(L"Viewport snapshot written to\n" + output_path.wstring());
        } else {
            show_message(L"Viewport snapshot could not be written.");
        }
    }

    void handle_user_settings() {
        UserSettingsDialog dialog;
        qbit_miner::MiningConnectionSettings updated_settings;
        if (!dialog.show_modal(instance_, hwnd_, service_.mining_settings(), snapshot_.mining_status, &updated_settings)) {
            return;
        }

        service_.set_mining_settings(updated_settings);
        snapshot_ = service_.snapshot();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void copy_metrics_to_clipboard() {
        std::wostringstream out;
        const std::wstring preview_candidates = snapshot_.has_stratum_authority
            ? std::to_wstring(snapshot_.stratum_authority.offline_valid_submit_preview_count)
                + L" / " + std::to_wstring(snapshot_.stratum_authority.submit_preview_count)
            : L"0 / 0";
        const std::wstring last_disconnect = snapshot_.tcp_adapter_state.last_disconnect_reason.empty()
            ? L"-"
            : widen_utf8(snapshot_.tcp_adapter_state.last_disconnect_reason);
        out << L"Quantum Miner Control Center\r\n"
            << L"Rig Hashrate: " << format_hashrate(snapshot_.metrics.rig.hashrate_hs) << L"\r\n"
            << L"Rig Reward: " << format_coin(snapshot_.metrics.rig.reward.coin) << L" | " << format_currency(snapshot_.metrics.rig.reward.value_usd) << L"\r\n"
            << L"Mining Runtime: " << (snapshot_.mining_runtime_running ? L"Running" : L"Stopped") << L"\r\n"
            << L"Pool Policy: " << widen_utf8(snapshot_.mining_status.pool_policy_label) << L"\r\n"
            << L"Authorize Login: " << widen_utf8(snapshot_.mining_status.derived_worker_name) << L"\r\n"
            << L"Stratum State: " << widen_utf8(snapshot_.mining_status.connection_state) << L"\r\n"
            << L"Session Mode: " << mining_session_mode_text(snapshot_.mining_settings) << L"\r\n"
            << L"Policy / Window: " << pool_policy_window_text() << L"\r\n"
            << L"Share Path: " << share_path_status_text() << L"\r\n"
            << L"Session Budget: " << session_budget_text() << L"\r\n"
            << L"Dispatch / Response: " << snapshot_.tcp_adapter_state.live_dispatch_count << L" / " << snapshot_.tcp_adapter_state.response_count << L"\r\n"
            << L"Shares / Accepted: "
            << (snapshot_.has_stratum_authority ? std::to_wstring(snapshot_.stratum_authority.submit_dispatch_count) : L"0")
            << L" / "
            << (snapshot_.has_stratum_authority ? std::to_wstring(snapshot_.stratum_authority.accepted_submit_count) : L"0")
            << L"\r\n"
            << L"Preview Valid / Seen: " << preview_candidates << L"\r\n"
            << L"Pool Window Goal: " << pool_window_target_text() << L"\r\n"
            << L"Worker Window Goal: " << worker_window_target_text() << L"\r\n"
            << L"Last Disconnect: " << last_disconnect << L"\r\n"
            << L"Pool Hashrate: " << format_hashrate(snapshot_.metrics.pool.pool_hashrate_hs) << L"\r\n"
            << L"Network Hashrate: " << format_hashrate(snapshot_.metrics.blockchain.network_hashrate_hs) << L"\r\n"
            << L"Trace: " << widen_utf8(snapshot_.metrics.trace_id) << L"\r\n"
            << L"Viewport: " << viewport_.device_label() << L"\r\n";
        const std::wstring text = out.str();

        if (!OpenClipboard(hwnd_)) {
            return;
        }
        EmptyClipboard();
        const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (global != nullptr) {
            void* memory = GlobalLock(global);
            if (memory != nullptr) {
                std::memcpy(memory, text.c_str(), bytes);
                GlobalUnlock(global);
                SetClipboardData(CF_UNICODETEXT, global);
            } else {
                GlobalFree(global);
            }
        }
        CloseClipboard();
    }

    void show_message(const std::wstring& text) const {
        MessageBoxW(hwnd_, text.c_str(), L"Quantum Miner Control Center", MB_OK | MB_ICONINFORMATION);
    }

    void create_fonts() {
        title_font_ = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold");
        accent_font_ = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold");
        body_font_ = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
        mono_font_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");
    }

    void destroy_fonts() {
        if (title_font_ != nullptr) {
            DeleteObject(title_font_);
            title_font_ = nullptr;
        }
        if (accent_font_ != nullptr) {
            DeleteObject(accent_font_);
            accent_font_ = nullptr;
        }
        if (body_font_ != nullptr) {
            DeleteObject(body_font_);
            body_font_ = nullptr;
        }
        if (mono_font_ != nullptr) {
            DeleteObject(mono_font_);
            mono_font_ = nullptr;
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT accent_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    RECT file_rect_ {};
    RECT edit_rect_ {};
    RECT window_rect_ {};
    RECT operator_panel_rect_ {};
    std::array<RECT, 3> reward_rects_ {};
    std::array<RECT, 4> operator_button_rects_ {};
    std::array<RECT, 3> session_mode_button_rects_ {};
    RECT metric_workspace_rect_ {};
    std::array<RECT, 4> metric_tab_rects_ {};
    RECT metric_panel_action_rect_ {};
    RECT connection_card_rect_ {};
    RECT share_card_rect_ {};
    RECT viewport_rect_ {};
    std::array<MetricDockWindowState, 4> metric_dock_windows_ {};
    std::size_t active_metric_panel_index_ = 0;
    ViewportPresenter viewport_;
    bool viewport_created_ = false;
    qbit_miner::LiveControlCenter service_;
    qbit_miner::ControlCenterSnapshot snapshot_;
    FieldAudioOutput audio_output_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command_show) {
    ControlCenterWindow window;
    return window.run(instance, command_show);
}
