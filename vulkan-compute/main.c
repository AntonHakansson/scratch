#if IN_SHELL /* $ bash main.c
cc main.c -fsanitize=undefined -Wall -g3 -O0 -Wall -lvulkan
exit # */
#endif

/*
#+title: Minimal Vulkan Compute Shader executing SDF command buffer
#+ref: https://www.neilhenning.dev/posts/a-simple-vulkan-compute-example/
#+author: anton@hakanssn.com
#+licence: This is free and unencumbered software released into the public
 domain.
*/

#include "vulkan/vulkan_core.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef unsigned char      U8;
typedef signed   long long I64;
typedef signed   long long Size;

#define countof(s)  (sizeof((s)) / sizeof(*(s)))
#define assert(c)    while((!(c))) __builtin_unreachable()
#define new(a, t, n) ((t *) arena_alloc(a, sizeof(t), (Size)_Alignof(t), (n)))

typedef struct { U8 *beg, *end; } Arena;

__attribute((malloc, alloc_size(2, 4), alloc_align(3)))
static U8 * arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(uint64_t)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
  assert(total < (a->end - a->beg) && "out of memory");
  U8 *p = a->beg + padding;
  __builtin_memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

#define VK_CHECK(result)                                                       \
  if (VK_SUCCESS != (result)) {                                                \
    fprintf(stderr, "Failure at %u %s\n", __LINE__, __FILE__);                 \
    exit(-1);                                                                  \
  }

static VkBool32
debug_message_func(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                   VkDebugUtilsMessageTypeFlagsEXT message_type,
                   VkDebugUtilsMessengerCallbackDataEXT const *message_info,
                   void *user_data) {
  char *message_severity_cstr = "";
  switch(message_severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: { message_severity_cstr = "VERBOSE"; } break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT: { message_severity_cstr = "INFO";  } break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: {  message_severity_cstr = "WARNING"; } break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT: { message_severity_cstr = "ERROR";} break;
    default: assert(0 && "unreachable");
  }
  char *message_type_cstr = "";
  switch (message_type) {
    case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT: { message_type_cstr = "GENERAL"; } break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT: { message_type_cstr = "VALIDATION";  } break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT: {  message_type_cstr = "PERFORMANCE"; } break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT: { message_type_cstr = "DEVICE_ADDRESS_BINDING";} break;
    default: assert(0 && "unreachable");
  }
  printf("[Vulkan %s][%s][%s]: %s\n", message_severity_cstr, message_type_cstr, message_info->pMessageIdName, message_info->pMessage);
  return VK_FALSE;
}

static void run(Arena arena)
{
  #define COMPUTE_BUFFER_SIZE (256 * 2)
  #define COMPUTE_BUFFER_NBYTES (COMPUTE_BUFFER_SIZE * sizeof(compute_input[0]))
  uint32_t *compute_input = new(&arena, uint32_t, COMPUTE_BUFFER_SIZE);
  uint32_t *compute_output = new (&arena, uint32_t, COMPUTE_BUFFER_SIZE);

  int i = 0;

  compute_input[i++] = 4; // draw cmd count

  compute_input[i++] = 1; // draw rect cmd
  compute_input[i++] = 220; // x pos left
  compute_input[i++] = 160; // y pos upper
  compute_input[i++] = 25; // width
  compute_input[i++] = 70; // height
  compute_input[i++] = 0; // radius

  compute_input[i++] = 2; // draw circle cmd
  compute_input[i++] = 100; // x pos left
  compute_input[i++] = 70; // y pos upper
  compute_input[i++] = 50; // diameter

  compute_input[i++] = 2; // draw circle cmd
  compute_input[i++] = 300; // x pos left
  compute_input[i++] = 70; // y pos upper
  compute_input[i++] = 50; // diameter

  compute_input[i++] = 3; // draw line segment
  compute_input[i++] = 8; // stroke_width
  compute_input[i++] = 3; // points count
  compute_input[i++] = 150; // x0
  compute_input[i++] = 300; // y0
  compute_input[i++] = 225; // x1
  compute_input[i++] = 350; // y1
  compute_input[i++] = 300; // x2
  compute_input[i++] = 300; // y2

  #define OUTPUT_IMAGE_WIDTH (600)
  #define OUTPUT_IMAGE_HEIGHT (400)

  // 1. Create Vulkan instance
  VkInstance instance = {0};
  {
    // 1.1 Specify application information
    VkApplicationInfo application_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application_info.pApplicationName = "vulkan compute shader";
    application_info.apiVersion = VK_API_VERSION_1_3;

    // 1.2 Specify instance creation information
    // validations layers and debug layers
    static const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};

    VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    {
      debug_messenger_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        /* VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | */
        /* VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | */
        0;
      debug_messenger_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        /* VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | */
        0;
      debug_messenger_info.pfnUserCallback = &debug_message_func;
    }

    VkInstanceCreateInfo instance_create_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    {
      instance_create_info.pNext = &debug_messenger_info;
      instance_create_info.pApplicationInfo = &application_info;
      instance_create_info.enabledLayerCount = countof(validation_layers),
      instance_create_info.ppEnabledLayerNames = validation_layers;
    }

    // 1.3 Create Vulkan instance
    VK_CHECK(vkCreateInstance(&instance_create_info, 0, &instance));
  }

  // 2. Select PhysicalDevice and Queue Family Index
  VkPhysicalDevice physical_device = {0};
  VkPhysicalDeviceMemoryProperties physical_device_memory_props = {0};
  int32_t queue_family_index = 0;
  {
    uint32_t physical_device_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, 0));
    VkPhysicalDevice *physical_devices = new (&arena, VkPhysicalDevice, physical_device_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                      physical_devices));
    physical_device = physical_devices[0];

    // Print selected device name
    VkPhysicalDeviceProperties physical_device_props = {0};
    vkGetPhysicalDeviceProperties(physical_device, &physical_device_props);
    printf("GPU%i: %s\n", 0, physical_device_props.deviceName);

    vkGetPhysicalDeviceMemoryProperties(physical_device, &physical_device_memory_props);

    // 2.3. Get queue index
    if (0) {
      uint32_t queue_family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, 0);
      VkQueueFamilyProperties *queue_family_props = new(&arena, VkQueueFamilyProperties, queue_family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_family_props);

      for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT && (queue_family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
          queue_family_index = i;
          printf("GPU Queue Family Index: %i\n", i);
          break;
        }
      }
    }
  }

  VkDevice device = {0};
  {
    float default_queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_create_info.queueFamilyIndex = queue_family_index;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &default_queue_priority;

    VkDeviceCreateInfo device_create_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    VK_CHECK(vkCreateDevice(physical_device, &device_create_info, 0, &device));
  }

  VkQueue queue = {0};
  {
    vkGetDeviceQueue(device, queue_family_index, 0, &queue);
  }

  VkCommandPool command_pool = {0};
  {
    VkCommandPoolCreateInfo cmd_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmd_pool_info.queueFamilyIndex = queue_family_index;
    cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device, &cmd_pool_info, 0, &command_pool));
  }

  VkDeviceMemory staging_memory = {0};
  VkBuffer staging_buffer = {0};
  {
    VkBufferCreateInfo buffer_create_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_create_info.size = COMPUTE_BUFFER_NBYTES;
    buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &buffer_create_info, 0, &staging_buffer));

    VkMemoryAllocateInfo mem_alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    {
      VkMemoryRequirements mem_reqs = {0};
      vkGetBufferMemoryRequirements(device, staging_buffer, &mem_reqs);
      mem_alloc_info.allocationSize = mem_reqs.size;

      VkBool32 memory_type_found = VK_FALSE;
      for (Size i = 0; i < physical_device_memory_props.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & 1) == 1) {
          if ((physical_device_memory_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            mem_alloc_info.memoryTypeIndex = i;
            memory_type_found = VK_TRUE;
            break;
          }
        }
        mem_reqs.memoryTypeBits >>= 1;
      }
      assert(memory_type_found == VK_TRUE && "Could not find memory type");
      VK_CHECK(vkAllocateMemory(device, &mem_alloc_info, 0, &staging_memory));
      VK_CHECK(vkBindBufferMemory(device, staging_buffer, staging_memory, 0));
    }

    void *mapped;
    vkMapMemory(device, staging_memory, 0, COMPUTE_BUFFER_NBYTES, 0, &mapped);
    __builtin_memcpy(mapped, compute_input, COMPUTE_BUFFER_NBYTES);
    VkMappedMemoryRange mapped_range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mapped_range.memory = staging_memory;
    mapped_range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device, 1, &mapped_range);
    vkUnmapMemory(device, staging_memory);
  }

  VkDeviceMemory compute_memory = {0};
  VkBuffer compute_buffer = {0};
  {
    VkBufferCreateInfo buffer_create_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_create_info.size = COMPUTE_BUFFER_NBYTES;
    buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &buffer_create_info, 0, &compute_buffer));

    VkMemoryAllocateInfo mem_alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    {
      VkMemoryRequirements mem_reqs = {0};
      vkGetBufferMemoryRequirements(device, compute_buffer, &mem_reqs);
      mem_alloc_info.allocationSize = mem_reqs.size;

      VkBool32 memory_type_found = VK_FALSE;
      for (Size i = 0; i < physical_device_memory_props.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & 1) == 1) {
          if ((physical_device_memory_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            mem_alloc_info.memoryTypeIndex = i;
            memory_type_found = VK_TRUE;
            break;
          }
        }
        mem_reqs.memoryTypeBits >>= 1;
      }
      assert(memory_type_found == VK_TRUE && "Could not find memory type");
      VK_CHECK(vkAllocateMemory(device, &mem_alloc_info, 0, &compute_memory));

      VK_CHECK(vkBindBufferMemory(device, compute_buffer,
                                  compute_memory, 0));
    }
  }

  VkImage output_image = {0};
  VkImageView output_image_view = {0};
  VkDeviceMemory output_image_memory = {0};
  {
    VkImageCreateInfo image_create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_create_info.extent = (VkExtent3D){ OUTPUT_IMAGE_WIDTH, OUTPUT_IMAGE_HEIGHT, 1 };
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_LINEAR; // Linear is PPM compatible (I think), Optimal is optizmized for GPU but we read the result directly from cpu.
    image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    VK_CHECK(vkCreateImage(device, &image_create_info, 0, &output_image));

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, output_image, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    VkBool32 memory_type_found = VK_FALSE;
    for (Size i = 0; i < physical_device_memory_props.memoryTypeCount; i++) {
      if ((mem_reqs.memoryTypeBits & 1) == 1) {
        if ((physical_device_memory_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
          alloc_info.memoryTypeIndex = i;
          memory_type_found = VK_TRUE;
          break;
        }
      }
      mem_reqs.memoryTypeBits >>= 1;
    }
    assert(memory_type_found == VK_TRUE && "Could not find memory type");

    VK_CHECK(vkAllocateMemory(device, &alloc_info, 0, &output_image_memory));
    VK_CHECK(vkBindImageMemory(device, output_image, output_image_memory, 0));

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = output_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    VK_CHECK(vkCreateImageView(device, &view_info, 0, &output_image_view));
  }

  { // Copy staging_buffer to compute_buffer
    VkCommandBuffer copy_cmd = {0};
    {
      VkCommandBufferAllocateInfo command_buffer_alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      command_buffer_alloc_info.commandPool = command_pool;
      command_buffer_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_alloc_info.commandBufferCount = 1;

      VK_CHECK(vkAllocateCommandBuffers(device, &command_buffer_alloc_info, &copy_cmd));

		  VkCommandBufferBeginInfo cmd_begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		  VK_CHECK(vkBeginCommandBuffer(copy_cmd, &cmd_begin_info));

      VkBufferCopy copy_region = {0};
      copy_region.size = COMPUTE_BUFFER_NBYTES;
		  vkCmdCopyBuffer(copy_cmd, staging_buffer, compute_buffer, 1, &copy_region);

      VK_CHECK(vkEndCommandBuffer(copy_cmd));
    }

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    {
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &copy_cmd;
    }

    VkFence fence = {0};
    {
      VkFenceCreateInfo fence_create_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VK_CHECK(vkCreateFence(device, &fence_create_info, 0, &fence));
    }

    VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, (1ll << 50)));
  }

  VkDescriptorPool descriptor_pool = {0};
  {
    VkDescriptorPoolSize pool_sizes[] = {
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
    };

    VkDescriptorPoolCreateInfo descriptor_pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool_create_info.maxSets = 1;
    descriptor_pool_create_info.poolSizeCount = countof(pool_sizes);
    descriptor_pool_create_info.pPoolSizes = pool_sizes;

    VK_CHECK(vkCreateDescriptorPool(device, &descriptor_pool_create_info, 0, &descriptor_pool));
  }

  VkDescriptorSetLayout descriptor_set_layout = {0};
  {
    VkDescriptorSetLayoutBinding set_layout_bindings[] = {
      { /* binding = */ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, /* count */ 1, VK_SHADER_STAGE_COMPUTE_BIT, 0},
      { /* binding = */ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, /* count */ 1, VK_SHADER_STAGE_COMPUTE_BIT, 0},
    };
    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_set_layout_create_info.bindingCount = countof(set_layout_bindings);
    descriptor_set_layout_create_info.pBindings = set_layout_bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, 0, &descriptor_set_layout));
  }

  VkDescriptorSet descriptor_set = {0};
  {
    VkDescriptorSetAllocateInfo descriptor_set_alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptor_set_alloc_info.descriptorPool = descriptor_pool;
    descriptor_set_alloc_info.descriptorSetCount = 1;
    descriptor_set_alloc_info.pSetLayouts = &descriptor_set_layout;
    VK_CHECK(vkAllocateDescriptorSets(device, &descriptor_set_alloc_info, &descriptor_set));


    VkWriteDescriptorSet compute_write_descriptor_set[2] = {0};

    VkDescriptorBufferInfo buffer_descriptor = { compute_buffer, 0, VK_WHOLE_SIZE };
    compute_write_descriptor_set[0] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    compute_write_descriptor_set[0].dstSet = descriptor_set;
    compute_write_descriptor_set[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    compute_write_descriptor_set[0].dstBinding = 0;
    compute_write_descriptor_set[0].descriptorCount = 1;
    compute_write_descriptor_set[0].pBufferInfo = &buffer_descriptor;

    VkDescriptorImageInfo dst_info = { 0, output_image_view, VK_IMAGE_LAYOUT_GENERAL };
    compute_write_descriptor_set[1] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    compute_write_descriptor_set[1].dstSet = descriptor_set;
    compute_write_descriptor_set[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    compute_write_descriptor_set[1].dstBinding = 1;
    compute_write_descriptor_set[1].descriptorCount = 1;
    compute_write_descriptor_set[1].pImageInfo = &dst_info;

    vkUpdateDescriptorSets(device, countof(compute_write_descriptor_set), compute_write_descriptor_set, 0, 0);
  }

  VkPipelineLayout pipeline_layout = {0};
  {
    VkPushConstantRange pushConstantRange = {};
    {
      pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      pushConstantRange.offset     = 0;
      pushConstantRange.size       = sizeof(int32_t) * 2;
    }

    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_create_info.setLayoutCount = 1;
    pipeline_layout_create_info.pSetLayouts = &descriptor_set_layout;
    pipeline_layout_create_info.pushConstantRangeCount = 1;
    pipeline_layout_create_info.pPushConstantRanges = &pushConstantRange;

    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_create_info, 0, &pipeline_layout));
  }

  VkPipeline pipeline = {0};
  {
    VkPipelineCache pipeline_cache = {0};
    VkPipelineCacheCreateInfo pipeline_cache_create_info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(device, &pipeline_cache_create_info, 0, &pipeline_cache);

    VkPipelineShaderStageCreateInfo shader_stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    {
      VkShaderModule shader_module = {0};
      {
        FILE *f = fopen("headless.comp.spv", "rb");
        fseek(f, 0, SEEK_END);
        Size source_size = ftell(f);
        unsigned char *source_cstr = arena_alloc(&arena, 1, 16, source_size + 1);
        fseek(f, 0, SEEK_SET);
        int r = fread(source_cstr, 1, source_size, f);
        assert(r >= 0);
        source_cstr[source_size] = '\0';
        fclose(f);

        VkShaderModuleCreateInfo module_create_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_create_info.codeSize = source_size;
        module_create_info.pCode = (uint32_t *)source_cstr;

        VK_CHECK(vkCreateShaderModule(device, &module_create_info, 0, &shader_module));
      }

      shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      shader_stage.module = shader_module;
      shader_stage.pName = "main";
    }

    VkComputePipelineCreateInfo compute_pipeline_create_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute_pipeline_create_info.layout = pipeline_layout;
    compute_pipeline_create_info.stage = shader_stage;

    VK_CHECK(vkCreateComputePipelines(device, pipeline_cache, 1, &compute_pipeline_create_info, 0, &pipeline));
  }

  VkCommandBuffer command_buffer = {0};
  VkFence fence = {0};
  {
    VkCommandBufferAllocateInfo cmd_buffer_allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_buffer_allocate_info.commandPool = command_pool;
    cmd_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_buffer_allocate_info.commandBufferCount = 1;

    VkFenceCreateInfo fence_create_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

    VK_CHECK(vkAllocateCommandBuffers(device, &cmd_buffer_allocate_info, &command_buffer));
    VK_CHECK(vkCreateFence(device, &fence_create_info, 0, &fence));
  }

  VkCommandBufferBeginInfo cmd_begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(command_buffer, &cmd_begin_info));

  { // wait for copy staging_buffer -> compute_buffer
    VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer_barrier.buffer = compute_buffer;
    buffer_barrier.size = VK_WHOLE_SIZE;
    buffer_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 1, &buffer_barrier, 0, 0);
  }

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_layout, 0, 1, &descriptor_set, 0, 0);
  int32_t push_constants[2] = { COMPUTE_BUFFER_SIZE, 0 };
  vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), push_constants);
  vkCmdDispatch(command_buffer, OUTPUT_IMAGE_WIDTH / 8, OUTPUT_IMAGE_HEIGHT / 8, 1);

  { // wait for compute shader to finish with compute_buffer
    VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer_barrier.buffer = compute_buffer;
    buffer_barrier.size = VK_WHOLE_SIZE;
    buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    VkImageMemoryBarrier image_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image_barrier.image = output_image;
    image_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.layerCount = 1;
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 1, &buffer_barrier, 1, &image_barrier);
  }

  { // copy compute_buffer -> staging_buffer
    VkBufferCopy copy_region = {0};
    copy_region.size = COMPUTE_BUFFER_NBYTES;
    vkCmdCopyBuffer(command_buffer, compute_buffer, staging_buffer, 1, &copy_region);
  }

  { // wait for copy compute_buffer -> staging_buffer
    VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer_barrier.buffer = staging_buffer;
    buffer_barrier.size = VK_WHOLE_SIZE;
    buffer_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, 0, 1, &buffer_barrier, 0, 0);
  }
  
  VK_CHECK(vkEndCommandBuffer(command_buffer));

  // Submit compute work
  vkResetFences(device, 1, &fence);
  VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.pWaitDstStageMask = &wait_stage_mask;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
  VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, (1ll << 50)));

  {
    void *mapped;
    vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    VkMappedMemoryRange mapped_range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mapped_range.memory = staging_memory;
    mapped_range.offset = 0;
    mapped_range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device, 1, &mapped_range);
    __builtin_memcpy(compute_output, mapped, COMPUTE_BUFFER_NBYTES);
    vkUnmapMemory(device, staging_memory);
  }

  vkQueueWaitIdle(queue);
  vkDeviceWaitIdle(device);

  {
    U8* data = 0;
    vkMapMemory(device, output_image_memory, 0, VK_WHOLE_SIZE, 0, (void **)&data);

    if (1) {
      VkSubresourceLayout res_layout = {0};
      VkImageSubresource image_res_layout = {0};
      image_res_layout.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vkGetImageSubresourceLayout(device, output_image, &image_res_layout,
                                  &res_layout);
      data += res_layout.offset;

      FILE *f = fopen("out.ppm", "w+");
      char sprintf_buffer[1024];
      sprintf(sprintf_buffer, "P6\n%d\n%d\n255\n", OUTPUT_IMAGE_WIDTH, OUTPUT_IMAGE_HEIGHT);
      fwrite(sprintf_buffer, 1, __builtin_strlen(sprintf_buffer), f);

      for (Size i = 0; i < OUTPUT_IMAGE_HEIGHT; i++) {
        for (Size x = 0; x < OUTPUT_IMAGE_WIDTH; x++) {
          uint32_t c = ((uint32_t*)data)[x];
          fwrite(&c, 1, 3, f);
        }
        data += res_layout.rowPitch;
      }
      fflush(f);
      fclose(f);
    }

    vkUnmapMemory(device, output_image_memory);
  }

  vkDestroyBuffer(device, compute_buffer, 0);
  vkFreeMemory(device, compute_memory, 0);
  vkDestroyBuffer(device, staging_buffer, 0);
  vkFreeMemory(device, staging_memory, 0);
  vkDestroyCommandPool(device, command_pool, 0);

  vkDestroyDevice(device, 0);
  vkDestroyInstance(instance, 0);
}

// Platform
#define HEAP_CAP     (1ll << 30)

int main(int argc, char **argv) {
  void *heap = malloc(HEAP_CAP);
  Arena arena = (Arena){heap, heap + HEAP_CAP};
  run(arena);
  free(heap);
  return 0;
}
