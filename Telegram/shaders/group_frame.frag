#version 450

layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec2 b_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D s_texture;
layout(binding = 2) uniform sampler2D b_texture;
layout(binding = 3) uniform sampler2D n_texture;

layout(std140, binding = 0) uniform Params {
	vec2 viewport;
	vec4 frameBg;
	vec4 shadow;
	float paused;
	vec4 roundRect;
	vec2 radiusOutline;
	vec4 roundBg;
	vec4 outlineFg;
};

vec2 roundedCorner() {
	vec2 rectHalf = roundRect.zw / 2.0;
	vec2 rectCenter = roundRect.xy + rectHalf;
	vec2 fromRectCenter = abs(gl_FragCoord.xy - rectCenter);
	vec2 vectorRadius = radiusOutline.xx + vec2(0.5);
	vec2 fromCenterWithRadius = fromRectCenter + vectorRadius;
	vec2 fromRoundingCenter = max(fromCenterWithRadius, rectHalf) - rectHalf;
	float rounded = length(fromRoundingCenter) - radiusOutline.x;
	float outline = rounded + radiusOutline.y;
	return vec2(
		1.0 - smoothstep(0.0, 1.0, rounded),
		1.0 - (smoothstep(0.0, 1.0, outline) * outlineFg.a));
}

bool insideTexture() {
	return v_texcoord.x >= 0.0
		&& v_texcoord.x <= 1.0
		&& v_texcoord.y >= 0.0
		&& v_texcoord.y <= 1.0;
}

vec4 background() {
	vec4 blur = texture(b_texture, b_texcoord);
	float blurOpacity = shadow.w;
	return mix(frameBg, blur, blurOpacity);
}

void main() {
	vec4 result;
	if (insideTexture()) {
		result = texture(s_texture, v_texcoord);
		result = mix(result, background(), paused);
	} else {
		result = background();
	}

	float shadowCoord = shadow.y - gl_FragCoord.y;
	float shadowValue = clamp(shadowCoord / shadow.x, 0.0, 1.0);
	float shadowShown = shadowValue * shadow.z;
	result = vec4(min(result.rgb, vec3(1.0)) * (1.0 - shadowShown), result.a);

	float noiseValue = texture(n_texture,
		gl_FragCoord.xy / vec2(256.0)).r;
	result.rgb += (noiseValue - 0.5) * 0.002;

	vec2 roundOutline = roundedCorner();
	result = result * roundOutline.y
		+ vec4(outlineFg.rgb, 1.0) * (1.0 - roundOutline.y);
	result = result * roundOutline.x + roundBg * (1.0 - roundOutline.x);
	fragColor = result;
}
