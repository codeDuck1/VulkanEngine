#version 450
#extension GL_EXT_buffer_reference : require // for buffer references

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;

struct Vertex {

	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
}; 

// TELLS PUSH CONSTANT HOW TO INTERPRET GPU MEMORY:
layout(buffer_reference, std430) readonly buffer VertexBuffer{ 
	Vertex vertices[];
};

//push constants block
layout( push_constant ) uniform constants
{	
	mat4 render_matrix;
	VertexBuffer vertexBuffer; // handle to gpu memory, use Vertexbuffer declaration above to interpret it
} PushConstants;

void main() 
{	
	//load vertex data from device address
	// PushConstants.vertexBuffer contains gpu memory address
	Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];

	//output data
	gl_Position = PushConstants.render_matrix *vec4(v.position, 1.0f);
	outColor = v.color.xyz;
	outUV.x = v.uv_x;
	outUV.y = v.uv_y;
}


// upload code in engine puts data into gpu memory, and push_constant passes
// memory address to shader so it may access that memory (read-only)