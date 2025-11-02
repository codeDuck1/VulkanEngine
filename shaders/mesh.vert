#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#include "input_structures.glsl"
layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outWorldPos;
layout (location = 4) out mat3 outTBN;
layout (location = 7) out vec3 outViewPos;
/// light positions in tangent space
layout (location = 8) out vec3 outLightPos[4];
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
layout( push_constant ) uniform constants
{
    mat4 modelMatrix;
    VertexBuffer vertexBuffer; // handle to gpu memory, use Vertexbuffer declaration above to interpret it
} PushConstants;
const vec3 lightPositions[4] = vec3[4](
    vec3(-1.0, 1.0, 1.0),   // Much closer
    vec3(1.0, 1.0, 1.0),    
    vec3(-1.0, -1.0, 1.0),  
    vec3(1.0, -1.0, 1.0)    
);
void main() 
{
    Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];

    // transform position to world space then to clip space
    vec4 worldPos = PushConstants.modelMatrix * vec4(v.position, 1.0f);
    gl_Position = sceneData.viewproj * worldPos;

    // matrix for non-uniform scaling that causes normal vecs not to be perpend to surface anymore
    mat3 normalMatrix = mat3(transpose(inverse(PushConstants.modelMatrix)));

    // Transform TBN vectors to world space
    vec3 T = normalize(normalMatrix * v.tangent.xyz);
    vec3 B = normalize(normalMatrix * v.bitangent.xyz);
    vec3 N = normalize(normalMatrix * v.normal);

    // Build TBN matrix (tangent space to world space)
    outTBN = mat3(T, B, N);

    // World to tangent space transform matrix
    mat3 inverseTBN = transpose(mat3(T, B, N)); // for orthogonal matrices, their inverse=transpose

    // transform all relevant lighting variables to tangent space
    // with this approach, no need to transform vecs to tangent space in frag 
    for(int i = 0; i < 4; ++i) {
        outLightPos[i] = inverseTBN * lightPositions[i];
    }
    outNormal = vec3(0.0, 0.0, 1.0);
    outColor = v.color.xyz * materialData.colorFactors.xyz;
    outUV.x = v.uv_x;
    outUV.y = v.uv_y;
    outWorldPos = inverseTBN * worldPos.xyz;
    outViewPos = inverseTBN * sceneData.cameraPos.xyz;
}