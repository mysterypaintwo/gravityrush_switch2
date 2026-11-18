/* Gravity Rush Switch 2 port: Vulkan capability probe (switch2/PLAN.md, phase 3).
 * Lists what OunceVK offers for the port's GPU library (the shadPS4 video core): the device,
 * its limits, the features and extensions the library requires or uses, and every device
 * extension. Output goes to the screen and to sdmc:/gr_vkprobe.txt. Nothing here can crash on
 * purpose; press + to exit. */
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static FILE *logf;

static void out(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    consoleUpdate(NULL);
    if (logf) { fprintf(logf, "%s\n", line); fflush(logf); }
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void probe(void) {
    out("gr_vkprobe v1 for the Gravity Rush Switch 2 port. Log: sdmc:/gr_vkprobe.txt");
    uint32_t api = 0;
    vkEnumerateInstanceVersion(&api);
    out("instance API %u.%u.%u", VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api));

    uint32_t n_iext = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n_iext, NULL);
    VkExtensionProperties *iext = calloc(n_iext ? n_iext : 1, sizeof *iext);
    vkEnumerateInstanceExtensionProperties(NULL, &n_iext, iext);
    out("== %u instance extensions", n_iext);
    for (uint32_t i = 0; i < n_iext; i++) out("  %s", iext[i].extensionName);

    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gr_vkprobe",
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    out("vkCreateInstance: %d", r);
    if (r != VK_SUCCESS) return;

    uint32_t n_dev = 0;
    vkEnumeratePhysicalDevices(instance, &n_dev, NULL);
    out("physical devices: %u", n_dev);
    if (!n_dev) return;
    VkPhysicalDevice devs[4];
    if (n_dev > 4) n_dev = 4;
    vkEnumeratePhysicalDevices(instance, &n_dev, devs);
    VkPhysicalDevice pd = devs[0];

    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(pd, &props);
    const VkPhysicalDeviceLimits *l = &props.properties.limits;
    out("== device: %s, API %u.%u.%u, driver %s (%s)", props.properties.deviceName,
        VK_VERSION_MAJOR(props.properties.apiVersion), VK_VERSION_MINOR(props.properties.apiVersion),
        VK_VERSION_PATCH(props.properties.apiVersion), driver.driverName, driver.driverInfo);
    out("vendor 0x%x device 0x%x type %d", props.properties.vendorID, props.properties.deviceID,
        props.properties.deviceType);
    out("limits: maxStorageBufferRange %u, maxUniformBufferRange %u, maxImageDimension2D %u",
        l->maxStorageBufferRange, l->maxUniformBufferRange, l->maxImageDimension2D);
    out("limits: maxPushConstantsSize %u, maxBoundDescriptorSets %u, maxPerStageResources %u",
        l->maxPushConstantsSize, l->maxBoundDescriptorSets, l->maxPerStageResources);
    out("limits: minStorageBufferOffsetAlignment %llu, minUniformBufferOffsetAlignment %llu, timestampPeriod %.3f",
        (unsigned long long)l->minStorageBufferOffsetAlignment, (unsigned long long)l->minUniformBufferOffsetAlignment,
        l->timestampPeriod);
    out("sparse: residencyStandard2DBlockShape %u, residencyNonResidentStrict %u",
        props.properties.sparseProperties.residencyStandard2DBlockShape,
        props.properties.sparseProperties.residencyNonResidentStrict);

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    out("== memory: %u heaps, %u types", mem.memoryHeapCount, mem.memoryTypeCount);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        out("  heap %u: %llu MiB, flags 0x%x", i, (unsigned long long)(mem.memoryHeaps[i].size >> 20), mem.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        out("  type %u: heap %u, flags 0x%x", i, mem.memoryTypes[i].heapIndex, mem.memoryTypes[i].propertyFlags);

    VkPhysicalDeviceRobustness2FeaturesEXT rob = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &rob};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f);
    const VkPhysicalDeviceFeatures *c = &f.features;
    out("== features the GPU library needs or uses (1 = yes)");
    out("robustness2: robustBufferAccess2 %u, robustImageAccess2 %u, nullDescriptor %u",
        rob.robustBufferAccess2, rob.robustImageAccess2, rob.nullDescriptor);
    out("sparse: binding %u, residencyBuffer %u, residencyImage2D %u, residencyAliased %u",
        c->sparseBinding, c->sparseResidencyBuffer, c->sparseResidencyImage2D, c->sparseResidencyAliased);
    out("core: geometryShader %u, tessellationShader %u, shaderInt64 %u, shaderFloat64 %u, shaderInt16 %u",
        c->geometryShader, c->tessellationShader, c->shaderInt64, c->shaderFloat64, c->shaderInt16);
    out("core: depthClamp %u, depthBiasClamp %u, logicOp %u, dualSrcBlend %u, independentBlend %u, fillModeNonSolid %u",
        c->depthClamp, c->depthBiasClamp, c->logicOp, c->dualSrcBlend, c->independentBlend, c->fillModeNonSolid);
    out("core: imageCubeArray %u, samplerAnisotropy %u, textureCompressionBC %u, multiDrawIndirect %u, drawIndirectFirstInstance %u",
        c->imageCubeArray, c->samplerAnisotropy, c->textureCompressionBC, c->multiDrawIndirect, c->drawIndirectFirstInstance);
    out("core: shaderStorageImageReadWithoutFormat %u, WriteWithoutFormat %u, vertexPipelineStoresAndAtomics %u, fragmentStoresAndAtomics %u",
        c->shaderStorageImageReadWithoutFormat, c->shaderStorageImageWriteWithoutFormat,
        c->vertexPipelineStoresAndAtomics, c->fragmentStoresAndAtomics);
    out("1.2: bufferDeviceAddress %u, timelineSemaphore %u, descriptorIndexing %u, shaderFloat16 %u, shaderInt8 %u, scalarBlockLayout %u",
        f12.bufferDeviceAddress, f12.timelineSemaphore, f12.descriptorIndexing, f12.shaderFloat16, f12.shaderInt8,
        f12.scalarBlockLayout);
    out("1.2: storageBuffer8BitAccess %u, uniformAndStorageBuffer8BitAccess %u, hostQueryReset %u, drawIndirectCount %u",
        f12.storageBuffer8BitAccess, f12.uniformAndStorageBuffer8BitAccess, f12.hostQueryReset, f12.drawIndirectCount);
    out("1.1: storageBuffer16BitAccess %u, shaderDrawParameters %u", f11.storageBuffer16BitAccess, f11.shaderDrawParameters);
    out("1.3: dynamicRendering %u, synchronization2 %u, maintenance4 %u, shaderDemoteToHelperInvocation %u",
        f13.dynamicRendering, f13.synchronization2, f13.maintenance4, f13.shaderDemoteToHelperInvocation);

    uint32_t n_ext = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n_ext, NULL);
    VkExtensionProperties *ext = calloc(n_ext ? n_ext : 1, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n_ext, ext);
    static const char *const wanted[] = {
        "VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_EXT_vertex_attribute_divisor",
        "VK_EXT_robustness2", "VK_KHR_maintenance5", "VK_KHR_maintenance8", "VK_KHR_pipeline_binary",
        "VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state", "VK_EXT_depth_clip_control",
        "VK_EXT_depth_clip_enable", "VK_EXT_primitive_topology_list_restart", "VK_EXT_attachment_feedback_loop_layout",
        "VK_EXT_shader_stencil_export", "VK_EXT_image_view_min_lod", "VK_KHR_shader_clock",
        "VK_KHR_workgroup_memory_explicit_layout", "VK_KHR_compute_shader_derivatives", "VK_EXT_legacy_vertex_attributes",
        "VK_EXT_provoking_vertex", "VK_EXT_color_write_enable", "VK_EXT_custom_border_color",
        "VK_KHR_fragment_shader_barycentric", "VK_EXT_shader_atomic_float", "VK_KHR_shader_float_controls2",
        "VK_EXT_pipeline_creation_cache_control", "VK_EXT_memory_budget",
    };
    out("== extensions the GPU library asks for");
    for (size_t i = 0; i < sizeof wanted / sizeof *wanted; i++)
        out("  %-42s %s", wanted[i], has_ext(ext, n_ext, wanted[i]) ? "yes" : "NO");
    out("== all %u device extensions", n_ext);
    for (uint32_t i = 0; i < n_ext; i++) out("  %s %u", ext[i].extensionName, ext[i].specVersion);

    uint32_t n_q = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n_q, NULL);
    VkQueueFamilyProperties q[8];
    if (n_q > 8) n_q = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n_q, q);
    out("== %u queue families", n_q);
    for (uint32_t i = 0; i < n_q; i++) out("  family %u: flags 0x%x, %u queues", i, q[i].queueFlags, q[i].queueCount);

    /* a device with the sparse queue, to show the driver accepts it */
    float prio = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    r = vkCreateDevice(pd, &dci, NULL, &device);
    out("vkCreateDevice (queue family 0): %d", r);
    if (r == VK_SUCCESS) vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    out("== done. Press + to exit.");
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    logf = fopen("sdmc:/gr_vkprobe.txt", "w");
    probe();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    if (logf) fclose(logf);
    consoleExit(NULL);
    return 0;
}
