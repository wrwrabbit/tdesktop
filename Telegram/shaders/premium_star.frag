#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D u_Texture;
layout(binding = 2) uniform sampler2D u_NormalMap;

layout(std140, binding = 0) uniform Params {
	mat4 mvp;
	mat4 world;
	vec4 grad1;
	vec4 grad2;
	vec4 params;
	vec4 extra;
};

void main() {
	float f_xOffset = params.x;
	float spec1 = params.y;
	float spec2 = params.z;
	float u_diffuse = params.w;
	float normalSpec = extra.x;
	float alpha = extra.y;
	vec3 gradientColor1 = grad1.rgb;
	vec3 gradientColor2 = grad2.rgb;
	vec3 normalSpecColor = vec3(1.0);

	vec3 cameraPosition = vec3(0.0, 0.0, 100.0);
	vec3 vLightPosition2 = vec3(-400.0, 400.0, 400.0);
	vec3 vLightPosition4 = vec3(0.0, 0.0, 100.0);
	vec3 vLightPositionNormal = vec3(100.0, -200.0, 400.0);

	vec3 vNormalW = normalize(vec3(world * vec4(vNormal, 0.0)));
	vec3 vTextureNormal = normalize(
		texture(u_NormalMap, (vUV + vec2(-f_xOffset, f_xOffset)) * 2.0).xyz
			* 2.0 - 1.0);
	vec3 finalNormal = normalize(vNormalW + vTextureNormal);

	vec3 color = texture(u_Texture, vUV).xyz;
	vec3 viewDirectionW = normalize(cameraPosition);

	vec3 angleW = normalize(viewDirectionW + vLightPosition2);
	float specComp2 = pow(max(0.0, dot(vNormalW, angleW)), 128.0) * spec1;

	angleW = normalize(viewDirectionW + vLightPosition4);
	float specComp3 = pow(max(0.0, dot(vNormalW, angleW)), 30.0) * spec2;

	float diffuse = max(dot(vNormalW, viewDirectionW), 1.0 - u_diffuse);

	float mixValue = distance(vUV, vec2(1.0, 0.0));
	vec3 gradientColorFinal = mix(gradientColor1, gradientColor2, mixValue);

	angleW = normalize(viewDirectionW + vLightPositionNormal);
	float normalSpecComp = pow(max(0.0, dot(finalNormal, angleW)), 128.0)
		* normalSpec;

	angleW = normalize(viewDirectionW + vLightPosition2);
	float normalSpecComp2 = pow(max(0.0, dot(finalNormal, angleW)), 128.0)
		* normalSpec;

	vec3 normalSpecFinal = normalSpecColor * (normalSpecComp + normalSpecComp2);
	vec3 specFinal = color * (specComp2 + specComp3);

	vec3 lit = gradientColorFinal + specFinal + normalSpecFinal;

	float outAlpha = clamp(diffuse, 0.0, 1.0) * alpha;
	fragColor = vec4(lit * outAlpha, outAlpha);
}
