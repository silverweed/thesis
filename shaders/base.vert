#version 450
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec3 inPosition;
layout (location = 2) in vec2 inTexCoords;

layout (location = 0) out vec2 outTexCoords;

out gl_PerVertex {
	vec4 gl_Position;
};

void main() {
	outTexCoords = inTexCoords;
	gl_Position = vec4(inPosition, 1.0);
}
