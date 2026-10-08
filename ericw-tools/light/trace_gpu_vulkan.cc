/*  Copyright (C) 2026 ericw-tools contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

// Vulkan ray-query backend for vklight (the default; -cpu disables it). See include/light/trace_gpu.hh.

#include <light/trace_gpu.hh>

#include <common/bspfile.hh>
#include <common/log.hh>
#include <light/light.hh>
#include <light/trace_embree.hh>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace gpu_light
{
namespace
{

constexpr std::uint32_t NO_GEOMETRY = 0xffffffffu;
constexpr std::uint32_t WORKGROUP_SIZE = 64;
// keep each submit short: Windows resets the GPU (TDR) if one takes ~2 s
constexpr double TARGET_SUBMIT_MS = 100.0;

struct buffer_t
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct as_t
{
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    buffer_t storage;
    VkDeviceAddress address = 0;
};

struct direct_push_t
{
    std::uint32_t sample_count;
    std::uint32_t slot_count;
    std::uint32_t retrace_capacity;
    std::uint32_t sky_geometry;
    std::uint32_t sample_base;
};

struct surflight_push_t
{
    std::uint32_t sample_count;
    std::uint32_t sample_base;
    std::uint32_t slot_count;
    std::uint32_t retrace_capacity;
    float standard_scale;
    float sky_scale;
    float hotspot_clamp;
    float gate;
    float sky_dist;
    float scaledist;
    float dirtgain;
    float dirtscale;
    std::uint32_t dirt;
};

struct dirt_push_t
{
    std::uint32_t sample_count;
    std::uint32_t sample_base;
    std::uint32_t vector_count;
    std::uint32_t sky_geometry;
    float depth;
};

// device-local buffer fed from / read back through a host-visible one
struct staged_t
{
    buffer_t device;
    buffer_t host;
};

// a compute shader; binding 0 is the scene, the rest are storage buffers
struct pipeline_t
{
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t binding_count = 0;
    std::uint32_t push_size = 0;
    std::uint32_t dispatch_samples = 16384; // samples per submit, adapted to TARGET_SUBMIT_MS
};

struct context_t
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_props{};
    std::string device_name;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;

    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress_ = nullptr;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR_ = nullptr;

    std::vector<buffer_t> geometry_buffers;
    buffer_t instances;
    as_t blas;
    as_t tlas;
    std::uint32_t sky_geometry = NO_GEOMETRY;

    pipeline_t direct, dirt, surflight;

    staged_t lights, samples, faces, entries, accum, retraces, counters, sky_textures;
    staged_t dirt_vectors, dirt_occlusion, emitters, points;
    std::uint32_t retrace_capacity = 0;

    std::unordered_map<const void *, std::uint32_t> sky_texture_ids;
    stats_t stats;
};

std::mutex g_mutex;
context_t g;

std::string vk_result_string(VkResult r)
{
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return "VkResult(" + std::to_string(static_cast<int>(r)) + ")";
    }
}

bool check(VkResult r, const char *what, std::string &error)
{
    if (r == VK_SUCCESS)
        return true;
    error = std::string(what) + " failed: " + vk_result_string(r);
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties> &props, const char *name)
{
    return std::any_of(props.begin(), props.end(),
        [&](const VkExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
}

void destroy_buffer(buffer_t &b)
{
    if (b.mapped)
        vkUnmapMemory(g.device, b.memory);
    if (b.buffer)
        vkDestroyBuffer(g.device, b.buffer, nullptr);
    if (b.memory)
        vkFreeMemory(g.device, b.memory, nullptr);
    b = {};
}

void destroy_staged(staged_t &s)
{
    destroy_buffer(s.device);
    destroy_buffer(s.host);
}

void destroy_as(as_t &a)
{
    if (a.as)
        g.vkDestroyAccelerationStructureKHR_(g.device, a.as, nullptr);
    destroy_buffer(a.storage);
    a = {};
}

void destroy_locked()
{
    if (g.device) {
        vkDeviceWaitIdle(g.device);

        for (staged_t *s : {&g.lights, &g.samples, &g.faces, &g.entries, &g.accum, &g.retraces, &g.counters,
                 &g.sky_textures, &g.dirt_vectors, &g.dirt_occlusion, &g.emitters, &g.points}) {
            destroy_staged(*s);
        }
        for (pipeline_t *p : {&g.direct, &g.dirt, &g.surflight}) {
            if (p->pipeline)
                vkDestroyPipeline(g.device, p->pipeline, nullptr);
            if (p->layout)
                vkDestroyPipelineLayout(g.device, p->layout, nullptr);
            if (p->pool)
                vkDestroyDescriptorPool(g.device, p->pool, nullptr);
            if (p->set_layout)
                vkDestroyDescriptorSetLayout(g.device, p->set_layout, nullptr);
        }

        destroy_as(g.tlas);
        destroy_as(g.blas);
        destroy_buffer(g.instances);
        for (auto &b : g.geometry_buffers)
            destroy_buffer(b);

        if (g.command_pool)
            vkDestroyCommandPool(g.device, g.command_pool, nullptr);
        vkDestroyDevice(g.device, nullptr);
    }
    if (g.instance)
        vkDestroyInstance(g.instance, nullptr);
    g = {};
}

bool find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags props, std::uint32_t &type_index)
{
    for (std::uint32_t i = 0; i < g.memory_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && ((g.memory_props.memoryTypes[i].propertyFlags & props) == props)) {
            type_index = i;
            return true;
        }
    }
    return false;
}

enum class memory_t
{
    device, // device local
    upload, // host visible, written by the CPU
    readback // host visible + cached if possible, read by the CPU
};

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, memory_t memory, buffer_t &out, std::string &error)
{
    out = {};
    out.size = std::max<VkDeviceSize>(size, 16);

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = out.size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(g.device, &bi, nullptr, &out.buffer), "vkCreateBuffer", error))
        return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g.device, out.buffer, &req);

    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t mem_type = 0;
    bool found = false;
    if (memory == memory_t::device) {
        found = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem_type);
    } else if (memory == memory_t::readback) {
        found = find_memory_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, mem_type) ||
                find_memory_type(req.memoryTypeBits, host, mem_type);
    } else {
        found = find_memory_type(req.memoryTypeBits, host, mem_type);
    }
    if (!found) {
        error = "no compatible Vulkan memory type for buffer";
        destroy_buffer(out);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = flags.flags ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type;
    if (!check(vkAllocateMemory(g.device, &ai, nullptr, &out.memory), "vkAllocateMemory", error) ||
        !check(vkBindBufferMemory(g.device, out.buffer, out.memory, 0), "vkBindBufferMemory", error)) {
        destroy_buffer(out);
        return false;
    }
    if (memory != memory_t::device &&
        !check(vkMapMemory(g.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory", error)) {
        destroy_buffer(out);
        return false;
    }
    return true;
}

// grows (never shrinks) a staged buffer to hold `size` bytes
bool ensure_staged(staged_t &s, VkDeviceSize size, bool readback, std::string &error)
{
    if (s.device.buffer && s.device.size >= size)
        return true;
    destroy_staged(s);
    const VkDeviceSize alloc = std::max<VkDeviceSize>(size + size / 4, 4096);
    const VkBufferUsageFlags device_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkBufferUsageFlags host_usage =
        readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    return create_buffer(alloc, device_usage, memory_t::device, s.device, error) &&
           create_buffer(alloc, host_usage, readback ? memory_t::readback : memory_t::upload, s.host, error);
}

VkDeviceAddress buffer_address(const buffer_t &b)
{
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b.buffer;
    return g.vkGetBufferDeviceAddress_(g.device, &info);
}

bool submit(const std::function<void(VkCommandBuffer)> &record, std::string &error)
{
    if (!check(vkResetCommandBuffer(g.command_buffer, 0), "vkResetCommandBuffer", error))
        return false;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(g.command_buffer, &bi), "vkBeginCommandBuffer", error))
        return false;
    record(g.command_buffer);
    if (!check(vkEndCommandBuffer(g.command_buffer), "vkEndCommandBuffer", error))
        return false;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.command_buffer;
    return check(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit", error) &&
           check(vkQueueWaitIdle(g.queue), "vkQueueWaitIdle", error);
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
    VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

bool create_instance(std::string &error)
{
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ericw-tools light";
    app.pEngineName = "ericw-tools";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    return check(vkCreateInstance(&ci, nullptr, &g.instance), "vkCreateInstance", error);
}

bool pick_device(std::string &error)
{
    std::uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(g.instance, &count, nullptr), "vkEnumeratePhysicalDevices", error))
        return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (!check(vkEnumeratePhysicalDevices(g.instance, &count, devices.data()), "vkEnumeratePhysicalDevices", error))
        return false;

    // prefer a discrete GPU
    std::stable_sort(devices.begin(), devices.end(), [](VkPhysicalDevice a, VkPhysicalDevice b) {
        VkPhysicalDeviceProperties pa, pb;
        vkGetPhysicalDeviceProperties(a, &pa);
        vkGetPhysicalDeviceProperties(b, &pb);
        return (pa.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) >
               (pb.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    });

    for (VkPhysicalDevice dev : devices) {
        std::uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());
        if (!has_extension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
            !has_extension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
            !has_extension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            continue;
        }

        VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
        bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        VkPhysicalDeviceRayQueryFeaturesKHR rq{};
        rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rq.pNext = &bda;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
        as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        as.pNext = &rq;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &as;
        vkGetPhysicalDeviceFeatures2(dev, &f2);
        if (!as.accelerationStructure || !rq.rayQuery || !bda.bufferDeviceAddress)
            continue;

        std::uint32_t q_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, nullptr);
        std::vector<VkQueueFamilyProperties> qs(q_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, qs.data());
        for (std::uint32_t i = 0; i < q_count; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(dev, &props);
                g.physical = dev;
                g.queue_family = i;
                g.device_name = props.deviceName;
                vkGetPhysicalDeviceMemoryProperties(dev, &g.memory_props);
                return true;
            }
        }
    }

    error = "no Vulkan device with acceleration_structure + ray_query + buffer_device_address found";
    return false;
}

bool create_device(std::string &error)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
    bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bda.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{};
    rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rq.rayQuery = VK_TRUE;
    rq.pNext = &bda;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};
    as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    as.accelerationStructure = VK_TRUE;
    as.pNext = &rq;

    const char *extensions[] = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &as;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(std::size(extensions));
    dci.ppEnabledExtensionNames = extensions;
    if (!check(vkCreateDevice(g.physical, &dci, nullptr, &g.device), "vkCreateDevice", error))
        return false;
    vkGetDeviceQueue(g.device, g.queue_family, 0, &g.queue);

#define LOAD_DEVICE_PROC(name) \
    g.name##_ = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(g.device, #name)); \
    if (!g.name##_) { \
        error = "missing device proc " #name; \
        return false; \
    }
    LOAD_DEVICE_PROC(vkGetBufferDeviceAddress);
    LOAD_DEVICE_PROC(vkCreateAccelerationStructureKHR);
    LOAD_DEVICE_PROC(vkDestroyAccelerationStructureKHR);
    LOAD_DEVICE_PROC(vkGetAccelerationStructureBuildSizesKHR);
    LOAD_DEVICE_PROC(vkCmdBuildAccelerationStructuresKHR);
    LOAD_DEVICE_PROC(vkGetAccelerationStructureDeviceAddressKHR);
#undef LOAD_DEVICE_PROC

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g.queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!check(vkCreateCommandPool(g.device, &pci, nullptr, &g.command_pool), "vkCreateCommandPool", error))
        return false;

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g.command_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    return check(vkAllocateCommandBuffers(g.device, &cai, &g.command_buffer), "vkAllocateCommandBuffers", error);
}

bool create_acceleration_structure(
    VkAccelerationStructureTypeKHR type, VkDeviceSize size, as_t &out, std::string &error)
{
    if (!create_buffer(size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            memory_t::device, out.storage, error))
        return false;

    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.type = type;
    ci.size = size;
    ci.buffer = out.storage.buffer;
    if (!check(g.vkCreateAccelerationStructureKHR_(g.device, &ci, nullptr, &out.as),
            "vkCreateAccelerationStructureKHR", error))
        return false;

    VkAccelerationStructureDeviceAddressInfoKHR ai{};
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = out.as;
    out.address = g.vkGetAccelerationStructureDeviceAddressKHR_(g.device, &ai);
    return true;
}

bool build_as(VkAccelerationStructureBuildGeometryInfoKHR &build, const std::vector<std::uint32_t> &prim_counts,
    as_t &out, std::string &error)
{
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g.vkGetAccelerationStructureBuildSizesKHR_(
        g.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, prim_counts.data(), &sizes);

    if (!create_acceleration_structure(build.type, sizes.accelerationStructureSize, out, error))
        return false;

    buffer_t scratch;
    if (!create_buffer(sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, memory_t::device, scratch,
            error))
        return false;

    build.dstAccelerationStructure = out.as;
    build.scratchData.deviceAddress = buffer_address(scratch);

    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(prim_counts.size());
    for (size_t i = 0; i < prim_counts.size(); i++)
        ranges[i].primitiveCount = prim_counts[i];
    const VkAccelerationStructureBuildRangeInfoKHR *range_ptr = ranges.data();

    const bool ok = submit(
        [&](VkCommandBuffer cmd) { g.vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &build, &range_ptr); }, error);
    destroy_buffer(scratch);
    return ok;
}

// triangle soup (3 vertices per triangle) of one Embree geometry
void append_embree_triangles(const sceneinfo &info, std::vector<float> &out)
{
    if (info.triInfo.empty())
        return;
    RTCGeometry geom = rtcGetGeometry(scene, info.geomID);
    const float *verts = static_cast<const float *>(rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0));
    const int *tris = static_cast<const int *>(rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_INDEX, 0));
    for (size_t t = 0; t < info.triInfo.size(); t++) {
        for (int k = 0; k < 3; k++) {
            const float *v = verts + 4 * tris[3 * t + k]; // Embree vertices are padded to 4 floats
            out.insert(out.end(), v, v + 3);
        }
    }
}

bool build_scene(std::string &error)
{
    std::vector<float> solid, sky, filtered;
    append_embree_triangles(solidgeom, solid);
    for (const qvec3f &v : skipgeom_vertices)
        solid.insert(solid.end(), {v[0], v[1], v[2]});
    append_embree_triangles(skygeom, sky);
    append_embree_triangles(filtergeom, filtered);

    g.stats.solid_tris = solid.size() / 9;
    g.stats.sky_tris = sky.size() / 9;
    g.stats.filtered_tris = filtered.size() / 9;

    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<std::uint32_t> prim_counts;

    auto add_geometry = [&](const std::vector<float> &verts, bool opaque) -> bool {
        const std::uint32_t tri_count = static_cast<std::uint32_t>(verts.size() / 9);
        buffer_t &vb = g.geometry_buffers.emplace_back();
        if (!create_buffer(verts.size() * sizeof(float),
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                memory_t::upload, vb, error))
            return false;
        std::memcpy(vb.mapped, verts.data(), verts.size() * sizeof(float));

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
        geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geom.geometry.triangles.vertexData.deviceAddress = buffer_address(vb);
        geom.geometry.triangles.vertexStride = 3 * sizeof(float);
        geom.geometry.triangles.maxVertex = tri_count * 3 - 1;
        geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        geoms.push_back(geom);
        prim_counts.push_back(tri_count);
        return true;
    };

    if (solid.empty()) {
        error = "no solid geometry";
        return false;
    }
    if (!add_geometry(solid, true))
        return false;
    if (!sky.empty()) {
        g.sky_geometry = static_cast<std::uint32_t>(geoms.size());
        if (!add_geometry(sky, true))
            return false;
    }
    if (!filtered.empty() && !add_geometry(filtered, false))
        return false;

    VkAccelerationStructureBuildGeometryInfoKHR blas{};
    blas.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blas.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blas.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blas.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blas.geometryCount = static_cast<std::uint32_t>(geoms.size());
    blas.pGeometries = geoms.data();
    if (!build_as(blas, prim_counts, g.blas, error))
        return false;

    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = 1.0f;
    inst.transform.matrix[1][1] = 1.0f;
    inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xff;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = g.blas.address;
    if (!create_buffer(sizeof(inst),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            memory_t::upload, g.instances, error))
        return false;
    std::memcpy(g.instances.mapped, &inst, sizeof(inst));

    VkAccelerationStructureGeometryKHR tgeom{};
    tgeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tgeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeom.geometry.instances.data.deviceAddress = buffer_address(g.instances);

    VkAccelerationStructureBuildGeometryInfoKHR tlas{};
    tlas.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlas.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas.geometryCount = 1;
    tlas.pGeometries = &tgeom;
    return build_as(tlas, {1}, g.tlas, error);
}

// per sky triangle: id of its texture, for _suntexture
bool upload_sky_textures(std::string &error)
{
    std::vector<std::uint32_t> ids(std::max<size_t>(skygeom.triInfo.size(), 1), 0);
    for (size_t i = 0; i < skygeom.triInfo.size(); i++) {
        const void *tex = skygeom.triInfo[i].texture;
        auto [it, inserted] = g.sky_texture_ids.try_emplace(tex, static_cast<std::uint32_t>(g.sky_texture_ids.size()));
        ids[i] = it->second;
    }
    const VkDeviceSize size = ids.size() * sizeof(std::uint32_t);
    if (!ensure_staged(g.sky_textures, size, false, error))
        return false;
    std::memcpy(g.sky_textures.host.mapped, ids.data(), size);
    return submit(
        [&](VkCommandBuffer cmd) {
            VkBufferCopy copy{0, 0, size};
            vkCmdCopyBuffer(cmd, g.sky_textures.host.buffer, g.sky_textures.device.buffer, 1, &copy);
        },
        error);
}

std::filesystem::path exe_dir()
{
#if defined(_WIN32)
    std::array<wchar_t, 32768> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size())
        return std::filesystem::path(buf.data()).parent_path();
#elif defined(__linux__)
    std::array<char, PATH_MAX> buf{};
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0) {
        buf[static_cast<std::size_t>(len)] = '\0';
        return std::filesystem::path(buf.data()).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

bool read_spirv(const std::filesystem::path &path, std::vector<std::uint32_t> &words, std::string &error)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "could not open shader: " + path.string();
        return false;
    }
    const auto size = static_cast<std::streamoff>(f.tellg());
    if (size <= 0 || (size % 4) != 0) {
        error = "invalid SPIR-V: " + path.string();
        return false;
    }
    words.resize(static_cast<size_t>(size / 4));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(words.data()), size)) {
        error = "failed to read shader: " + path.string();
        return false;
    }
    return true;
}

bool create_pipeline(const char *shader_name, std::uint32_t binding_count, std::uint32_t push_size,
    pipeline_t &p, std::string &error)
{
    p.binding_count = binding_count;
    p.push_size = push_size;

    std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
    for (std::uint32_t i = 0; i < binding_count; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = binding_count;
    dlci.pBindings = bindings.data();
    if (!check(vkCreateDescriptorSetLayout(g.device, &dlci, nullptr, &p.set_layout), "vkCreateDescriptorSetLayout",
            error))
        return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = push_size;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (!check(vkCreatePipelineLayout(g.device, &plci, nullptr, &p.layout), "vkCreatePipelineLayout", error))
        return false;

    std::vector<std::uint32_t> spv;
    if (!read_spirv(exe_dir() / "gpu_shaders" / shader_name, spv, error))
        return false;
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size() * sizeof(std::uint32_t);
    smci.pCode = spv.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(g.device, &smci, nullptr, &shader), "vkCreateShaderModule", error))
        return false;

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName = "main";
    cpci.layout = p.layout;
    const bool ok = check(vkCreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipeline),
        "vkCreateComputePipelines", error);
    vkDestroyShaderModule(g.device, shader, nullptr);
    if (!ok)
        return false;

    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    sizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count - 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    dpci.pPoolSizes = sizes.data();
    if (!check(vkCreateDescriptorPool(g.device, &dpci, nullptr, &p.pool), "vkCreateDescriptorPool", error))
        return false;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = p.pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &p.set_layout;
    return check(vkAllocateDescriptorSets(g.device, &dsai, &p.set), "vkAllocateDescriptorSets", error);
}

// binding 0: the scene; bindings 1..: buffers, in order
void update_set(pipeline_t &p, std::initializer_list<const staged_t *> buffers)
{
    Q_assert(buffers.size() + 1 == p.binding_count);

    VkWriteDescriptorSetAccelerationStructureKHR as_info{};
    as_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as_info.accelerationStructureCount = 1;
    as_info.pAccelerationStructures = &g.tlas.as;

    std::vector<VkDescriptorBufferInfo> infos(p.binding_count);
    std::vector<VkWriteDescriptorSet> writes(p.binding_count);
    auto it = buffers.begin();
    for (std::uint32_t i = 0; i < p.binding_count; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = p.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == 0) {
            writes[i].pNext = &as_info;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        } else {
            infos[i] = {(*it++)->device.buffer, 0, VK_WHOLE_SIZE};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
    }
    vkUpdateDescriptorSets(g.device, p.binding_count, writes.data(), 0, nullptr);
}

void upload(VkCommandBuffer cmd, staged_t &s, const void *data, VkDeviceSize size)
{
    if (!size)
        return;
    std::memcpy(s.host.mapped, data, size);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, s.host.buffer, s.device.buffer, 1, &copy);
}

void download(VkCommandBuffer cmd, staged_t &s, VkDeviceSize size)
{
    if (!size)
        return;
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, s.device.buffer, s.host.buffer, 1, &copy);
}

// Runs `p` over `sample_count` samples in short submits (see TARGET_SUBMIT_MS).
// push(base) returns the push constants for the submit starting at sample `base`.
bool dispatch_samples(pipeline_t &p, std::size_t sample_count,
    const std::function<const void *(std::uint32_t)> &push, double &gpu_ms, std::string &error)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t base = 0; base < sample_count;) {
        const std::uint32_t count =
            static_cast<std::uint32_t>(std::min<std::size_t>(p.dispatch_samples, sample_count - base));
        const void *pc = push(static_cast<std::uint32_t>(base));

        const auto s0 = std::chrono::steady_clock::now();
        if (!submit(
                [&](VkCommandBuffer cmd) {
                    memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
                    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, p.push_size, pc);
                    vkCmdDispatch(cmd, (count + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE, 1, 1);
                },
                error))
            return false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();

        base += count;
        if (count == p.dispatch_samples) {
            const double scale = std::clamp(TARGET_SUBMIT_MS / std::max(ms, 0.01), 0.5, 2.0);
            p.dispatch_samples =
                static_cast<std::uint32_t>(std::clamp<double>(p.dispatch_samples * scale, WORKGROUP_SIZE, 1u << 22));
        }
    }
    gpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace

bool init(const mbsp_t *bsp, std::string &error)
{
    (void)bsp;
    std::lock_guard<std::mutex> lock(g_mutex);
    destroy_locked();

    const bool ok = create_instance(error) && pick_device(error) && create_device(error) && build_scene(error) &&
                    create_pipeline("direct.comp.spv", 9, sizeof(direct_push_t), g.direct, error) &&
                    create_pipeline("dirt.comp.spv", 4, sizeof(dirt_push_t), g.dirt, error) &&
                    create_pipeline("surflight.comp.spv", 9, sizeof(surflight_push_t), g.surflight, error) &&
                    upload_sky_textures(error) && ensure_staged(g.counters, sizeof(std::uint32_t), true, error);
    if (!ok) {
        destroy_locked();
        return false;
    }
    logging::print("GPU light: {}; scene: {} solid, {} sky, {} filtered triangles\n", g.device_name,
        g.stats.solid_tris, g.stats.sky_tris, g.stats.filtered_tris);
    return true;
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    destroy_locked();
}

bool available()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g.direct.pipeline != VK_NULL_HANDLE;
}

stats_t stats()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g.stats;
}

std::uint32_t sky_texture_id(const void *texture)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g.sky_texture_ids.find(texture);
    return it == g.sky_texture_ids.end() ? 0xffffffffu : it->second;
}

bool trace_batch(const batch_t &batch, batch_result_t &result, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    result = {};
    if (!g.direct.pipeline) {
        error = "GPU backend not initialized";
        return false;
    }
    if (!batch.sample_count)
        return true;

    const VkDeviceSize lights_size = batch.light_count * sizeof(light_t);
    const VkDeviceSize samples_size = batch.sample_count * sizeof(sample_t);
    const VkDeviceSize faces_size = batch.face_count * sizeof(face_t);
    const VkDeviceSize entries_size = batch.entry_count * sizeof(std::uint32_t);
    const VkDeviceSize accum_size = batch.sample_count * batch.slot_count * sizeof(accum_t);

    g.retrace_capacity =
        std::max(g.retrace_capacity, static_cast<std::uint32_t>(std::max<std::size_t>(batch.sample_count, 65536)));

    for (;;) {
        const VkDeviceSize retraces_size = VkDeviceSize(g.retrace_capacity) * sizeof(retrace_t);
        if (!ensure_staged(g.lights, lights_size, false, error) ||
            !ensure_staged(g.samples, samples_size, false, error) ||
            !ensure_staged(g.faces, faces_size, false, error) ||
            !ensure_staged(g.entries, entries_size, false, error) ||
            !ensure_staged(g.accum, accum_size, true, error) ||
            !ensure_staged(g.retraces, retraces_size, true, error))
            return false;
        update_set(g.direct,
            {&g.lights, &g.samples, &g.faces, &g.entries, &g.accum, &g.retraces, &g.counters, &g.sky_textures});

        // upload inputs, reset the retrace counter
        if (!submit(
                [&](VkCommandBuffer cmd) {
                    upload(cmd, g.lights, batch.lights, lights_size);
                    upload(cmd, g.samples, batch.samples, samples_size);
                    upload(cmd, g.faces, batch.faces, faces_size);
                    upload(cmd, g.entries, batch.entries, entries_size);
                    vkCmdFillBuffer(cmd, g.counters.device.buffer, 0, sizeof(std::uint32_t), 0);
                },
                error))
            return false;

        direct_push_t pc{};
        pc.sample_count = static_cast<std::uint32_t>(batch.sample_count);
        pc.slot_count = batch.slot_count;
        pc.retrace_capacity = g.retrace_capacity;
        pc.sky_geometry = g.sky_geometry;
        if (!dispatch_samples(
                g.direct, batch.sample_count,
                [&](std::uint32_t base) -> const void * {
                    pc.sample_base = base;
                    return &pc;
                },
                result.gpu_ms, error))
            return false;

        // read back the retrace counter first; if the list overflowed, grow it and redo the batch
        if (!submit(
                [&](VkCommandBuffer cmd) {
                    memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                    download(cmd, g.counters, sizeof(std::uint32_t));
                },
                error))
            return false;
        const std::uint32_t retrace_count = *static_cast<const std::uint32_t *>(g.counters.host.mapped);
        if (retrace_count > g.retrace_capacity) {
            g.retrace_capacity = retrace_count + retrace_count / 4;
            continue;
        }

        if (!submit(
                [&](VkCommandBuffer cmd) {
                    download(cmd, g.accum, accum_size);
                    download(cmd, g.retraces, VkDeviceSize(retrace_count) * sizeof(retrace_t));
                    memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                },
                error))
            return false;

        result.accum = static_cast<const accum_t *>(g.accum.host.mapped);
        result.retraces = static_cast<const retrace_t *>(g.retraces.host.mapped);
        result.retrace_count = retrace_count;
        return true;
    }
}

bool trace_surflight(const surflight_batch_t &batch, batch_result_t &result, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    result = {};
    if (!g.surflight.pipeline) {
        error = "GPU backend not initialized";
        return false;
    }
    if (!batch.sample_count)
        return true;

    const VkDeviceSize emitters_size = batch.emitter_count * sizeof(emitter_t);
    const VkDeviceSize points_size = batch.point_count * 4 * sizeof(float);
    const VkDeviceSize samples_size = batch.sample_count * sizeof(sample_t);
    const VkDeviceSize faces_size = batch.face_count * sizeof(face_t);
    const VkDeviceSize entries_size = batch.entry_count * sizeof(std::uint32_t);
    const VkDeviceSize accum_size = batch.sample_count * batch.slot_count * sizeof(accum_t);

    g.retrace_capacity =
        std::max(g.retrace_capacity, static_cast<std::uint32_t>(std::max<std::size_t>(batch.sample_count, 65536)));

    for (;;) {
        const VkDeviceSize retraces_size = VkDeviceSize(g.retrace_capacity) * sizeof(retrace_t);
        if (!ensure_staged(g.emitters, emitters_size, false, error) ||
            !ensure_staged(g.points, points_size, false, error) ||
            !ensure_staged(g.samples, samples_size, false, error) ||
            !ensure_staged(g.faces, faces_size, false, error) ||
            !ensure_staged(g.entries, entries_size, false, error) ||
            !ensure_staged(g.accum, accum_size, true, error) ||
            !ensure_staged(g.retraces, retraces_size, true, error))
            return false;
        update_set(g.surflight,
            {&g.emitters, &g.points, &g.samples, &g.faces, &g.entries, &g.accum, &g.retraces, &g.counters});

        if (!submit(
                [&](VkCommandBuffer cmd) {
                    upload(cmd, g.emitters, batch.emitters, emitters_size);
                    upload(cmd, g.points, batch.points, points_size);
                    upload(cmd, g.samples, batch.samples, samples_size);
                    upload(cmd, g.faces, batch.faces, faces_size);
                    upload(cmd, g.entries, batch.entries, entries_size);
                    vkCmdFillBuffer(cmd, g.counters.device.buffer, 0, sizeof(std::uint32_t), 0);
                },
                error))
            return false;

        surflight_push_t pc{};
        pc.sample_count = static_cast<std::uint32_t>(batch.sample_count);
        pc.slot_count = batch.slot_count;
        pc.retrace_capacity = g.retrace_capacity;
        pc.standard_scale = batch.params.standard_scale;
        pc.sky_scale = batch.params.sky_scale;
        pc.hotspot_clamp = batch.params.hotspot_clamp;
        pc.gate = batch.params.gate;
        pc.sky_dist = batch.params.sky_dist;
        pc.scaledist = batch.params.scaledist;
        pc.dirtgain = batch.params.dirtgain;
        pc.dirtscale = batch.params.dirtscale;
        pc.dirt = batch.params.dirt;
        if (!dispatch_samples(
                g.surflight, batch.sample_count,
                [&](std::uint32_t base) -> const void * {
                    pc.sample_base = base;
                    return &pc;
                },
                result.gpu_ms, error))
            return false;

        if (!submit(
                [&](VkCommandBuffer cmd) {
                    memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                    download(cmd, g.counters, sizeof(std::uint32_t));
                },
                error))
            return false;
        const std::uint32_t retrace_count = *static_cast<const std::uint32_t *>(g.counters.host.mapped);
        if (retrace_count > g.retrace_capacity) {
            g.retrace_capacity = retrace_count + retrace_count / 4;
            continue;
        }

        if (!submit(
                [&](VkCommandBuffer cmd) {
                    download(cmd, g.accum, accum_size);
                    download(cmd, g.retraces, VkDeviceSize(retrace_count) * sizeof(retrace_t));
                    memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                },
                error))
            return false;

        result.accum = static_cast<const accum_t *>(g.accum.host.mapped);
        result.retraces = static_cast<const retrace_t *>(g.retraces.host.mapped);
        result.retrace_count = retrace_count;
        return true;
    }
}

bool trace_dirt(const dirt_batch_t &batch, dirt_result_t &result, std::string &error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    result = {};
    if (!g.dirt.pipeline) {
        error = "GPU backend not initialized";
        return false;
    }
    if (!batch.sample_count)
        return true;

    const VkDeviceSize samples_size = batch.sample_count * sizeof(sample_t);
    const VkDeviceSize vectors_size = batch.vector_count * 4 * sizeof(float);
    const VkDeviceSize out_size = batch.sample_count * sizeof(float);
    if (!ensure_staged(g.samples, samples_size, false, error) ||
        !ensure_staged(g.dirt_vectors, vectors_size, false, error) ||
        !ensure_staged(g.dirt_occlusion, out_size, true, error))
        return false;
    update_set(g.dirt, {&g.samples, &g.dirt_vectors, &g.dirt_occlusion});

    if (!submit(
            [&](VkCommandBuffer cmd) {
                upload(cmd, g.samples, batch.samples, samples_size);
                upload(cmd, g.dirt_vectors, batch.vectors, vectors_size);
            },
            error))
        return false;

    dirt_push_t pc{};
    pc.sample_count = static_cast<std::uint32_t>(batch.sample_count);
    pc.vector_count = static_cast<std::uint32_t>(batch.vector_count);
    pc.sky_geometry = g.sky_geometry;
    pc.depth = batch.depth;
    if (!dispatch_samples(
            g.dirt, batch.sample_count,
            [&](std::uint32_t base) -> const void * {
                pc.sample_base = base;
                return &pc;
            },
            result.gpu_ms, error))
        return false;

    if (!submit(
            [&](VkCommandBuffer cmd) {
                memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                download(cmd, g.dirt_occlusion, out_size);
                memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            },
            error))
        return false;

    result.occlusion = static_cast<const float *>(g.dirt_occlusion.host.mapped);
    return true;
}

} // namespace gpu_light
