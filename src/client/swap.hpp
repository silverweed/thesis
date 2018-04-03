#pragma once

#include <vector>
#include <vulkan/vulkan.h>

struct Application;

struct SwapChain final {
	VkSwapchainKHR handle;
	VkExtent2D extent;
	VkFormat imageFormat;
	std::vector<VkImage> images;
	std::vector<VkImageView> imageViews;
	std::vector<VkFramebuffer> framebuffers;
};

SwapChain createSwapChain(const Application& app);
void createSwapChainImageViews(Application& app);
void createSwapChainFramebuffers(Application& app);