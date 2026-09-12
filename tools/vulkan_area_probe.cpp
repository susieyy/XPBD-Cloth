#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr char kRevision[] = "c7019d35414e75c77ede2c3500a762837bcab6a6";

void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed: " +
                             std::to_string(result));
  }
}

struct alignas(16) Vec4 {
  float x, y, z, w;
};

struct alignas(16) SimParams {
  Vec4 gravity{};
  float dt{};
  float thickness{};
  float friction{};
  float max_speed{};
  float global_damping{};
  float relaxation_factor{};
  float neighbor_friction{};
  float p1{};
  uint32_t num_particles{};
  uint32_t num_edges{};
  uint32_t num_shears{};
  uint32_t num_bends{};
  uint32_t num_areas{};
  uint32_t num_tries{};
  uint32_t num_volumes{};
  uint32_t num_colliders{};
  float cell_size{};
  uint32_t num_tables{};
  uint32_t max_neighbors{};
  float collision_radius{};
  float wind_dir[3]{};
  uint32_t wind_enable{};
  float wind_force{};
  float air_density{};
  float drag_coefficient{};
  float lift_coefficient{};
};
static_assert(sizeof(SimParams) == 128);

struct PushConstant {
  uint32_t base{};
  uint32_t count{};
  float compliance{};
  float beta{};
  float stiffness{};
  float solve_pad[3]{};
  float ray_origin[3]{};
  uint32_t select_mode{};
  float ray_dir[3]{};
  float radius{};
  uint32_t depth_mode{};
  float mouse_pad[3]{};
};
static_assert(sizeof(PushConstant) == 80);

struct alignas(16) Area {
  uint32_t i0, i1, i2;
  float rest_area;
  float rest_normal[3];
  float lambda;
};
static_assert(sizeof(Area) == 32);

struct Buffer {
  VkDevice device{};
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};

  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept { *this = std::move(other); }
  Buffer& operator=(Buffer&& other) noexcept {
    if (this != &other) {
      destroy();
      device = other.device;
      buffer = other.buffer;
      memory = other.memory;
      size = other.size;
      other.buffer = VK_NULL_HANDLE;
      other.memory = VK_NULL_HANDLE;
    }
    return *this;
  }
  ~Buffer() { destroy(); }
  void destroy() {
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }
};

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    const auto flags = properties.memoryTypes[i].propertyFlags;
    if ((bits & (1u << i)) &&
        (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      return i;
    }
  }
  throw std::runtime_error("no host-visible coherent Vulkan memory type");
}

Buffer make_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                   VkBufferUsageFlags usage) {
  Buffer result;
  result.device = device;
  result.size = size;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer),
        "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
  VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits);
  check(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory),
        "vkAllocateMemory");
  check(vkBindBufferMemory(device, result.buffer, result.memory, 0),
        "vkBindBufferMemory");
  return result;
}

template <typename T>
void upload(VkDevice device, Buffer& buffer, const T* values, size_t count) {
  void* mapped = nullptr;
  check(vkMapMemory(device, buffer.memory, 0, sizeof(T) * count, 0, &mapped),
        "vkMapMemory(upload)");
  std::memcpy(mapped, values, sizeof(T) * count);
  vkUnmapMemory(device, buffer.memory);
}

template <typename T>
std::vector<T> download(VkDevice device, Buffer& buffer, size_t count) {
  void* mapped = nullptr;
  check(vkMapMemory(device, buffer.memory, 0, sizeof(T) * count, 0, &mapped),
        "vkMapMemory(download)");
  std::vector<T> result(count);
  std::memcpy(result.data(), mapped, sizeof(T) * count);
  vkUnmapMemory(device, buffer.memory);
  return result;
}

std::vector<uint32_t> read_spirv() {
  std::ifstream input(XPBD_AREA_SHADER_PATH, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open solve_area SPIR-V");
  const auto bytes = input.tellg();
  if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid SPIR-V size");
  std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char*>(code.data()), bytes);
  return code;
}

void emit(const char* device, const char* metric, double value) {
  std::cout << "{\"schemaVersion\":1,\"sourceRevision\":\"" << kRevision
            << "\",\"implementation\":\"donor-vulkan-gpu\",\"gpuExecuted\":true"
            << ",\"device\":\"" << device
            << "\",\"case\":\"area-rigid-rotation-90\",\"metric\":\""
            << metric << "\",\"value\":" << std::setprecision(9) << value
            << "}\n";
}

}  // namespace

int main() {
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSetLayout sim_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout storage_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;

  try {
    const char* instance_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "XPBDClothVulkanAreaProbe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");

    uint32_t physical_count = 0;
    check(vkEnumeratePhysicalDevices(instance, &physical_count, nullptr),
          "vkEnumeratePhysicalDevices(count)");
    if (physical_count == 0) throw std::runtime_error("no Vulkan physical device");
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    check(vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data()),
          "vkEnumeratePhysicalDevices");
    const VkPhysicalDevice physical = physical_devices.front();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
      if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queue_family = i;
        break;
      }
    }
    if (queue_family == UINT32_MAX) throw std::runtime_error("no compute queue");

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &atomic_features;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!atomic_features.shaderBufferFloat32AtomicAdd) {
      throw std::runtime_error("shaderBufferFloat32AtomicAdd is unavailable");
    }
    atomic_features.shaderBufferFloat32AtomicAdd = VK_TRUE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {"VK_KHR_portability_subset",
                                       VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &atomic_features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = device_extensions;
    check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    VkQueue queue{};
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkDescriptorSetLayoutBinding uniform_binding{};
    uniform_binding.binding = 0;
    uniform_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform_binding.descriptorCount = 1;
    uniform_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &uniform_binding;
    check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &sim_layout),
          "vkCreateDescriptorSetLayout(sim)");

    std::array<VkDescriptorSetLayoutBinding, 29> storage_bindings{};
    for (uint32_t i = 0; i < storage_bindings.size(); ++i) {
      storage_bindings[i].binding = i;
      storage_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      storage_bindings[i].descriptorCount = 1;
      storage_bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    layout_info.bindingCount = storage_bindings.size();
    layout_info.pBindings = storage_bindings.data();
    check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &storage_layout),
          "vkCreateDescriptorSetLayout(storage)");

    const std::array layouts{sim_layout, storage_layout};
    VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(PushConstant)};
    VkPipelineLayoutCreateInfo pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = layouts.size();
    pipeline_layout_info.pSetLayouts = layouts.data();
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &pipeline_layout),
          "vkCreatePipelineLayout");

    const auto spirv = read_spirv();
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = spirv.size() * sizeof(uint32_t);
    shader_info.pCode = spirv.data();
    check(vkCreateShaderModule(device, &shader_info, nullptr, &shader),
          "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                   &pipeline),
          "vkCreateComputePipelines");

    Buffer sim = make_buffer(physical, device, sizeof(SimParams),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::array<Buffer, 29> buffers;
    for (uint32_t i = 0; i < buffers.size(); ++i) {
      VkDeviceSize size = 256;
      if (i == 0 || i == 1 || i == 2 || i == 20 || i == 21) size = 3 * sizeof(Vec4);
      if (i == 3 || (i >= 4 && i <= 7)) size = 3 * sizeof(uint32_t);
      if (i == 12) size = sizeof(Area);
      buffers[i] = make_buffer(physical, device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    SimParams sim_params{};
    sim_params.dt = 1.0f / 600.0f;
    sim_params.num_particles = 3;
    sim_params.num_areas = 1;
    upload(device, sim, &sim_params, 1);
    const std::array<Vec4, 3> positions{{{0, 0, 0, 1}, {0, 0, -1, 1},
                                        {0, 1, 0, 1}}};
    const std::array<float, 3> weights{1, 1, 1};
    const std::array<float, 3> zeros{};
    const std::array<uint32_t, 3> zero_counts{};
    Area area{0, 1, 2, 0.5f, {0, 0, 1}, 0};
    upload(device, buffers[0], positions.data(), positions.size());
    upload(device, buffers[1], positions.data(), positions.size());
    upload(device, buffers[3], weights.data(), weights.size());
    for (uint32_t i = 4; i <= 6; ++i) upload(device, buffers[i], zeros.data(), 3);
    upload(device, buffers[7], zero_counts.data(), 3);
    upload(device, buffers[12], &area, 1);

    const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 29},
    }};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = pool_sizes.size();
    pool_info.pPoolSizes = pool_sizes.data();
    check(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool),
          "vkCreateDescriptorPool");
    std::array<VkDescriptorSet, 2> sets{};
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = layouts.size();
    set_info.pSetLayouts = layouts.data();
    check(vkAllocateDescriptorSets(device, &set_info, sets.data()),
          "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo sim_buffer{sim.buffer, 0, sim.size};
    VkWriteDescriptorSet sim_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    sim_write.dstSet = sets[0];
    sim_write.dstBinding = 0;
    sim_write.descriptorCount = 1;
    sim_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sim_write.pBufferInfo = &sim_buffer;
    std::array<VkDescriptorBufferInfo, 29> buffer_infos{};
    std::array<VkWriteDescriptorSet, 29> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
      buffer_infos[i] = {buffers[i].buffer, 0, buffers[i].size};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = sets[1];
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device, 1, &sim_write, 0, nullptr);
    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool),
          "vkCreateCommandPool");
    VkCommandBuffer command{};
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &command_info, &command),
          "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const uint32_t dynamic_offset = 0;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, sets.size(), sets.data(), 1, &dynamic_offset);
    PushConstant push{};
    push.count = 1;
    push.stiffness = 1;
    vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push), &push);
    vkCmdDispatch(command, 1, 1, 1);
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    const auto delta_x = download<float>(device, buffers[4], 3);
    const auto counts = download<uint32_t>(device, buffers[7], 3);
    const auto areas = download<Area>(device, buffers[12], 1);
    emit(properties.deviceName, "lambda", areas[0].lambda);
    emit(properties.deviceName, "delta-x-0", delta_x[0]);
    emit(properties.deviceName, "delta-x-1", delta_x[1]);
    emit(properties.deviceName, "delta-count-0", counts[0]);

    const bool valid = std::abs(areas[0].lambda - 1.0f) < 1e-6f &&
                       std::abs(delta_x[0] - 0.5f) < 1e-6f &&
                       std::abs(delta_x[1] + 0.5f) < 1e-6f && counts[0] == 1;
    vkDeviceWaitIdle(device);
    vkDestroyCommandPool(device, command_pool, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, shader, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, storage_layout, nullptr);
    vkDestroyDescriptorSetLayout(device, sim_layout, nullptr);
    buffers = {};
    sim.destroy();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return valid ? 0 : 5;
  } catch (const std::exception& error) {
    std::cerr << "XPBDClothVulkanAreaProbe: " << error.what() << '\n';
    if (device) vkDeviceWaitIdle(device);
    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (storage_layout) vkDestroyDescriptorSetLayout(device, storage_layout, nullptr);
    if (sim_layout) vkDestroyDescriptorSetLayout(device, sim_layout, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    return 4;
  }
}
