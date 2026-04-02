#version 450

layout(location = 0) in vec2 inQuadPos;

layout(location = 1) in vec2 inOffset;
layout(location = 2) in float inLifetime;

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out float v_alpha;

layout(std140, binding = 0) uniform Params {
	vec4 rect;
	vec2 size;
	uvec2 particleResolution;
};

void main() {
	uint particleId = uint(gl_InstanceIndex);
	uint pX = particleId % particleResolution.x;
	uint pY = particleId / particleResolution.x;

	vec2 particleSize = size / vec2(particleResolution);

	vec2 topLeft = vec2(float(pX) * particleSize.x, float(pY) * particleSize.y);
	v_texcoord = (topLeft + inQuadPos * particleSize) / size;

	topLeft += inOffset;
	vec2 position = topLeft + inQuadPos * particleSize;

	vec2 ndc;
	ndc.x = rect.x + position.x / size.x * rect.z;
	ndc.y = rect.y + position.y / size.y * rect.w;
	ndc.x = -1.0 + ndc.x * 2.0;
	ndc.y = -1.0 + ndc.y * 2.0;

	gl_Position = vec4(ndc, 0.0, 1.0);

	v_alpha = clamp(inLifetime / 0.3, 0.0, 1.0);
}
