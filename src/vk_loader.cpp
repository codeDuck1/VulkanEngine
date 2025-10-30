
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

void calculateTangents(std::vector<VertexOG>& vertices, const std::vector<uint32_t>& indices); // forward declaration
VkFilter extract_filter(fastgltf::Filter filter);
VkSamplerMipmapMode extract_mipmap_mode(fastgltf::Filter filter);

std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanEngine* engine, std::string_view filePath)
{
    fmt::print("Loading GLTF: {}", filePath);

    std::shared_ptr<LoadedGLTF> scene = std::make_shared<LoadedGLTF>();
    scene->creator = engine;
    LoadedGLTF& file = *scene.get();

    fastgltf::Parser parser{};

    constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;

    fastgltf::GltfDataBuffer data;
    data.loadFromFile(filePath);

    fastgltf::Asset gltf;

    std::filesystem::path path = filePath;

    auto type = fastgltf::determineGltfFileType(&data);
    if (type == fastgltf::GltfType::glTF) {
        auto load = parser.loadGLTF(&data, path.parent_path(), gltfOptions);
        if (load) {
            gltf = std::move(load.get());
        }
        else {
            std::cerr << "Failed to load glTF: " << fastgltf::to_underlying(load.error()) << std::endl;
            return {};
        }
    }
    else if (type == fastgltf::GltfType::GLB) {
        auto load = parser.loadBinaryGLTF(&data, path.parent_path(), gltfOptions);
        if (load) {
            gltf = std::move(load.get());
        }
        else {
            std::cerr << "Failed to load glTF: " << fastgltf::to_underlying(load.error()) << std::endl;
            return {};
        }
    }
    else {
        std::cerr << "Failed to determine glTF container" << std::endl;
        return {};
    }

    // Updated descriptor pool sizes for 5 image samplers per material
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 },  // Changed from 3 to 5
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }
    };

    file.descriptorPool.init(engine->_device, gltf.materials.size(), sizes);

    // load samplers
    for (fastgltf::Sampler& sampler : gltf.samplers) {
        VkSamplerCreateInfo sampl = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = nullptr };
        sampl.maxLod = VK_LOD_CLAMP_NONE;
        sampl.minLod = 0;

        sampl.magFilter = extract_filter(sampler.magFilter.value_or(fastgltf::Filter::Nearest));
        sampl.minFilter = extract_filter(sampler.minFilter.value_or(fastgltf::Filter::Nearest));
        sampl.mipmapMode = extract_mipmap_mode(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

        VkSampler newSampler;
        vkCreateSampler(engine->_device, &sampl, nullptr, &newSampler);

        file.samplers.push_back(newSampler);
    }

    // temporal arrays for all the objects to use while creating the GLTF data
    std::vector<std::shared_ptr<MeshAsset>> meshes;
    std::vector<std::shared_ptr<Node>> nodes;
    std::vector<AllocatedImage> images;
    std::vector<std::shared_ptr<GLTFMaterial>> materials;

    // load all textures
    for (fastgltf::Image& image : gltf.images) {
        std::optional<AllocatedImage> img = load_image(engine, gltf, image);

        if (img.has_value()) {
            images.push_back(*img);
            file.images[image.name.c_str()] = *img;
        }
        else {
            images.push_back(engine->_errorCheckerboardImage);
            std::cout << "gltf failed to load texture " << image.name << std::endl;
        }
    }

    // create buffer to hold the material data
    file.materialDataBuffer = engine->create_buffer(sizeof(GLTFMetallic_Roughness::MaterialConstants) * gltf.materials.size(),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    int data_index = 0;
    GLTFMetallic_Roughness::MaterialConstants* sceneMaterialConstants = (GLTFMetallic_Roughness::MaterialConstants*)file.materialDataBuffer.info.pMappedData;

    // load materials
    for (fastgltf::Material& mat : gltf.materials) {
        std::shared_ptr<GLTFMaterial> newMat = std::make_shared<GLTFMaterial>();
        materials.push_back(newMat);
        file.materials[mat.name.c_str()] = newMat;

        GLTFMetallic_Roughness::MaterialConstants constants;
        constants.colorFactors.x = mat.pbrData.baseColorFactor[0];
        constants.colorFactors.y = mat.pbrData.baseColorFactor[1];
        constants.colorFactors.z = mat.pbrData.baseColorFactor[2];
        constants.colorFactors.w = mat.pbrData.baseColorFactor[3];

        constants.metal_rough_factors.x = mat.pbrData.metallicFactor;
        constants.metal_rough_factors.y = mat.pbrData.roughnessFactor;

        // default emissive texture
        constants.emissiveFactor.x = mat.emissiveFactor[0];
        constants.emissiveFactor.y = mat.emissiveFactor[1];
        constants.emissiveFactor.z = mat.emissiveFactor[2];
        constants.emissiveFactor.w = 0.0f; // unused

        // default optional textures as unused
        constants.textureFlags = glm::vec4(0.0f);

        // write material parameters to buffer
        sceneMaterialConstants[data_index] = constants;

        MaterialPass passType = MaterialPass::MainColor;
        if (mat.alphaMode == fastgltf::AlphaMode::Blend) {
            passType = MaterialPass::Transparent;
        }

        GLTFMetallic_Roughness::MaterialResources materialResources;

        // Default all textures to engine defaults
        materialResources.colorImage = engine->_whiteImage;
        materialResources.colorSampler = engine->_defaultSamplerLinear;
        materialResources.metalRoughImage = engine->_whiteImage;
        materialResources.metalRoughSampler = engine->_defaultSamplerLinear;

        // default textures
        materialResources.normalImage = engine->_defaultNormalImage;
        materialResources.normalSampler = engine->_defaultSamplerLinear;
        materialResources.occlusionImage = engine->_whiteImage;
        materialResources.occlusionSampler = engine->_defaultSamplerLinear;
        materialResources.emissiveImage = engine->_blackImage;
        materialResources.emissiveSampler = engine->_defaultSamplerLinear;

        // set the uniform buffer for the material data
        materialResources.dataBuffer = file.materialDataBuffer.buffer;
        materialResources.dataBufferOffset = data_index * sizeof(GLTFMetallic_Roughness::MaterialConstants);

        // Load base color texture
        if (mat.pbrData.baseColorTexture.has_value()) {
            size_t img = gltf.textures[mat.pbrData.baseColorTexture.value().textureIndex].imageIndex.value();
            size_t sampler = gltf.textures[mat.pbrData.baseColorTexture.value().textureIndex].samplerIndex.value();

            materialResources.colorImage = images[img];
            materialResources.colorSampler = file.samplers[sampler];
        }

        // Load metallic-roughness texture
        if (mat.pbrData.metallicRoughnessTexture.has_value()) {
            size_t img = gltf.textures[mat.pbrData.metallicRoughnessTexture.value().textureIndex].imageIndex.value();
            size_t sampler = gltf.textures[mat.pbrData.metallicRoughnessTexture.value().textureIndex].samplerIndex.value();

            materialResources.metalRoughImage = images[img];
            materialResources.metalRoughSampler = file.samplers[sampler];
        }

        // Load normal texture
        if (mat.normalTexture.has_value()) {
            size_t img = gltf.textures[mat.normalTexture.value().textureIndex].imageIndex.value();
            size_t sampler = gltf.textures[mat.normalTexture.value().textureIndex].samplerIndex.value();

            materialResources.normalImage = images[img];
            materialResources.normalSampler = file.samplers[sampler];
            constants.textureFlags.x = 1.0f; // Flag: has normal map
        }

        // Load occlusion texture
        if (mat.occlusionTexture.has_value()) {
            size_t img = gltf.textures[mat.occlusionTexture.value().textureIndex].imageIndex.value();
            size_t sampler = gltf.textures[mat.occlusionTexture.value().textureIndex].samplerIndex.value();
            materialResources.occlusionImage = images[img];
            materialResources.occlusionSampler = file.samplers[sampler];
            constants.textureFlags.y = 1.0f; // Flag: has occlusion map
        }

        // Load emissive texture
        if (mat.emissiveTexture.has_value()) {
            size_t img = gltf.textures[mat.emissiveTexture.value().textureIndex].imageIndex.value();
            size_t sampler = gltf.textures[mat.emissiveTexture.value().textureIndex].samplerIndex.value();

            materialResources.emissiveImage = images[img];
            materialResources.emissiveSampler = file.samplers[sampler];
            constants.textureFlags.z = 1.0f; // Flag: has emissive map
        }

        fmt::println("Writing material '{}' to buffer with textureFlags: ({}, {}, {}, {})",
            mat.name,
            constants.textureFlags.x,
            constants.textureFlags.y,
            constants.textureFlags.z,
            constants.textureFlags.w);
        // Update the material constants with texture flags
        sceneMaterialConstants[data_index] = constants;

        // build material
        newMat->data = engine->metalRoughMaterial.write_material(engine->_device, passType, materialResources, file.descriptorPool);

        data_index++;
    }

    // load meshes
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;

    for (fastgltf::Mesh& mesh : gltf.meshes) {
        std::shared_ptr<MeshAsset> newmesh = std::make_shared<MeshAsset>();
        meshes.push_back(newmesh);
        file.meshes[mesh.name.c_str()] = newmesh;
        newmesh->name = mesh.name;

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

            if (p.materialIndex.has_value()) {
                newSurface.material = materials[p.materialIndex.value()];
            }
            else {
                newSurface.material = materials[0];
            }

            newmesh->surfaces.push_back(newSurface);
        }

        newmesh->meshBuffers = engine->uploadMesh(indices, vertices);
    }

    // load all nodes and their meshes
    for (fastgltf::Node& node : gltf.nodes) {
        std::shared_ptr<Node> newNode;

        if (node.meshIndex.has_value()) {
            newNode = std::make_shared<MeshNode>();
            static_cast<MeshNode*>(newNode.get())->mesh = meshes[*node.meshIndex];
        }
        else {
            newNode = std::make_shared<Node>();
        }

        nodes.push_back(newNode);
        file.nodes[node.name.c_str()];

        std::visit(fastgltf::visitor{
            [&](fastgltf::Node::TransformMatrix matrix) {
                memcpy(&newNode->localTransform, matrix.data(), sizeof(matrix));
            },
            [&](fastgltf::Node::TRS transform) {
                glm::vec3 tl(transform.translation[0], transform.translation[1], transform.translation[2]);
                glm::quat rot(transform.rotation[3], transform.rotation[0], transform.rotation[1], transform.rotation[2]);
                glm::vec3 sc(transform.scale[0], transform.scale[1], transform.scale[2]);

                glm::mat4 tm = glm::translate(glm::mat4(1.f), tl);
                glm::mat4 rm = glm::toMat4(rot);
                glm::mat4 sm = glm::scale(glm::mat4(1.f), sc);

                newNode->localTransform = tm * rm * sm;
            }
            }, node.transform);
    }

    // run loop again to setup transform hierarchy
    for (int i = 0; i < gltf.nodes.size(); i++) {
        fastgltf::Node& node = gltf.nodes[i];
        std::shared_ptr<Node>& sceneNode = nodes[i];

        for (auto& c : node.children) {
            sceneNode->children.push_back(nodes[c]);
            nodes[c]->parent = sceneNode;
        }
    }

    // find the top nodes, with no parents
    for (auto& node : nodes) {
        if (node->parent.lock() == nullptr) {
            file.topNodes.push_back(node);
            node->refreshTransform(glm::mat4{ 1.f });
        }
    }

    return scene;
}



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
    std::vector<VertexOG> vertices;
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
                        VertexOG newvtx;
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
         //Only calculate tangents if not in the GLTF file
        if (!hasTangents) {
            calculateTangents(vertices, indices);
            fmt::print("Calculated tangents for mesh: {}\n", newmesh.name);
        }
        else {
            fmt::print("Using tangents from GLTF for mesh: {}\n", newmesh.name);
        }

        // display the vertex normals
        // constexpr means can figure our while compiling, not while running! (baked in value)
        // while const mean wont change, but variable might be detrermined at runtime
        constexpr bool OverrideColors = false;
        if (OverrideColors) {
            for (VertexOG& vtx : vertices) {
                vtx.color = glm::vec4(vtx.normal, 1.f);
            }
        }

       // fmt::print("Vertex size with manual padding: {}\n", sizeof(Vertex));

        // upload mesh data to gpu buffers
        newmesh.meshBuffers = engine->uploadMeshOG(indices, vertices);
        // moves newmesh into MeshAsset. vec will contain ptr to MeshAsset objects and owns MeshAsset object
        meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newmesh)));
    }

    return meshes;
}


std::optional<AllocatedImage> load_image(VulkanEngine* engine, fastgltf::Asset& asset, fastgltf::Image& image)
{
        AllocatedImage newImage{};

        int width, height, nrChannels;

        std::visit(
            fastgltf::visitor{
                [](auto& arg) {},
                [&](fastgltf::sources::URI& filePath) {
                    assert(filePath.fileByteOffset == 0); // We don't support offsets with stbi.
                    assert(filePath.uri.isLocalPath()); // We're only capable of loading
                    // local files.

    const std::string path(filePath.uri.path().begin(),
        filePath.uri.path().end()); // Thanks C++.
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrChannels, 4);
    if (data) {
        VkExtent3D imagesize;
        imagesize.width = width;
        imagesize.height = height;
        imagesize.depth = 1;

        newImage = engine->create_image(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,false);

        stbi_image_free(data);
    }
    },
    [&](fastgltf::sources::Vector& vector) {
        unsigned char* data = stbi_load_from_memory(vector.bytes.data(), static_cast<int>(vector.bytes.size()),
            &width, &height, &nrChannels, 4);
        if (data) {
            VkExtent3D imagesize;
            imagesize.width = width;
            imagesize.height = height;
            imagesize.depth = 1;

            newImage = engine->create_image(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,false);

            stbi_image_free(data);
        }
    },
    [&](fastgltf::sources::BufferView& view) {
        auto& bufferView = asset.bufferViews[view.bufferViewIndex];
        auto& buffer = asset.buffers[bufferView.bufferIndex];

        std::visit(fastgltf::visitor { // We only care about VectorWithMime here, because we
            // specify LoadExternalBuffers, meaning all buffers
            // are already loaded into a vector.
    [](auto& arg) {},
    [&](fastgltf::sources::Vector& vector) {
        unsigned char* data = stbi_load_from_memory(vector.bytes.data() + bufferView.byteOffset,
            static_cast<int>(bufferView.byteLength),
            &width, &height, &nrChannels, 4);
        if (data) {
            VkExtent3D imagesize;
            imagesize.width = width;
            imagesize.height = height;
            imagesize.depth = 1;

            newImage = engine->create_image(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT,false);

            stbi_image_free(data);
        }
    } },
    buffer.data);
    },
            },
            image.data);

        // if any of the attempts to load the data failed, we havent written the image
        // so handle is null
        if (newImage.image == VK_NULL_HANDLE) {
            return {};
        }
        else {
            return newImage;
        }
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
    float* cubemapData[6];
    int width, height, channels;

    for (int i = 0; i < 6; i++) {
        cubemapData[i] = stbi_loadf(paths[i].c_str(), &width, &height, &channels, 4);
        if (!cubemapData[i]) {
            fmt::print("Failed to load cubemap face: {}\n", paths[i]);
        }
    }

    // Rest of your original code...
    VkExtent3D imageSize = { (uint32_t)width, (uint32_t)height, 1 };
    void* cubemapDataVoid[6];
    for (int i = 0; i < 6; i++) {
        cubemapDataVoid[i] = cubemapData[i];
    }

    AllocatedImage cubemap = engine->create_cubemap_hdr(
        cubemapDataVoid,
        imageSize,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        true
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
void calculateTangents(std::vector<VertexOG>& vertices, const std::vector<uint32_t>& indices)
{
    std::vector<glm::vec3> tangents(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertices.size(), glm::vec3(0.0f));
    // Process each triangle
    for (size_t i = 0; i < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        VertexOG& v0 = vertices[i0];
        VertexOG& v1 = vertices[i1];
        VertexOG& v2 = vertices[i2];
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


// opengl gltf sampler converters
VkFilter extract_filter(fastgltf::Filter filter)
{
    switch (filter) {
        // nearest samplers
    case fastgltf::Filter::Nearest:
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::NearestMipMapLinear:
        return VK_FILTER_NEAREST;

        // linear samplers
    case fastgltf::Filter::Linear:
    case fastgltf::Filter::LinearMipMapNearest:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode extract_mipmap_mode(fastgltf::Filter filter)
{
    switch (filter) {
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::LinearMipMapNearest:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;

    case fastgltf::Filter::NearestMipMapLinear:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

void LoadedGLTF::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
    // create renderables from the scenenodes
    // draw funct only loops top nodes and calls draw, which propagates to their children
    for (auto& n : topNodes) {
        n->Draw(topMatrix, ctx);
    }
}

void LoadedGLTF::clearAll()
{
    VkDevice dv = creator->_device;

    descriptorPool.destroy_pools(dv);
    creator->destroy_buffer(materialDataBuffer);

    for (auto& [k, v] : meshes) {

        creator->destroy_buffer(v->meshBuffers.indexBuffer);
        creator->destroy_buffer(v->meshBuffers.vertexBuffer);
    }

    for (auto& [k, v] : images) {

        if (v.image == creator->_errorCheckerboardImage.image) {
            //dont destroy the default images
            continue;
        }
        creator->destroy_image(v);
    }

    for (auto& sampler : samplers) {
        vkDestroySampler(dv, sampler, nullptr);
    }
}
