//> includes
#include "vk_engine.h"
#include "vk_images.h";

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
    fmt::print("About to init vulkan\n");
    init_vulkan();
    fmt::print("Vulkan init complete\n");

    fmt::print("About to init swapchain\n");
    init_swapchain();
    fmt::print("Swapchain init complete\n");
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

            // destroy sync objects
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._renderSemaphore, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);
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
    // wait until the gpu has finished rendering the last frame. timeout of 1 second (in nanoseconds). after 1 second return vk timeout
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));
    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence)); // femces must be reset between uses

    // request image index from the swapchain. if doesnt have any image we can use, block thread for 1 second. after 1 second return vk timeout
    // we use index given from following funct to decide which swapchain images to use for drawing
    uint32_t swapchainImageIndex;
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));

    // copy command buffer handle from framedata (to shorten)
    // vulkan handles only a 64 bit handle/pointer, fine to copy around

    VkCommandBuffer cmdBuffer = get_current_frame()._mainCommandBuffer; 

    // now that we are sure that the commands finished executing, can safely 
    // reset the command buffer to begin recording again
    VK_CHECK(vkResetCommandBuffer(cmdBuffer, 0));

    // begin the command buffer recording. we will use this command buffer ONLY once (before resetting) again, so want to let vulkan know that (maybe gives small speedup)
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    // start the command buffer recording
    VK_CHECK(vkBeginCommandBuffer(cmdBuffer, &cmdBeginInfo));

    // make swapchain image into writeable mode before rendering
    // undefined bc we dont care abt data already in image, fine with gpu destroying. general allows read/write (not most optimal)
    vkutil::transition_image(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // make a clear-color from frame number. this will flash with a 120 frame period.
    VkClearColorValue clearValue;
    float flash = std::abs(std::sin(_frameNumber / 120.f));
    clearValue = { {0.0f, 0.0f, flash, 1.0f} };

    // allows us to target part of image with barrier
    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    // clear image
    // ssubresource range for which part of image to clear
    vkCmdClearColorImage(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange); 

    // make swapchain image into presentable mode. last param is only img layout swapchain allows for present to screen
    // // Q: what is present again?
    vkutil::transition_image(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // finalize the command buffer (can no longer add commands, but it can now be executed!)
    VK_CHECK(vkEndCommandBuffer(cmdBuffer));


    /// connect sync structures with swapchain 
    // prepare the submission to the queue.
    // we want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
    // we will signal the _renderSemaphore, to signal that rendering has finished
    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmdBuffer);

    // ensure commands executed here wont begin until swapchain is rdy
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
    // sync with presenting image on the screen
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, get_current_frame()._renderSemaphore);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);

    // submit command buffer to the queue and execute it!
    // _renderFence will now block until the graphics commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    // last thing we need on the frame is to present image we have just draw into the scree!
    // prepare present
    // this will put the image we just rendered into the visible window
    // we want to wait on the _renderSemaphore for that,
    // as its necessary that drawing commands have finished before the image is displayed
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    // wait on render semaphore and connect to swapchain, this way not presenting image to screen until it
    // has finished the rendering commands right before it
    presentInfo.pWaitSemaphores = &get_current_frame()._renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    // increase the number of frames drawn
    _frameNumber++;



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
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

}

void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);
}

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
    // create syncronization structures
    // one fence to control when GPU has finished rendering the frame,
    // and 2 semaphores to syncronize rendering with swapchain
    // we want fence to start signalled so we can wait on it on the first frame
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT); // flag to wwit on freshly created fence without errors (for first frame)
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._renderSemaphore));

    }

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

