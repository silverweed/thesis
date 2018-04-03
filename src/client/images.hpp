#pragma once

#include <vulkan/vulkan.h>

struct Application;

struct Image final {
	VkImage handle;
	VkDeviceMemory memory;
	VkImageView view;
	VkSampler sampler;
};

VkImageView createImageView(const Application& app, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
Image createImage(
		const Application& app,
		uint32_t width,
		uint32_t height,
		VkFormat format,
		VkImageTiling tiling,
		VkImageUsageFlags usage,
		VkMemoryPropertyFlags properties);

void transitionImageLayout(const Application& app,
		VkImage image, VkFormat format,
		VkImageLayout oldLayout, VkImageLayout newLayout);