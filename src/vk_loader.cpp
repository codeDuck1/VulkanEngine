
#include <vk_loader.h>
#include "stb_image.h"
#include <iostream>
#include <vk_loader.h>

#define STB_IMAGE_IMPLEMENTATION  // include the actual function implementations, not just the declarations. needed only once per project
#include "stb_image.h"
#include "vk_engine.h"
#include "vk_initializers.h"
#include "vk_types.h"
#include <glm/gtx/quaternion.hpp>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>

void calculateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices); // forward declaration

std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath)
{
    std::cout << "Loading GLTF: " << filePath << std::endl;

    fastgltf::GltfDataBuffer data;
    data.loadFromFile(filePath);

    constexpr auto gltfOptions = fastgltf::Options::LoadGLBBuffers
        | fastgltf::Options::LoadExternalBuffers;

    fastgltf::Asset gltf;
    fastgltf::Parser parser{};

    auto load = parser.loadBinaryGLTF(&data, filePath.parent_path(), gltfOptions);
    if (load) {
        gltf = std::move(load.get());
    }
    else {
        fmt::print("Failed to load glTF: {} \n", fastgltf::to_underlying(load.error()));
        return {};
    }

    // loop through each mesh, copy vertices and indices, store in temp vectors
    // position array always there, other attributes need to check if data exists first

    std::vector<std::shared_ptr<MeshAsset>> meshes;

    // use the same vectors for all meshes so that the memory doesnt reallocate as
    // often
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;
    for (fastgltf::Mesh& mesh : gltf.meshes) {
        MeshAsset newmesh;

        newmesh.name = mesh.name;

        // clear the mesh arrays each mesh, we dont want to merge them by error
        indices.clear();
        vertices.clear();

        for (auto&& p : mesh.primitives) {
            GeoSurface newSurface;
            newSurface.startIndex = (uint32_t)indices.size();
            newSurface.count = (uint32_t)gltf.accessors[p.indicesAccessor.value()].count;

            size_t initial_vtx = vertices.size();

            // load indexes
            {
                fastgltf::Accessor& indexaccessor = gltf.accessors[p.indicesAccessor.value()];
                indices.reserve(indices.size() + indexaccessor.count);

                fastgltf::iterateAccessor<std::uint32_t>(gltf, indexaccessor,
                    [&](std::uint32_t idx) {
                        indices.push_back(idx + initial_vtx);
                    });
            }

            // load vertex positions
            {
                fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->second];
                vertices.resize(vertices.size() + posAccessor.count);

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor,
                    [&](glm::vec3 v, size_t index) {
                        Vertex newvtx;
                        newvtx.position = v;
                        newvtx.normal = { 1, 0, 0 };
                        newvtx.color = glm::vec4{ 1.f };
                        newvtx.uv_x = 0;
                        newvtx.uv_y = 0;
                        //newvtx.ok = 0.f;
                        newvtx.tangent = glm::vec4{ 0.f };
                        newvtx.bitangent = glm::vec4{ 0.f };
                        //newvtx.ok2 = 0.f;
                        vertices[initial_vtx + index] = newvtx; // storing newly created vertexx into pos in vertices array

                    });
            }

            // load vertex normals
            auto normals = p.findAttribute("NORMAL");
            if (normals != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).second],
                    [&](glm::vec3 v, size_t index) {
                        vertices[initial_vtx + index].normal = v;
                    });
            }

            // load UVs
            auto uv = p.findAttribute("TEXCOORD_0");
            if (uv != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uv).second],
                    [&](glm::vec2 v, size_t index) {
                        vertices[initial_vtx + index].uv_x = v.x;
                        vertices[initial_vtx + index].uv_y = v.y;
                    });
            }

            // load vertex colors
            auto colors = p.findAttribute("COLOR_0");
            if (colors != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*colors).second],
                    [&](glm::vec4 v, size_t index) {
                        vertices[initial_vtx + index].color = v;
                    });
            }


             // Load tangents if present in GLTF
            auto tangents = p.findAttribute("TANGENT");
            if (tangents != p.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*tangents).second],
                    [&](glm::vec4 v, size_t index) {
                        vertices[initial_vtx + index].tangent = glm::vec4(v.x, v.y, v.z, 1.0f);
                        // GLTF stores handedness in w component
                        // Bitangent = cross(normal, tangent) * handedness
                        glm::vec3 n = vertices[initial_vtx + index].normal;
                        glm::vec3 t = glm::vec3(v.x, v.y, v.z);
                        vertices[initial_vtx + index].bitangent = glm::vec4(glm::cross(n, t) * v.w, 0.0f);
                    });
            }
            newmesh.surfaces.push_back(newSurface);
        }
  

        //// Check if any tangents were loaded from GLTF
        bool hasTangents = false;
        for (const auto& vtx : vertices) {
            if (glm::length(vtx.tangent) > 0.001f) {
                hasTangents = true;
                break;
            }
        }
        // Only calculate tangents if not in the GLTF file
        if (!hasTangents) {
            calculateTangents(vertices, indices);
            fmt::print("Calculated tangents for mesh: {}\n", newmesh.name);
        }
        else {
            //fmt::print("Using tangents from GLTF for mesh: {}\n", newmesh.name);
        }

        // display the vertex normals
        // constexpr means can figure our while compiling, not while running! (baked in value)
        // while const mean wont change, but variable might be detrermined at runtime
        constexpr bool OverrideColors = true;
        if (OverrideColors) {
            for (Vertex& vtx : vertices) {
                vtx.color = glm::vec4(vtx.normal, 1.f);
            }
        }

       // fmt::print("Vertex size with manual padding: {}\n", sizeof(Vertex));

        // upload mesh data to gpu buffers
        newmesh.meshBuffers = engine->uploadMesh(indices, vertices);
        // moves newmesh into MeshAsset. vec will contain ptr to MeshAsset objects and owns MeshAsset object
        meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newmesh)));
    }

    return meshes;
}


AllocatedImage load_image_from_file(VulkanEngine* engine, std::filesystem::path filePath, bool mipmapped)
{
    // use stb lib to load image from file
    // texchannels how many channels original img had, but converted into rgba per last param
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(filePath.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

    if (!pixels) {
        fmt::print("Failed to load texture at: {}", filePath.string());
    }

    VkExtent3D imageSize;
    imageSize.width = texWidth;
    imageSize.height = texHeight;
    imageSize.depth = 1;

    // Use your existing function with the pixel data
    AllocatedImage image = engine->create_image(
        pixels,
        imageSize,
        // INPUT TEXTURE FORMAT (depends on how image was authored):
        // - Image authored in sRGB → VK_FORMAT_R8G8B8A8_SRGB (auto-converts non-linear to linear for use in shader calcs)
        // - Image authored linear → VK_FORMAT_R8G8B8A8_UNORM (no conversion needed)
        // - Image in sRGB but manual conversion → VK_FORMAT_R8G8B8A8_UNORM (you convert in shader)
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        mipmapped
    );

    // free image data
    stbi_image_free(pixels);

    return image;
}

AllocatedImage load_cubemap_from_files(VulkanEngine* engine, std::string paths[6])
{
    // Load all 6 faces using stbi_load
    void* cubemapData[6];
    int width, height, channels;

    for (int i = 0; i < 6; i++) {
        cubemapData[i] = stbi_load(paths[i].c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!cubemapData[i]) {
            fmt::print("Failed to load cubemap face: {}\n", paths[i]);
        }
    }

    VkExtent3D imageSize = { (uint32_t)width, (uint32_t)height, 1 };

    // Use the create_cubemap function that creates 1 VkImage with 6 layers
    AllocatedImage cubemap = engine->create_cubemap(
        cubemapData,
        imageSize,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT
    );

    // Free the pixel data
    for (int i = 0; i < 6; i++) {
        if (cubemapData[i]) {
            stbi_image_free(cubemapData[i]);
        }
    }

    return cubemap;
}

AllocatedImage load_cubemap_from_files_hdr(VulkanEngine* engine, std::string paths[6])
{
    // Load all 6 faces using stbi_loadf for HDR data
    float* cubemapData[6];
    int width, height, channels;

    for (int i = 0; i < 6; i++) {
        cubemapData[i] = stbi_loadf(paths[i].c_str(), &width, &height, &channels, 4);
        if (!cubemapData[i]) {
            fmt::print("Failed to load cubemap face: {}\n", paths[i]);
        }
    }

    VkExtent3D imageSize = { (uint32_t)width, (uint32_t)height, 1 };

    // Cast to void* for the create_cubemap function
    void* cubemapDataVoid[6]; // type agnostic
    for (int i = 0; i < 6; i++) {
        cubemapDataVoid[i] = cubemapData[i];
    }

    AllocatedImage cubemap = engine->create_cubemap_hdr(
        cubemapDataVoid,
        imageSize,
        VK_FORMAT_R32G32B32A32_SFLOAT,  // 32-bit float HDR format
        VK_IMAGE_USAGE_SAMPLED_BIT
    );

    for (int i = 0; i < 6; i++) {
        if (cubemapData[i]) {
            stbi_image_free(cubemapData[i]);
        }
    }

    return cubemap;
}




/// <summary>
/// Helper function to calculate tangents and bitangents for each triangle, used for
/// normal mapping (Generated by Claude).
/// </summary>
void calculateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));
    // Process each triangle
    for (size_t i = 0; i < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        Vertex& v0 = vertices[i0];
        Vertex& v1 = vertices[i1];
        Vertex& v2 = vertices[i2];
        // Calculate edges and delta UVs
        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        glm::vec2 deltaUV1 = glm::vec2(v1.uv_x - v0.uv_x, v1.uv_y - v0.uv_y);
        glm::vec2 deltaUV2 = glm::vec2(v2.uv_x - v0.uv_x, v2.uv_y - v0.uv_y);
        // Calculate tangent and bitangent for this triangle
        float det = deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y;
        if (std::abs(det) > 1e-6f) {
            float f = 1.0f / det;
            glm::vec3 tangent;
            tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
            tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
            tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
            glm::vec3 bitangent;
            bitangent.x = f * (-deltaUV2.x * edge1.x + deltaUV1.x * edge2.x);
            bitangent.y = f * (-deltaUV2.x * edge1.y + deltaUV1.x * edge2.y);
            bitangent.z = f * (-deltaUV2.x * edge1.z + deltaUV1.x * edge2.z);
            // Accumulate tangents and bitangents for each vertex of this triangle
            // Shared vertices will accumulate contributions from multiple triangles
            tangents[i0] += tangent;
            tangents[i1] += tangent;
            tangents[i2] += tangent;
            bitangents[i0] += bitangent;
            bitangents[i1] += bitangent;
            bitangents[i2] += bitangent;
        }
    }
    // Important: Gram-Schmidt orthogonalization MUST happen AFTER normalizing the accumulated vectors
    // When vertices are shared between triangles, their tangents get averaged (accumulation + normalize).
    // This averaging causes T, B, N to no longer be perfectly perpendicular (non-orthogonal).
    // Gram-Schmidt re-orthogonalizes them to ensure they're at perfect 90° angles again.
    // Without this step TBN matrix will be slightly off and normal mapping will look incorrect.
    for (size_t i = 0; i < vertices.size(); ++i) {
        glm::vec3 n = vertices[i].normal;
        glm::vec3 t = tangents[i];
        glm::vec3 b = bitangents[i];
        // Gram-Schmidt to make tangent perpendicular to normal
        t = glm::normalize(t - n * glm::dot(n, t));
        // Gram-Schmidt to make bitangent perpendicular to both normal and tangent
        // This ensures all three vectors are orthogonal
        b = glm::normalize(b - n * glm::dot(n, b) - t * glm::dot(t, b));

        // Store as vec4 with w = 0
        vertices[i].tangent = glm::vec4(t, 0.0f);
        vertices[i].bitangent = glm::vec4(b, 0.0f);
    }
}