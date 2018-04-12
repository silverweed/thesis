#include "shaders.hpp"
#include "vulk_utils.hpp"
#include "vulk_errors.hpp"
#include "application.hpp"
#include <vector>

VkShaderModule createShaderModule(const Application& app, const char *fname) {
	const auto code = readFile(fname);

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

	VkShaderModule shaderModule;
	VLKCHECK(vkCreateShaderModule(app.device, &createInfo, nullptr, &shaderModule));

	return shaderModule;
}
