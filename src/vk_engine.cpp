//> includes
#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <VkBootstrap.h>

#include <chrono>
#include <thread>

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }

constexpr bool bUseValidationLayers = true; // Q: what is constexpr again?
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    // different strages of initializing vulkan instance
    init_vulkan();
    init_swapchain();
    init_commands();
    init_sync_structures();

    // everything went fine
    _isInitialized = true;
}

/// <summary>
/// objects have dependencies on each other, must delete in correct order (opposite way they were created)
/// </summary>
void VulkanEngine::cleanup()
{
    if (_isInitialized) {
        // make sure the gpu has stopped doing things
        vkDeviceWaitIdle(_device);

        // destroying parent pools of cmd buffers will destroy them as well. not possible
        // to indiv destroy cmd buffer
        for (int i = 0; i < FRAME_OVERLAP; i++)
        {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);
        }

        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);

        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
    // nothing yet
}

void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_KEYDOWN)
            {
                if (e.key.keysym.sym == SDLK_e)
                {
                    fmt::print("My first code in my new Vulkan engine, woohoo!!");
                }
            }

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        draw();
    }
}

void VulkanEngine::init_vulkan()
{
    // abstracts creation of vulkan instance using library
    vkb::InstanceBuilder builder;

    // make the vulkan instance, with basic debug features
    auto inst_ret = builder.set_app_name("My First Vulkan App")
        .request_validation_layers(bUseValidationLayers) // validation layers on for catching errors, slow down perform. turn off for real performance
        .use_default_debug_messenger() // cattches log msgs that validati layers output
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkb_inst = inst_ret.value();

    // grab the instance
    _instance = vkb_inst.instance;
    _debug_messenger = vkb_inst.debug_messenger;


    // actual window will be rendering to. need to tell physical device selector to grab gpu
    // that can render to this. Q: that why we passing it as ref to edit?
    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    // there are multiple levels of feature structs can use depending on vulkan version!
    // vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features.dynamicRendering = true; // allows us to completely skip renderpasses/framebuffers
    features.synchronization2 = true;

    // vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true; // use gpu pointers without binding buffers
    features12.descriptorIndexing = true; // bindless textures Q: what this? dont have to bind to texture unit or what?

    // use vkbootstrap to select a gpu
    // we want a gpu that can write to the sdl surface and support vulkan 1.3 with the correct features
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();
        
    // create the final vulkan device
    vkb::DeviceBuilder deviceBuilder{ physicalDevice };

    vkb::Device vkbDevice = deviceBuilder.build().value();

    // Get the VkDevice handle used in rest of vulkan application
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;

    // use vkbootstrap to get a graphics queue
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value(); // graphics queues can do everything others can
    _graphicsQueueFamily = vkbDevice.get_dedicated_queue_index(vkb::QueueType::graphics).value();

}

void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);
}

// gives us a way to send commands to the gpu!
void VulkanEngine::init_commands()
{

    // create a command pool for commands submitted to the graphics queue
    // also want the pool to allow for resetting of individual command buffers
    // abstracted
    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    //VkCommandPoolCreateInfo commandPoolInfo = {}; // init entire struct to 0, so no unit data
    //// basically vulkan's plujgin system for new features, without changing core API? sType/pNext
    //commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; // used by vkCreateX, vkAllocateX- helps the implementation know what struct is being used in the function
    //commandPoolInfo.pNext = nullptr; // to add extensions to core api?
    //commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;// we expect to be able to reset individ cmd buffers made from pool
    //commandPoolInfo.queueFamilyIndex = _graphicsQueueFamily; // pool will create cmds compatible with any queue of this graphics fam

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        // vkCheck checks if command succeeds, aborts if not
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // allocate the default command buffer that we will use for rendering
        // abstracted
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
        //VkCommandBufferAllocateInfo cmdAllocInfo = {};
        //cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        //cmdAllocInfo.pNext = nullptr;
        //cmdAllocInfo.commandPool = _frames[i]._commandPool; // parent of our command
        //cmdAllocInfo.commandBufferCount = 1;
        //// primary lvl are ones sent into vk queue and do all the work
        //cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
}

void VulkanEngine::init_sync_structures()
{
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        // .format is a member of vkSurfaceFormat, and designated initializer lets us put initializers in any order
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        // use vsync present mode
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR) // IMPORTANT!!! hard VSync
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    _swapchainExtent = vkbSwapchain.extent;
    // store swapchain and its related images
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapChainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain()
{
    // deletes images it holds as well as they are owned by swapchain
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    
    // destroy swapchain resources, image views for images
    for (int i = 0; i < _swapChainImageViews.size(); i++)
    {
        vkDestroyImageView(_device, _swapChainImageViews[i], nullptr);
    }
}

