#include <array>
#include <unordered_map>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <set>
#include <limits>
#include <memory>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <utility>
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <chrono>
#include "FPSCounter.hpp"
#include "vertex.hpp"
#include "validation.hpp"
#include "vulk_utils.hpp"
#include "config.hpp"
#include "window.hpp"
#include "phys_device.hpp"
#include "commands.hpp"
#include "application.hpp"
#include "client_endpoint.hpp"
#include "camera.hpp"
#include "camera_ctrl.hpp"
#include "clock.hpp"
#include "buffers.hpp"
#include "swap.hpp"
#include "formats.hpp"
#include "vulk_errors.hpp"
#include "images.hpp"
#include "renderpass.hpp"
#include "gbuffer.hpp"
#include "textures.hpp"

// Fuck off, Windows
#undef max
#undef min

using namespace std::literals::string_literals;
using std::size_t;

struct UniformBufferObject final {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 proj;
};

class HelloTriangleApplication final {
public:
	void run() {
		app.init();

		glfwSetWindowUserPointer(app.window, this);
		glfwSetWindowSizeCallback(app.window, onWindowResized);
		glfwSetInputMode(app.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		glfwSetCursorPosCallback(app.window, cursorPosCallback);

		initVulkan();
		mainLoop();
		cleanup();
	}

private:
	Application app;

	ClientPassiveEndpoint passiveEP;
	ClientActiveEndpoint activeEP;
	int64_t curFrame = -1;

	Camera camera;
	std::unique_ptr<CameraController> cameraCtrl;

	VkPipelineLayout pipelineLayout;
	VkPipeline graphicsPipeline;

	VkDescriptorPool descriptorPool;
	VkDescriptorSet descriptorSet;

	std::vector<VkCommandBuffer> commandBuffers;

	VkSemaphore imageAvailableSemaphore;
	VkSemaphore renderFinishedSemaphore;

	Buffer vertexBuffer;
	Buffer indexBuffer;
	Buffer uniformBuffer;

	Image texDiffuseImage;
	Image texSpecularImage;

	/** Pointer to the memory area staging vertices and indices coming from the server */
	uint8_t *streamingBufferData = nullptr;
	uint64_t nVertices = 0;
	uint64_t nIndices = 0;
	static constexpr size_t VERTEX_BUFFER_SIZE = 1<<24;
	static constexpr size_t INDEX_BUFFER_SIZE = 1<<24;
	static constexpr size_t UNIFORM_BUFFER_SIZE = sizeof(UniformBufferObject);


	void initVulkan() {
		app.swapChain = createSwapChain(app);
		createSwapChainImageViews(app);

		// Create the gbuffer for the geometry pass
		const auto gBufAttachments = createGBufferAttachments(app);
		app.geomRenderPass = createGeometryRenderPass(app, gBufAttachments);
		app.gBuffer = createGBuffer(app, gBufAttachments);

		app.gBuffer.descriptorSetLayout = createGBufferDescriptorSetLayout(app);

		std::tie(graphicsPipeline, pipelineLayout) = createGBufferPipeline(app);

		app.commandPool = createCommandPool(app);

		app.depthImage = createDepthImage(app);

		// Create a framebuffer for each image in the swap chain for the presentation
		createSwapChainFramebuffers(app);

		texDiffuseImage = createTextureImage(app, cfg::TEXTURE_PATH, TextureFormat::RGBA);
		texDiffuseImage.sampler = createTextureSampler(app);
		texSpecularImage = createTextureImage(app, cfg::TEXTURE_PATH, TextureFormat::GREY);
		texSpecularImage.sampler = createTextureSampler(app);

		createDescriptorPool();
		createDescriptorSet();
		createCommandBuffers();
		createSemaphores();


		// Prepare buffer memory
		streamingBufferData = new uint8_t[VERTEX_BUFFER_SIZE + INDEX_BUFFER_SIZE];

		vertexBuffer = createBuffer(app, VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		indexBuffer = createBuffer(app, INDEX_BUFFER_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		uniformBuffer = createBuffer(app, UNIFORM_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

		// Prepare camera
		camera = createCamera();
		cameraCtrl = std::make_unique<CameraController>(camera);
		activeEP.setCamera(&camera);
	}

	void mainLoop() {
		passiveEP.startPassive(cfg::CLIENT_PASSIVE_IP, cfg::CLIENT_PASSIVE_PORT);
		passiveEP.runLoop();

		activeEP.startActive(cfg::CLIENT_ACTIVE_IP, cfg::CLIENT_ACTIVE_PORT);
		activeEP.runLoop();

		FPSCounter fps;
		fps.start();

		updateVertexBuffer();
		updateIndexBuffer();
		updateUniformBuffer();

		auto& clock = Clock::instance();
		auto beginTime = std::chrono::high_resolution_clock::now();

		while (!glfwWindowShouldClose(app.window)) {
			glfwPollEvents();

			runFrame();

			calcTimeStats(fps, beginTime);
		}

		passiveEP.close();
		vkDeviceWaitIdle(app.device);
	}

	void runFrame() {
		static size_t pvs = nVertices,
		              pis = nIndices;

		// Receive network data
		receiveData();

		if (nVertices != pvs || nIndices != pis) {
			pvs = nVertices;
			pis = nIndices;
			vkDeviceWaitIdle(app.device);
			vkFreeCommandBuffers(app.device, app.commandPool, static_cast<uint32_t>(commandBuffers.size()),
				commandBuffers.data());
			createCommandBuffers();
		}

		updateVertexBuffer();
		updateIndexBuffer();
		updateUniformBuffer();

		cameraCtrl->processInput(app.window);

		drawFrame();
	}

	// TODO
	void receiveData() {
		//std::cerr << "receive data. curFrame = " << curFrame << ", passive.get = " << passiveEP.getFrameId() << "\n";
		if (curFrame >= 0 && passiveEP.getFrameId() == curFrame)
			return;

		const auto data = passiveEP.peek();
		//std::cerr << "data = " << data << "\n";
		if (data == nullptr)
			return;

		curFrame = passiveEP.getFrameId();

		// data is [(64b)nVertices|(64b)nIndices|vertices|indices]
		nVertices = *reinterpret_cast<const uint64_t*>(data);
		nIndices = *(reinterpret_cast<const uint64_t*>(data) + 1);
		std::cerr << "\nn vertices: " << nVertices << ", n indices: " << nIndices << "\n";
		//for (size_t i = 0; i < nVertices; ++i)
			//std::cerr << "v[" << i << "] = "
				//<< *((Vertex*)(data + 20 + sizeof(Vertex)*i)) << std::endl;

		//vertices.resize(nVertices);
		//const auto vOff = 2 * sizeof(uint64_t);
		//for (unsigned i = 0; i < nVertices; ++i)
			//vertices[i] = *(Vertex*)(data + vOff + i * sizeof(Vertex));
		//std::cerr << "begin vertices\n";
		//[>for (auto& v : vertices) {
			//std::cerr << v << std::endl;
			////v.pos.x += 0.001;
			////if (v.pos.x > 1) v.pos.x = 0;
		//}*/
		//std::cerr << "end vertices (" << vertices.size() << ")\n";

		//indices.resize(nIndices);
		////memcpy(indices.data(), data + 28 + nVertices * sizeof(Vertex), nIndices * sizeof(Index));
		//const auto iOff = vOff + nVertices * sizeof(Vertex);
		//std::cerr << "iOff = " << iOff << "\n";
		//for (unsigned i = 0; i < nIndices; ++i)
			//indices[i] = *(Index*)(data + iOff + i * sizeof(Index));

		//std::cerr << "begin indices\n";
		//[>for (auto& i : indices) {
			//std::cerr << i << ", ";
		//}*/
		//std::cerr << "\nend indices (" << indices.size() << ")\n";

		//printf("[%ld] raw data:\n", curFrame);
		//for (int i = 0; i < 150; ++i) {
			//if (i == vOff) printf("|> ");
			//if (i == iOff) printf("<| ");
			//printf("%hhx ", data[i]);
		//}
		//printf("\n");


		constexpr auto HEADER_SIZE = 2 * sizeof(uint64_t);
		memcpy(streamingBufferData, data + HEADER_SIZE, nVertices * sizeof(Vertex));
		memcpy(streamingBufferData + VERTEX_BUFFER_SIZE, data + HEADER_SIZE + nVertices * sizeof(Vertex),
				nIndices * sizeof(Index));

		//if (curFrame == 1) {
			//std::ofstream of("sb.data", std::ios::binary);
			//for (int i = 0; i < vertices.size() * sizeof(Vertex) + indices.size() * sizeof(Index); ++i)
				//of << ((uint8_t*)streamingBufferData)[i];
		//}
	}

	void calcTimeStats(FPSCounter& fps, std::chrono::time_point<std::chrono::high_resolution_clock>& beginTime) {
		auto& clock = Clock::instance();
		const auto endTime = std::chrono::high_resolution_clock::now();
		float dt = std::chrono::duration_cast<std::chrono::microseconds>(
				endTime - beginTime).count() / 1'000'000.f;
		if (dt > 1.f)
			dt = clock.targetDeltaTime;
		clock.update(dt);
		beginTime = endTime;
		//std::cerr << "dt = " << clock.deltaTime() << " (estimate FPS = " <<
			//1 / clock.deltaTime() << ")\n";

		fps.addFrame();
		fps.report();
	}

	void cleanupSwapChain() {
		vkDestroyImageView(app.device, app.depthImage.view, nullptr);
		vkDestroyImage(app.device, app.depthImage.handle, nullptr);
		vkFreeMemory(app.device, app.depthImage.memory, nullptr);

		for (auto framebuffer : app.swapChain.framebuffers) {
			vkDestroyFramebuffer(app.device, framebuffer, nullptr);
		}

		vkFreeCommandBuffers(app.device, app.commandPool, static_cast<uint32_t>(commandBuffers.size()),
				commandBuffers.data());

		vkDestroyPipeline(app.device, graphicsPipeline, nullptr);
		vkDestroyPipelineLayout(app.device, pipelineLayout, nullptr);
		vkDestroyRenderPass(app.device, app.geomRenderPass, nullptr);

		for (auto imageView : app.swapChain.imageViews)
			vkDestroyImageView(app.device, imageView, nullptr);

		vkDestroySwapchainKHR(app.device, app.swapChain.handle, nullptr);
	}

	void cleanup() {
		cleanupSwapChain();

		vkDestroySampler(app.device, texDiffuseImage.sampler, nullptr);
		vkDestroyImageView(app.device, texDiffuseImage.view, nullptr);
		vkDestroyImage(app.device, texDiffuseImage.handle, nullptr);
		vkFreeMemory(app.device, texDiffuseImage.memory, nullptr);

		vkDestroyDescriptorPool(app.device, descriptorPool, nullptr);
		vkDestroyDescriptorSetLayout(app.device, app.gBuffer.descriptorSetLayout, nullptr);

		vkDestroyBuffer(app.device, uniformBuffer.handle, nullptr);
		vkFreeMemory(app.device, uniformBuffer.memory, nullptr);
		vkDestroyBuffer(app.device, indexBuffer.handle, nullptr);
		vkFreeMemory(app.device, indexBuffer.memory, nullptr);
		vkDestroyBuffer(app.device, vertexBuffer.handle, nullptr);
		vkFreeMemory(app.device, vertexBuffer.memory, nullptr);

		vkDestroySemaphore(app.device, renderFinishedSemaphore, nullptr);
		vkDestroySemaphore(app.device, imageAvailableSemaphore, nullptr);
		vkDestroyCommandPool(app.device, app.commandPool, nullptr);

		delete [] streamingBufferData;

		app.cleanup();
	}

	static void onWindowResized(GLFWwindow *window, int, int) {
		auto appl = reinterpret_cast<HelloTriangleApplication*>(glfwGetWindowUserPointer(window));
		appl->recreateSwapChain();
	}

	static void cursorPosCallback(GLFWwindow *window, double xpos, double ypos) {
		static double prevX = cfg::WIDTH / 2.0,
		              prevY = cfg::HEIGHT / 2.0;
		auto appl = reinterpret_cast<HelloTriangleApplication*>(glfwGetWindowUserPointer(window));
		appl->cameraCtrl->turn(xpos - prevX, prevY - ypos);
		//prevX = xpos;
		//prevY = ypos;
		glfwSetCursorPos(window, prevX, prevY);
	}

	void recreateSwapChain() {
		int width, height;
		glfwGetWindowSize(app.window, &width, &height);
		if (width == 0 || height == 0) return;

		VLKCHECK(vkDeviceWaitIdle(app.device));

		cleanupSwapChain();

		app.swapChain = createSwapChain(app);
		createSwapChainImageViews(app);
		app.geomRenderPass = createRenderPass(app);
		std::tie(graphicsPipeline, pipelineLayout) = createGBufferPipeline(app);
		app.depthImage = createDepthImage(app);
		createSwapChainFramebuffers(app);
		createCommandBuffers();
	}

	void createDescriptorPool() {
		std::array<VkDescriptorPoolSize, 2> poolSizes = {};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSizes[0].descriptorCount = 1;
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSizes[1].descriptorCount = 2;

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = poolSizes.size();
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = 1;

		VLKCHECK(vkCreateDescriptorPool(app.device, &poolInfo, nullptr, &descriptorPool));
	}

	void createDescriptorSet() {
		VkDescriptorSetLayout layouts[] = { app.gBuffer.descriptorSetLayout };
		VkDescriptorSetAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = layouts;

		VLKCHECK(vkAllocateDescriptorSets(app.device, &allocInfo, &descriptorSet));

		VkDescriptorBufferInfo bufferInfo = {};
		bufferInfo.buffer = uniformBuffer.handle;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		VkDescriptorImageInfo texDiffuseInfo = {};
		texDiffuseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		texDiffuseInfo.imageView = texDiffuseImage.view;
		texDiffuseInfo.sampler = texDiffuseImage.sampler;

		VkDescriptorImageInfo texSpecularInfo = {};
		texSpecularInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		texSpecularInfo.imageView = texSpecularImage.view;
		texSpecularInfo.sampler = texSpecularImage.sampler;

		std::array<VkWriteDescriptorSet, 2> descriptorWrites = {};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].dstSet = descriptorSet;
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].pBufferInfo = &bufferInfo;

		descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[1].dstSet = descriptorSet;
		descriptorWrites[1].dstBinding = 1;
		descriptorWrites[1].dstArrayElement = 0;
		descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrites[1].descriptorCount = 1;
		descriptorWrites[1].pImageInfo = &texDiffuseInfo;

		descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[2].dstSet = descriptorSet;
		descriptorWrites[2].dstBinding = 2;
		descriptorWrites[2].dstArrayElement = 0;
		descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrites[2].descriptorCount = 1;
		descriptorWrites[2].pImageInfo = &texSpecularInfo;

		vkUpdateDescriptorSets(app.device, descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);
	}

	void createCommandBuffers() {
		commandBuffers.resize(app.swapChain.framebuffers.size());

		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = app.commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		VLKCHECK(vkAllocateCommandBuffers(app.device, &allocInfo, commandBuffers.data()));

		for (size_t i = 0; i < commandBuffers.size(); ++i) {
			VkCommandBufferBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
			beginInfo.pInheritanceInfo = nullptr;

			vkBeginCommandBuffer(commandBuffers[i], &beginInfo);

			VkRenderPassBeginInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = app.geomRenderPass;
			renderPassInfo.framebuffer = app.swapChain.framebuffers[i];
			renderPassInfo.renderArea.offset = {0, 0};
			renderPassInfo.renderArea.extent = app.swapChain.extent;
			std::array<VkClearValue, 2> clearValues = {};
			clearValues[0].color = {0.f, 0.f, 0.f, 1.f};
			clearValues[1].depthStencil = {1.f, 0};
			renderPassInfo.clearValueCount = clearValues.size();
			renderPassInfo.pClearValues = clearValues.data();

			vkCmdBeginRenderPass(commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
			VkBuffer vertexBuffers[] = { vertexBuffer.handle };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(commandBuffers[i], 0, 1, vertexBuffers, offsets);
			vkCmdBindIndexBuffer(commandBuffers[i], indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);
			vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
					pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
			vkCmdDrawIndexed(commandBuffers[i], nIndices, 1, 0, 0, 0);
			std::cerr << "recreating command buffer with v = "
				<< nVertices << ", i = " << nIndices << "\n";
			vkCmdEndRenderPass(commandBuffers[i]);

			VLKCHECK(vkEndCommandBuffer(commandBuffers[i]));
		}
	}

	void createSemaphores() {
		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if (vkCreateSemaphore(app.device, &semaphoreInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS
			|| vkCreateSemaphore(app.device, &semaphoreInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS)
			throw std::runtime_error("failed to create semaphores!");
	}

	void updateVertexBuffer() {
		// Acquire handle to device memory
		void *data;
		vkMapMemory(app.device, vertexBuffer.memory, 0, VERTEX_BUFFER_SIZE, 0, &data);
		// Copy host memory to device
		memcpy(data, streamingBufferData, VERTEX_BUFFER_SIZE);
		vkUnmapMemory(app.device, vertexBuffer.memory);
	}

	void updateIndexBuffer() {
		// Acquire handle to device memory
		void *data;
		vkMapMemory(app.device, indexBuffer.memory, 0, INDEX_BUFFER_SIZE, 0, &data);
		// Copy host memory to device
		memcpy(data, streamingBufferData + VERTEX_BUFFER_SIZE, INDEX_BUFFER_SIZE);
		vkUnmapMemory(app.device, indexBuffer.memory);
	}

	void updateUniformBuffer() {
		//static auto startTime = std::chrono::high_resolution_clock::now();

		//auto currentTime = std::chrono::high_resolution_clock::now();
		//float time = std::chrono::duration<float, std::chrono::seconds::period>(
				//currentTime - startTime).count();

		UniformBufferObject ubo = {};
		ubo.model = glm::mat4{1.0f};
		//ubo.model = glm::rotate(glm::mat4{1.0f}, time * glm::radians(90.f), glm::vec3{0.f, -1.f, 0.f});
		//std::cerr << "view mat = " << glm::to_string(camera.viewMatrix()) << "\n";
		ubo.view = camera.viewMatrix();
			//glm::lookAt(glm::vec3{140,140,140},glm::vec3{0,0,0},glm::vec3{0,1,0});
		ubo.proj = glm::perspective(glm::radians(60.f),
				app.swapChain.extent.width / float(app.swapChain.extent.height), 0.1f, 300.f);
		ubo.proj[1][1] *= -1;

		void *data;
		vkMapMemory(app.device, uniformBuffer.memory, 0, sizeof(ubo), 0, &data);
		memcpy(data, &ubo, sizeof(ubo));
		vkUnmapMemory(app.device, uniformBuffer.memory);
	}


	void drawFrame() {
		const auto imageIndex = acquireNextSwapImage(app, imageAvailableSemaphore);
		if (imageIndex < 0) {
			recreateSwapChain();
			return;
		}

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		const std::array<VkSemaphore, 1> waitSemaphores = { imageAvailableSemaphore };
		const std::array<VkPipelineStageFlags, 1> waitStages = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		static_assert(waitStages.size() == waitSemaphores.size());
		submitInfo.waitSemaphoreCount = waitSemaphores.size();
		submitInfo.pWaitSemaphores = waitSemaphores.data();
		submitInfo.pWaitDstStageMask = waitStages.data();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[imageIndex];
		const std::array<VkSemaphore, 1> signalSemaphores = { renderFinishedSemaphore };
		submitInfo.signalSemaphoreCount = signalSemaphores.size();
		submitInfo.pSignalSemaphores = signalSemaphores.data();

		VLKCHECK(vkQueueSubmit(app.queues.graphics, 1, &submitInfo, VK_NULL_HANDLE));

		VkPresentInfoKHR presentInfo = {};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = signalSemaphores.size();
		presentInfo.pWaitSemaphores = signalSemaphores.data();
		const std::array<VkSwapchainKHR, 1> swapChains = { app.swapChain.handle };
		presentInfo.swapchainCount = swapChains.size();
		presentInfo.pSwapchains = swapChains.data();
		presentInfo.pImageIndices = &imageIndex;
		presentInfo.pResults = nullptr;

		const auto result = vkQueuePresentKHR(app.queues.present, &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			recreateSwapChain();
		else if (result != VK_SUCCESS)
			throw std::runtime_error("failed to present swap chain image!");

		vkQueueWaitIdle(app.queues.present);
	}
};

int main() {
	if (!Endpoint::init()) {
		std::cerr << "Failed to initialize sockets." << std::endl;
		return EXIT_FAILURE;
	}
	std::atexit([]() { Endpoint::cleanup(); });

	HelloTriangleApplication app;

	try {
		app.run();
	} catch (const std::runtime_error& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
