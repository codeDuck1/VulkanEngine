#version 450

layout (location = 0) in vec3 inTexCoordDir;

layout(set = 0, binding = 0) uniform samplerCube skybox;

layout(location = 0) out vec4 outColor;

void main() 
{
    vec3 color = textureLod(skybox, inTexCoordDir, 0.0).rgb;
    
    // Much higher exposure for bright skybox
    float exposure = 0.8;  // Try values between 0.1 - 2.0
    color *= exposure;
    
    // Reinhard tone mapping, to convert hdr vals to ldr for non-hdr monitor
    color = color / (color + vec3(1.0));
    
    // Gamma correction for final display LINEAR TO NONLINEAR TO PLEASE OUR EYES!
    color = pow(color, vec3(1.0/2.2));
    
    outColor = vec4(color, 1.0);
}