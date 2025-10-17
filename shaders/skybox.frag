#version 450

layout (location = 0) in vec3 inTexCoordDir;

layout(set = 0, binding = 0) uniform samplerCube skybox;

layout(location = 0) out vec4 outColor;

void main() 
{
    // sample cubemap using 3d direction
    outColor = texture(skybox, inTexCoordDir);

    //outColor = textureLod(skybox, inTexCoordDir, 3.0);
}