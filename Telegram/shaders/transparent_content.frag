#version 450

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D s_texture;
layout(binding = 2) uniform sampler2D f_texture;

layout(std140, binding = 0) uniform Params {
	vec2 viewport;
	vec4 shadowTopRect;
	vec4 shadowBottomSkipOpacityFullFade;
	vec4 transparentBg;
	vec4 transparentFg;
	float transparentSize;
};

vec4 applyControlsFade(vec4 result) {
	float topHeight = shadowTopRect.w;
	float bottomHeight = shadowBottomSkipOpacityFullFade.x;
	float bottomSkip = shadowBottomSkipOpacityFullFade.y;
	float opacity = shadowBottomSkipOpacityFullFade.z;
	float fullFade = shadowBottomSkipOpacityFullFade.w;

	float topY = shadowTopRect.y;
	float bottomY = topY + shadowTopRect.z;
	float coord = gl_FragCoord.y;

	float fadeAlpha = 0.0;
	float fadeTexCoord = 0.0;

	if (coord < topY + topHeight) {
		fadeTexCoord = (topY + topHeight - coord) / topHeight;
		fadeAlpha = opacity;
	} else if (coord > bottomY - bottomHeight - bottomSkip) {
		fadeTexCoord = (coord - (bottomY - bottomHeight - bottomSkip)) / bottomHeight;
		fadeAlpha = opacity;
	}

	fadeAlpha = mix(fadeAlpha, 1.0, fullFade);
	vec4 fadeColor = texture(f_texture, vec2(fadeTexCoord, 0.5));
	fadeColor *= fadeAlpha;
	return result * (1.0 - fadeColor.a) + fadeColor;
}

void main() {
	vec4 result = texture(s_texture, v_texcoord);

	vec2 checkboardLadder = floor(gl_FragCoord.xy / transparentSize);
	float checkboard = mod(checkboardLadder.x + checkboardLadder.y, 2.0);
	vec4 bg = mix(transparentBg, transparentFg, checkboard);
	result = vec4(result.rgb * result.a + bg.rgb * (1.0 - result.a), 1.0);

	result = applyControlsFade(result);
	fragColor = result;
}
