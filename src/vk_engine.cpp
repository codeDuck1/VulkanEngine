//> includes
#include "vk_engine.h"

// define into only one .cpp file to store and compile the definitions
// for VMA functions
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#include "vk_images.h";
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>
#include <VkBootstrap.h>
#include <vk_pipelines.h>
#include <glm/gtx/transform.hpp>

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

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

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
    init_descriptors();
    init_pipelines();
    init_imgui();
    init_default_data();

    // everything went fine
    _isInitialized = true;

    // camera init vals
    mainCamera.velocity = glm::vec3(0.f);
    mainCamera.position = glm::vec3(0, 0, 5);
    mainCamera.pitch = 0;
    mainCamera.yaw = 0;
    _lastTime = SDL_GetTicks64();  
    _deltaTime = 0.0f;        
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
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);
        }

        // Clean up submit semaphores
        for (size_t i = 0; i < _submitSemaphores.size(); i++) {
            vkDestroySemaphore(_device, _submitSemaphores[i], nullptr);
        }
        _submitSemaphores.clear(); 

        // cleanup meshes
        for (auto& mesh : testMeshes) {
            destroy_buffer(mesh->meshBuffers.indexBuffer);
            destroy_buffer(mesh->meshBuffers.vertexBuffer);
        }

        // flush global deletion queue 
        _mainDeletionQueue.flush();



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
    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence)); // fences must be reset between uses

    // delete objects marked for deletion in previous frames
    get_current_frame()._deletionQueue.flush();


    // request image index from the swapchain. if doesnt have any image we can use, block thread for 1 second. after 1 second return vk timeout
    // we use index given from following funct to decide which swapchain images to use for drawing
    uint32_t swapchainImageIndex;
    // stop rendering if resize on window requested
    VkResult e = vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex);
    if (e == VK_ERROR_OUT_OF_DATE_KHR)
    {
        resize_requested = true;
        return;
    }

    // copy command buffer handle from framedata (to shorten)
    // vulkan handles only a 64 bit handle/pointer, fine to copy around

    VkCommandBuffer cmdBuffer = get_current_frame()._mainCommandBuffer; 

    // now that we are sure that the commands finished executing, can safely 
    // reset the command buffer to begin recording again
    VK_CHECK(vkResetCommandBuffer(cmdBuffer, 0));

    // begin the command buffer recording. we will use this command buffer ONLY once (before resetting) again, so want to let vulkan know that (maybe gives small speedup)
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    // render scale mult is for dynamic resolution
    // without it, we still support making window larger with taking min
    _drawExtent.height = std::min(_swapchainExtent.height, _drawImage.imageExtent.height) * renderScale;
    _drawExtent.width = std::min(_swapchainExtent.width, _drawImage.imageExtent.width) * renderScale;

    // start command buffer recording
    VK_CHECK(vkBeginCommandBuffer(cmdBuffer, &cmdBeginInfo));

    // transition our main draw image into general layout so we can write into it
    // we will overwrite it all so we dont care about what was the older layout
    // undefined bc we dont care abt data already in image, fine with gpu destroying. general allows read/write (not most optimal)
    vkutil::transition_image(cmdBuffer, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    draw_background(cmdBuffer);

    // when doing geometry rendering, we need to use color_attachment optimal
    vkutil::transition_image(cmdBuffer, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmdBuffer, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
  
    draw_geometry(cmdBuffer);

    // transition the draw image and the swapchain image into their correct transfer layouts
    // We are copying FROM our draw image INTO our swapchain image
    vkutil::transition_image(cmdBuffer, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // copying FROM draw image
    // undefined bc dont know what layout swapchain image is in when obtained from vkacquire
    vkutil::transition_image(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); // copying TO swapchain image

    // execute a copy from the draw image into the swapchain
     vkutil::copy_image_to_image(cmdBuffer, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

    // set swapchain image layout to Attachment Optimal so we can draw it
    vkutil::transition_image(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // draw imgui into the swapchain image
    draw_imgui(cmdBuffer, _swapChainImageViews[swapchainImageIndex]);

    // set swapchain image layout to Present so we can show it on the screen. last param is only img layout swapchain allows for present to screen
    vkutil::transition_image(cmdBuffer, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    //finalize the command buffer (we can no longer add commands, but it can now be executed)
    VK_CHECK(vkEndCommandBuffer(cmdBuffer));


    /// connect sync structures with swapchain 
    // prepare the submission to the queue.
    // we want to wait on the _presentSemaphore, as that semaphore is signaled when the swapchain is ready
    // we will signal the _submitSemaphores, to signal that rendering has finished
    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmdBuffer);

    // ensure commands executed here wont begin until swapchain is rdy (per-frame)
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
    // sync with presenting image on the screen (per-image)
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, _submitSemaphores[swapchainImageIndex]);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);

    // submit command buffer to the queue and execute it!
    // _renderFence will now block until the graphics commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));

    // last thing we need on the frame is to present image we have just draw into the scree!
    // prepare present
    // this will put the image we just rendered into the visible window
    // we want to wait on the _submitSemaphores
    // for that,
    // as its necessary that drawing commands have finished before the image is displayed
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    // wait on submit semaphore and connect to swapchain, this way not presenting image to screen until it
    // has finished the rendering commands right before it
    presentInfo.pWaitSemaphores = &_submitSemaphores[swapchainImageIndex];
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        resize_requested = true;
    }

    // increase the number of frames drawn
    _frameNumber++;

}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr); // no depth attachment right now

    vkCmdBeginRendering(cmd, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); // make imgui record its draw commands into the buffer

    vkCmdEndRendering(cmd); // end render pass
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
    // make a clear-color from frame number. this will flash with a 120 frame period.
    VkClearColorValue clearValue;
    //float flash = std::abs(std::sin(_frameNumber / 120.f));
    //clearValue = { {0.0f, 0.0f, flash, 1.0f} };

    // allows us to target part of image with barrier
    //VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    //// clear image
    //// ssubresource range for which part of image to clear
    //vkCmdClearColorImage(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);

    // use currently selected compute shader
    ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];
    // computer shader invocation
    // bind the background compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

    // bind the descriptor set containing the draw image so the shader can access it
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

    // push constants, allow for small amt of data to be sent to gpu on a fast path
    vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);


    // execute the compute pipeline dispatch. we are using 16x16 workgroup size so we need to divide by it
    //ex. if 1920x1080 pixels then get 8,160 (120x68) (width x height) workgroups each working on a 256(16x16) tile of pixels/threads
    // total shader invocations (one thread executing shader code once) = 120x68x256 = 2,088,960. some extra threads that typically return early,
    //  bc processing pixels that dont exist
    // IN THIS CASE WE HAVE 1 THREAD PER PIXEL BUT THIS IS NOT ALWAYS THE CASE!
    vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0), std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd)
{
    //begin a render pass connected to our draw image
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);

    // use mesh pipeline!
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshPipeline);

    // set dynamic viewport and scissor since we didnt hard code when creating pipeline
    // dynamic pipeline state
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = _drawExtent.width;
    viewport.height = _drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // scissor clips rendering to specific rect region on screen
    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = _drawExtent.width;
    scissor.extent.height = _drawExtent.height;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    GPUDrawPushConstants push_constants;

    mainCamera.update(_deltaTime);
    // draw monkey
    glm::mat4 view = mainCamera.getViewMatrix();
    // camera projection
    glm::mat4 projection = glm::perspective(glm::radians(70.f), (float)_drawExtent.width / (float)_drawExtent.height, 10000.f, 0.1f);

    // invert the Y direction on projection matrix so that we are more similar
    // to opengl and gltf axis
    projection[1][1] *= -1;
    push_constants.worldMatrix = projection * view; // model matrix is implicit as identity
    push_constants.vertexBuffer = testMeshes[2]->meshBuffers.vertexBufferAddress; // access this buffer memory on gpu via address
    vkCmdPushConstants(cmd, _meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &push_constants);
    vkCmdBindIndexBuffer(cmd, testMeshes[2]->meshBuffers.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, testMeshes[2]->surfaces[0].count, 1, testMeshes[2]->surfaces[0].startIndex, 0, 0);

    vkCmdEndRendering(cmd);

}

void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        updateDeltaTime();
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

            mainCamera.processSDLEvent(e);
            // send SDL event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        // do not draw if we are minimized
        if (stop_rendering)
        {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // check if resize was requested in draw loop
        if (resize_requested)
        {
            resize_swapchain();
        }

        // imgui new frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("background")) {

            ImGui::SliderFloat("Render Scale", &renderScale, 0.3f, 1.f);

            ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];
            ImGui::Text("Selected effect: ", selected.name);

            // for edits on compute shaders
            // choose effect
            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

            ImGui::InputFloat4("data1", (float*)&selected.data.data1);
            ImGui::InputFloat4("data2", (float*)&selected.data.data2);
            ImGui::InputFloat4("data3", (float*)&selected.data.data3);
            ImGui::InputFloat4("data4", (float*)&selected.data.data4);
        }

        ImGui::End();
        //make imgui calculate internal draw structures
        // vertices, draws, etc, but no drawing on its own
        ImGui::Render();

        draw();
    }
}

void VulkanEngine::updateDeltaTime()
{
    uint32_t currentTime = SDL_GetTicks64();
    _deltaTime = (currentTime - _lastTime) / 1000.0f; // Convert to seconds
    _lastTime = currentTime;
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

    // initialize the memory allocator using vma lib
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT; // lets us use gpu ptrs later when need them
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    _mainDeletionQueue.push_function([&]()
        {
            vmaDestroyAllocator(_allocator);
        });
}

void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);

    // draw image szie will match the window
    VkExtent3D drawImageExtent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    // hardcoding the draw format to 16 bit float (different from integer format like 24 bit color)
    // 64 bits per pixel. 2x data of 32 bit rgba
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;
    // in vulkan, all images and buffers must fill a usageflags with what they will be used for
    // so drivers can do optimizations
    VkImageUsageFlags drawImageUsages{};
    // copy from and into the image
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; 
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // computer shader writeable layout
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    // use graphics pipelines to draw gemoetry into image
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    //for the draw image, we want to allocate it from gpu local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    // flags to setup:
    // gpu texture that wont ever be accessed from cpu, letting us put into gpu vram. 
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY; // allocate image on vram, but outside of upload heap
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); // a flag only gpu vram has, guarantees fastest access

    // allocate and create the image
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

    //build a image-view for the draw image to use for rendering
    // in vulkan need image view to accesss images. generally thin wrapper
    // over img that lets do thing like limit access to only 1 mipmap
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));


    // init depth image
    _depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
    _depthImage.imageExtent = drawImageExtent;
    VkImageUsageFlags depthImageUsages{};
    depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    VkImageCreateInfo dimg_info = vkinit::image_create_info(_depthImage.imageFormat, depthImageUsages, drawImageExtent);

    //allocate and create the image
    vmaCreateImage(_allocator, &dimg_info, &rimg_allocinfo, &_depthImage.image, &_depthImage.allocation, nullptr);

    //build a image-view for the draw image to use for rendering
    VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(_depthImage.imageFormat, _depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

    VK_CHECK(vkCreateImageView(_device, &dview_info, nullptr, &_depthImage.imageView));

    //add to deletion queues
    _mainDeletionQueue.push_function([=]() {
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);

        vkDestroyImageView(_device, _depthImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
        });


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

    // sync structures for immediate submit
    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immCommandPool));

    // allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));
       
    // lamba capture = means copy functions and ALL variables in scope above
    _mainDeletionQueue.push_function([=]() {
        vkDestroyCommandPool(_device, _immCommandPool, nullptr);
        });
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
    }


    // for immediate submit
    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immFence));
    _mainDeletionQueue.push_function([=]() { vkDestroyFence(_device, _immFence, nullptr); });

}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };

    // 32 bit per pixel
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

    // Create one submit semaphore per swapchain image to avoid reusing one while still in use
    _submitSemaphores.resize(_swapchainImages.size());
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    for (size_t i = 0; i < _swapchainImages.size(); i++) {
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_submitSemaphores[i]));
    }
}

void VulkanEngine::destroy_swapchain()
{
    // destroy submit semaphores that are created in create swapchain,
    // since new swapchain created when resizing
    for (size_t i = 0; i < _submitSemaphores.size(); i++) {
        vkDestroySemaphore(_device, _submitSemaphores[i], nullptr);
    }
    _submitSemaphores.clear();

    // deletes images it holds as well as they are owned by swapchain
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    
    // destroy swapchain resources, image views for images
    for (int i = 0; i < _swapChainImageViews.size(); i++)
    {
        vkDestroyImageView(_device, _swapChainImageViews[i], nullptr);
    }
}

// for window resizing
void VulkanEngine::resize_swapchain()
{
    vkDeviceWaitIdle(_device); // wait until gpu has finished all rendering commands

    // destroy swapchain, query window size from SDL then create swapchain
    // with new size
    destroy_swapchain();

    int w, h;
    SDL_GetWindowSize(_window, &w, &h);
    _windowExtent.width = w;
    _windowExtent.height = h;

    create_swapchain(_windowExtent.width, _windowExtent.height);

    resize_requested = false;
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    // allocate buffer
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo vmaallocInfo = {};
    vmaallocInfo.usage = memoryUsage; // so we can control where buffer memory is
    vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT; // map ptr so we can write to memory if buffer accessible from cpu
    AllocatedBuffer newBuffer;

    // allocate the buffer, setting all the struct's members
    VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation,
        &newBuffer.info));

    return newBuffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
    vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

// span is a pointer view of array type, doesnt own it.
// avoids copies
GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface;

    //create vertex buffer
    newSurface.vertexBuffer = create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    //find the adress of the vertex buffer
    VkBufferDeviceAddressInfo deviceAdressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer = newSurface.vertexBuffer.buffer };
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAdressInfo);

    //create index buffer
    newSurface.indexBuffer = create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    
    // _____________ common patter witn vulkan, first write memory on temp staging buffer that is cpu writeable
    // then execute copy into GPU buffers that cant be written on CPU_______________________________________
    // staging buffer for both copies to index and vertex buffers
    AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY); // transfer usage flag to copy

    void* data = staging.allocation->GetMappedData();

    // copy vertex buffer from span TO staging buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // copy index buffer from span TO staging buffer
    memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

    // gpu command to perform copy from staging buffer into GPU buffers
    // not very efficient, since waiting on gpu cmds to execute before continuing with cpu work.
    // most ppl put on a background thread, sole jon to execute uploads
    immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size = vertexBufferSize;

        // COPYING STAGING BUFFER INTO VERTEX BUFFER ON GPU
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize; // same offset as with memcpy!
        indexCopy.size = indexBufferSize;

        // COPYING STAGING BUFFER INTO INDEX BUFFER ON GPU
        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
        });

    destroy_buffer(staging);

    return newSurface;
}

void VulkanEngine::init_descriptors()
{
    // create a descriptor pool that will hold 10 sets with 1 image each
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
    {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}
    };

    globalDescriptorAllocator.init_pool(_device, 10, sizes);

    // make the descriptor set layout for our compute draw
    // end of braces builder out of scope and destroyed
    {
        DescriptorLayoutBuilder builder;
        // 1 single binding at binding number 0
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    //allocate a descriptor set for our draw image
    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    // the actual image data we want to bind
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = _drawImage.imageView;

    // the individual updates to pass to vkUpdateDescriptorSets,
    // in order to update descriptor set with our draw image
    VkWriteDescriptorSet drawImageWrite = {};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;

    drawImageWrite.dstBinding = 0;
    drawImageWrite.dstSet = _drawImageDescriptors;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);

    //make sure both the descriptor allocator and the new layout get cleaned up properly
    _mainDeletionQueue.push_function([&]() {
        globalDescriptorAllocator.destroy_pool(_device);

        vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
        });
}

void VulkanEngine::init_pipelines()
{
    // COMPUTE PIPELINES
    init_background_pipelines();
    init_mesh_pipeline();
}

void VulkanEngine::init_background_pipelines()
{
    // create pipeline layout
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;
    
    // push constants
    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    // create pipeline object itself, loading shader module and adding it to pipelineCreateInfo
    VkShaderModule gradientShader;
    if (!vkutil::load_shader_module("../../shaders/gradient_color.comp.spv", _device, &gradientShader))
    {
        fmt::print("Error when building the compute shader \n");
    }

    // second shader
    VkShaderModule skyShader;
    if (!vkutil::load_shader_module("../../shaders/sky.comp.spv", _device, &skyShader)) {
        fmt::print("Error when building the compute shader \n");
    }

    VkPipelineShaderStageCreateInfo stageinfo{};
    stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageinfo.pNext = nullptr;
    stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageinfo.module = gradientShader;
    stageinfo.pName = "main"; // name of the function we want the shader to use

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageinfo;

    ComputeEffect gradient;
    gradient.layout = _gradientPipelineLayout;
    gradient.name = "gradient";
    gradient.data = {};

    // default colors
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);

    // create pipeline and store in compute effects
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    // change the shader module on the createinfo only to create the sky shader
    computePipelineCreateInfo.stage.module = skyShader;

    ComputeEffect sky;
    sky.layout = _gradientPipelineLayout;
    sky.name = "sky";
    sky.data = {};
    // default sky params
    sky.data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);

    // create second pipeline and store in compute effects
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    // add the 2 background effects into the array
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    // cleanup
    vkDestroyShaderModule(_device, gradientShader, nullptr); // can destroy directly, after creating pipeline dont need
    vkDestroyShaderModule(_device, skyShader, nullptr);
    // = for capture surrounding vars by copy, & for capture by reference
    // note: even a copy of the handle points to the SAME vulkan object thus destroying via copy works
    _mainDeletionQueue.push_function([=]() { 
        vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
        vkDestroyPipeline(_device, sky.pipeline, nullptr);
        vkDestroyPipeline(_device, gradient.pipeline, nullptr);
        });
}

// largely same as init triangle pipeline
void VulkanEngine::init_mesh_pipeline()
{
    VkShaderModule triangleFragShader;
    if (!vkutil::load_shader_module("../../shaders/color_triangle.frag.spv", _device, &triangleFragShader)) {
        fmt::print("Error when building the triangle fragment shader module");
    }
    else {
        fmt::print("Triangle fragment shader succesfully loaded");
    }

    VkShaderModule triangleVertexShader;
    if (!vkutil::load_shader_module("../../shaders/color_triangle_mesh.vert.spv", _device, &triangleVertexShader)) {
        fmt::print("Error when building the triangle vertex shader module");
    }
    else {
        fmt::print("Triangle vertex shader succesfully loaded");
    }

    VkPushConstantRange bufferRange{};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushConstants); // new for mesh pipeline
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
    pipeline_layout_info.pPushConstantRanges = &bufferRange;
    pipeline_layout_info.pushConstantRangeCount = 1;
    VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr, &_meshPipelineLayout));

    PipelineBuilder pipelineBuilder;
    //use the triangle layout we created
    pipelineBuilder._pipelineLayout = _meshPipelineLayout;
    //connecting the vertex and pixel shaders to the pipeline
    pipelineBuilder.set_shaders(triangleVertexShader, triangleFragShader);
    //it will draw triangles
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    //filled triangles
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    //no backface culling
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    //no multisampling
    pipelineBuilder.set_multisampling_none();
    //no blending
    //pipelineBuilder.disable_blending();
    pipelineBuilder.enable_blending_additive();


    //pipelineBuilder.disable_depthtest();
    pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

    //connect the image format we will draw into, from draw image and depth image
    pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(_depthImage.imageFormat);

    //finally build the pipeline
    _meshPipeline = pipelineBuilder.build_pipeline(_device);

    //clean structures
    vkDestroyShaderModule(_device, triangleFragShader, nullptr);
    vkDestroyShaderModule(_device, triangleVertexShader, nullptr);

    _mainDeletionQueue.push_function([&]() {
        vkDestroyPipelineLayout(_device, _meshPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _meshPipeline, nullptr);
        });
}

void VulkanEngine::init_default_data()
{
    // load some meshes
    testMeshes = loadGltfMeshes(this, "..\\..\\assets\\basicmesh.glb").value();

}

    void VulkanEngine::init_imgui()
    {
         // 1: create descriptor pool for IMGUI
	    //  the size of the pool is very oversize, but it's copied from imgui demo
	    //  itself.
	    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		    { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		    { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		    { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	    VkDescriptorPoolCreateInfo pool_info = {};
	    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	    pool_info.maxSets = 1000;
	    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	    pool_info.pPoolSizes = pool_sizes;

	    VkDescriptorPool imguiPool;
	    VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

	    // 2: initialize imgui library

	    // this initializes the core structures of imgui
	    ImGui::CreateContext();

	    // this initializes imgui for SDL
	    ImGui_ImplSDL2_InitForVulkan(_window);

	    // this initializes imgui for Vulkan
	    ImGui_ImplVulkan_InitInfo init_info = {};
	    init_info.Instance = _instance;
	    init_info.PhysicalDevice = _chosenGPU;
	    init_info.Device = _device;
	    init_info.Queue = _graphicsQueue;
	    init_info.DescriptorPool = imguiPool;
	    init_info.MinImageCount = 3;
	    init_info.ImageCount = 3;
	    init_info.UseDynamicRendering = true;

	    //dynamic rendering parameters for imgui to use
	    init_info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;
	

	    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	    ImGui_ImplVulkan_Init(&init_info);

        // uses imgui's own internal immediate submit command
	    ImGui_ImplVulkan_CreateFontsTexture();

	    // add destroy for the imgui created structures
	    _mainDeletionQueue.push_function([=]() {
		    ImGui_ImplVulkan_Shutdown();
		    vkDestroyDescriptorPool(_device, imguiPool, nullptr);
	    });
    }

    void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
    {
        VK_CHECK(vkResetFences(_device, 1, &_immFence));
        VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));

        VkCommandBuffer cmd = _immCommandBuffer;

        VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

        VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

        // interesting C++ feature didnt know about
        function(cmd);

        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
        VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

        // submit command buffer to the queue and execute it.
        //  _renderFence will now block until the graphic commands finish execution
        VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));

        VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
    }