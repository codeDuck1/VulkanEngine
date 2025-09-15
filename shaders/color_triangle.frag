#version 450

//shader input
layout (location = 0) in vec3 inColor;

//output write
// connects to the render attachments of render pass
layout (location = 0) out vec4 outFragColor;

void main() 
{
	//return red
	outFragColor = vec4(inColor,1.0f);
}