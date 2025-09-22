// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_loader.h>
#include <camera.h>


struct FrameData
{
	VkCommandPool _commandPool; // allocate cmd buffer with
	VkCommandBuffer _mainCommandBuffer; // record cmds into

	// syncronization structures
	VkSemaphore _swapchainSemaphore; // render cmds wait on swapchain image request (gpu to gpu)
	VkFence _renderFence; // wait for draw cmds of a given frame to be finished (cpu to gpu)

	// delete objects next frame after used
	DeletionQueue _deletionQueue;
};

struct ComputePushConstants
{
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};


// for use with imgui, to switch between different compute shaders
struct ComputeEffect
{
	const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class VulkanEngine {
public:

	VkInstance _instance; // vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // GPU chosen as the default device
	VkDevice _device; // vulkan device for commands (actual gpu driver)
	VkSurfaceKHR _surface; // vulkan window surface (rendering target?)


	bool _isInitialized{ false };
	int _frameNumber {0};
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };
	bool resize_requested{ false };

	struct SDL_Window* _window{ nullptr };

	//  used for swap chain
	VkSwapchainKHR _swapchain; //swap chain itself
	VkFormat _swapchainImageFormat; // format that the swapchain images use

	std::vector<VkImage> _swapchainImages; // actual image obj to use as texture or render into
	std::vector<VkImageView> _swapChainImageViews;// wrapper for vkImage, allows for ex. swap color
	VkExtent2D _swapchainExtent;

	// for dynamic resolution
	float renderScale = 1.f;

	// control presenting img to OS once drawing finishes, one per swapchain image
	// needs to be done per-image bc only when you acquire same image again do you know
	// previous presentation of that image finished
	// that is, when  present an image, you get nothing that tells you when presentation is done
	// so dont know when to safe to reuse semaphore
	std::vector<VkSemaphore> _submitSemaphores;

	// setting up vulkan cmds
	FrameData _frames[FRAME_OVERLAP];
	// flips between our 2 framedata structs
	// even nums always 0, odd always 1 after mod 2
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; };

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	// for global objects
	DeletionQueue _mainDeletionQueue;

	VmaAllocator _allocator;

	// draw resources
	AllocatedImage _drawImage;
	AllocatedImage _depthImage;
	VkExtent2D _drawExtent;

	// descriptors
	DescriptorAllocator globalDescriptorAllocator;

	// descriptor set will bind our render image
	VkDescriptorSet _drawImageDescriptors;
	// descriptor layout for above set, need for creating pipeline
	VkDescriptorSetLayout _drawImageDescriptorLayout;

	// pipelines
	VkPipeline _gradientPipeline;
	VkPipelineLayout _gradientPipelineLayout;
	
	// immediate gpu submit structures
	VkFence _immFence;
	VkCommandBuffer _immCommandBuffer;
	VkCommandPool _immCommandPool;

	// for imgui selection
	std::vector<ComputeEffect> backgroundEffects;
	int currentBackgroundEffect{ 0 };

	VkPipelineLayout _meshPipelineLayout;
	VkPipeline _meshPipeline;

	// meshes!
	std::vector<std::shared_ptr<MeshAsset>> testMeshes;

	Camera mainCamera;
	
	// for data uploads and other instant operations outside of render loop
	// could improve would be to run it on different queue to overlap execution
	void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	// sync, command buffer managaement, and transitions
	void draw();

	// render imgui
	void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);

	// draw commands themselves
	void draw_background(VkCommandBuffer cmd);

	void draw_geometry(VkCommandBuffer cmd);
	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

	//run main loop
	void run();

	
	uint32_t _lastTime{ 0 };
	float _deltaTime{ 0.0f };
	void updateDeltaTime();

private:
	void init_vulkan();
	void init_swapchain();
	// gives us a way to send commands to the gpu!
	void init_commands();
	void init_sync_structures();

	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();
	void resize_swapchain();

	AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);
	

	void init_descriptors();
	void init_pipelines();
	void init_background_pipelines();
	
	void init_imgui();
	void init_mesh_pipeline();

	void init_default_data();
};
