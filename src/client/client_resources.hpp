#pragma once

#include "hashing.hpp"
#include "images.hpp"
#include "materials.hpp"
#include "models.hpp"
#include "shared_resources.hpp"
#include "stack_allocator.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>

/** This class provides a RAII facility to store resources.
 *  Memory used by this class is owned and allocated by this own class.
 *  The data is stored contiguously via a stack allocator, while the
 *  pointers to it are accessed via a map.
 *  Note that pointers retreived from said maps must not outlive this object,
 *  or the memory they point to will become invalid.
 *  As the name implies, this class is designed to be a temporary stage
 *  for resources that won't be needed for long (typically the resources which
 *  must be received from the server and immediately transferred on the device,
 *  such as textures and shaders)
 */
class ClientTmpResources final {
	std::vector<uint8_t> memory;

public:
	StackAllocator allocator;

	std::unordered_map<StringId, shared::Texture> textures;
	std::unordered_map<StringId, shared::SpirvShader> shaders;
	std::vector<shared::Material> materials;
	std::vector<ModelInfo> models;
	std::vector<shared::PointLight> pointLights;

	explicit ClientTmpResources(std::size_t size)
		: memory(size)
	{
		allocator.init(memory.data(), memory.size());
	}

	void clear()
	{
		allocator.deallocAll();
		textures.clear();
		shaders.clear();
		materials.clear();
		models.clear();
		pointLights.clear();
	}
};

/** This struct stores the "final form" of the resources received via network. */
struct NetworkResources {
	/** Map textureId => texture */
	std::unordered_map<StringId, Image> textures;

	std::vector<Material> materials;

	std::vector<ModelInfo> models;

	std::vector<shared::PointLight> pointLights;

	std::vector<shared::SpirvShader> shaders;
	/** Memory for staging shader code */
	std::vector<uint8_t> shadersCode;

	/** Default resources, used when actual ones are missing */
	struct {
		Image diffuseTex;
		Image specularTex;
		Image normalTex;
		Material material;
	} defaults;
};

using ObjectTransforms = std::unordered_map<StringId, glm::mat4>;
