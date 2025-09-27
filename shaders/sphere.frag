#version 450

//shader input
layout (location = 0) in vec3 inColor;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 CameraPos;
layout (location = 3) in vec3 Normal;
layout (location = 4) in vec3 WorldPos;

// output write
// connects to the render attachments of render pass
layout (location = 0) out vec4 outFragColor;

// texture to access
layout(set =0, binding = 0) uniform sampler2D displayTexture;

void main() 
{

    //outFragColor = vec4(1.0, 1.0, 1.0, 1.0);
	outFragColor = texture(displayTexture, inUV);
	
}