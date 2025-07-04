#include <Geranium.h>
#include <Hyacinth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

extern bool createPipeline(const VkExtent2D *const extent,
                           const VkDevice device, VkFormat format);
extern void beginRenderpass(VkFramebuffer framebuffer, VkCommandBuffer buffer,
                            const VkExtent2D *const extent);

extern VkRenderPass gRenderpass;

static uint32_t currentFrame = 0;

static VkInstance pInstance = nullptr;
static VkPhysicalDevice pPhysicalDevice = nullptr;
static VkDevice pLogicalDevice = nullptr;
static VkQueue pGraphicsQueue = nullptr;
static VkQueue pPresentQueue = nullptr;
static uint32_t pGraphicsIndex = 0;
static uint32_t pPresentIndex = 0;

static VkSurfaceKHR pSurface = nullptr;
static uint32_t pFormatCount = 0;
static VkSurfaceFormatKHR *pFormats = nullptr;
static uint32_t pModeCount = 0;
static VkPresentModeKHR *pModes = nullptr;

static VkSurfaceCapabilitiesKHR pCapabilities;
static VkSurfaceFormatKHR pFormat;
static VkPresentModeKHR pMode;

static VkSwapchainKHR pSwapchain = nullptr;
static uint32_t pImageCount = 0;
static VkImageView *pSwapchainImages = nullptr;
static VkFramebuffer *pSwapchainFramebuffers = nullptr;

static VkCommandPool pCommandPool;
static VkCommandBuffer pCommandBuffers[GERANIUM_CONCURRENT_FRAMES];

static VkSemaphore pImageAvailableSemaphores[GERANIUM_CONCURRENT_FRAMES];
static VkSemaphore pRenderFinishedSemaphores[GERANIUM_CONCURRENT_FRAMES];
static VkFence pFences[GERANIUM_CONCURRENT_FRAMES];

// TODO: Get this the fuck outta here.
// https://stackoverflow.com/questions/427477/fastest-way-to-clamp-a-real-fixed-floating-point-value#16659263
uint32_t _clamp(uint32_t d, uint32_t min, uint32_t max)
{
    const uint32_t t = d < min ? min : d;
    return t > max ? max : t;
}

VkExtent2D getSurfaceExtent(uint32_t width, uint32_t height)
{
    if (pCapabilities.currentExtent.width != UINT32_MAX)
        return pCapabilities.currentExtent;

    VkExtent2D surfaceExtent = {.width = width, .height = height};

    surfaceExtent.width =
        _clamp(surfaceExtent.width, pCapabilities.minImageExtent.width,
               pCapabilities.maxImageExtent.width);
    surfaceExtent.height =
        _clamp(surfaceExtent.height, pCapabilities.minImageExtent.height,
               pCapabilities.maxImageExtent.height);

    return surfaceExtent;
}

VkSurfaceFormatKHR chooseSurfaceFormat(void)
{
    for (size_t i = 0; i < pFormatCount; i++)
    {
        VkSurfaceFormatKHR format = pFormats[i];
        // This is the best combination. If not available, we'll just
        // select the first provided colorspace.
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            pFormat = format;
            return pFormat;
        }
    }
    return pFormats[0];
}

VkPresentModeKHR chooseSurfaceMode(void)
{
    for (size_t i = 0; i < pModeCount; i++)
    {
        // This is the best mode. If not available, we'll just select
        // VK_PRESENT_MODE_FIFO_KHR.
        if (pModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            pMode = pModes[i];
            return pMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceCapabilitiesKHR findSurfaceCapabilities(void)
{
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pPhysicalDevice, pSurface,
                                              &pCapabilities);
    return pCapabilities;
}

VkSurfaceCapabilitiesKHR getSurfaceCapabilities() { return pCapabilities; }

bool createSwapchain(const VkExtent2D *const extent)
{
    VkSurfaceFormatKHR format = chooseSurfaceFormat();
    VkPresentModeKHR mode = chooseSurfaceMode();
    VkSurfaceCapabilitiesKHR capabilities = getSurfaceCapabilities();

    pImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 &&
        pImageCount > capabilities.maxImageCount)
        pImageCount = capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = pSurface;
    createInfo.minImageCount = pImageCount;
    createInfo.imageFormat = format.format;
    createInfo.imageColorSpace = format.colorSpace;
    createInfo.imageExtent = *extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = mode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    uint32_t indices[2] = {pGraphicsIndex, pPresentIndex};
    if (indices[0] != indices[1])
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = indices;
    }
    else createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateSwapchainKHR(pLogicalDevice, &createInfo, nullptr,
                             &pSwapchain) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create swapchain.\n");
        return false;
    }

    vkGetSwapchainImagesKHR(pLogicalDevice, pSwapchain, &pImageCount, nullptr);
    VkImage *images = malloc(sizeof(VkImage) * pImageCount);
    pSwapchainImages = malloc(sizeof(VkImageView) * pImageCount);
    pSwapchainFramebuffers = malloc(sizeof(VkFramebuffer) * pImageCount);
    vkGetSwapchainImagesKHR(pLogicalDevice, pSwapchain, &pImageCount, images);

    VkImageViewCreateInfo imageCreateInfo = {0};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageCreateInfo.format = format.format;
    imageCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCreateInfo.subresourceRange.baseMipLevel = 0;
    imageCreateInfo.subresourceRange.levelCount = 1;
    imageCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageCreateInfo.subresourceRange.layerCount = 1;

    for (size_t i = 0; i < pImageCount; i++)
    {
        imageCreateInfo.image = images[i];
        if (vkCreateImageView(pLogicalDevice, &imageCreateInfo, nullptr,
                              &pSwapchainImages[i]) != VK_SUCCESS)
        {
            fprintf(stderr, "Failed to create image view %zu.", i);
            return false;
        }
    }

    return true;
}

VkSurfaceFormatKHR *getSurfaceFormats(VkPhysicalDevice device)
{
    if (pFormats != nullptr) return pFormats;

    vkGetPhysicalDeviceSurfaceFormatsKHR(device, pSurface, &pFormatCount,
                                         nullptr);
    if (pFormatCount == 0) return nullptr;
    pFormats = malloc(sizeof(VkSurfaceFormatKHR) * pFormatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, pSurface, &pFormatCount,
                                         pFormats);
    return pFormats;
}

VkPresentModeKHR *getSurfaceModes(VkPhysicalDevice device)
{
    if (pModes != nullptr) return pModes;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, pSurface, &pModeCount,
                                              nullptr);
    if (pModeCount == 0) return nullptr;
    pModes = malloc(sizeof(VkPresentModeKHR) * pModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, pSurface, &pModeCount,
                                              pModes);
    return pModes;
}

uint32_t scoreDevice(VkPhysicalDevice device, const char **extensions,
                     size_t extensionCount)
{
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceProperties(device, &properties);
    vkGetPhysicalDeviceFeatures(device, &features);

    uint32_t score = 0;
    switch (properties.deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score += 4; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score += 2; break;
        default:                                     score += 1; break;
    }

    uint32_t availableExtensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr,
                                         &availableExtensionCount, nullptr);
    VkExtensionProperties *availableExtensions =
        malloc(sizeof(VkExtensionProperties) * availableExtensionCount);
    vkEnumerateDeviceExtensionProperties(
        device, nullptr, &availableExtensionCount, availableExtensions);

    size_t foundCount = 0;
    for (size_t i = 0; i < availableExtensionCount; i++)
    {
        VkExtensionProperties extension = availableExtensions[i];

        for (size_t j = 0; j < extensionCount; j++)
            if (strcmp(extension.extensionName, extensions[j]) == 0)
            {
                foundCount++;
                printf("Found: %s device extension.\n",
                       extension.extensionName);
                break;
            }
    }
    free(availableExtensions);
    if (foundCount != extensionCount)
    {
        fprintf(stderr, "Failed to find all device extensions.\n");
        return 0;
    }

    if (getSurfaceFormats(device) == nullptr ||
        getSurfaceModes(device) == nullptr)
    {
        fprintf(stderr, "Failed to find surface format/present modes.\n");
        return 0;
    }

    // TODO: Extra grading to be done here.

    return score;
}

bool createFramebuffers(const VkExtent2D *const extent)
{
    VkFramebufferCreateInfo framebufferInfo = {0};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = gRenderpass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.width = extent->width;
    framebufferInfo.height = extent->height;
    framebufferInfo.layers = 1;

    for (size_t i = 0; i < pImageCount; i++)
    {
        framebufferInfo.pAttachments = &pSwapchainImages[i];
        if (vkCreateFramebuffer(pLogicalDevice, &framebufferInfo, nullptr,
                                &pSwapchainFramebuffers[i]) != VK_SUCCESS)
        {
            fprintf(stderr, "Failed to create framebuffer.\n");
            return false;
        }
    }
    return true;
}

bool createCommandBuffers(void)
{
    VkCommandPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = pGraphicsIndex;

    if (vkCreateCommandPool(pLogicalDevice, &poolInfo, nullptr,
                            &pCommandPool) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create command pool.\n");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = GERANIUM_CONCURRENT_FRAMES;

    if (vkAllocateCommandBuffers(pLogicalDevice, &allocInfo, pCommandBuffers) !=
        VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create command buffer.\n");
        return false;
    }

    return true;
}

bool createSyncObjects(void)
{
    VkSemaphoreCreateInfo semaphoreInfo = {0};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo = {0};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < GERANIUM_CONCURRENT_FRAMES; i++)
    {
        if (vkCreateSemaphore(pLogicalDevice, &semaphoreInfo, nullptr,
                              &pImageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(pLogicalDevice, &semaphoreInfo, nullptr,
                              &pRenderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(pLogicalDevice, &fenceInfo, nullptr, &pFences[i]) !=
                VK_SUCCESS)
        {
            fprintf(stderr, "Failed to create sync object.\n");
            return false;
        }
    }
    return true;
}

bool recordCommandBuffer(VkCommandBuffer commandBuffer,
                         const VkExtent2D *extent, uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to begin command buffer.\n");
        return false;
    }

    beginRenderpass(pSwapchainFramebuffers[imageIndex],
                    pCommandBuffers[currentFrame], extent);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to end command buffer.\n");
        return false;
    }

    return true;
}

bool createDevice(uint32_t framebufferWidth, uint32_t framebufferHeight)
{
    uint32_t physicalCount = 0;
    vkEnumeratePhysicalDevices(pInstance, &physicalCount, nullptr);
    VkPhysicalDevice *physicalDevices =
        malloc(sizeof(VkPhysicalDevice) * physicalCount);
    vkEnumeratePhysicalDevices(pInstance, &physicalCount, physicalDevices);

    const size_t extensionCount = 1;
    const char *extensions[1] = {"VK_KHR_swapchain"};

    VkPhysicalDevice currentChosen = nullptr;
    uint32_t bestScore = 0;
    for (size_t i = 0; i < physicalCount; i++)
    {
        uint32_t score =
            scoreDevice(physicalDevices[i], extensions, extensionCount);
        if (score > bestScore)
        {
            currentChosen = physicalDevices[i];
            bestScore = score;
        }
    }

    if (currentChosen == nullptr)
    {
        fprintf(stderr, "Failed to find suitable Vulkan device.\n");
        return false;
    }
    pPhysicalDevice = currentChosen;
    free(physicalDevices);

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pPhysicalDevice, &queueFamilyCount,
                                             nullptr);

    VkQueueFamilyProperties *queueFamilies =
        malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pPhysicalDevice, &queueFamilyCount,
                                             queueFamilies);

    bool foundGraphicsQueue = false, foundPresentQueue = false;
    for (size_t i = 0; i < queueFamilyCount; i++)
    {
        if (foundGraphicsQueue && foundPresentQueue) break;

        VkQueueFamilyProperties family = queueFamilies[i];
        if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            pGraphicsIndex = i;
            foundGraphicsQueue = true;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(pPhysicalDevice, i, pSurface,
                                             &presentSupport);
        if (presentSupport)
        {
            pPresentIndex = i;
            foundPresentQueue = true;
        }
    }

    if (!foundGraphicsQueue)
    {
        fprintf(stderr, "Failed to find graphics queue.\n");
        return false;
    }

    if (!foundPresentQueue)
    {
        fprintf(stderr, "Failed to find presentation queue.\n");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfos[2] = {{0}, {0}};
    queueCreateInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfos[0].queueFamilyIndex = pGraphicsIndex;
    queueCreateInfos[0].queueCount = 1;
    queueCreateInfos[0].pQueuePriorities = &priority;

    queueCreateInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfos[1].queueFamilyIndex = pPresentIndex;
    queueCreateInfos[1].queueCount = 1;
    queueCreateInfos[1].pQueuePriorities = &priority;

    // We don't need any features at the moment.
    VkPhysicalDeviceFeatures usedFeatures = {0};

    // Layers for logical devices no longer need to be set in newer
    // implementations.
    VkDeviceCreateInfo logicalDeviceCreateInfo = {0};
    logicalDeviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    logicalDeviceCreateInfo.pQueueCreateInfos = queueCreateInfos;
    if (pPresentIndex != pGraphicsIndex)
        logicalDeviceCreateInfo.queueCreateInfoCount = 2;
    else logicalDeviceCreateInfo.queueCreateInfoCount = 1;
    logicalDeviceCreateInfo.pEnabledFeatures = &usedFeatures;

    logicalDeviceCreateInfo.enabledExtensionCount = extensionCount;
    logicalDeviceCreateInfo.ppEnabledExtensionNames = extensions;

    VkResult code = vkCreateDevice(pPhysicalDevice, &logicalDeviceCreateInfo,
                                   nullptr, &pLogicalDevice);
    if (code != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create logical device. Code: %d.\n", code);
        return false;
    }

    vkGetDeviceQueue(pLogicalDevice, pGraphicsIndex, 0, &pGraphicsQueue);
    vkGetDeviceQueue(pLogicalDevice, pPresentIndex, 0, &pPresentQueue);

    findSurfaceCapabilities();
    VkExtent2D extent = getSurfaceExtent(framebufferWidth, framebufferHeight);
    if (!createSwapchain(&extent)) return false;
    if (!createPipeline(&extent, pLogicalDevice, pFormat.format)) return false;
    if (!createFramebuffers(&extent)) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;

    return true;
}

bool geranium_create(const char *name, uint32_t version)
{
    VkApplicationInfo applicationInfo = {0};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = name;
    applicationInfo.applicationVersion = version;
    applicationInfo.pEngineName = nullptr;
    applicationInfo.engineVersion = 0;
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo = {0};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &applicationInfo;

    char *extensions[2];
    geranium_getExtensions(extensions);
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = (const char **)extensions;

    const char *layers[1] = {"VK_LAYER_KHRONOS_validation"};
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = layers;

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &pInstance);
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create Vulkan instance. Code: %d.\n",
                result);
        return false;
    }

    // This is provided by the current target file.
    extern VkSurfaceKHR createSurface(VkInstance instance, void **data);
    void *data[2];
    hyacinth_getData(data);
    pSurface = createSurface(pInstance, data);
    if (pSurface == nullptr) return false;

    uint32_t width, height;
    hyacinth_getSize(&width, &height);
    if (!createDevice(width, height)) return false;

    return true;
}

void geranium_destroy(void) { vkDeviceWaitIdle(pLogicalDevice); }

void cleanupSwapchain(void)
{
    for (size_t i = 0; i < pImageCount; i++)
    {
        vkDestroyFramebuffer(pLogicalDevice, pSwapchainFramebuffers[i],
                             nullptr);
        vkDestroyImageView(pLogicalDevice, pSwapchainImages[i], nullptr);
    }
    vkDestroySwapchainKHR(pLogicalDevice, pSwapchain, nullptr);
}

bool recreateSwapchain(const VkExtent2D *const extent)
{
    vkDeviceWaitIdle(pLogicalDevice);

    cleanupSwapchain();
    if (!createSwapchain(extent)) return false;
    if (!createFramebuffers(extent)) return false;

    return true;
}

bool geranium_render(uint32_t framebufferWidth,
                                 uint32_t framebufferHeight)
{
    vkWaitForFences(pLogicalDevice, 1, &pFences[currentFrame], VK_TRUE,
                    UINT64_MAX);

    VkExtent2D extent = getSurfaceExtent(framebufferWidth, framebufferHeight);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        pLogicalDevice, pSwapchain, UINT64_MAX,
        pImageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (!recreateSwapchain(&extent)) return false;
        return true;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        fprintf(stderr, "Failed to acquire swapchain image.\n");
        return false;
    }

    vkResetFences(pLogicalDevice, 1, &pFences[currentFrame]);

    vkResetCommandBuffer(pCommandBuffers[currentFrame], 0);
    if (!recordCommandBuffer(pCommandBuffers[currentFrame], &extent, imageIndex))
        return false;

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {pImageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &pCommandBuffers[currentFrame];

    VkSemaphore signalSemaphores[] = {pRenderFinishedSemaphores[currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    if (vkQueueSubmit(pGraphicsQueue, 1, &submitInfo, pFences[currentFrame]) !=
        VK_SUCCESS)
    {
        fprintf(stderr, "Failed to submit to the queue.\n");
        return false;
    }

    VkPresentInfoKHR presentInfo = {0};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {pSwapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(pPresentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        if (!recreateSwapchain(&extent)) return false;
    }
    else if (result != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to present swapchain image.\n");
        return false;
    }

    currentFrame = (currentFrame + 1) % GERANIUM_CONCURRENT_FRAMES;
    return true;
}

bool geranium_sync(void)
{
    return vkDeviceWaitIdle(pLogicalDevice) == VK_SUCCESS;
}
