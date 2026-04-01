#version 450

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Params {
	vec2 viewport;
	vec4 roundRect;
	float roundRadius;
};

float roundedCorner() {
	vec2 rectHalf = roundRect.zw / 2.0;
	vec2 rectCenter = roundRect.xy + rectHalf;
	vec2 fromRectCenter = abs(gl_FragCoord.xy - rectCenter);
	vec2 vectorRadius = vec2(roundRadius + 0.5);
	vec2 fromCenterWithRadius = fromRectCenter + vectorRadius;
	vec2 fromRoundingCenter = max(fromCenterWithRadius, rectHalf) - rectHalf;
	float rounded = length(fromRoundingCenter) - roundRadius;
	return 1.0 - smoothstep(0.0, 1.0, rounded);
}

void main() {
	fragColor = vec4(1.0) * roundedCorner();
}
