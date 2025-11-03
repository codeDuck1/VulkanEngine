
#pragma once 

namespace vkutil {

	void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);

	// for copying images
	void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);
	
	void generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);


	// used for loaded cubemap, not generated ones
	void generate_cubemap_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);

	void transition_image_mip(VkCommandBuffer cmd, VkImage image, uint32_t mipLevel, VkImageLayout oldLayout, VkImageLayout newLayout);

};