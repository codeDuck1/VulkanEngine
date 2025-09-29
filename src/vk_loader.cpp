
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
                        vertices[initial_vtx + index] = newvtx;
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
            newmesh.surfaces.push_back(newSurface);
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
