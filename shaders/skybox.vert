#version 450
#extension GL_EXT_buffer_reference : require

layout (location = 0) out vec3 outTexCoordDir;

struct Vertex {
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
	vec4 tangent;    
    vec4 bitangent;
}; 

layout(buffer_reference, std430) readonly buffer VertexBuffer{ 
	Vertex vertices[];
};



layout(push_constant) uniform constants
{	
	mat4 viewProj;  // Combined view-projection matrix
	VertexBuffer vertexBuffer;  // GPU memory address for cube vertices
} PushConstants;

void main() 
{	
	// Load vertex data from device address
	Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
	
	// Use vertex position as cubemap sampling direction
	outTexCoordDir = v.position;
	
	// Transform cube vertex to clip space
	gl_Position = PushConstants.viewProj * vec4(v.position, 1.0);
	// we using reversed depth so set z to 0.0 (far plane)
	gl_Position.z = 0.0;
}