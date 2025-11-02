#version 450
#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

// Input from vertex shader (all in tangent space)
layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;
layout (location = 4) in mat3 inTBN;
layout (location = 7) in vec3 inViewPos;

// Light positions in tangent space (array of 4 lights)
layout (location = 8) in vec3 inLightPos[4];

// Output
layout (location = 0) out vec4 outFragColor;

// light colors with intensity
const vec3 lightColors[4] = vec3[4](
    vec3(2.0, 2.0, 2.0),  // Slightly brighter than pure white
    vec3(2.0, 2.0, 2.0),
    vec3(2.0, 2.0, 2.0),
    vec3(2.0, 2.0, 2.0)
);

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    
    return num / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}


void main() 
{
    vec3 albedo = texture(colorTex, inUV).rgb;
    albedo = pow(albedo, vec3(2.2)); // Convert to linear space
    albedo *= inColor; // Apply vertex color
    
    // Sample metallic and roughness from metalRoughTex
    // glTF: metallic = B channel, roughness = G channel
    vec2 metalRough = texture(metalRoughTex, inUV).bg;
    float metallic = metalRough.x * materialData.metal_rough_factors.x;
    float roughness = metalRough.y * materialData.metal_rough_factors.y;
    
    // clamp roughness to avoid division by zero and artifacts
    roughness = clamp(roughness, 0.04, 1.0);
    
    // sample ambient occlusion if available
    float ao = 1.0;
    if (materialData.textureFlags.y > 0.5) {
        ao = texture(occlusionTex, inUV).r;
    }
    
    // sample normal from normal map if available
    vec3 N;
    if (materialData.textureFlags.x > 0.5) {
        N = texture(normalTex, inUV).rgb;
        N = normalize(N * 2.0 - 1.0); // Convert from [0,1] to [-1,1]
    } else {
        N = vec3(0.0, 0.0, 1.0);
    }
    
    // View direction (already in tangent space)
    vec3 V = normalize(inViewPos - inWorldPos);
    
    // Base reflectance at 0 degrees (for dielectrics (non-metallic))
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    // Reflectance equation - accumulate lighting from all lights
    vec3 Lo = vec3(0.0);
    
    for(int i = 0; i < 4; ++i) 
    {
        // Light direction (in tangent space)
        vec3 L = normalize(inLightPos[i] - inWorldPos);
        vec3 H = normalize(V + L);
        
        // Calculate attenuation
        float distance = length(inLightPos[i] - inWorldPos);
        float attenuation = 1.0 / (distance * distance);
        vec3 radiance = lightColors[i] * attenuation;
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        // Specular and diffuse components
        vec3 kS = F; // Specular contribution
        vec3 kD = vec3(1.0) - kS; // Diffuse contribution
        kD *= 1.0 - metallic; // Metals don't have diffuse
        
        // Calculate specular BRDF
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;
        
        // Add to outgoing radiance Lo
        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;


    }
    
    // Ambient lighting (simple approximation)
    vec3 ambient = vec3(0.03) * albedo * ao;
    
    // Add emissive if available
    vec3 emissive = vec3(0.0);
    if (materialData.textureFlags.z > 0.5) {
        emissive = texture(emissiveTex, inUV).rgb * materialData.emissiveFactor.rgb;
    }
    
    // Combine lighting
    vec3 color = ambient + Lo + emissive;

    // HDR tonemapping (Reinhard)
    color = color / (color + vec3(1.0));
    
    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));
    
    outFragColor = vec4(color, 1.0);
}