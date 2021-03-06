#include "buffers.hpp"
#include "application.hpp"
#include "commands.hpp"
#include "logging.hpp"
#include "phys_device.hpp"
#include "vertex.hpp"
#include "vulk_errors.hpp"
#include "vulk_memory.hpp"
#include <array>
#include <cstring>
#include <unordered_set>

using namespace logging;

void destroyBuffer(VkDevice device, Buffer& buffer)
{
	vkDestroyBuffer(device, buffer.handle, nullptr);
	vkFreeMemory(device, buffer.memory, nullptr);
#ifndef NDEBUG
	gMemMonitor.newFree(buffer.memory);
#endif
}

void BufferAllocator::addBuffer(Buffer& buffer, const BufferAllocator::BufferCreateInfo& info)
{
	addBuffer(buffer, std::get<0>(info), std::get<1>(info), std::get<2>(info));
}

void BufferAllocator::addBuffer(Buffer& buffer,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags properties)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	createInfos.emplace_back(bufferInfo);
	this->properties.emplace_back(properties);

	buffer.size = size;
	buffers.emplace_back(&buffer);
}

void BufferAllocator::create(const Application& app)
{
	// (memory type) => (memory size)
	std::unordered_map<uint32_t, VkDeviceSize> requiredSizes;

	std::vector<uint32_t> memTypesNeeded;
	memTypesNeeded.reserve(createInfos.size());

	// Create the buffers and figure out what memory they need
	for (unsigned i = 0; i < createInfos.size(); ++i) {
		VkBuffer bufHandle;
		VLKCHECK(vkCreateBuffer(app.device, &createInfos[i], nullptr, &bufHandle));
		app.validation.addObjectInfo(bufHandle, __FILE__, __LINE__);
		buffers[i]->handle = bufHandle;

		VkMemoryRequirements memRequirements;
		vkGetBufferMemoryRequirements(app.device, bufHandle, &memRequirements);

		const auto memType = findMemoryType(app.physicalDevice, memRequirements.memoryTypeBits, properties[i]);
		buffers[i]->offset = requiredSizes[memType];
		requiredSizes[memType] += memRequirements.size;

		memTypesNeeded.emplace_back(memType);
	}

	// The newly allocated device memories
	std::unordered_map<uint32_t, VkDeviceMemory> memories;
	memories.reserve(requiredSizes.size());

	// Allocate memory of the proper types
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	for (const auto& pair : requiredSizes) {
		allocInfo.memoryTypeIndex = pair.first;
		allocInfo.allocationSize = pair.second;
		VkDeviceMemory bufferMemory;
		VLKCHECK(vkAllocateMemory(app.device, &allocInfo, nullptr, &bufferMemory));
		app.validation.addObjectInfo(bufferMemory, __FILE__, __LINE__);
#ifndef NDEBUG
		gMemMonitor.newAlloc(bufferMemory, allocInfo);
#endif
		memories[pair.first] = bufferMemory;
	}

	// Bind the memory to the buffers
	for (unsigned i = 0; i < buffers.size(); ++i) {
		auto& memType = memTypesNeeded[i];
		auto buf = buffers[i];
		VLKCHECK(vkBindBufferMemory(app.device, buf->handle, memories[memType], buf->offset));
		buf->memory = memories[memType];
	}

	info("Created ", buffers.size(), " buffers via ", memories.size(), " allocations.");
}

Buffer createBuffer(const Application& app,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags properties)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer bufferHandle;
	VLKCHECK(vkCreateBuffer(app.device, &bufferInfo, nullptr, &bufferHandle));
	app.validation.addObjectInfo(bufferHandle, __FILE__, __LINE__);

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(app.device, bufferHandle, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.memoryTypeIndex = findMemoryType(app.physicalDevice, memRequirements.memoryTypeBits, properties);
	allocInfo.allocationSize = memRequirements.size;

	VkDeviceMemory bufferMemory;
	VLKCHECK(vkAllocateMemory(app.device, &allocInfo, nullptr, &bufferMemory));
	app.validation.addObjectInfo(bufferMemory, __FILE__, __LINE__);
#ifndef NDEBUG
	gMemMonitor.newAlloc(bufferMemory, allocInfo);
#endif

	VLKCHECK(vkBindBufferMemory(app.device, bufferHandle, bufferMemory, 0));

	Buffer buffer;
	buffer.handle = bufferHandle;
	buffer.memory = bufferMemory;
	buffer.size = size;

	return buffer;
}

void copyBuffer(const Application& app, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
	auto commandBuffer = beginSingleTimeCommands(app, app.commandPool);

	VkBufferCopy copyRegion = {};
	copyRegion.srcOffset = 0;
	copyRegion.dstOffset = 0;
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(app.device, app.queues.graphics, app.commandPool, commandBuffer);
}

void copyBufferToImage(const Application& app,
	Buffer buffer,
	VkImage image,
	uint32_t width,
	uint32_t height,
	VkDeviceSize bufOffset,
	uint32_t baseArrayLayer)
{
	VkCommandBuffer commandBuffer = beginSingleTimeCommands(app, app.commandPool);

	VkBufferImageCopy region = {};
	region.bufferOffset = bufOffset;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = baseArrayLayer;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { width, height, 1 };

	if (buffer.size - bufOffset < static_cast<VkDeviceSize>(region.imageExtent.width) * region.imageExtent.height *
					      region.imageExtent.depth) {
		const auto& e = region.imageExtent;
		err("copyBufferToImage: buffer has not enough room to contain the image!\n",
			"\tNeeded: ",
			e.width * e.height * e.depth,
			"\n\tAvailable: ",
			buffer.size - bufOffset);
		return;
	}
	vkCmdCopyBufferToImage(commandBuffer, buffer.handle, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	endSingleTimeCommands(app.device, app.queues.graphics, app.commandPool, commandBuffer);
}

Buffer createStagingBuffer(const Application& app, VkDeviceSize size)
{
	auto buf = createBuffer(app,
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

	VLKCHECK(vkMapMemory(app.device, buf.memory, 0, buf.size, 0, &buf.ptr));

	return buf;
}

void destroyAllBuffers(VkDevice device, const std::vector<Buffer>& buffers)
{
	std::unordered_set<VkDeviceMemory> mems;
	for (auto& b : buffers) {
		mems.emplace(b.memory);
		vkDestroyBuffer(device, b.handle, nullptr);
	}

	for (auto& mem : mems) {
		vkFreeMemory(device, mem, nullptr);
#ifndef NDEBUG
		gMemMonitor.newFree(mem);
#endif
	}
}

void mapBuffersMemory(VkDevice device, const std::vector<Buffer*>& buffers)
{
	// 1. Figure out how many memories are to be bound and their size
	// 2. Bind a pointer to the whole memory chunk for each one of them
	// 3. Assign pointers in `mappedPointers` to different offsets of these base pointers.

	struct MemInfo {
		VkDeviceSize size;
		void* ptr;
	};
	std::unordered_map<VkDeviceMemory, MemInfo> mems;

	for (const auto b : buffers)
		mems[b->memory].size += b->size;

	for (auto& mem : mems)
		VLKCHECK(vkMapMemory(device, mem.first, 0, mem.second.size, 0, &mem.second.ptr));

	for (unsigned i = 0; i < buffers.size(); ++i) {
		auto& b = buffers[i];
		const auto& mem = mems[b->memory];
		b->ptr = reinterpret_cast<uint8_t*>(mem.ptr) + b->offset;
	}
}

void unmapBuffersMemory(VkDevice device, const std::vector<Buffer>& buffers)
{
	std::unordered_set<VkDeviceMemory> mems;
	for (const auto b : buffers)
		mems.emplace(b.memory);

	for (auto& mem : mems)
		vkUnmapMemory(device, mem);
}

BufferAllocator::BufferCreateInfo getScreenQuadBufferProperties()
{
	// Note: need to be explicit with the arguments or Clang complains
	return std::make_tuple(VkDeviceSize{ sizeof(Vertex) * 4 },
		VkBufferUsageFlags{ VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT },
		VkMemoryPropertyFlags{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
}

BufferAllocator::BufferCreateInfo getSkyboxBufferProperties()
{
	return std::make_tuple(VkDeviceSize{ sizeof(Vertex) * 25 + sizeof(Index) * 36 },
		VkBufferUsageFlags{ VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
				    VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
		VkMemoryPropertyFlags{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
}

bool fillScreenQuadBuffer(const Application& app, Buffer& screenQuadBuf, Buffer& stagingBuf)
{
	const std::array<Vertex, 4> quadVertices = {
		// position, normal, texCoords, tangent, bitangent
		Vertex{ glm::vec3{ -1.0f, 1.0f, 0.0f },
			glm::vec3{},
			glm::vec2{ 0.0f, 1.0f },
			glm::vec3{},
			glm::vec3{} },
		Vertex{ glm::vec3{ -1.0f, -1.0f, 0.0f },
			glm::vec3{},
			glm::vec2{ 0.0f, 0.0f },
			glm::vec3{},
			glm::vec3{} },
		Vertex{ glm::vec3{ 1.0f, 1.0f, 0.0f }, glm::vec3{}, glm::vec2{ 1.0f, 1.0f }, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 1.0f, -1.0f, 0.0f },
			glm::vec3{},
			glm::vec2{ 1.0f, 0.0f },
			glm::vec3{},
			glm::vec3{} },
	};

	if (screenQuadBuf.size < quadVertices.size() * sizeof(Vertex)) {
		err("Buffer given to fillScreenQuadBuffer is too small!");
		return false;
	}

	memcpy(stagingBuf.ptr, quadVertices.data(), quadVertices.size() * sizeof(Vertex));
	copyBuffer(app, stagingBuf.handle, screenQuadBuf.handle, quadVertices.size() * sizeof(Vertex));

	return true;
}

int64_t fillSkyboxBuffer(const Application& app, Buffer& skyboxBuffer, Buffer& stagingBuf)
{
	const std::array<Vertex, 25> cubeVertices = {
		Vertex{ glm::vec3{ -0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, -0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ 0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, 0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
		Vertex{ glm::vec3{ -0.5f, 0.5f, -0.5f }, glm::vec3{}, glm::vec3{}, glm::vec3{}, glm::vec3{} },
	};
	// clang-format off
	const std::array<uint16_t,  36> cubeIndices = {
		0,  1,  2,  1,  0,  3,
		4,  5,  6,  6,  7,  4,
		8,  9,  10, 10, 11, 8,
		12, 13, 14, 13, 12, 15,
		16, 17, 18, 18, 19, 16,
		20, 21, 22, 23, 22, 24,
	};
	// clang-format on

	if (skyboxBuffer.size < cubeVertices.size() * sizeof(Vertex) + cubeIndices.size() * sizeof(uint16_t)) {
		err("Buffer given to fillSkyboxBuffer is too small!");
		return -1;
	}

	// Copy vertices and indices in the buffer
	memcpy(stagingBuf.ptr, cubeVertices.data(), cubeVertices.size() * sizeof(Vertex));
	memcpy(reinterpret_cast<uint8_t*>(stagingBuf.ptr) + cubeVertices.size() * sizeof(Vertex),
		cubeIndices.data(),
		cubeIndices.size() * sizeof(Index));
	copyBuffer(app,
		stagingBuf.handle,
		skyboxBuffer.handle,
		cubeVertices.size() * sizeof(Vertex) + cubeIndices.size() * sizeof(uint16_t));

	return cubeVertices.size() * sizeof(Vertex);
}
