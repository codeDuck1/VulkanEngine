#include <vk_images.h>
#include <vk_initializers.h>

void vkutil::transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout)
{
	VkImageMemoryBarrier2 imageBarrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	imageBarrier.pNext = nullptr;

	// inefficent StageMask, but fine for us as only few transitions per frame. if do many as part of post-proces chain, avoid this and instead use smth more accurate
	// ALLCommands StageMask means that barrier will stop the gpu commands completely when it arrives at the barrier- idk what that means

	// AccessMask controls how barrier stops different parts of the GPU
	// using generic options that will be fine
	imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; 
	imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

	imageBarrier.oldLayout = currentLayout;
	imageBarrier.newLayout = newLayout;

	// color attachment under all cases except when target layout is depth attachment
	VkImageAspectFlags aspectMask = (newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	// lets us target a part of the image with the barrier. useful for array images or mipmapped images-
	// where only need barrier on a given layer or mipmap level.
	// defaulting it to transition all mipmap levels and layers.
	imageBarrier.subresourceRange = vkinit::image_subresource_range(aspectMask);
	imageBarrier.image = image;

	VkDependencyInfo depInfo{};
	depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	depInfo.pNext = nullptr;

	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &imageBarrier;

	vkCmdPipelineBarrier2(cmd, &depInfo);
}

void vkutil::copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize)
{
	// blit image lets copy images of dif formats and sizes into one another
	VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

	blitRegion.srcOffsets[1].x = srcSize.width;
	blitRegion.srcOffsets[1].y = srcSize.height;
	blitRegion.srcOffsets[1].z = 1;

	blitRegion.dstOffsets[1].x = dstSize.width;
	blitRegion.dstOffsets[1].y = dstSize.height;
	blitRegion.dstOffsets[1].z = 1;

	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcSubresource.mipLevel = 0;

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstSubresource.mipLevel = 0;

	VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
	blitInfo.dstImage = destination;
	blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	blitInfo.srcImage = source;
	blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	blitInfo.filter = VK_FILTER_LINEAR;
	blitInfo.regionCount = 1;
	blitInfo.pRegions = &blitRegion;

	vkCmdBlitImage2(cmd, &blitInfo);
}

void vkutil::generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize)
{
	int mipLevels = int(std::floor(std::log2(std::max(imageSize.width, imageSize.height)))) + 1;
	for (int mip = 0; mip < mipLevels; mip++) {

		VkExtent2D halfSize = imageSize;
		halfSize.width /= 2;
		halfSize.height /= 2;

		VkImageMemoryBarrier2 imageBarrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr };

		imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

		imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBarrier.subresourceRange = vkinit::image_subresource_range(aspectMask);
		imageBarrier.subresourceRange.levelCount = 1;
		imageBarrier.subresourceRange.baseMipLevel = mip;
		imageBarrier.image = image;

		VkDependencyInfo depInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr };
		depInfo.imageMemoryBarrierCount = 1;
		depInfo.pImageMemoryBarriers = &imageBarrier;

		// performs layout transition and waits for prev writes to finish
		vkCmdPipelineBarrier2(cmd, &depInfo);

		if (mip < mipLevels - 1) {
			VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

			blitRegion.srcOffsets[1].x = imageSize.width;
			blitRegion.srcOffsets[1].y = imageSize.height;
			blitRegion.srcOffsets[1].z = 1;

			blitRegion.dstOffsets[1].x = halfSize.width;
			blitRegion.dstOffsets[1].y = halfSize.height;
			blitRegion.dstOffsets[1].z = 1;

			// COPY FROM IMAGE AT MIP LEVEL, TO NEXT MIPLEVEL 
			// WITH HALF RES. 
			blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blitRegion.srcSubresource.baseArrayLayer = 0;
			blitRegion.srcSubresource.layerCount = 1;
			blitRegion.srcSubresource.mipLevel = mip;

			blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blitRegion.dstSubresource.baseArrayLayer = 0;
			blitRegion.dstSubresource.layerCount = 1;
			blitRegion.dstSubresource.mipLevel = mip + 1;

			// COPYING FROM AND TO SAME IMAGE (no effect)
			VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
			blitInfo.dstImage = image;
			blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			blitInfo.srcImage = image;
			blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			blitInfo.filter = VK_FILTER_LINEAR;
			blitInfo.regionCount = 1;
			blitInfo.pRegions = &blitRegion;

			// blit is simply copy with resize and filtering 
			vkCmdBlitImage2(cmd, &blitInfo);

			imageSize = halfSize;

			//fmt::print("Mipmap level successfully generated\n");
		}
	}

	// transition all mip levels into the final read_only layout
	transition_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// same as above function, but with cubemap support
void vkutil::generate_cubemap_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize) {
	int mipLevels = int(std::floor(std::log2(std::max(imageSize.width, imageSize.height)))) + 1;

	for (int mip = 0; mip < mipLevels; mip++) {
		VkExtent2D halfSize = imageSize;
		halfSize.width /= 2;
		halfSize.height /= 2;

		// Barrier for ALL 6 faces at once
		VkImageMemoryBarrier2 imageBarrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
		imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageBarrier.subresourceRange.baseMipLevel = mip;
		imageBarrier.subresourceRange.levelCount = 1;
		imageBarrier.subresourceRange.baseArrayLayer = 0;
		imageBarrier.subresourceRange.layerCount = 6;  //  ALL 6 FACES!
		imageBarrier.image = image;

		VkDependencyInfo depInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		depInfo.imageMemoryBarrierCount = 1;
		depInfo.pImageMemoryBarriers = &imageBarrier;
		vkCmdPipelineBarrier2(cmd, &depInfo);

		if (mip < mipLevels - 1) {
			// Blit EACH face separately
			for (int face = 0; face < 6; face++) {
				VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2 };
				blitRegion.srcOffsets[1] = { (int32_t)imageSize.width, (int32_t)imageSize.height, 1 };
				blitRegion.dstOffsets[1] = { (int32_t)halfSize.width, (int32_t)halfSize.height, 1 };

				blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blitRegion.srcSubresource.mipLevel = mip;
				blitRegion.srcSubresource.baseArrayLayer = face;  // Current face
				blitRegion.srcSubresource.layerCount = 1;

				blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blitRegion.dstSubresource.mipLevel = mip + 1; // new mip level!s
				blitRegion.dstSubresource.baseArrayLayer = face;  // Same face
				blitRegion.dstSubresource.layerCount = 1;

				VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2 };
				blitInfo.srcImage = image;
				blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				blitInfo.dstImage = image;
				blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				blitInfo.filter = VK_FILTER_LINEAR;
				blitInfo.regionCount = 1;
				blitInfo.pRegions = &blitRegion;

				vkCmdBlitImage2(cmd, &blitInfo);

				fmt::print("COMPLETED CUBEMAP MIPMAP");
			}
			imageSize = halfSize;
		}
	}

	// Final transition of ALL mip levels to SHADER_READ_ONLY
	VkImageMemoryBarrier2 finalBarrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	finalBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	finalBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	finalBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	finalBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
	finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	finalBarrier.subresourceRange.baseMipLevel = 0;
	finalBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;  // All mips
	finalBarrier.subresourceRange.baseArrayLayer = 0;
	finalBarrier.subresourceRange.layerCount = 6;  // All faces
	finalBarrier.image = image;

	VkDependencyInfo finalDepInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	finalDepInfo.imageMemoryBarrierCount = 1;
	finalDepInfo.pImageMemoryBarriers = &finalBarrier;
	vkCmdPipelineBarrier2(cmd, &finalDepInfo);

}


