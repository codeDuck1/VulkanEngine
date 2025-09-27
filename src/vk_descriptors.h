#pragma once

#include <vk_types.h>

// DESCRIPTOR SET LAYOUT, ALLOC, UPDATE EXPLAINED:
// dset layout says: i have a descriptor set with binding 0 that expects a storage image
// dset alloc says: give me an actual descriptor set instance that follows that layout
// dset update/writer says: for binding 0 in this descriptor set, point to this specific image in gpu memory

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

struct DescriptorAllocatorGrowable {
public:
	struct PoolSizeRatio {
		VkDescriptorType type;
		float ratio;
	};

	void init(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios);
	void clear_pools(VkDevice device);
	void destroy_pools(VkDevice device);

	VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout, void* pNext = nullptr);
private:
	VkDescriptorPool get_pool(VkDevice device);
	VkDescriptorPool create_pool(VkDevice device, uint32_t setCount, std::span<PoolSizeRatio> poolRatios);

	std::vector<PoolSizeRatio> ratios;
	std::vector<VkDescriptorPool> fullPools;
	std::vector<VkDescriptorPool> readyPools;
	uint32_t setsPerPool;

};

struct DescriptorWriter {

	// deque is guaranteed to keep ptrs to elements valid
	// compared to queue, can add/remove from front AND back

	// need to keep pointers to the infos bc writes stores them in their struct
	std::deque<VkDescriptorImageInfo> imageInfos; 
	std::deque<VkDescriptorBufferInfo> bufferInfos;
	std::vector<VkWriteDescriptorSet> writes;

	// used to bind the data
	void write_image(int binding, VkImageView image, VkSampler sampler, VkImageLayout layout, VkDescriptorType type);
	void write_buffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type);

	void clear();
	void update_set(VkDevice device, VkDescriptorSet set);
};