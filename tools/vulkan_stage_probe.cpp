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
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
  }
}

struct alignas(16) Vec4 { float x, y, z, w; };

struct alignas(16) SimParams {
  Vec4 gravity{};
  float dt{}, thickness{}, friction{}, max_speed{};
  float global_damping{}, relaxation_factor{}, neighbor_friction{}, p1{};
  uint32_t num_particles{}, num_edges{}, num_shears{}, num_bends{};
  uint32_t num_areas{}, num_tries{}, num_volumes{}, num_colliders{};
  float cell_size{};
  uint32_t num_tables{}, max_neighbors{};
  float collision_radius{};
  float wind_dir[3]{};
  uint32_t wind_enable{};
  float wind_force{}, air_density{}, drag_coefficient{}, lift_coefficient{};
};
static_assert(sizeof(SimParams) == 128);

struct PushConstant {
  uint32_t base{}, count{};
  float compliance{}, beta{}, stiffness{};
  float solve_pad[3]{};
  float ray_origin[3]{};
  uint32_t select_mode{};
  float ray_dir[3]{};
  float radius{};
  uint32_t depth_mode{};
  float mouse_pad[3]{};
};
static_assert(sizeof(PushConstant) == 80);

struct alignas(16) Edge {
  uint32_t i, j;
  float rest, lambda;
};
static_assert(sizeof(Edge) == 16);

struct alignas(16) Shear {
  uint32_t i0, i1, i2;
  float rest_dot;
  float lambda, p0, p1, p2;
};
static_assert(sizeof(Shear) == 32);

struct alignas(16) Bend {
  uint32_t i0, i1, i2, i3;
  float rest_angle, lambda, p0, p1;
};
static_assert(sizeof(Bend) == 32);

struct alignas(16) Collider {
  Vec4 first;
  Vec4 second;
  Vec4 meta;
};
static_assert(sizeof(Collider) == 48);

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
    const auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((bits & (1u << i)) && (flags & required) == required) return i;
  }
  throw std::runtime_error("no host-visible coherent Vulkan memory type");
}

Buffer make_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                   VkBufferUsageFlags usage) {
  Buffer result;
  result.device = device;
  result.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(device, &info, nullptr, &result.buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits);
  check(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory");
  check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
  return result;
}

void clear(VkDevice device, Buffer& buffer) {
  void* mapped = nullptr;
  check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped), "vkMapMemory(clear)");
  std::memset(mapped, 0, static_cast<size_t>(buffer.size));
  vkUnmapMemory(device, buffer.memory);
}

template <typename T>
void upload(VkDevice device, Buffer& buffer, const T* values, size_t count) {
  void* mapped = nullptr;
  check(vkMapMemory(device, buffer.memory, 0, sizeof(T) * count, 0, &mapped),
        "vkMapMemory(upload)");
  std::memcpy(mapped, values, sizeof(T) * count);
  vkUnmapMemory(device, buffer.memory);
}

template <typename T, size_t N>
void upload(VkDevice device, Buffer& buffer, const std::array<T, N>& values) {
  upload(device, buffer, values.data(), values.size());
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

std::vector<uint32_t> read_spirv(const char* path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error(std::string("cannot open SPIR-V: ") + path);
  const auto bytes = input.tellg();
  if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid SPIR-V size");
  std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char*>(code.data()), bytes);
  return code;
}

void emit(const char* device, const char* case_name, const char* stage,
          const char* metric, double value) {
  std::cout << "{\"schemaVersion\":1,\"sourceRevision\":\"" << kRevision
            << "\",\"implementation\":\"donor-vulkan-gpu\",\"gpuExecuted\":true"
            << ",\"device\":\"" << device << "\",\"case\":\"" << case_name
            << "\",\"stage\":\"" << stage << "\",\"metric\":\"" << metric
            << "\",\"value\":" << std::setprecision(9) << value << "}\n";
}

enum class Stage : size_t {
  Integrate, Stretch, Shear, Bend, Apply, LRA, Collide, Velocity,
  TriNormal, VertexNormal, Count
};

constexpr std::array<const char*, static_cast<size_t>(Stage::Count)> kShaderPaths{
  XPBD_INTEGRATE_SHADER_PATH,
  XPBD_SOLVE_STRETCH_SHADER_PATH,
  XPBD_SOLVE_SHEAR_SHADER_PATH,
  XPBD_SOLVE_BEND_SHADER_PATH,
  XPBD_APPLY_DELTAS_SHADER_PATH,
  XPBD_SOLVE_LRA_SHADER_PATH,
  XPBD_COLLIDE_SDF_SHADER_PATH,
  XPBD_UPDATE_VELOCITY_SHADER_PATH,
  XPBD_TRI_NORMAL_SHADER_PATH,
  XPBD_VERTEX_NORMAL_SHADER_PATH,
};

}  // namespace

int main() {
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSetLayout sim_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout storage_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::vector<VkShaderModule> modules;
  std::vector<VkPipeline> pipelines;

  try {
    const char* instance_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "XPBDClothVulkanStageProbe";
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
    const auto physical = physical_devices.front();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
      if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queue_family = i; break; }
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
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
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
    VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstant)};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = layouts.size();
    pipeline_layout_info.pSetLayouts = layouts.data();
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout),
          "vkCreatePipelineLayout");

    for (const char* path : kShaderPaths) {
      const auto spirv = read_spirv(path);
      VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shader_info.codeSize = spirv.size() * sizeof(uint32_t);
      shader_info.pCode = spirv.data();
      VkShaderModule module{};
      check(vkCreateShaderModule(device, &shader_info, nullptr, &module),
            "vkCreateShaderModule");
      modules.push_back(module);
      VkPipelineShaderStageCreateInfo stage_info{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      stage_info.module = module;
      stage_info.pName = "main";
      VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipeline_info.stage = stage_info;
      pipeline_info.layout = pipeline_layout;
      VkPipeline pipeline{};
      check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                     &pipeline), "vkCreateComputePipelines");
      pipelines.push_back(pipeline);
    }

    Buffer sim = make_buffer(physical, device, sizeof(SimParams),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::array<Buffer, 29> buffers;
    for (auto& buffer : buffers) {
      buffer = make_buffer(physical, device, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      clear(device, buffer);
    }

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
    const uint32_t dynamic_offset = 0;
    auto dispatch = [&](Stage stage, const PushConstant& push) {
      check(vkResetCommandPool(device, command_pool, 0), "vkResetCommandPool");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelines[static_cast<size_t>(stage)]);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                              0, sets.size(), sets.data(), 1, &dynamic_offset);
      vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(push), &push);
      vkCmdDispatch(command, 1, 1, 1);
      check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
      check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    };
    auto reset = [&] {
      for (auto& buffer : buffers) clear(device, buffer);
    };

    // Integrate and update velocity: far above the donor's implicit ground path.
    reset();
    SimParams params{};
    params.gravity = {0, -2, 0, 0};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 3;
    params.num_tries = 1;
    params.relaxation_factor = 1;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 3> rest{{{0, 10, 0, 1}, {1, 10, 0, 1}, {0, 11, 0, 1}}};
    const std::array<float, 3> unit_weights{1, 1, 1};
    upload(device, buffers[0], rest);
    upload(device, buffers[1], rest);
    upload(device, buffers[2], std::array<Vec4, 3>{});
    upload(device, buffers[3], unit_weights);
    PushConstant push{};
    push.count = 3;
    push.stiffness = 1;
    dispatch(Stage::Integrate, push);
    const auto integrated = download<Vec4>(device, buffers[1], 3);
    emit(properties.deviceName, "integrate-gravity", "Integrate", "position-0-y",
         integrated[0].y);
    dispatch(Stage::Velocity, push);
    const auto integrated_velocity = download<Vec4>(device, buffers[2], 3);
    emit(properties.deviceName, "integrate-gravity", "UpdateVelocity", "velocity-0-y",
         integrated_velocity[0].y);

    // Stretch compatibility fixture: beta=0 matches the Metal port's documented omission.
    reset();
    params = {};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 3;
    params.num_edges = 3;
    params.num_tries = 1;
    params.relaxation_factor = 1;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 3> stretch_positions{{
        {0, 10, 0, 1}, {1.2f, 10, 0, 1}, {0, 11, 0, 1}}};
    const std::array<float, 3> stretch_weights{0, 1, 0};
    const std::array<Edge, 3> edges{{
        {0, 1, 1, 0}, {0, 2, 1, 0}, {1, 2, std::sqrt(2.0f), 0}}};
    const std::array<uint32_t, 3> indices{0, 1, 2};
    const std::array<uint32_t, 4> offsets{0, 1, 2, 3};
    const std::array<uint32_t, 3> incidents{0, 0, 0};
    upload(device, buffers[0], stretch_positions);
    upload(device, buffers[1], stretch_positions);
    upload(device, buffers[3], stretch_weights);
    upload(device, buffers[8], edges);
    upload(device, buffers[19], indices);
    upload(device, buffers[22], offsets);
    upload(device, buffers[23], incidents);
    push = {};
    push.count = 1;
    push.stiffness = 1;
    for (uint32_t edge = 0; edge < 3; ++edge) {
      push.base = edge;
      dispatch(Stage::Stretch, push);
    }
    const auto stretched = download<Vec4>(device, buffers[1], 3);
    const auto stretch_lambdas = download<Edge>(device, buffers[8], 3);
    emit(properties.deviceName, "stretch-single-free-particle", "SolveStretch",
         "position-1-x", stretched[1].x);
    emit(properties.deviceName, "stretch-single-free-particle", "SolveStretch",
         "lambda-0", stretch_lambdas[0].lambda);
    push.base = 0;
    push.count = 3;
    dispatch(Stage::Velocity, push);
    const auto stretch_velocity = download<Vec4>(device, buffers[2], 3);
    emit(properties.deviceName, "stretch-single-free-particle", "UpdateVelocity",
         "velocity-1-x", stretch_velocity[1].x);
    dispatch(Stage::TriNormal, push);
    dispatch(Stage::VertexNormal, push);
    const auto normals = download<Vec4>(device, buffers[20], 3);
    emit(properties.deviceName, "stretch-single-free-particle", "ComputeNormals",
         "normal-0-z", normals[0].z);

    // Shear plus apply-deltas, with one free particle and deterministic accumulation.
    reset();
    params = {};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 3;
    params.num_shears = 1;
    params.num_tries = 1;
    params.relaxation_factor = 1;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 3> shear_positions{{
        {0, 10, 0, 1}, {1, 10, 0, 1}, {0.2f, 11, 0, 1}}};
    const std::array<float, 3> shear_weights{0, 0, 1};
    const Shear shear{0, 1, 2, 0, 0, 0, 0, 0};
    upload(device, buffers[0], shear_positions);
    upload(device, buffers[1], shear_positions);
    upload(device, buffers[3], shear_weights);
    upload(device, buffers[9], &shear, 1);
    push = {};
    push.count = 1;
    push.stiffness = 1;
    dispatch(Stage::Shear, push);
    push.count = 3;
    dispatch(Stage::Apply, push);
    const auto sheared = download<Vec4>(device, buffers[1], 3);
    const auto shear_lambdas = download<Shear>(device, buffers[9], 1);
    emit(properties.deviceName, "shear-single-free-particle", "SolveShear",
         "position-2-x", sheared[2].x);
    emit(properties.deviceName, "shear-single-free-particle", "SolveShear",
         "lambda-0", shear_lambdas[0].lambda);
    dispatch(Stage::Velocity, push);
    const auto shear_velocity = download<Vec4>(device, buffers[2], 3);
    emit(properties.deviceName, "shear-single-free-particle", "UpdateVelocity",
         "velocity-2-x", shear_velocity[2].x);

    // Bend intentionally differs: donor q-gradient vs corrected/wrapped Metal gradient.
    reset();
    params = {};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 4;
    params.num_bends = 1;
    params.relaxation_factor = 1;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 4> bend_rest{{
        {0, 10, 0, 1}, {1, 10, 0, 1}, {0, 11, 0, 1}, {1, 11, 0, 1}}};
    const std::array<Vec4, 4> bend_positions{{
        {0, 10, 0, 1}, {1, 10, 0, 1}, {0, 11, 0, 1}, {1, 11, 0.2f, 1}}};
    const std::array<float, 4> bend_weights{0, 0, 0, 1};
    const Vec4 e{bend_rest[2].x - bend_rest[1].x,
                 bend_rest[2].y - bend_rest[1].y, 0, 0};
    const Vec4 n1{0, 0, (bend_rest[2].x - bend_rest[1].x) *
                            (bend_rest[0].y - bend_rest[1].y) -
                        (bend_rest[2].y - bend_rest[1].y) *
                            (bend_rest[0].x - bend_rest[1].x), 0};
    const Vec4 n2{0, 0, (bend_rest[2].x - bend_rest[1].x) *
                            (bend_rest[3].y - bend_rest[1].y) -
                        (bend_rest[2].y - bend_rest[1].y) *
                            (bend_rest[3].x - bend_rest[1].x), 0};
    const float edge_length = std::sqrt(e.x * e.x + e.y * e.y);
    const float rest_angle = std::atan2(
        (e.x / edge_length) * ((n1.y * n2.z - n1.z * n2.y)) +
            (e.y / edge_length) * ((n1.z * n2.x - n1.x * n2.z)),
        (n1.z * n2.z) / (std::abs(n1.z) * std::abs(n2.z)));
    const Bend bend{1, 2, 0, 3, rest_angle, 0, 0, 0};
    upload(device, buffers[0], bend_positions);
    upload(device, buffers[1], bend_positions);
    upload(device, buffers[3], bend_weights);
    upload(device, buffers[10], &bend, 1);
    push = {};
    push.count = 1;
    push.compliance = 0;
    push.stiffness = 1;
    dispatch(Stage::Bend, push);
    push.count = 4;
    dispatch(Stage::Apply, push);
    const auto bent = download<Vec4>(device, buffers[1], 4);
    const auto bend_lambdas = download<Bend>(device, buffers[10], 1);
    emit(properties.deviceName, "bend-intentional-correction", "SolveBend",
         "position-3-z", bent[3].z);
    emit(properties.deviceName, "bend-intentional-correction", "SolveBend",
         "lambda-0", bend_lambdas[0].lambda);

    // Long-range attachment with one fixed anchor.
    reset();
    params = {};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 3;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 3> lra_positions{{
        {0, 10, 0, 1}, {1.2f, 10, 0, 1}, {0, 11, 0, 1}}};
    const std::array<float, 3> lra_weights{0, 1, 1};
    const std::array<uint32_t, 6> lra_ids{
        UINT32_MAX, UINT32_MAX, 0, UINT32_MAX, 0, UINT32_MAX};
    const std::array<float, 6> lra_rests{0, 0, 1, 0, 1, 0};
    upload(device, buffers[0], lra_positions);
    upload(device, buffers[1], lra_positions);
    upload(device, buffers[3], lra_weights);
    upload(device, buffers[27], lra_ids);
    upload(device, buffers[28], lra_rests);
    push = {};
    push.count = 3;
    push.stiffness = 1;
    dispatch(Stage::LRA, push);
    const auto lra_solved = download<Vec4>(device, buffers[1], 3);
    emit(properties.deviceName, "lra-single-anchor", "SolveLRA", "position-1-x",
         lra_solved[1].x);
    dispatch(Stage::Velocity, push);
    const auto lra_velocity = download<Vec4>(device, buffers[2], 3);
    emit(properties.deviceName, "lra-single-anchor", "UpdateVelocity", "velocity-1-x",
         lra_velocity[1].x);

    // Sphere collision; friction is zero so both implementations share the same projection.
    reset();
    params = {};
    params.dt = 0.1f;
    params.max_speed = 1000;
    params.num_particles = 3;
    params.num_colliders = 1;
    upload(device, sim, &params, 1);
    const std::array<Vec4, 3> collision_positions{{
        {0.5f, 10, 0, 1}, {2, 10, 0, 1}, {0, 12, 0, 1}}};
    const std::array<float, 3> collision_weights{1, 0, 0};
    const Collider sphere{{0, 10, 0, 0}, {0, 0, 0, 1}, {1, 1, 0, 0}};
    upload(device, buffers[0], collision_positions);
    upload(device, buffers[1], collision_positions);
    upload(device, buffers[3], collision_weights);
    upload(device, buffers[24], &sphere, 1);
    push = {};
    push.count = 3;
    push.stiffness = 1;
    dispatch(Stage::Collide, push);
    const auto collided = download<Vec4>(device, buffers[1], 3);
    emit(properties.deviceName, "collide-sphere", "Collide", "position-0-x",
         collided[0].x);
    dispatch(Stage::Velocity, push);
    const auto collision_velocity = download<Vec4>(device, buffers[2], 3);
    emit(properties.deviceName, "collide-sphere", "UpdateVelocity", "velocity-0-x",
         collision_velocity[0].x);

    vkDeviceWaitIdle(device);
    buffers = {};
    sim.destroy();
    vkDestroyCommandPool(device, command_pool, nullptr);
    command_pool = VK_NULL_HANDLE;
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    descriptor_pool = VK_NULL_HANDLE;
    for (auto pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
    pipelines.clear();
    for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
    modules.clear();
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    pipeline_layout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(device, storage_layout, nullptr);
    storage_layout = VK_NULL_HANDLE;
    vkDestroyDescriptorSetLayout(device, sim_layout, nullptr);
    sim_layout = VK_NULL_HANDLE;
    vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    vkDestroyInstance(instance, nullptr);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "XPBDClothVulkanStageProbe: " << error.what() << '\n';
    if (device) vkDeviceWaitIdle(device);
    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    for (auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    for (auto module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (storage_layout) vkDestroyDescriptorSetLayout(device, storage_layout, nullptr);
    if (sim_layout) vkDestroyDescriptorSetLayout(device, sim_layout, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    return 4;
  }
}
