#version 450

// PBR material textures
layout(set = 0, binding = 0) uniform sampler2D albedoMap;
layout(set = 0, binding = 1) uniform sampler2D normalMap;
layout(set = 0, binding = 2) uniform sampler2D metallicMap;
layout(set = 0, binding = 3) uniform sampler2D roughnessMap;
layout(set = 0, binding = 4) uniform sampler2D aoMap;
layout(set = 0, binding = 5) uniform sampler2D heightMap;

//shader input
layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUVs; // uvs and texcoord same
layout (location = 2) in vec3 CameraPos;
layout (location = 3) in vec3 Normal;
layout (location = 4) in vec3 WorldPos;
layout (location = 5) in mat3 tbnMatrix;

//output write
// connects to the render attachments of render pass
layout (location = 0) out vec4 outFragColor;

layout(push_constant) uniform BumpConstants {
    layout(offset = 152) float heightScale;
    layout(offset = 156) int numLayers;
    layout(offset = 160) int bumpMode;
} bump;

// must match in sphere positions in world space
vec3 lightPositions[4] = vec3[4](
    vec3(-3.0, 3.0, 3.0),   // Upper left
    vec3(3.0, 3.0, 3.0),    // Upper right  
    vec3(-3.0, -3.0, 3.0),  // Lower left
    vec3(3.0, -3.0, 3.0)    // Lower right
);
vec3 lightColors[4] = vec3[4](
    vec3(300.0, 300.0, 300.0),    // White light
    vec3(300.0, 300.0, 300.0),    // White light
    vec3(300.0, 300.0, 300.0),    // White light  
    vec3(300.0, 300.0, 300.0)     // White light
);

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
	
    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
	
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
	
    return num / denom;
}


float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2  = GeometrySchlickGGX(NdotV, roughness);
    float ggx1  = GeometrySchlickGGX(NdotL, roughness);
	
    return ggx1 * ggx2;
}


vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec2 ParallaxMapping(vec2 texCoords, vec3 viewDir)
{ 
    const float height_scale = bump.heightScale;
    const float numLayers = bump.numLayers;  // More layers = better quality


    float layerDepth = 1.0 / numLayers;
    float currentLayerDepth = 0.0;
    vec2 P = viewDir.xy / viewDir.z * height_scale; 

    vec2 deltaTexCoords = P / numLayers;
    
    vec2 currentTexCoords = texCoords;
    float currentDepthMapValue = texture(heightMap, currentTexCoords).r;
  
    while(currentLayerDepth < currentDepthMapValue)
    {
        currentTexCoords -= deltaTexCoords;
        currentDepthMapValue = texture(heightMap, currentTexCoords).r;  
        currentLayerDepth += layerDepth;  
    }

    // get texture coordinates before collision (reverse operations)
    vec2 prevTexCoords = currentTexCoords + deltaTexCoords;

    // get depth after and before collision for linear interpolation
    float afterDepth  = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = texture(heightMap, prevTexCoords).r - currentLayerDepth + layerDepth;
 
    // interpolation of texture coordinates
    float weight = afterDepth / (afterDepth - beforeDepth);
    vec2 finalTexCoords = prevTexCoords * weight + currentTexCoords * (1.0 - weight);
    
    return finalTexCoords;
}


void main() 
{
    // Everything is already in tangent space
    vec3 viewDir = normalize(CameraPos - WorldPos);


    vec2 preUVs;

    if(bump.bumpMode == 0 || bump.bumpMode == 1)
    {
        preUVs = ParallaxMapping(inUVs, viewDir);
    }
    if(bump.bumpMode == 2 || bump.bumpMode == 3)
    {
        preUVs = inUVs;
    }
    vec3 albedo = pow(texture(albedoMap, preUVs).rgb, vec3(2.2));

    //float metallic  = texture(metallicMap, preUVs).r;
    //float roughness = texture(roughnessMap, preUVs).r;
    //float ao        = texture(aoMap, preUVs).r;
    float metallic  = 0.0;  // Default: non-metallic
    float roughness = 0.5;  // Default: medium roughness
    float ao        = 1.0;  // Default: no occlusion

    // bump mode 0 should be both normal map and parallax
    // bump mode 1 only parallax
    // bump mode 2 only normal map
    // bump mode 3 neither parallax or normal map
    vec3 N;
    if(bump.bumpMode == 0 || bump.bumpMode == 2)
    {
        N = texture(normalMap, preUVs).rgb;
        N = normalize(N * 2.0 - 1.0);
    }
    if(bump.bumpMode == 1 || bump.bumpMode == 3)
    {
        // straight up normal since in tangent space
        N = vec3(0.0, 0.0, 1.0);
    }

    
    
    vec3 V = normalize(CameraPos - WorldPos);
    
    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);
    
    vec3 Lo = vec3(0.0);
    for(int i = 0; i < 4; ++i) 
    {
        // Transform light position to tangent space
        vec3 lightPosTangent = tbnMatrix * lightPositions[i];
        
        vec3 L = normalize(lightPosTangent - WorldPos);
        vec3 H = normalize(V + L);
        float distance = length(lightPosTangent - WorldPos);
        float attenuation = 1.0 / (distance * distance);
        vec3 radiance     = lightColors[i] * attenuation;        
        
        // cook-torrance brdf
        float NDF = DistributionGGX(N, H, roughness);        
        float G   = GeometrySmith(N, V, L, roughness);      
        vec3 F    = fresnelSchlick(max(dot(H, V), 0.0), F0);       
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;	  
        
        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular     = numerator / denominator;  
            
        // add to outgoing radiance Lo
        float NdotL = max(dot(N, L), 0.0);                
        Lo += (kD * albedo / PI + specular) * radiance * NdotL; 
    }   
  
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;
	
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0/2.2));  
   
    outFragColor = vec4(color, 1.0);
}