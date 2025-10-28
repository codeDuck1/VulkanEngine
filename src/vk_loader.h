#pragma once

#include <vk_types.h>
#include <vk_descriptors.h> 
#include <unordered_map>
#include <filesystem>

//forward declaration
class VulkanEngine;

struct GLTFMaterial {
    MaterialInstance data;
};

// believe this contains indices for each submesh/primitve
// one per submesh/primitive
struct GeoSurface {
    uint32_t startIndex;
    uint32_t count;
    std::shared_ptr<GLTFMaterial> material;
};

// given mesh will have a name, and mesh buffer
// array of geosurfaces that has the sub-meshes of this mesh
// each submesh will be its own draw
struct MeshAsset {
    std::string name;

    std::vector<GeoSurface> surfaces;
    GPUMeshBuffers meshBuffers;
};

struct LoadedGLTF : public IRenderable {

    // storage for all the data on a given glTF file
    std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
    std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
    std::unordered_map<std::string, AllocatedImage> images;
    std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;

    // nodes that dont have a parent, for iterating through the file in tree order
    std::vector<std::shared_ptr<Node>> topNodes;

    std::vector<VkSampler> samplers;

    DescriptorAllocatorGrowable descriptorPool;

    AllocatedBuffer materialDataBuffer;

    VulkanEngine* creator;

    ~LoadedGLTF() { clearAll(); }

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override;

    private:

    void clearAll();
};




// standard class that wraps a type, vector of mesh assets and allows for it to be errored/null
// since file loading can fail for many reasons
std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath);
std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanEngine* engine, std::string_view filePath);

AllocatedImage load_image_from_file(VulkanEngine* engine, std::filesystem::path filePath, bool mipmapped);
AllocatedImage load_cubemap_from_files(VulkanEngine* engine, std::string paths[6]);
AllocatedImage load_cubemap_from_files_hdr(VulkanEngine* engine, std::string paths[6]);

