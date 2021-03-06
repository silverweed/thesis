#pragma once

#include <array>
#include <vector>
#include <vulkan/vulkan.h>

VkFormat findSupportedFormat(VkPhysicalDevice physicalDevice,
	const std::vector<VkFormat>& candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

void findBestFormats(VkPhysicalDevice device);

namespace formats {
extern VkFormat depth;
extern VkFormat position;
extern VkFormat normal;
extern VkFormat albedoSpec;
}   // namespace formats

constexpr bool hasStencilComponent(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

VkVertexInputBindingDescription getVertexBindingDescription();
std::array<VkVertexInputAttributeDescription, 5> getVertexAttributeDescriptions();
