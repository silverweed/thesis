#pragma once

#include "buffers.hpp"
#include "images.hpp"
#include "shared_resources.hpp"
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

struct Application;

// TODO
class TextureLoader final {
	struct ImageInfo {
		VkFormat format;
		uint32_t width;
		uint32_t height;
	};

	Buffer& stagingBuffer;
	std::size_t stagingBufferOffset = 0;

	/** Used by `addTextureAsync` */
	std::mutex mtx;

	std::vector<ImageInfo> imageInfos;
	std::vector<Image*> images;

	/** Holds the latest error message */
	std::string latestError = "";

public:
	explicit TextureLoader(Buffer& stagingBuffer)
		: stagingBuffer{ stagingBuffer }
	{}

	/** Load a texture from raw data pointed by `texture` */
	bool addTexture(Image& image, const shared::Texture& texture);
	/** Load a texture from file with given format */
	bool addTexture(Image& image, const std::string& texturePath, shared::TextureFormat format);

	/** Like `addTexture`, but asynchronous. Returns a `future` which must be waited for
	 *  before calling `create`. It is the caller's responsibility to wait it.
	 *  @return a Future which will contain `true` in case of success, `false` otherwise.
	 */
	std::future<bool> addTextureAsync(Image& image, const shared::Texture& texture);
	/** @see addTextureAsync */
	std::future<bool> addTextureAsync(Image& image, const std::string& texturePath, shared::TextureFormat format);

	void create(const Application& app);

	std::string getLatestError() const { return latestError; }
};

/** Create a sampler appropriate for sampling a texture and return it. */
VkSampler createTextureSampler(const Application& app);
