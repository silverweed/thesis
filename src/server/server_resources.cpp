#include "server_resources.hpp"

Model ServerResources::loadModel(const char* file)
{
	const auto fileSid = sid(file);
	if (models.count(fileSid) > 0) {
		logging::warn("Tried to load model ", file, " which is already loaded!");
		return models[fileSid];
	}

	// Reserve the whole remaining memory for loading the resource, then shrink to fit.
	std::size_t bufsize;
	auto buffer = allocator.allocAll(&bufsize);

	auto& model = models[fileSid];
	model = ::loadModel(file, buffer, bufsize);

	assert(model.vertices && "Failed to load model!");

	allocator.deallocLatest();
	allocator.alloc(model.size());

	return model;
}

shared::Texture ServerResources::loadTexture(const char* file)
{
	const auto fileSid = sid(file);
	if (textures.count(fileSid) > 0) {
		logging::warn("Tried to load texture ", file, " which is already loaded!");
		return textures[fileSid];
	}

	std::size_t bufsize;
	auto buffer = allocator.allocAll(&bufsize);
	auto size = readFileIntoMemory(file, buffer, bufsize);

	assert(size > 0 && "Failed to load texture!");

	auto& texture = textures[fileSid];
	texture.size = size;
	texture.data = buffer;
	texture.format = shared::TextureFormat::UNKNOWN;

	allocator.deallocLatest();
	allocator.alloc(texture.size);

	logging::info("Loaded texture ", file, " (", texture.size / 1024., " KiB)");

	return texture;
}

shared::SpirvShader ServerResources::loadShader(const char* file)
{
	const auto fileSid = sid(file);
	if (shaders.count(fileSid) > 0) {
		logging::warn("Tried to load shader ", file, " which is already loaded!");
		return shaders[fileSid];
	}

	std::size_t bufsize;
	auto buffer = allocator.allocAll(&bufsize);
	auto size = readFileIntoMemory(file, buffer, bufsize);

	assert(size > 0 && "Failed to load shader!");

	auto& shader = shaders[fileSid];
	shader.codeSizeInBytes = size;
	shader.code = reinterpret_cast<uint32_t*>(buffer);

	allocator.deallocLatest();
	allocator.alloc(shader.codeSizeInBytes);

	logging::info("Loaded shader ", file, " (", shader.codeSizeInBytes, " B)");

	return shader;
}
