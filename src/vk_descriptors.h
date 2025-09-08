#pragma once

#include <vk_types.h>

struct DescriptorLayoutBuilder
{
	// config info struct
	std::vector<VkDescriptorSetLayoutBinding> bindings;

	void add_binding(uint32_t binding, VkDescriptorType type);
	void clear();

	// vulkan obj, not config info struct
	// contains the info abt what descriptor set holds
	VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);

};

struct DescriptorAllocator {

	struct PoolSizeRatio {
		VkDescriptorType type;
		float ratio;
	};

	// memory allocator for specific descriptors
	// when reset pool, all descriptor sets allocated from it destroyed
	VkDescriptorPool pool;

	void init_pool(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios);
	// destroy all descriptors created from pool,and put it back to initial state. doesnt delete itself
	void clear_descriptors(VkDevice device);
	void destroy_pool(VkDevice device);

	// pack of pointers into resources like buffer or image
	VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout);
};