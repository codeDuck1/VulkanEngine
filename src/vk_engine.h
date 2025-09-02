// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>

struct FrameData
{
	VkCommandPool _commandPool; // allocate cmd buffer with
	VkCommandBuffer _mainCommandBuffer; // record cmds into
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

	struct SDL_Window* _window{ nullptr };

	//  used for swap chain
	VkSwapchainKHR _swapchain; //swap chain itself
	VkFormat _swapchainImageFormat; // format that the swapchain images use

	std::vector<VkImage> _swapchainImages; // actual image obj to use as texture or render into
	std::vector<VkImageView> _swapChainImageViews;// wrapper for vkImage, allows for ex. swap color
	VkExtent2D _swapchainExtent;

	// setting up vulkan cmds
	FrameData _frames[FRAME_OVERLAP];
	// flips between our 2 framedata structs
	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; };

	VkQueue _graphicsQueue;
	uint32_t _graphicsQueueFamily;

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

private:
	void init_vulkan();
	void init_swapchain();
	// gives us a way to send commands to the gpu!
	void init_commands();
	void init_sync_structures();

	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();

};
