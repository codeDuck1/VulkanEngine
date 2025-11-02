#version 450
#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
    // Sample base color texture
    vec3 baseColor = inColor * texture(colorTex, inUV).xyz;
    
    // Sample metallic-roughness texture (G=roughness, B=metallic)
    vec3 metallicRoughness = texture(metalRoughTex, inUV).rgb;
    float roughness = metallicRoughness.g * materialData.metal_rough_factors.y;
    float metallic = metallicRoughness.b * materialData.metal_rough_factors.x;
    
    // Sample occlusion texture (R channel only)
    float ao = 1.0;
    if (materialData.textureFlags.y > 0.5) {
        ao = texture(occlusionTex, inUV).r;
    }
    
    // Use vertex normal for now (normal mapping requires tangent space)
    vec3 normal = normalize(inNormal);
    
    // Simple doffise lighting calculation
    float lightValue = max(dot(normal, sceneData.sunlightDirection.xyz), 0.1);
    
    // Apply AO to BOTH ambient and direct lighting for maximum visibility
    vec3 ambient = baseColor * sceneData.ambientColor.xyz * ao;
    vec3 directLight = baseColor * lightValue * sceneData.sunlightColor.w * ao;
    vec3 litColor = directLight + ambient;
    
    // Sample and add emissive
    vec3 emissive = vec3(0.0);
    if (materialData.textureFlags.z > 0.5) {
        emissive = texture(emissiveTex, inUV).rgb * materialData.emissiveFactor.rgb;
    }
    
    vec3 finalColor = litColor + emissive;
    
    outFragColor = vec4(finalColor, 1.0);
}