#include "application.hpp"
#include "client_resources.hpp"
#include "commands.hpp"
#include "formats.hpp"
#include "logging.hpp"
#include "phys_device.hpp"
#include "vulk_errors.hpp"
#include "window.hpp"
#include <array>
#include <set>

using namespace logging;

static VkInstance createInstance(const Validation& validation)
{
	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Hello Triangle";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "No Engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	auto extensions = getRequiredExtensions(validation.enabled());
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();
	if (validation.enabled())
		validation.enableOn(createInfo);

	VkInstance instance;
	VLKCHECK(vkCreateInstance(&createInfo, nullptr, &instance));

	return instance;
}

static VkSurfaceKHR createSurface(VkInstance instance, GLFWwindow* window)
{
	VkSurfaceKHR surface;
	VLKCHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));

	return surface;
}

static void createLogicalDevice(Application& app)
{
	QueueFamilyIndices indices = findQueueFamilies(app.physicalDevice, app.surface);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<int> uniqueQueueFamilies = { indices.graphicsFamily, indices.presentFamily };

	float queuePriority = 1.0f;
	for (int queueFamily : uniqueQueueFamilies) {
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures = {};
	deviceFeatures.samplerAnisotropy = VK_TRUE;

	VkDeviceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &deviceFeatures;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(gDeviceExtensions.size());
	createInfo.ppEnabledExtensionNames = gDeviceExtensions.data();
	if (app.validation.enabled())
		app.validation.enableOn(createInfo);

	VLKCHECK(vkCreateDevice(app.physicalDevice, &createInfo, nullptr, &app.device));

	vkGetDeviceQueue(app.device, indices.graphicsFamily, 0, &app.queues.graphics);
	vkGetDeviceQueue(app.device, indices.presentFamily, 0, &app.queues.present);
}

VkDescriptorPool createDescriptorPool(const Application& app)
{
	constexpr auto MAX_MATERIALS_EXPECTED = 64;
	constexpr auto MAX_MODELS_EXPECTED = 24;

	// TODO: re-read and reason about this procedure, it's kinda messy

	/* We have 4 kinds of DescriptorSets, each updated at a different frequency:
	 * #0: view resources (CompUbo)
	 * #1: gbuffer resources (G-pos, G-norm, G-albedoSpec)
	 * #2: material resources (texDiffuse, texSpecular, texNormal)
	 * #3: object resources (MVPUbo)
	 * @see https://developer.nvidia.com/vulkan-shader-resource-binding
	 */
	std::array<VkDescriptorPoolSize, 4> poolSizes = {};

	// 2 uniform buffers for the view
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = 2;

	// at most 3 textures per material
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = 3 * (MAX_MATERIALS_EXPECTED + 1);   // diff/spec/normal + default ones

	// 1 input attachment per G-buffer attachment
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	poolSizes[2].descriptorCount = 3;

	// 1 dynamic uniform buffer per model
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	poolSizes[3].descriptorCount = std::max(1, MAX_MODELS_EXPECTED);

	debug("Created descriptorPool with sizes ",
		poolSizes[0].descriptorCount,
		", ",
		poolSizes[1].descriptorCount,
		", ",
		poolSizes[2].descriptorCount,
		", ",
		poolSizes[3].descriptorCount);

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = poolSizes.size();
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = 2 + std::max(1, MAX_MODELS_EXPECTED) + std::max(1, MAX_MATERIALS_EXPECTED);
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

	VkDescriptorPool descriptorPool;
	VLKCHECK(vkCreateDescriptorPool(app.device, &poolInfo, nullptr, &descriptorPool));
	app.validation.addObjectInfo(descriptorPool, __FILE__, __LINE__);

	return descriptorPool;
}

void Application::init()
{
#ifndef NDEBUG
	const std::vector<const char*> enabledLayers = { "VK_LAYER_LUNARG_standard_validation" };
	validation.requestLayers(enabledLayers);
#endif
	window = initWindow();
	monitor = glfwGetPrimaryMonitor();
	instance = createInstance(validation);

	uint32_t version;
	vkEnumerateInstanceVersion(&version);
	info("Vulkan: using version ",
		VK_VERSION_MAJOR(version),
		".",
		VK_VERSION_MINOR(version),
		".",
		VK_VERSION_PATCH(version));

	validation.init(instance);

	surface = createSurface(instance, window);
	physicalDevice = pickPhysicalDevice(instance, surface);
	findBestFormats(physicalDevice);
	createLogicalDevice(*this);

	commandPool = createCommandPool(*this);
}

void Application::cleanup()
{
	vkDestroyCommandPool(device, commandPool, nullptr);
	vkDestroyDescriptorPool(device, descriptorPool, nullptr);

	validation.cleanup();

	vkDestroyDevice(device, nullptr);
	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyInstance(instance, nullptr);

	cleanupWindow(window);
}
