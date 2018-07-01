#include "buffer_array.hpp"
#include "application.hpp"
#include "logging.hpp"
#include <algorithm>

using namespace logging;

void BufferArray::initialize(const Application& app, VkDeviceSize minBufferSize)
{
	this->app = &app;
	minAlign = findMinUboAlign(app.physicalDevice);
	if (minBufferSize == 0)
		this->minBufferSize = minAlign * 4;
	else {
		this->minBufferSize = minBufferSize;
		if (minBufferSize % minAlign != 0) {
			err("minBufferSize should be multiple of minAlign (", minAlign, ")");
			throw;
		}
	}
	debug("BufferArray: set minAlign to ", minAlign, " B");
}

void BufferArray::reserve(VkDeviceSize initialSize)
{
	if (app == nullptr) {
		err("BufferArray wasn't initialized when calling reserve()!");
		return;
	}
	backingBuffers.emplace_back(createBuffer(*app, std::max(minBufferSize, initialSize), usage, properties));
	bufferFreeRanges.emplace_back(std::vector<BufferFreeRange>{ BufferFreeRange{ 0, backingBuffers[0].size } });
	debug("BufferArray: called reserve(", initialSize, "). Resized to ", backingBuffers.back().size, " B");
}

/** Maps all currently and future allocated buffers to host memory.
 *  Only valid if `properties` includes HOST_VISIBLE.
 */
void BufferArray::mapAllBuffers()
{
	if (mappingBuffers)
		return;
	if ((properties >> VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT & 1) != 1) {
		err("BufferArray: trying to map buffers not visible to host!");
		return;
	}
	mappingBuffers = true;

	std::vector<Buffer*> bufPointers(backingBuffers.size());
	for (unsigned i = 0; i < backingBuffers.size(); ++i)
		bufPointers[i] = &backingBuffers[i];
	mapBuffersMemory(app->device, bufPointers);

	// Update all pointers in already-allocated buffers
	for (auto& pair : allocatedBuffers) {
		auto& buf = pair.second;
		auto it = std::find_if(backingBuffers.begin(),
			backingBuffers.end(),
			[handle = buf.handle](const auto& buf) { return buf.handle == handle; });
		assert(it != backingBuffers.end());
		buf.ptr = reinterpret_cast<uint8_t*>(it->ptr) + buf.bufOffset;
	}
}

/** Unmaps all currently allocated buffers and stops mapping future ones. */
void BufferArray::unmapAllBuffers()
{
	if (!mappingBuffers) {
		warn("BufferArray: trying to unmap buffers which are not mapped.");
		return;
	}
	mappingBuffers = false;
	unmapBuffersMemory(app->device, backingBuffers);
	for (auto& pair : allocatedBuffers)
		pair.second.ptr = nullptr;
}

void BufferArray::cleanup()
{
	destroyAllBuffers(app->device, backingBuffers);
	backingBuffers.clear();
	bufferFreeRanges.clear();
}

/** Adds a logical buffer to the array and returns it.
 *  If the buffer fits the already-allocated buffer(s), it will be part of one of them,
 *  else, a new backing Buffer will be allocated.
 */
SubBuffer* BufferArray::addBuffer(StringId name, VkDeviceSize size)
{
	if (app == nullptr) {
		err("BufferArray wasn't initialized when calling addBuffer()!");
		return nullptr;
	}

	assert(backingBuffers.size() == bufferFreeRanges.size());
	if (allocatedBuffers.find(name) != allocatedBuffers.end()) {
		err("BufferArray: trying to add duplicate buffer '", name, "'!");
		return nullptr;
	}

	// Round up size to the nearest align
	if (size % minAlign != 0)
		size = (1 + size / minAlign) * minAlign;

	// Search a free spot in existing buffers
	for (unsigned i = 0; i < backingBuffers.size(); ++i) {
		auto& freeRanges = bufferFreeRanges[i];
		verbose("freeRanges[", i, "] = ", listToString(freeRanges));
		// Find best-fit
		std::sort(freeRanges.begin(), freeRanges.end(), [size](auto r1, auto r2) {
			if (static_cast<VkDeviceSize>(r1.len()) < size)
				return false;
			if (static_cast<VkDeviceSize>(r2.len()) < size)
				return true;
			return r1.len() < r2.len();
		});
		if (static_cast<VkDeviceSize>(freeRanges[0].len()) < size) {
			// no fit found
			continue;
		}

		// Found fit: "allocate" from it
		auto& buf = backingBuffers[i];
		auto& buffer = allocatedBuffers[name];   // create a new mapping
		buffer.handle = buf.handle;
		buffer.memory = buf.memory;
		buffer.offset = buf.offset;
		buffer.ptr = buf.ptr;
		buffer.size = size;
		buffer.bufOffset = freeRanges[0].start;

		freeRanges[0].start += size;
		assert(freeRanges[0].len() >= 0);

		debug("BufferArray: added buffer ",
			name,
			" at backingBuf #",
			i,
			" { start: ",
			buffer.bufOffset,
			", size: ",
			buffer.size,
			" }");

		return &buffer;
	}

	// Need to allocate another buffer
	backingBuffers.emplace_back(createBuffer(*app, std::max(minBufferSize, size), usage, properties));
	const auto& buf = backingBuffers.back();
	debug("Allocated new buffer in BufferArray with size ", buf.size, " B");
	bufferFreeRanges.emplace_back(std::vector<BufferFreeRange>{});
	if (size < buf.size)
		bufferFreeRanges.back().emplace_back(BufferFreeRange{ size, buf.size });

	auto& buffer = allocatedBuffers[name];   // create a new mapping
	buffer.handle = buf.handle;
	buffer.memory = buf.memory;
	buffer.offset = buf.offset;
	buffer.ptr = buf.ptr;
	buffer.size = size;
	buffer.bufOffset = 0;

	return &buffer;
}

SubBuffer* BufferArray::getBuffer(StringId name) const
{
	auto it = allocatedBuffers.find(name);
	if (it == allocatedBuffers.end())
		return nullptr;

	return const_cast<SubBuffer*>(&it->second);
}
