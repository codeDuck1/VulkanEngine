#version 450
#extension GL_EXT_buffer_reference : require // for buffer references

layout (location = 0) out vec3 outColor;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outCameraPos;
layout (location = 3) out vec3 outNormal;
layout (location = 4) out vec3 outWorldPos;
// TREATED AS 3 VEC 3 SLOTS, thus using locations 5,6,7
layout (location = 5) out mat3 tbnMatrix; 

struct Vertex {

	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;

	vec4 tangent;
	vec4 bitangent;
}; 

// TELLS PUSH CONSTANT HOW TO INTERPRET GPU MEMORY:
layout(buffer_reference, std430) readonly buffer VertexBuffer{ 
	Vertex vertices[];
};

//push constants block
layout( push_constant ) uniform constants
{	
	mat4 render_matrix;
	vec4 camera_pos;
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

	outCameraPos = PushConstants.camera_pos.xyz;
	outNormal = v.normal;
	outWorldPos = v.position;

	// create TBN matrix that transforms tangent-space vector to a diff coord space
	vec3 T = normalize(v.tangent.xyz);
	vec3 B = normalize(v.bitangent.xyz);
	vec3 N = normalize(v.normal.xyz);
	tbnMatrix = mat3(T, B, N);

}


// upload code in engine puts data into gpu memory, and push_constant passes
// memory address to shader so it may access that memory (read-only)