#include "qbit_miner/control_center/substrate_compute_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#if defined(QBIT_MINER_HAS_VULKAN)
#if defined(_WIN32)
#include <windows.h>
#endif
#include <vulkan/vulkan.h>

#include "../../.github/skills/phase-encoding/assets/substrate_firmware_layout.hpp"
#endif

#include "qbit_miner/runtime/substrate_stratum_pow.hpp"

namespace qbit_miner {

namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

float clamp01f(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float clamp_signed(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

float wrap01f(float value) {
    const float wrapped = std::fmod(value, 1.0f);
    return wrapped < 0.0f ? wrapped + 1.0f : wrapped;
}

std::array<float, 3> normalize3f(const std::array<float, 3>& value) {
    const float length_sq = (value[0] * value[0]) + (value[1] * value[1]) + (value[2] * value[2]);
    if (length_sq <= 1.0e-8f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv_length = 1.0f / std::sqrt(length_sq);
    return {
        value[0] * inv_length,
        value[1] * inv_length,
        value[2] * inv_length,
    };
}

double steady_now_seconds() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

std::wstring widen_utf8(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::uint32_t flatten_index(std::uint32_t x, std::uint32_t y, std::uint32_t z, const std::array<std::uint32_t, 3>& extent) {
    return (z * extent[1] * extent[0]) + (y * extent[0]) + x;
}

template <std::size_t WordCount>
std::string words_to_hex_be(const std::array<std::uint32_t, WordCount>& words) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0');
    for (std::uint32_t word : words) {
        out << std::setw(8) << word;
    }
    return out.str();
}

bool has_nonzero_hash_words(const std::array<std::uint32_t, 8>& words) {
    return std::any_of(words.begin(), words.end(), [](std::uint32_t word) { return word != 0U; });
}

#if defined(QBIT_MINER_HAS_VULKAN)

using phase_encoding_template::ActuationGateState;
using phase_encoding_template::AssociationSignature6DoF;
using phase_encoding_template::CarrierCell9D;
using phase_encoding_template::MiningLaneWorkItem;
using phase_encoding_template::MiningValidationState;
using phase_encoding_template::NextPulseOutput;
using phase_encoding_template::PulseQuartet;
using phase_encoding_template::TensorEncounter6DoF;

constexpr VkFormat kSurfaceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkTimeDomainEXT kInvalidTimeDomain = static_cast<VkTimeDomainEXT>(-1);

struct BufferHandle {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct ImageHandle {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct PushConstants {
    std::int32_t lattice_extent[3] {0, 0, 0};
    std::uint32_t activation_tick = 0;
};

using PFN_ResetQueryPoolAny = void (VKAPI_PTR*)(VkDevice, VkQueryPool, std::uint32_t, std::uint32_t);

struct DeviceTelemetryCapabilities {
    VkPhysicalDeviceProperties properties {};
    bool supports_timestamp_queries = false;
    bool supports_pipeline_statistics = false;
    bool supports_calibrated_timestamps = false;
    bool supports_host_query_reset = false;
    VkTimeDomainEXT preferred_host_time_domain = kInvalidTimeDomain;
};

std::vector<char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Unable to open shader file");
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Shader file is empty");
    }
    std::vector<char> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(data.data(), size);
    return data;
}

std::uint32_t find_memory_type(
    VkPhysicalDevice physical_device,
    std::uint32_t type_filter,
    VkMemoryPropertyFlags properties
) {
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const bool supported = (type_filter & (1U << index)) != 0U;
        const bool has_properties = (memory_properties.memoryTypes[index].propertyFlags & properties) == properties;
        if (supported && has_properties) {
            return index;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type found");
}

VkShaderModule create_shader_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = code.size();
    create_info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &create_info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create compute shader module");
    }
    return module;
}

BufferHandle create_buffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties
) {
    BufferHandle buffer;
    buffer.size = size;

    VkBufferCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = size;
    create_info.usage = usage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &create_info, nullptr, &buffer.buffer) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan buffer");
    }

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);

    VkMemoryAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, properties);
    if (vkAllocateMemory(device, &alloc_info, nullptr, &buffer.memory) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate Vulkan buffer memory");
    }
    if (vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
        throw std::runtime_error("Unable to bind Vulkan buffer memory");
    }
    return buffer;
}

void destroy_buffer(VkDevice device, BufferHandle& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

ImageHandle create_image(
    VkPhysicalDevice physical_device,
    VkDevice device,
    const std::array<std::uint32_t, 3>& extent
) {
    ImageHandle image;

    VkImageCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_3D;
    create_info.format = kSurfaceFormat;
    create_info.extent = {extent[0], extent[1], extent[2]};
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &create_info, nullptr, &image.image) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan storage image");
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, image.image, &requirements);

    VkMemoryAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc_info, nullptr, &image.memory) != VK_SUCCESS) {
        throw std::runtime_error("Unable to allocate Vulkan image memory");
    }
    if (vkBindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS) {
        throw std::runtime_error("Unable to bind Vulkan image memory");
    }

    VkImageViewCreateInfo view_info {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view_info.format = kSurfaceFormat;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view_info, nullptr, &image.view) != VK_SUCCESS) {
        throw std::runtime_error("Unable to create Vulkan image view");
    }

    return image;
}

void destroy_image(VkDevice device, ImageHandle& image) {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
}

void upload_bytes(VkDevice device, const BufferHandle& buffer, const void* data, std::size_t size) {
    void* mapped = nullptr;
    if (vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("Unable to map Vulkan buffer for upload");
    }
    std::memcpy(mapped, data, std::min<std::size_t>(size, static_cast<std::size_t>(buffer.size)));
    vkUnmapMemory(device, buffer.memory);
}

float half_to_float(std::uint16_t value) {
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    const std::uint32_t exponent = (value & 0x7C00u) >> 10;
    const std::uint32_t mantissa = value & 0x03FFu;
    std::uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            std::int32_t exp = -14;
            std::uint32_t norm_mantissa = mantissa;
            while ((norm_mantissa & 0x0400u) == 0U) {
                norm_mantissa <<= 1U;
                --exp;
            }
            norm_mantissa &= 0x03FFu;
            bits = sign | (static_cast<std::uint32_t>(exp + 127) << 23U) | (norm_mantissa << 13U);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }

    float output = 0.0f;
    std::memcpy(&output, &bits, sizeof(output));
    return output;
}

std::vector<float> download_half_rgba_surface(VkDevice device, const BufferHandle& buffer, std::size_t texel_count) {
    void* mapped = nullptr;
    if (vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("Unable to map Vulkan buffer for download");
    }

    const auto* half_values = static_cast<const std::uint16_t*>(mapped);
    std::vector<float> output(texel_count * 4U, 0.0f);
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = half_to_float(half_values[index]);
    }
    vkUnmapMemory(device, buffer.memory);
    return output;
}

template <typename ValueT>
std::vector<ValueT> download_buffer_vector(VkDevice device, const BufferHandle& buffer, std::size_t value_count) {
    void* mapped = nullptr;
    if (vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("Unable to map Vulkan buffer for vector download");
    }

    std::vector<ValueT> output(value_count);
    std::memcpy(output.data(), mapped, std::min<std::size_t>(buffer.size, sizeof(ValueT) * value_count));
    vkUnmapMemory(device, buffer.memory);
    return output;
}

std::filesystem::path shader_path() {
#if defined(QBIT_MINER_PHASE_COMPUTE_SHADER_PATH)
    return std::filesystem::path(QBIT_MINER_PHASE_COMPUTE_SHADER_PATH);
#else
    return {};
#endif
}

std::vector<VkExtensionProperties> enumerate_device_extensions(VkPhysicalDevice physical_device) {
    std::uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count != 0U) {
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, extensions.data());
    }
    return extensions;
}

bool has_device_extension(const std::vector<VkExtensionProperties>& extensions, const char* extension_name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, extension_name) == 0) {
            return true;
        }
    }
    return false;
}

VkTimeDomainEXT choose_preferred_host_time_domain(const std::vector<VkTimeDomainEXT>& domains) {
#if defined(_WIN32)
    for (VkTimeDomainEXT domain : domains) {
        if (domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT) {
            return domain;
        }
    }
#endif
    for (VkTimeDomainEXT domain : domains) {
        if (domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) {
            return domain;
        }
        if (domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) {
            return domain;
        }
    }
    return kInvalidTimeDomain;
}

std::vector<VkTimeDomainEXT> query_time_domains(
    VkPhysicalDevice physical_device,
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT get_time_domains
) {
    if (get_time_domains == nullptr) {
        return {};
    }

    std::uint32_t domain_count = 0;
    if (get_time_domains(physical_device, &domain_count, nullptr) != VK_SUCCESS || domain_count == 0U) {
        return {};
    }

    std::vector<VkTimeDomainEXT> domains(domain_count);
    if (get_time_domains(physical_device, &domain_count, domains.data()) != VK_SUCCESS) {
        return {};
    }
    return domains;
}

DeviceTelemetryCapabilities query_device_telemetry_capabilities(
    VkInstance instance,
    VkPhysicalDevice physical_device,
    std::uint32_t queue_family_index
) {
    DeviceTelemetryCapabilities capabilities;
    vkGetPhysicalDeviceProperties(physical_device, &capabilities.properties);

    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(physical_device, &features);
    capabilities.supports_pipeline_statistics = features.pipelineStatisticsQuery == VK_TRUE;

    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_properties(queue_count);
    if (queue_count != 0U) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queue_properties.data());
    }
    if (queue_family_index < queue_properties.size()) {
        capabilities.supports_timestamp_queries =
            capabilities.properties.limits.timestampComputeAndGraphics == VK_TRUE
            && queue_properties[queue_family_index].timestampValidBits > 0U;
    }

    const std::vector<VkExtensionProperties> extensions = enumerate_device_extensions(physical_device);
    const bool calibrated_extension_present =
        has_device_extension(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool host_query_reset_extension_present =
        has_device_extension(extensions, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);

    VkPhysicalDeviceHostQueryResetFeatures host_query_reset_features {};
    host_query_reset_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &host_query_reset_features;
    vkGetPhysicalDeviceFeatures2(physical_device, &features2);
    capabilities.supports_host_query_reset =
        host_query_reset_features.hostQueryReset == VK_TRUE
        && (VK_API_VERSION_MAJOR(capabilities.properties.apiVersion) > 1
            || VK_API_VERSION_MINOR(capabilities.properties.apiVersion) >= 2
            || host_query_reset_extension_present);

    if (calibrated_extension_present) {
        const auto get_time_domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        const std::vector<VkTimeDomainEXT> domains = query_time_domains(physical_device, get_time_domains);
        capabilities.preferred_host_time_domain = choose_preferred_host_time_domain(domains);
        capabilities.supports_calibrated_timestamps = capabilities.preferred_host_time_domain != kInvalidTimeDomain;
    }

    return capabilities;
}

VkQueryPool create_query_pool(
    VkDevice device,
    VkQueryType query_type,
    std::uint32_t query_count,
    VkQueryPipelineStatisticFlags pipeline_statistics = 0
) {
    VkQueryPoolCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    create_info.queryType = query_type;
    create_info.queryCount = query_count;
    create_info.pipelineStatistics = pipeline_statistics;

    VkQueryPool query_pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(device, &create_info, nullptr, &query_pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return query_pool;
}

void destroy_query_pool(VkDevice device, VkQueryPool& query_pool) {
    if (query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, query_pool, nullptr);
        query_pool = VK_NULL_HANDLE;
    }
}

double host_time_domain_timestamp_to_seconds(std::uint64_t timestamp, VkTimeDomainEXT time_domain) {
    switch (time_domain) {
#if defined(_WIN32)
    case VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT: {
        LARGE_INTEGER frequency {};
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart <= 0) {
            return 0.0;
        }
        return static_cast<double>(timestamp) / static_cast<double>(frequency.QuadPart);
    }
#endif
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT:
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT:
        return static_cast<double>(timestamp) * 1.0e-9;
    default:
        return 0.0;
    }
}

double gpu_tick_delta_seconds(std::uint64_t newer, std::uint64_t older, double timestamp_period_s) {
    if (newer >= older) {
        return static_cast<double>(newer - older) * timestamp_period_s;
    }
    return -static_cast<double>(older - newer) * timestamp_period_s;
}

#endif

}  // namespace

struct SubstrateComputeRuntime::Impl {
    bool initialize();
    bool update(const FieldViewportFrame& frame, std::uint32_t preview_width, std::uint32_t preview_height);
    void publish_kernel_iteration(const GpuKernelIterationEvent& event);
    void publish_mining_validation(const MiningValidationSnapshot& snapshot);

    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    std::vector<std::uint8_t> preview_rgba;
    StereoPcmFrame audio_frame;
    std::wstring device_label = L"Software surface fallback";
    bool available = false;
    std::uint64_t kernel_iteration_counter = 0;
    std::optional<GpuKernelIterationEvent> last_kernel_iteration;
    std::optional<MiningValidationSnapshot> last_mining_validation;
    std::function<void(const GpuKernelIterationEvent&)> kernel_iteration_observer;
    std::function<void(const MiningValidationSnapshot&)> mining_validation_observer;

#if defined(QBIT_MINER_HAS_VULKAN)
    bool ensure_resources(const std::array<std::uint32_t, 3>& extent);
    void destroy_resources();
    void cleanup();
    void pack_authoritative_buffers(
        const FieldViewportFrame& frame,
        std::vector<CarrierCell9D>& carriers,
        std::vector<TensorEncounter6DoF>& tensors,
        std::vector<AssociationSignature6DoF>& associations,
        std::vector<ActuationGateState>& gates,
        std::vector<NextPulseOutput>& pulses,
        std::vector<MiningLaneWorkItem>& lane_work_items,
        std::vector<MiningValidationState>& validations
    );
    bool dispatch(const FieldViewportFrame& frame);
    void build_preview_from_surfaces(std::uint32_t width, std::uint32_t height);
    void build_audio_from_surface();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family_index = 0;
    DeviceTelemetryCapabilities telemetry_capabilities;
    PFN_vkGetCalibratedTimestampsEXT get_calibrated_timestamps = nullptr;
    PFN_ResetQueryPoolAny reset_query_pool = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader_module = VK_NULL_HANDLE;

    std::array<std::uint32_t, 3> current_extent {0U, 0U, 0U};
    std::size_t texel_count = 0;
    std::uint32_t activation_tick = 0;
    bool images_initialized = false;

    BufferHandle carrier_buffer;
    BufferHandle tensor_buffer;
    BufferHandle association_buffer;
    BufferHandle gate_buffer;
    BufferHandle pulse_buffer;
    BufferHandle mining_lane_work_buffer;
    BufferHandle mining_validation_buffer;
    BufferHandle visual_readback_buffer;
    BufferHandle material_readback_buffer;
    BufferHandle audio_readback_buffer;

    ImageHandle visual_image;
    ImageHandle material_image;
    ImageHandle audio_image;

    std::vector<float> visual_surface;
    std::vector<float> material_surface;
    std::vector<float> audio_surface;
    std::vector<MiningValidationState> mining_validation_surface;
#endif
};

#if defined(QBIT_MINER_HAS_VULKAN)

void SubstrateComputeRuntime::Impl::publish_kernel_iteration(const GpuKernelIterationEvent& event) {
    last_kernel_iteration = event;
    if (kernel_iteration_observer) {
        kernel_iteration_observer(event);
    }
}

void SubstrateComputeRuntime::Impl::publish_mining_validation(const MiningValidationSnapshot& snapshot) {
    last_mining_validation = snapshot;
    if (mining_validation_observer) {
        mining_validation_observer(snapshot);
    }
}

bool SubstrateComputeRuntime::Impl::initialize() {
    if (available) {
        return true;
    }

    const std::filesystem::path compute_shader_path = shader_path();
    if (compute_shader_path.empty() || !std::filesystem::exists(compute_shader_path)) {
        device_label = L"Software surface fallback";
        return false;
    }

    try {
        VkApplicationInfo app_info {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "Quantum Miner Phase Runtime";
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "QuantumMinerPhaseEncoding";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instance_info {};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app_info;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute Vulkan instance");
        }

        std::uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        if (device_count == 0U) {
            throw std::runtime_error("No Vulkan compute device available");
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        int best_score = std::numeric_limits<int>::min();
        for (VkPhysicalDevice candidate : devices) {
            std::uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());

            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceProperties(candidate, &properties);

            for (std::uint32_t index = 0; index < queue_count; ++index) {
                if ((queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0U) {
                    continue;
                }
                int score = 0;
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    score += 1000;
                } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                    score += 100;
                }
                score += static_cast<int>(properties.limits.maxComputeSharedMemorySize / 1024U);
                if (score > best_score) {
                    best_score = score;
                    physical_device = candidate;
                    queue_family_index = index;
                    device_label = widen_utf8(properties.deviceName) + L" | phase compute";
                }
            }
        }

        if (physical_device == VK_NULL_HANDLE) {
            throw std::runtime_error("Unable to find Vulkan compute queue family");
        }

        telemetry_capabilities = query_device_telemetry_capabilities(instance, physical_device, queue_family_index);

        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_index;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        std::vector<const char*> device_extensions;
        if (telemetry_capabilities.supports_calibrated_timestamps) {
            device_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        }
        if (telemetry_capabilities.supports_host_query_reset
            && VK_API_VERSION_MAJOR(telemetry_capabilities.properties.apiVersion) == 1
            && VK_API_VERSION_MINOR(telemetry_capabilities.properties.apiVersion) < 2) {
            device_extensions.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
        }

        VkPhysicalDeviceFeatures device_features {};
        device_features.pipelineStatisticsQuery =
            telemetry_capabilities.supports_pipeline_statistics ? VK_TRUE : VK_FALSE;

        VkPhysicalDeviceHostQueryResetFeatures host_query_reset_features {};
        host_query_reset_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
        host_query_reset_features.hostQueryReset =
            telemetry_capabilities.supports_host_query_reset ? VK_TRUE : VK_FALSE;

        VkDeviceCreateInfo device_info {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = telemetry_capabilities.supports_host_query_reset ? &host_query_reset_features : nullptr;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
        device_info.pEnabledFeatures = &device_features;
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create Vulkan compute device");
        }
        vkGetDeviceQueue(device, queue_family_index, 0, &queue);

        if (telemetry_capabilities.supports_calibrated_timestamps) {
            get_calibrated_timestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT"));
            if (get_calibrated_timestamps == nullptr) {
                telemetry_capabilities.supports_calibrated_timestamps = false;
            }
        }

        reset_query_pool = reinterpret_cast<PFN_ResetQueryPoolAny>(vkGetDeviceProcAddr(device, "vkResetQueryPool"));
        if (reset_query_pool == nullptr) {
            reset_query_pool = reinterpret_cast<PFN_ResetQueryPoolAny>(vkGetDeviceProcAddr(device, "vkResetQueryPoolEXT"));
        }
        if (reset_query_pool == nullptr) {
            telemetry_capabilities.supports_timestamp_queries = false;
            telemetry_capabilities.supports_pipeline_statistics = false;
            telemetry_capabilities.supports_host_query_reset = false;
        }

        if (telemetry_capabilities.supports_timestamp_queries) {
            device_label += L" | driver kernel feed";
        }
        if (telemetry_capabilities.supports_calibrated_timestamps) {
            device_label += L" | calibrated clock";
        }

        VkCommandPoolCreateInfo pool_info {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family_index;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute command pool");
        }

        VkCommandBufferAllocateInfo command_info {};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &command_info, &command_buffer) != VK_SUCCESS) {
            throw std::runtime_error("Unable to allocate compute command buffer");
        }

        std::array<VkDescriptorSetLayoutBinding, 10> bindings {};
        for (std::uint32_t index = 0; index < 7U; ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (std::uint32_t index = 7U; index < 10U; ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layout_info {};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute descriptor set layout");
        }

        VkPushConstantRange push_range {};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo pipeline_layout_info {};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_set_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute pipeline layout");
        }

        const std::vector<char> shader_code = read_binary_file(compute_shader_path);
        shader_module = create_shader_module(device, shader_code);

        VkPipelineShaderStageCreateInfo stage_info {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader_module;
        stage_info.pName = "main";

        VkComputePipelineCreateInfo pipeline_info {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = pipeline_layout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute pipeline");
        }

        std::array<VkDescriptorPoolSize, 2> pool_sizes {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[0].descriptorCount = 7;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_sizes[1].descriptorCount = 3;

        VkDescriptorPoolCreateInfo pool_create_info {};
        pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_create_info.maxSets = 1;
        pool_create_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_create_info.pPoolSizes = pool_sizes.data();
        if (vkCreateDescriptorPool(device, &pool_create_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
            throw std::runtime_error("Unable to create compute descriptor pool");
        }

        VkDescriptorSetAllocateInfo set_allocate_info {};
        set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_allocate_info.descriptorPool = descriptor_pool;
        set_allocate_info.descriptorSetCount = 1;
        set_allocate_info.pSetLayouts = &descriptor_set_layout;
        if (vkAllocateDescriptorSets(device, &set_allocate_info, &descriptor_set) != VK_SUCCESS) {
            throw std::runtime_error("Unable to allocate compute descriptor set");
        }

        available = true;
        return true;
    } catch (...) {
        cleanup();
        available = false;
        device_label = L"Software surface fallback";
        return false;
    }
}

bool SubstrateComputeRuntime::Impl::ensure_resources(const std::array<std::uint32_t, 3>& extent) {
    if (!available) {
        return false;
    }
    if (current_extent == extent && texel_count != 0U) {
        return true;
    }

    destroy_resources();

    current_extent = extent;
    texel_count = static_cast<std::size_t>(extent[0]) * extent[1] * extent[2];
    if (texel_count == 0U) {
        return false;
    }

    const VkDeviceSize carrier_size = sizeof(CarrierCell9D) * texel_count;
    const VkDeviceSize tensor_size = sizeof(TensorEncounter6DoF) * texel_count;
    const VkDeviceSize association_size = sizeof(AssociationSignature6DoF) * texel_count;
    const VkDeviceSize gate_size = sizeof(ActuationGateState) * texel_count;
    const VkDeviceSize pulse_size = sizeof(NextPulseOutput) * texel_count;
    const VkDeviceSize mining_lane_work_size = sizeof(MiningLaneWorkItem) * texel_count;
    const VkDeviceSize mining_validation_size = sizeof(MiningValidationState) * texel_count;
    const VkDeviceSize surface_size = sizeof(std::uint16_t) * 4U * texel_count;

    carrier_buffer = create_buffer(physical_device, device, carrier_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    tensor_buffer = create_buffer(physical_device, device, tensor_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    association_buffer = create_buffer(physical_device, device, association_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gate_buffer = create_buffer(physical_device, device, gate_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    pulse_buffer = create_buffer(physical_device, device, pulse_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mining_lane_work_buffer = create_buffer(physical_device, device, mining_lane_work_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mining_validation_buffer = create_buffer(physical_device, device, mining_validation_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    visual_image = create_image(physical_device, device, extent);
    material_image = create_image(physical_device, device, extent);
    audio_image = create_image(physical_device, device, extent);

    visual_readback_buffer = create_buffer(physical_device, device, surface_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    material_readback_buffer = create_buffer(physical_device, device, surface_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    audio_readback_buffer = create_buffer(physical_device, device, surface_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    std::array<VkDescriptorBufferInfo, 7> buffer_infos {{
        {carrier_buffer.buffer, 0, carrier_buffer.size},
        {tensor_buffer.buffer, 0, tensor_buffer.size},
        {association_buffer.buffer, 0, association_buffer.size},
        {gate_buffer.buffer, 0, gate_buffer.size},
        {pulse_buffer.buffer, 0, pulse_buffer.size},
        {mining_lane_work_buffer.buffer, 0, mining_lane_work_buffer.size},
        {mining_validation_buffer.buffer, 0, mining_validation_buffer.size},
    }};
    std::array<VkDescriptorImageInfo, 3> image_infos {{
        {VK_NULL_HANDLE, visual_image.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, material_image.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, audio_image.view, VK_IMAGE_LAYOUT_GENERAL},
    }};
    std::array<VkWriteDescriptorSet, 10> writes {};
    for (std::uint32_t index = 0; index < 7U; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = descriptor_set;
        writes[index].dstBinding = index;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].descriptorCount = 1;
        writes[index].pBufferInfo = &buffer_infos[index];
    }
    for (std::uint32_t index = 0; index < 3U; ++index) {
        writes[index + 7U].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index + 7U].dstSet = descriptor_set;
        writes[index + 7U].dstBinding = index + 7U;
        writes[index + 7U].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index + 7U].descriptorCount = 1;
        writes[index + 7U].pImageInfo = &image_infos[index];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    return true;
}

void SubstrateComputeRuntime::Impl::destroy_resources() {
    destroy_buffer(device, carrier_buffer);
    destroy_buffer(device, tensor_buffer);
    destroy_buffer(device, association_buffer);
    destroy_buffer(device, gate_buffer);
    destroy_buffer(device, pulse_buffer);
    destroy_buffer(device, mining_lane_work_buffer);
    destroy_buffer(device, mining_validation_buffer);
    destroy_buffer(device, visual_readback_buffer);
    destroy_buffer(device, material_readback_buffer);
    destroy_buffer(device, audio_readback_buffer);
    destroy_image(device, visual_image);
    destroy_image(device, material_image);
    destroy_image(device, audio_image);
    texel_count = 0U;
    current_extent = {0U, 0U, 0U};
    images_initialized = false;
    mining_validation_surface.clear();
    last_mining_validation.reset();
}

void SubstrateComputeRuntime::Impl::cleanup() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    destroy_resources();
    if (descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
    }
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (shader_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, shader_module, nullptr);
        shader_module = VK_NULL_HANDLE;
    }
    if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
    }
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    physical_device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    telemetry_capabilities = DeviceTelemetryCapabilities{};
    get_calibrated_timestamps = nullptr;
    reset_query_pool = nullptr;
}

void SubstrateComputeRuntime::Impl::pack_authoritative_buffers(
    const FieldViewportFrame& frame,
    std::vector<CarrierCell9D>& carriers,
    std::vector<TensorEncounter6DoF>& tensors,
    std::vector<AssociationSignature6DoF>& associations,
    std::vector<ActuationGateState>& gates,
    std::vector<NextPulseOutput>& pulses,
    std::vector<MiningLaneWorkItem>& lane_work_items,
    std::vector<MiningValidationState>& validations
) {
    carriers.assign(texel_count, CarrierCell9D{});
    tensors.assign(texel_count, TensorEncounter6DoF{});
    associations.assign(texel_count, AssociationSignature6DoF{});
    gates.assign(texel_count, ActuationGateState{});
    pulses.assign(texel_count, NextPulseOutput{});
    lane_work_items.assign(texel_count, MiningLaneWorkItem{});
    validations.assign(texel_count, MiningValidationState{});

    const auto& authoritative = frame.authoritative_frame;
    const auto& saved_state = frame.saved_state;
    const auto& mining = saved_state.mining_phase_encoding;
    const auto& gpu_mining = frame.gpu_mining_authority;
    const std::uint32_t lane_count = std::max<std::uint32_t>(mining.target_lane_count, 1U);
    constexpr float kTauF = 6.28318530717958647692f;
    std::vector<std::size_t> active_worker_slots;
    active_worker_slots.reserve(gpu_mining.workers.size());
    for (std::size_t worker_slot = 0; worker_slot < gpu_mining.workers.size(); ++worker_slot) {
        if (gpu_mining.workers[worker_slot].active) {
            active_worker_slots.push_back(worker_slot);
        }
    }

    for (std::uint32_t z = 0; z < current_extent[2]; ++z) {
        for (std::uint32_t y = 0; y < current_extent[1]; ++y) {
            for (std::uint32_t x = 0; x < current_extent[0]; ++x) {
                const std::uint32_t index = flatten_index(x, y, z, current_extent);
                const std::uint32_t lane_index = index % lane_count;
                const float lane_fraction = lane_count <= 1U
                    ? 0.0f
                    : (static_cast<float>(lane_index) / static_cast<float>(lane_count));
                const float nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(current_extent[0]) * 2.0f) - 1.0f;
                const float ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(current_extent[1]) * 2.0f) - 1.0f;
                const float nz = ((static_cast<float>(z) + 0.5f) / static_cast<float>(current_extent[2]) * 2.0f) - 1.0f;
                const float radial = std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
                const float harmonic = 0.5f + (0.5f * std::sin((static_cast<float>(activation_tick) * 0.07f) + (nx * 2.7f) + (ny * 2.3f) + (nz * 2.9f)));
                const float sha_schedule = mining.sha256_schedule_phase_turns;
                const float sha_round = mining.sha256_round_phase_turns;
                const float sha_digest = mining.sha256_digest_phase_turns;
                const float sha_bias = mining.sha256_frequency_bias_norm;
                const float sha_density = mining.sha256_harmonic_density_norm;
                const float target_resonance = mining.target_resonance_norm;
                const float resonance_activation = mining.resonance_activation_norm;
                const float phase_flux_conservation = mining.phase_flux_conservation_norm;
                const float nonce_collapse_confidence = mining.nonce_collapse_confidence_norm;
                const float observer_collapse_strength = mining.observer_collapse_strength_norm;
                const float validation_structure = mining.validation_structure_norm;
                const float target_sequence_phase = mining.target_sequence_phase_turns;
                const float target_sequence_frequency = mining.target_sequence_frequency_norm;
                const float target_repeat_flux = mining.target_repeat_flux_norm;
                const float reverse_observer_collapse = mining.reverse_observer_collapse_norm;
                const float spider_code_frequency = mining.spider_code_frequency_norm;
                const float spider_code_amplitude = mining.spider_code_amplitude_norm;
                const float spider_code_voltage = mining.spider_code_voltage_norm;
                const float spider_code_amperage = mining.spider_code_amperage_norm;
                const float spider_projection_coherence = mining.spider_projection_coherence_norm;
                const float spider_harmonic_gate = mining.spider_harmonic_gate_norm;
                const float spider_noise_sink = mining.spider_noise_sink_norm;
                const float frontier_activation_budget = mining.frontier_activation_budget_norm;
                const float cumulative_activation_budget = mining.cumulative_activation_budget_norm;
                const float pulse_operator_density = mining.pulse_operator_density_norm;
                const float nested_fourier_resonance = mining.nested_fourier_resonance_norm;
                const float pool_ingest_vector = mining.pool_ingest_vector_norm;
                const float pool_submit_vector = mining.pool_submit_vector_norm;
                const float transfer_drive = mining.transfer_drive_norm;
                const float stability_gate = mining.stability_gate_norm;
                const float damping = mining.damping_norm;
                const float transport_drive = mining.transport_drive_norm;
                const float temporal_admissibility = mining.temporal_admissibility_norm;
                const float zero_point_proximity = mining.zero_point_proximity_norm;
                const float transport_readiness = mining.transport_readiness_norm;
                const float share_confidence = mining.share_confidence_norm;
                const float validation_rate = mining.validation_rate_norm;
                const float valid_nonce_fraction = mining.attempted_nonce_count == 0U
                    ? 0.0f
                    : clamp01f(
                        static_cast<float>(mining.valid_nonce_count)
                        / static_cast<float>(std::max(mining.attempted_nonce_count, 1U)));
                const std::uint32_t mining_flags =
                    (mining.active ? 0x1U : 0U)
                    | (mining.submit_path_ready ? 0x2U : 0U)
                    | (mining.all_parallel_harmonics_verified ? 0x4U : 0U);
                const float lane_phase = mining.active
                    ? wrap01f(
                        mining.share_target_phase_turns
                        + (0.35f * mining.header_phase_turns)
                        + (0.20f * mining.nonce_origin_phase_turns)
                        + (0.16f * sha_schedule)
                        + (0.12f * sha_round)
                        + (0.08f * sha_digest)
                        + (0.08f * phase_flux_conservation)
                        + (0.06f * nonce_collapse_confidence)
                        + lane_fraction
                        + (0.03125f * static_cast<float>(activation_tick & 0xffU)))
                    : harmonic;
                const float mining_certainty = mining.active
                    ? clamp01f(
                        (0.24f * mining.lane_coherence_norm)
                        + (0.22f * mining.phase_pressure_norm)
                        + (0.16f * mining.worker_parallelism_norm)
                        + (0.16f * target_resonance)
                        + (0.12f * resonance_activation)
                        + (0.10f * phase_flux_conservation)
                        + (0.08f * observer_collapse_strength)
                        + (0.10f * spider_projection_coherence)
                        + (0.08f * spider_harmonic_gate)
                        + (0.06f * spider_code_frequency)
                        + (0.04f * (1.0f - spider_noise_sink)))
                    : clamp01f(0.42f + (0.35f * harmonic) + (0.23f * (1.0f - std::min(radial, 1.0f))));
                const auto mining_direction = mining.active
                    ? normalize3f({
                        clamp_signed(mining.target_direction_xyz[0] + (0.35f * ((2.0f * lane_fraction) - 1.0f)) + (0.18f * std::sin(kTauF * sha_schedule)) + (0.14f * transfer_drive)),
                        clamp_signed(mining.target_direction_xyz[1] + (0.25f * std::sin(kTauF * lane_phase)) + (0.16f * std::cos(kTauF * sha_round)) + (0.12f * transport_drive)),
                        clamp_signed((0.45f * mining.target_direction_xyz[2]) + (0.20f * target_resonance) + (0.15f * resonance_activation) + (0.10f * phase_flux_conservation) + (0.05f * share_confidence) + (0.10f * sha_bias) + (0.08f * target_sequence_frequency) + (0.06f * reverse_observer_collapse) + (0.08f * sha_density) + (0.08f * nested_fourier_resonance))
                    })
                    : normalize3f({nx, ny, nz});

                CarrierCell9D carrier {};
                for (std::size_t channel = 0; channel < carrier.carrier_9d.size(); ++channel) {
                    carrier.carrier_9d[channel] = static_cast<float>(authoritative.texture_map_9d[channel]);
                }
                carrier.carrier_9d[0] = clamp01f(
                    static_cast<float>(authoritative.texture_map_9d[0] * (0.64 + (0.14 * harmonic)))
                    + (mining.active ? ((0.10f * lane_phase) + (0.08f * mining.target_frequency_norm) + (0.10f * sha_bias) + (0.08f * sha_schedule)) : 0.0f));
                carrier.carrier_9d[1] = clamp_signed(
                    static_cast<float>((0.45 * authoritative.texture_map_9d[1]) + (0.18 * nx))
                    + (0.37f * mining_direction[0]));
                carrier.carrier_9d[2] = clamp_signed(
                    static_cast<float>((0.45 * authoritative.texture_map_9d[2]) + (0.18 * ny))
                    + (0.37f * mining_direction[1]));
                carrier.carrier_9d[3] = clamp_signed(
                    static_cast<float>((0.45 * authoritative.texture_map_9d[3]) + (0.18 * nz))
                    + (0.37f * mining_direction[2]));
                carrier.carrier_9d[4] = clamp01f(
                    static_cast<float>(authoritative.texture_map_9d[4] * (0.58 + (0.18 * (1.0f - std::min(radial, 1.0f)))))
                    + (mining.active ? (0.16f * mining.phase_pressure_norm) + (0.12f * target_resonance) + (0.10f * sha_round) : 0.12f * mining_certainty));
                carrier.carrier_9d[5] = clamp01f(
                    static_cast<float>(0.62 * authoritative.texture_map_9d[5])
                    + (mining.active ? (0.22f * mining.lane_coherence_norm) + (0.16f * target_resonance) : 0.18f * mining_certainty));
                carrier.carrier_9d[6] = clamp01f(
                    static_cast<float>(authoritative.texture_map_9d[6] * (0.55 + (0.15 * harmonic)))
                    + (mining.active ? (0.18f * mining.worker_parallelism_norm) + (0.12f * sha_density) + (0.10f * target_resonance) : 0.18f * mining_certainty));
                carrier.carrier_9d[7] = clamp01f(
                    static_cast<float>(authoritative.texture_map_9d[7] * (0.52 + (0.18 * (1.0f - std::min(radial, 1.0f)))))
                    + (mining.active ? (0.16f * mining.target_frequency_norm) + (0.12f * sha_digest) + (0.10f * sha_bias) : 0.16f * mining_certainty));
                carrier.carrier_9d[8] = clamp01f(
                    static_cast<float>(authoritative.texture_map_9d[8] * (0.56 + (0.12 * harmonic)))
                    + (0.18f * mining_certainty)
                    + (mining.active ? (0.14f * target_resonance) + (0.12f * sha_density) : 0.0f));
                carrier.quartet = PulseQuartet{
                    clamp01f(saved_state.pulse_quartet[0] + (0.08f * lane_phase) + (mining.active ? ((0.10f * spider_code_frequency) + (0.06f * sha_schedule) + (0.06f * sha_bias) + (0.06f * transport_drive) + (0.04f * spider_harmonic_gate)) : 0.0f)),
                    clamp01f(saved_state.pulse_quartet[1] + (0.10f * mining_certainty) + (mining.active ? ((0.10f * spider_code_amplitude) + (0.06f * target_resonance) + (0.06f * sha_round) + (0.04f * reverse_observer_collapse) + (0.06f * stability_gate) + (0.04f * spider_projection_coherence)) : 0.0f)),
                    clamp01f(saved_state.pulse_quartet[2] + (0.08f * mining_certainty) + (mining.active ? ((0.10f * spider_code_amperage) + (0.06f * sha_density) + (0.04f * target_repeat_flux) + (0.06f * sha_digest) + (0.06f * phase_flux_conservation) + (0.04f * spider_harmonic_gate)) : 0.0f)),
                    clamp01f(saved_state.pulse_quartet[3] + (0.08f * mining_certainty) + (mining.active ? ((0.10f * spider_code_voltage) + (0.06f * target_resonance) + (0.04f * pool_submit_vector) + (0.06f * sha_bias) + (0.06f * resonance_activation) + (0.04f * spider_projection_coherence)) : 0.0f)),
                };
                carrier.phase_direction_xyz = {
                    mining_direction[0],
                    mining_direction[1],
                    mining_direction[2],
                };
                carrier.phase_magnitude = clamp01f(saved_state.phase_magnitude + (0.14f * mining_certainty) + (0.08f * target_resonance));
                carrier.phase_bias = clamp01f(saved_state.phase_lock_error + (mining.active ? (0.18f * (1.0f - mining.lane_coherence_norm)) : 0.08f * (1.0f - mining_certainty)));
                carrier.band_weight = clamp01f(saved_state.anchor_correlation + (0.14f * mining_certainty) + (0.08f * target_resonance));
                carrier.zero_point_proximity = clamp01f(saved_state.zero_point_proximity + (0.16f * (1.0f - std::abs((2.0f * lane_phase) - 1.0f))));
                carrier.resonance_energy = clamp01f(saved_state.resonance_energy + (0.14f * mining_certainty) + (0.10f * target_resonance));
                carrier.compton_frequency_hz = 1.0e6f * clamp01f(static_cast<float>(0.25 + authoritative.texture_map_9d[0] + (0.10 * harmonic)));
                carrier.carrier_signature = index;
                carrier.latent_attractor_id = mining.active ? lane_index : index;
                carrier.association_mask = mining.active ? (1U << (lane_index % 31U)) : 0U;
                carriers[index] = carrier;

                TensorEncounter6DoF tensor {};
                tensor.phase_coherence = clamp01f(static_cast<float>(authoritative.tensor_signature_6d[0]) + (0.14f * mining_certainty) + (0.08f * target_resonance));
                tensor.curvature = static_cast<float>(authoritative.tensor_signature_6d[1] + (0.18 * radial));
                tensor.flux = clamp01f(static_cast<float>(authoritative.tensor_signature_6d[2] * (0.68 + (0.18 * harmonic))) + (0.10f * mining_certainty) + (0.08f * sha_density));
                tensor.inertia = clamp01f(static_cast<float>(authoritative.tensor_signature_6d[3]));
                tensor.dtheta_dt = static_cast<float>(authoritative.tensor_signature_6d[4] + (0.10 * nx))
                    + (mining.active ? (0.12f * (lane_phase - 0.5f)) + (0.08f * (sha_schedule - 0.5f)) : 0.0f);
                tensor.d2theta_dt2 = static_cast<float>(authoritative.tensor_signature_6d[5] + (0.10 * ny))
                    + (mining.active ? (0.08f * (mining.header_phase_turns - 0.5f)) + (0.08f * (sha_round - 0.5f)) + (0.06f * (sha_digest - 0.5f)) : 0.0f);
                tensor.freq_xyz = {
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[0] * (0.35 + (0.30 * authoritative.texture_map_9d[1]))) + (0.35f * mining_direction[0])),
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[1] * (0.35 + (0.30 * authoritative.texture_map_9d[2]))) + (0.35f * mining_direction[1])),
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[2] * (0.35 + (0.30 * authoritative.texture_map_9d[3]))) + (0.35f * mining_direction[2])),
                };
                tensor.spin_vector = {
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[0] * (0.30 + (0.30 * authoritative.texture_map_9d[6]))) + (0.40f * mining_direction[0])),
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[1] * (0.30 + (0.30 * authoritative.texture_map_9d[6]))) + (0.40f * mining_direction[1])),
                    clamp_signed(static_cast<float>(authoritative.viewport_direction[2] * (0.30 + (0.30 * authoritative.texture_map_9d[6]))) + (0.40f * mining_direction[2])),
                };
                tensor.oam_twist = static_cast<float>(authoritative.visual_rgba[2] + (0.15 * harmonic)) + (0.08f * mining_certainty);
                tensor.conservation_pressure = clamp01f(static_cast<float>(1.0 - authoritative.anchor_correlation) + (0.18f * (1.0f - mining_certainty)));
                tensor.external_resistance = clamp01f(static_cast<float>(authoritative.sideband_energy_norm) + (0.16f * (1.0f - mining_certainty)));
                tensors[index] = tensor;

                AssociationSignature6DoF association {};
                association.node_to_segment = clamp01f((0.45f * harmonic) + (0.25f * transfer_drive) + (0.30f * mining_certainty));
                association.node_to_face = clamp01f((0.24f * (1.0f - std::min(radial, 1.0f))) + (0.20f * mining.worker_parallelism_norm) + (0.18f * target_resonance) + (0.18f * resonance_activation) + (0.20f * mining_certainty));
                association.node_to_volume = clamp01f((0.22f * mining.worker_parallelism_norm) + (0.18f * phase_flux_conservation) + (0.18f * nonce_collapse_confidence) + (0.18f * validation_rate) + (0.24f * mining_certainty));
                association.segment_to_face = clamp01f((0.30f * std::abs(mining_direction[0])) + (0.20f * harmonic) + (0.18f * share_confidence) + (0.16f * mining_certainty) + (0.16f * transfer_drive));
                association.segment_to_volume = clamp01f((0.30f * std::abs(mining_direction[1])) + (0.18f * mining.worker_parallelism_norm) + (0.18f * transport_drive) + (0.18f * mining_certainty) + (0.16f * resonance_activation));
                association.face_to_volume = clamp01f((0.30f * std::abs(mining_direction[2])) + (0.16f * phase_flux_conservation) + (0.16f * nonce_collapse_confidence) + (0.18f * share_confidence) + (0.20f * harmonic));
                association.nanoscale_segment_id = static_cast<float>(lane_index);
                association.boundary_sharpness = clamp01f((0.18f * mining_certainty) + (0.16f * mining.lane_coherence_norm) + (0.16f * target_resonance) + (0.12f * resonance_activation) + (0.12f * phase_flux_conservation) + (0.12f * share_confidence) + (0.14f * (1.0f - std::min(radial, 1.0f))));
                associations[index] = association;

                const std::uint32_t target_x = mining.active
                    ? static_cast<std::uint32_t>(std::min<float>(static_cast<float>(current_extent[0] - 1U), wrap01f(lane_phase + (0.25f * mining_direction[0])) * static_cast<float>(current_extent[0] - 1U)))
                    : x;
                const std::uint32_t target_y = mining.active
                    ? static_cast<std::uint32_t>(std::min<float>(static_cast<float>(current_extent[1] - 1U), wrap01f(lane_phase + (0.25f * mining_direction[1])) * static_cast<float>(current_extent[1] - 1U)))
                    : y;
                const std::uint32_t target_z = mining.active
                    ? static_cast<std::uint32_t>(std::min<float>(static_cast<float>(current_extent[2] - 1U), wrap01f(lane_phase + (0.25f * mining_direction[2])) * static_cast<float>(current_extent[2] - 1U)))
                    : z;

                ActuationGateState gate {};
                gate.readiness_norm = clamp01f((0.20f * mining.phase_pressure_norm) + (0.18f * mining.lane_coherence_norm) + (0.14f * mining.worker_parallelism_norm) + (0.18f * mining_certainty) + (0.16f * target_resonance) + (0.14f * sha_bias));
                gate.coherence_gate = clamp01f((0.34f * carrier.band_weight) + (0.22f * tensor.phase_coherence) + (0.22f * phase_flux_conservation) + (0.22f * nonce_collapse_confidence));
                gate.conservation_gate = clamp01f((0.34f * (1.0f - tensor.conservation_pressure)) + (0.22f * stability_gate) + (0.22f * phase_flux_conservation) + (0.22f * validation_rate));
                gate.interference_gate = clamp01f((0.38f * (1.0f - tensor.external_resistance)) + (0.20f * (1.0f - damping)) + (0.22f * observer_collapse_strength) + (0.20f * transport_readiness));
                gate.zero_point_gate = clamp01f((0.45f * carrier.zero_point_proximity) + (0.30f * zero_point_proximity) + (0.25f * temporal_admissibility));
                gate.magnitude_gate = clamp01f((0.40f * carrier.phase_magnitude) + (0.20f * target_resonance) + (0.20f * resonance_activation) + (0.20f * share_confidence));
                gate.direction_gate = clamp01f(0.35f + (0.25f * clamp01f(0.5f + (0.5f * mining_direction[2]))) + (0.20f * transfer_drive) + (0.20f * transport_drive));
                gate.target_resonance_norm = target_resonance;
                gate.resonance_activation_norm = resonance_activation;
                gate.phase_flux_conservation = phase_flux_conservation;
                gate.nonce_collapse_confidence = nonce_collapse_confidence;
                gate.observer_collapse_strength = observer_collapse_strength;
                gate.transfer_drive_norm = transfer_drive;
                gate.stability_gate_norm = stability_gate;
                gate.damping_norm = damping;
                gate.transport_drive_norm = transport_drive;
                gate.validation_structure_norm = validation_structure;
                gate.target_sequence_frequency_norm = target_sequence_frequency;
                gate.target_repeat_flux_norm = target_repeat_flux;
                gate.reverse_observer_collapse_norm = reverse_observer_collapse;
                gate.spider_code_frequency_norm = spider_code_frequency;
                gate.spider_code_amplitude_norm = spider_code_amplitude;
                gate.spider_code_voltage_norm = spider_code_voltage;
                gate.spider_code_amperage_norm = spider_code_amperage;
                gate.spider_projection_coherence_norm = spider_projection_coherence;
                gate.spider_harmonic_gate_norm = spider_harmonic_gate;
                gate.spider_noise_sink_norm = spider_noise_sink;
                gate.frontier_activation_budget_norm = frontier_activation_budget;
                gate.cumulative_activation_budget_norm = cumulative_activation_budget;
                gate.pulse_operator_density_norm = pulse_operator_density;
                gate.nested_fourier_resonance_norm = nested_fourier_resonance;
                gate.pool_ingest_vector_norm = pool_ingest_vector;
                gate.pool_submit_vector_norm = pool_submit_vector;
                gate.fourier_branch_factor = mining.fourier_branch_factor;
                gate.fourier_inner_tier_depth = mining.fourier_inner_tier_depth;
                gate.fourier_frontier_tier_depth = mining.fourier_frontier_tier_depth;
                gate.pulse_operator_capacity_bits = mining.pulse_operator_capacity_bits;
                gate.readiness_norm = clamp01f((0.15f * gate.readiness_norm) + (0.10f * gate.coherence_gate) + (0.08f * gate.conservation_gate) + (0.08f * gate.interference_gate) + (0.08f * gate.zero_point_gate) + (0.08f * gate.magnitude_gate) + (0.08f * gate.direction_gate) + (0.07f * resonance_activation) + (0.06f * transport_readiness) + (0.06f * validation_rate) + (0.05f * target_sequence_frequency) + (0.05f * reverse_observer_collapse) + (0.05f * spider_harmonic_gate) + (0.05f * spider_projection_coherence) + (0.05f * spider_code_frequency) + (0.05f * (1.0f - spider_noise_sink)) + (0.06f * validation_structure));
                gate.certainty = clamp01f((0.20f * mining_certainty) + (0.14f * target_resonance) + (0.10f * resonance_activation) + (0.10f * phase_flux_conservation) + (0.10f * nonce_collapse_confidence) + (0.10f * share_confidence) + (0.08f * validation_structure) + (0.05f * target_sequence_frequency) + (0.05f * reverse_observer_collapse) + (0.04f * spider_code_amplitude) + (0.04f * spider_projection_coherence));
                gate.refusal_code = gate.readiness_norm >= 0.5f ? 0U : 1U;
                gate.activation_tick = activation_tick;
                gate.target_lattice_index = flatten_index(target_x, target_y, target_z, current_extent);
                gate.active_worker_count = mining.active_worker_count;
                gate.attempted_nonce_count = mining.attempted_nonce_count;
                gate.valid_nonce_count = mining.valid_nonce_count;
                gate.mining_flags = mining_flags;
                gates[index] = gate;

                NextPulseOutput pulse {};
                pulse.quartet = PulseQuartet{
                    clamp01f(carrier.quartet.frequency + (0.08f * lane_phase) + (0.08f * sha_schedule) + (0.06f * transport_drive)),
                    clamp01f(carrier.quartet.amplitude + (0.08f * mining_certainty) + (0.08f * target_resonance) + (0.06f * resonance_activation)),
                    clamp01f(carrier.quartet.amperage + (0.08f * mining.phase_pressure_norm) + (0.08f * sha_density) + (0.06f * phase_flux_conservation)),
                    clamp01f(carrier.quartet.voltage + (0.08f * mining.worker_parallelism_norm) + (0.08f * sha_bias) + (0.06f * nonce_collapse_confidence)),
                };
                pulse.target_direction_xyz = {mining_direction[0], mining_direction[1], mining_direction[2]};
                pulse.correction_strength = clamp01f((0.42f * gate.readiness_norm) + (0.18f * transfer_drive) + (0.16f * stability_gate) + (0.12f * transport_drive) + (0.12f * resonance_activation));
                pulse.certainty = clamp01f((0.22f * mining_certainty) + (0.16f * target_resonance) + (0.14f * resonance_activation) + (0.14f * phase_flux_conservation) + (0.12f * nonce_collapse_confidence) + (0.10f * observer_collapse_strength) + (0.12f * valid_nonce_fraction));
                pulse.share_target_phase_turns = mining.share_target_phase_turns;
                pulse.header_phase_turns = mining.header_phase_turns;
                pulse.nonce_origin_phase_turns = mining.nonce_origin_phase_turns;
                pulse.target_sequence_phase_turns = target_sequence_phase;
                pulse.sha256_schedule_phase_turns = mining.sha256_schedule_phase_turns;
                pulse.sha256_round_phase_turns = mining.sha256_round_phase_turns;
                pulse.sha256_digest_phase_turns = mining.sha256_digest_phase_turns;
                pulse.validation_structure_norm = validation_structure;
                pulse.target_sequence_frequency_norm = target_sequence_frequency;
                pulse.target_repeat_flux_norm = target_repeat_flux;
                pulse.reverse_observer_collapse_norm = reverse_observer_collapse;
                pulse.spider_code_frequency_norm = spider_code_frequency;
                pulse.spider_code_amplitude_norm = spider_code_amplitude;
                pulse.spider_code_voltage_norm = spider_code_voltage;
                pulse.spider_code_amperage_norm = spider_code_amperage;
                pulse.spider_projection_coherence_norm = spider_projection_coherence;
                pulse.spider_harmonic_gate_norm = spider_harmonic_gate;
                pulse.spider_noise_sink_norm = spider_noise_sink;
                pulse.frontier_activation_budget_norm = frontier_activation_budget;
                pulse.cumulative_activation_budget_norm = cumulative_activation_budget;
                pulse.pulse_operator_density_norm = pulse_operator_density;
                pulse.nested_fourier_resonance_norm = nested_fourier_resonance;
                pulse.pool_ingest_vector_norm = pool_ingest_vector;
                pulse.pool_submit_vector_norm = pool_submit_vector;
                pulse.temporal_admissibility = temporal_admissibility;
                pulse.zero_point_proximity = zero_point_proximity;
                pulse.source_carrier_id = carrier.latent_attractor_id;
                pulse.activation_tick = activation_tick;
                pulse.fourier_branch_factor = mining.fourier_branch_factor;
                pulse.fourier_inner_tier_depth = mining.fourier_inner_tier_depth;
                pulse.fourier_frontier_tier_depth = mining.fourier_frontier_tier_depth;
                pulse.pulse_operator_capacity_bits = mining.pulse_operator_capacity_bits;
                pulse.target_x = target_x;
                pulse.target_y = target_y;
                pulse.target_z = target_z;
                pulses[index] = pulse;

                MiningLaneWorkItem lane_work {};
                if (!active_worker_slots.empty() && gpu_mining.active) {
                    const std::size_t active_slot =
                        active_worker_slots[index % active_worker_slots.size()];
                    const auto& worker = gpu_mining.workers[active_slot];
                    lane_work.header_template_words = worker.header_template_words;
                    lane_work.share_target_words = gpu_mining.share_target_words;
                    lane_work.block_target_words = gpu_mining.block_target_words;
                    lane_work.worker_index = worker.worker_index;
                    lane_work.nonce_start = worker.nonce_start;
                    lane_work.nonce_end = worker.nonce_end;
                    const std::uint64_t nonce_span =
                        static_cast<std::uint64_t>(worker.nonce_end) - static_cast<std::uint64_t>(worker.nonce_start) + 1ULL;
                    const std::uint32_t lane_scramble =
                        ((index + 1U) * 2654435761U)
                        ^ (activation_tick * 2246822519U)
                        ^ (worker.worker_index * 3266489917U);
                    lane_work.lane_nonce_seed = worker.nonce_start
                        + static_cast<std::uint32_t>(lane_scramble % std::max<std::uint64_t>(nonce_span, 1ULL));
                    lane_work.flags = 0x1U;
                }
                lane_work_items[index] = lane_work;

                MiningValidationState validation {};
                validation.validation_structure_norm = validation_structure;
                validation.pool_ingest_vector_norm = pool_ingest_vector;
                validation.pool_submit_vector_norm = pool_submit_vector;
                validation.target_sequence_phase_turns = target_sequence_phase;
                validation.target_sequence_frequency_norm = target_sequence_frequency;
                validation.target_repeat_flux_norm = target_repeat_flux;
                validation.reverse_observer_collapse_norm = reverse_observer_collapse;
                validation.spider_code_frequency_norm = spider_code_frequency;
                validation.spider_code_amplitude_norm = spider_code_amplitude;
                validation.spider_code_voltage_norm = spider_code_voltage;
                validation.spider_code_amperage_norm = spider_code_amperage;
                validation.spider_projection_coherence_norm = spider_projection_coherence;
                validation.spider_harmonic_gate_norm = spider_harmonic_gate;
                validation.spider_noise_sink_norm = spider_noise_sink;
                validation.frontier_activation_budget_norm = frontier_activation_budget;
                validation.cumulative_activation_budget_norm = cumulative_activation_budget;
                validation.pulse_operator_density_norm = pulse_operator_density;
                validation.nested_fourier_resonance_norm = nested_fourier_resonance;
                validation.selected_lane_phase_turns = lane_phase;
                validation.selected_lane_index = lane_index;
                validation.activation_tick = activation_tick;
                validation.active_worker_count = mining.active_worker_count;
                validation.fourier_branch_factor = mining.fourier_branch_factor;
                validation.fourier_inner_tier_depth = mining.fourier_inner_tier_depth;
                validation.fourier_frontier_tier_depth = mining.fourier_frontier_tier_depth;
                validation.pulse_operator_capacity_bits = mining.pulse_operator_capacity_bits;
                validation.attempted_nonce_count = mining.attempted_nonce_count;
                validation.valid_nonce_count = mining.valid_nonce_count;
                validation.share_target_pass_norm = mining.share_target_pass_norm;
                validation.block_target_pass_norm = mining.block_target_pass_norm;
                validation.block_coherence_norm = mining.block_coherence_norm;
                validation.reinforcement_norm = mining.reinforcement_norm;
                validation.noise_lane_fraction_norm = mining.noise_lane_fraction_norm;
                validation.submit_priority_score_norm = mining.submit_priority_score_norm;
                validation.resonance_reinforcement_count = mining.resonance_reinforcement_count;
                validation.noise_lane_count = mining.noise_lane_count;
                validation.queue_quality_class = mining.queue_quality_class;
                validations[index] = validation;
            }
        }
    }
}

bool SubstrateComputeRuntime::Impl::dispatch(const FieldViewportFrame& frame) {
    std::vector<CarrierCell9D> carriers;
    std::vector<TensorEncounter6DoF> tensors;
    std::vector<AssociationSignature6DoF> associations;
    std::vector<ActuationGateState> gates;
    std::vector<NextPulseOutput> pulses;
    std::vector<MiningLaneWorkItem> lane_work_items;
    std::vector<MiningValidationState> validations;
    pack_authoritative_buffers(frame, carriers, tensors, associations, gates, pulses, lane_work_items, validations);

    upload_bytes(device, carrier_buffer, carriers.data(), carriers.size() * sizeof(CarrierCell9D));
    upload_bytes(device, tensor_buffer, tensors.data(), tensors.size() * sizeof(TensorEncounter6DoF));
    upload_bytes(device, association_buffer, associations.data(), associations.size() * sizeof(AssociationSignature6DoF));
    upload_bytes(device, gate_buffer, gates.data(), gates.size() * sizeof(ActuationGateState));
    upload_bytes(device, pulse_buffer, pulses.data(), pulses.size() * sizeof(NextPulseOutput));
    upload_bytes(device, mining_lane_work_buffer, lane_work_items.data(), lane_work_items.size() * sizeof(MiningLaneWorkItem));
    upload_bytes(device, mining_validation_buffer, validations.data(), validations.size() * sizeof(MiningValidationState));

    VkQueryPool timestamp_query_pool = VK_NULL_HANDLE;
    VkQueryPool pipeline_stats_query_pool = VK_NULL_HANDLE;
    const bool collect_driver_timestamps =
        telemetry_capabilities.supports_timestamp_queries && reset_query_pool != nullptr;
    const bool collect_pipeline_statistics =
        telemetry_capabilities.supports_pipeline_statistics && reset_query_pool != nullptr;
    if (collect_driver_timestamps) {
        timestamp_query_pool = create_query_pool(device, VK_QUERY_TYPE_TIMESTAMP, 2U);
    }
    if (collect_pipeline_statistics) {
        pipeline_stats_query_pool = create_query_pool(
            device,
            VK_QUERY_TYPE_PIPELINE_STATISTICS,
            1U,
            VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT);
    }
    if (timestamp_query_pool != VK_NULL_HANDLE) {
        reset_query_pool(device, timestamp_query_pool, 0U, 2U);
    }
    if (pipeline_stats_query_pool != VK_NULL_HANDLE) {
        reset_query_pool(device, pipeline_stats_query_pool, 0U, 1U);
    }

    GpuKernelIterationEvent kernel_iteration;
    kernel_iteration.kernel_iteration = ++kernel_iteration_counter;
    kernel_iteration.kernel_name = "vkcompute_phase_substrate_update";
    kernel_iteration.kernel_phase = GpuKernelIterationPhase::Launch;
    kernel_iteration.launch_timestamp_s = steady_now_seconds();
    kernel_iteration.lattice_extent = current_extent;
    kernel_iteration.workgroup_count = {
        (current_extent[0] + 7U) / 8U,
        (current_extent[1] + 7U) / 8U,
        current_extent[2],
    };

    vkResetCommandPool(device, command_pool, 0);
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
        return false;
    }

    const auto transition_to_general = [&](VkImage image) {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = images_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
    };

    transition_to_general(visual_image.image);
    transition_to_general(material_image.image);
    transition_to_general(audio_image.image);

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    PushConstants push_constants {};
    push_constants.lattice_extent[0] = static_cast<std::int32_t>(current_extent[0]);
    push_constants.lattice_extent[1] = static_cast<std::int32_t>(current_extent[1]);
    push_constants.lattice_extent[2] = static_cast<std::int32_t>(current_extent[2]);
    push_constants.activation_tick = ++activation_tick;
    vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);
    if (timestamp_query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamp_query_pool, 0U);
    }
    if (pipeline_stats_query_pool != VK_NULL_HANDLE) {
        vkCmdBeginQuery(command_buffer, pipeline_stats_query_pool, 0U, 0U);
    }
    vkCmdDispatch(
        command_buffer,
        kernel_iteration.workgroup_count[0],
        kernel_iteration.workgroup_count[1],
        kernel_iteration.workgroup_count[2]);
    if (pipeline_stats_query_pool != VK_NULL_HANDLE) {
        vkCmdEndQuery(command_buffer, pipeline_stats_query_pool, 0U);
    }
    if (timestamp_query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamp_query_pool, 1U);
    }

    VkMemoryBarrier shader_barrier {};
    shader_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shader_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    shader_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        1,
        &shader_barrier,
        0,
        nullptr,
        0,
        nullptr);

    const auto copy_image_to_buffer = [&](VkImage image, const BufferHandle& buffer) {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);

        VkBufferImageCopy copy_region {};
        copy_region.bufferOffset = 0;
        copy_region.bufferRowLength = 0;
        copy_region.bufferImageHeight = 0;
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.mipLevel = 0;
        copy_region.imageSubresource.baseArrayLayer = 0;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent = {current_extent[0], current_extent[1], current_extent[2]};
        vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy_region);

        VkImageMemoryBarrier restore_barrier {};
        restore_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restore_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restore_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        restore_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore_barrier.image = image;
        restore_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        restore_barrier.subresourceRange.baseMipLevel = 0;
        restore_barrier.subresourceRange.levelCount = 1;
        restore_barrier.subresourceRange.baseArrayLayer = 0;
        restore_barrier.subresourceRange.layerCount = 1;
        restore_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restore_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &restore_barrier);
    };

    copy_image_to_buffer(visual_image.image, visual_readback_buffer);
    copy_image_to_buffer(material_image.image, material_readback_buffer);
    copy_image_to_buffer(audio_image.image, audio_readback_buffer);

    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        destroy_query_pool(device, pipeline_stats_query_pool);
        destroy_query_pool(device, timestamp_query_pool);
        return false;
    }

    publish_kernel_iteration(kernel_iteration);

    VkSubmitInfo submit_info {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    if (vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        destroy_query_pool(device, pipeline_stats_query_pool);
        destroy_query_pool(device, timestamp_query_pool);
        return false;
    }
    vkQueueWaitIdle(queue);
    kernel_iteration.kernel_phase = GpuKernelIterationPhase::Completion;
    kernel_iteration.completion_timestamp_s = steady_now_seconds();

    if (timestamp_query_pool != VK_NULL_HANDLE) {
        std::array<std::uint64_t, 2> gpu_timestamp_ticks {0U, 0U};
        if (vkGetQueryPoolResults(
                device,
                timestamp_query_pool,
                0U,
                2U,
                sizeof(gpu_timestamp_ticks),
                gpu_timestamp_ticks.data(),
                sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            const double timestamp_period_s =
                static_cast<double>(telemetry_capabilities.properties.limits.timestampPeriod) * 1.0e-9;
            kernel_iteration.driver_provider = "vulkan-query-pool";
            kernel_iteration.driver_timing_valid = true;
            kernel_iteration.gpu_execution_time_s =
                std::max(0.0, static_cast<double>(gpu_timestamp_ticks[1] - gpu_timestamp_ticks[0]) * timestamp_period_s);
            if (telemetry_capabilities.supports_calibrated_timestamps
                && get_calibrated_timestamps != nullptr
                && telemetry_capabilities.preferred_host_time_domain != kInvalidTimeDomain) {
                VkCalibratedTimestampInfoEXT timestamp_info[2] {};
                timestamp_info[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
                timestamp_info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
                timestamp_info[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
                timestamp_info[1].timeDomain = telemetry_capabilities.preferred_host_time_domain;

                std::uint64_t calibrated_timestamps[2] {0U, 0U};
                std::uint64_t max_deviation = 0U;
                if (get_calibrated_timestamps(device, 2U, timestamp_info, calibrated_timestamps, &max_deviation) == VK_SUCCESS) {
                    const double host_time_s = host_time_domain_timestamp_to_seconds(
                        calibrated_timestamps[1],
                        telemetry_capabilities.preferred_host_time_domain);
                    const double launch_delta_s =
                        gpu_tick_delta_seconds(calibrated_timestamps[0], gpu_timestamp_ticks[0], timestamp_period_s);
                    const double completion_delta_s =
                        gpu_tick_delta_seconds(calibrated_timestamps[0], gpu_timestamp_ticks[1], timestamp_period_s);
                    kernel_iteration.gpu_launch_timestamp_s = host_time_s - launch_delta_s;
                    kernel_iteration.gpu_completion_timestamp_s = host_time_s - completion_delta_s;
                }
                (void)max_deviation;
            }

            if (kernel_iteration.gpu_launch_timestamp_s <= 0.0 || kernel_iteration.gpu_completion_timestamp_s <= 0.0) {
                kernel_iteration.gpu_launch_timestamp_s = kernel_iteration.launch_timestamp_s;
                kernel_iteration.gpu_completion_timestamp_s =
                    kernel_iteration.gpu_launch_timestamp_s + kernel_iteration.gpu_execution_time_s;
            }
        }
    }

    if (pipeline_stats_query_pool != VK_NULL_HANDLE) {
        std::uint64_t compute_invocations = 0U;
        if (vkGetQueryPoolResults(
                device,
                pipeline_stats_query_pool,
                0U,
                1U,
                sizeof(compute_invocations),
                &compute_invocations,
                sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            kernel_iteration.pipeline_statistics_valid = true;
            kernel_iteration.compute_invocation_count = compute_invocations;
            if (kernel_iteration.driver_provider.empty()) {
                kernel_iteration.driver_provider = "vulkan-query-pool";
            }
        }
    }

    publish_kernel_iteration(kernel_iteration);
    destroy_query_pool(device, pipeline_stats_query_pool);
    destroy_query_pool(device, timestamp_query_pool);

    visual_surface = download_half_rgba_surface(device, visual_readback_buffer, texel_count);
    material_surface = download_half_rgba_surface(device, material_readback_buffer, texel_count);
    audio_surface = download_half_rgba_surface(device, audio_readback_buffer, texel_count);
    mining_validation_surface = download_buffer_vector<MiningValidationState>(device, mining_validation_buffer, texel_count);
    if (!mining_validation_surface.empty()) {
        std::uint32_t attempted_nonce_total = 0U;
        std::uint32_t valid_nonce_total = 0U;
        std::uint32_t share_positive_lane_count = 0U;
        std::uint32_t block_positive_lane_count = 0U;
        std::uint32_t noise_lane_total = 0U;
        for (const MiningValidationState& validation : mining_validation_surface) {
            attempted_nonce_total += validation.attempted_nonce_count;
            valid_nonce_total += validation.valid_nonce_count;
            noise_lane_total += validation.noise_lane_count;
            if (validation.valid_nonce_count > 0U || (validation.validation_flags & 0x40U) != 0U) {
                ++share_positive_lane_count;
            }
            if ((validation.validation_flags & 0x80U) != 0U) {
                ++block_positive_lane_count;
            }
        }
        const auto best_it = std::max_element(
            mining_validation_surface.begin(),
            mining_validation_surface.end(),
            [](const MiningValidationState& lhs, const MiningValidationState& rhs) {
                const std::tuple<int, int, float, float, float> lhs_rank {
                    (lhs.validation_flags & 0x80U) != 0U ? 1 : 0,
                    (lhs.valid_nonce_count > 0U || (lhs.validation_flags & 0x40U) != 0U) ? 1 : 0,
                    lhs.submit_priority_score_norm,
                    lhs.validation_certainty_norm,
                    lhs.accepted_lane_fraction,
                };
                const std::tuple<int, int, float, float, float> rhs_rank {
                    (rhs.validation_flags & 0x80U) != 0U ? 1 : 0,
                    (rhs.valid_nonce_count > 0U || (rhs.validation_flags & 0x40U) != 0U) ? 1 : 0,
                    rhs.submit_priority_score_norm,
                    rhs.validation_certainty_norm,
                    rhs.accepted_lane_fraction,
                };
                return lhs_rank < rhs_rank;
            });
        std::uint32_t snapshot_validation_flags = best_it->validation_flags;
        if (has_nonzero_hash_words(best_it->selected_hash_words)
            && best_it->selected_worker_index < frame.gpu_mining_authority.workers.size()) {
            const auto& worker = frame.gpu_mining_authority.workers[best_it->selected_worker_index];
            if (worker.active
                && !frame.gpu_mining_authority.nbits_hex.empty()
                && frame.gpu_mining_authority.share_difficulty > 0.0) {
                const std::string header_hex = words_to_hex_be(worker.header_template_words);
                const std::string gpu_hash_hex = words_to_hex_be(best_it->selected_hash_words);
                const SubstrateStratumPowEvaluation cpu_evaluation = evaluate_stratum_pow(
                    header_hex,
                    frame.gpu_mining_authority.nbits_hex,
                    best_it->selected_nonce_value,
                    frame.gpu_mining_authority.share_difficulty);
                snapshot_validation_flags |= 0x200U;
                if (cpu_evaluation.hash_hex == gpu_hash_hex) {
                    snapshot_validation_flags |= 0x400U;
                }
                if (cpu_evaluation.valid_share) {
                    snapshot_validation_flags |= 0x800U;
                }
                if (cpu_evaluation.valid_block) {
                    snapshot_validation_flags |= 0x1000U;
                }
            }
        }
        publish_mining_validation(MiningValidationSnapshot{
            best_it->same_pulse_validation_norm,
            best_it->candidate_surface_norm,
            best_it->validation_structure_norm,
            best_it->pool_ingest_vector_norm,
            best_it->pool_submit_vector_norm,
            best_it->target_sequence_phase_turns,
            best_it->target_sequence_frequency_norm,
            best_it->target_repeat_flux_norm,
            best_it->reverse_observer_collapse_norm,
            best_it->spider_code_frequency_norm,
            best_it->spider_code_amplitude_norm,
            best_it->spider_code_voltage_norm,
            best_it->spider_code_amperage_norm,
            best_it->spider_projection_coherence_norm,
            best_it->spider_harmonic_gate_norm,
            best_it->spider_noise_sink_norm,
            best_it->frontier_activation_budget_norm,
            best_it->cumulative_activation_budget_norm,
            best_it->pulse_operator_density_norm,
            best_it->nested_fourier_resonance_norm,
            best_it->target_phase_alignment_norm,
            best_it->header_phase_alignment_norm,
            best_it->nonce_phase_alignment_norm,
            best_it->sha_phase_alignment_norm,
            best_it->validation_certainty_norm,
            best_it->accepted_lane_fraction,
            best_it->selected_lane_phase_turns,
            best_it->selected_lane_index,
            best_it->activation_tick,
            best_it->active_worker_count,
            best_it->fourier_branch_factor,
            best_it->fourier_inner_tier_depth,
            best_it->fourier_frontier_tier_depth,
            best_it->pulse_operator_capacity_bits,
            attempted_nonce_total,
            valid_nonce_total,
            snapshot_validation_flags,
            share_positive_lane_count == 0U
                ? best_it->share_target_pass_norm
                : clamp01f(static_cast<float>(share_positive_lane_count) / static_cast<float>(mining_validation_surface.size())),
            block_positive_lane_count == 0U
                ? best_it->block_target_pass_norm
                : clamp01f(static_cast<float>(block_positive_lane_count) / static_cast<float>(mining_validation_surface.size())),
            best_it->block_coherence_norm,
            best_it->reinforcement_norm,
            attempted_nonce_total == 0U
                ? best_it->noise_lane_fraction_norm
                : clamp01f(static_cast<float>(noise_lane_total) / static_cast<float>(attempted_nonce_total)),
            best_it->submit_priority_score_norm,
            best_it->resonance_reinforcement_count,
            noise_lane_total,
            best_it->queue_quality_class,
            best_it->selected_worker_index,
            best_it->selected_nonce_value,
            best_it->selected_hash_words,
        });
    }
    images_initialized = true;
    return true;
}

void SubstrateComputeRuntime::Impl::build_preview_from_surfaces(std::uint32_t width, std::uint32_t height) {
    preview_width = std::max(width, 32U);
    preview_height = std::max(height, 32U);
    preview_rgba.assign(static_cast<std::size_t>(preview_width) * preview_height * 4U, 0U);

    for (std::uint32_t py = 0; py < preview_height; ++py) {
        const std::uint32_t sy = std::min(current_extent[1] - 1U, (py * current_extent[1]) / preview_height);
        for (std::uint32_t px = 0; px < preview_width; ++px) {
            const std::uint32_t sx = std::min(current_extent[0] - 1U, (px * current_extent[0]) / preview_width);
            float accum_r = 0.06f;
            float accum_g = 0.08f;
            float accum_b = 0.10f;
            float accum_a = 0.0f;

            for (std::uint32_t sz = 0; sz < current_extent[2]; ++sz) {
                const std::uint32_t cell = flatten_index(sx, sy, sz, current_extent);
                const std::size_t visual_offset = static_cast<std::size_t>(cell) * 4U;
                const float vr = clamp01f(visual_surface[visual_offset]);
                const float vg = clamp01f(visual_surface[visual_offset + 1U]);
                const float vb = clamp01f(visual_surface[visual_offset + 2U]);
                const float emissive = clamp01f(material_surface[visual_offset + 2U]);
                const float boundary = clamp01f(material_surface[visual_offset + 3U]);
                const float alpha = clamp01f(0.08f + (0.18f * boundary) + (0.12f * emissive));
                const float weight = (1.0f - accum_a) * alpha;
                accum_r += weight * clamp01f(vr + (0.20f * emissive));
                accum_g += weight * clamp01f(vg + (0.14f * emissive));
                accum_b += weight * clamp01f(vb + (0.28f * emissive));
                accum_a = clamp01f(accum_a + weight);
            }

            const std::size_t preview_offset = (static_cast<std::size_t>(py) * preview_width + px) * 4U;
            preview_rgba[preview_offset] = static_cast<std::uint8_t>(std::clamp(accum_r * 255.0f, 0.0f, 255.0f));
            preview_rgba[preview_offset + 1U] = static_cast<std::uint8_t>(std::clamp(accum_g * 255.0f, 0.0f, 255.0f));
            preview_rgba[preview_offset + 2U] = static_cast<std::uint8_t>(std::clamp(accum_b * 255.0f, 0.0f, 255.0f));
            preview_rgba[preview_offset + 3U] = 255U;
        }
    }
}

void SubstrateComputeRuntime::Impl::build_audio_from_surface() {
    constexpr std::size_t kFrameCount = 4096U;
    audio_frame.sample_rate_hz = 48000U;
    audio_frame.interleaved_samples.assign(kFrameCount * 2U, 0);
    if (texel_count == 0U) {
        return;
    }

    for (std::size_t sample_index = 0; sample_index < kFrameCount; ++sample_index) {
        const std::size_t cell = (static_cast<std::size_t>(activation_tick) * 97U + sample_index) % texel_count;
        const std::size_t offset = cell * 4U;
        const float left = std::clamp(audio_surface[offset] + (0.35f * audio_surface[offset + 2U]), -1.0f, 1.0f);
        const float right = std::clamp(audio_surface[offset + 1U] + (0.35f * audio_surface[offset + 3U]), -1.0f, 1.0f);
        audio_frame.interleaved_samples[sample_index * 2U] = static_cast<std::int16_t>(left * 32767.0f);
        audio_frame.interleaved_samples[(sample_index * 2U) + 1U] = static_cast<std::int16_t>(right * 32767.0f);
    }
}

bool SubstrateComputeRuntime::Impl::update(const FieldViewportFrame& frame, std::uint32_t width, std::uint32_t height) {
    if (!available) {
        return false;
    }

    const std::array<std::uint32_t, 3> extent {
        std::max(frame.extent_x, 1U),
        std::max(frame.extent_y, 1U),
        std::max(frame.extent_z, 1U),
    };
    if (!ensure_resources(extent)) {
        return false;
    }
    if (!dispatch(frame)) {
        return false;
    }

    build_preview_from_surfaces(width, height);
    build_audio_from_surface();
    return true;
}

#endif

SubstrateComputeRuntime::SubstrateComputeRuntime()
    : impl_(std::make_unique<Impl>()) {}

SubstrateComputeRuntime::~SubstrateComputeRuntime() {
#if defined(QBIT_MINER_HAS_VULKAN)
    impl_->cleanup();
#endif
}

bool SubstrateComputeRuntime::initialize() {
#if defined(QBIT_MINER_HAS_VULKAN)
    return impl_->initialize();
#else
    return false;
#endif
}

bool SubstrateComputeRuntime::is_available() const noexcept {
    return impl_->available;
}

bool SubstrateComputeRuntime::update(const FieldViewportFrame& frame, std::uint32_t preview_width, std::uint32_t preview_height) {
#if defined(QBIT_MINER_HAS_VULKAN)
    return impl_->update(frame, preview_width, preview_height);
#else
    (void)frame;
    (void)preview_width;
    (void)preview_height;
    return false;
#endif
}

void SubstrateComputeRuntime::set_kernel_iteration_observer(std::function<void(const GpuKernelIterationEvent&)> observer) {
    impl_->kernel_iteration_observer = std::move(observer);
}

void SubstrateComputeRuntime::set_mining_validation_observer(std::function<void(const MiningValidationSnapshot&)> observer) {
    impl_->mining_validation_observer = std::move(observer);
}

std::uint32_t SubstrateComputeRuntime::preview_width() const noexcept {
    return impl_->preview_width;
}

std::uint32_t SubstrateComputeRuntime::preview_height() const noexcept {
    return impl_->preview_height;
}

const std::vector<std::uint8_t>& SubstrateComputeRuntime::preview_rgba() const {
    return impl_->preview_rgba;
}

const StereoPcmFrame& SubstrateComputeRuntime::last_audio_frame() const {
    return impl_->audio_frame;
}

const std::wstring& SubstrateComputeRuntime::device_label() const {
    return impl_->device_label;
}

std::optional<GpuKernelIterationEvent> SubstrateComputeRuntime::last_kernel_iteration() const {
    return impl_->last_kernel_iteration;
}

std::optional<MiningValidationSnapshot> SubstrateComputeRuntime::last_mining_validation() const {
    return impl_->last_mining_validation;
}

}  // namespace qbit_miner
