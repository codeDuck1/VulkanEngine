#pragma once

#include <vk_types.h>
#include <unordered_map>
#include <filesystem>



// believe this contains indices for each submesh/primitve
// one per submesh/primitive
struct GeoSurface {
    uint32_t startIndex;
    uint32_t count;
};

// given mesh will have a name, and mesh buffer
// array of geosurfaces that has the sub-meshes of this mesh
// each submesh will be its own draw
struct MeshAsset {
    std::string name;

    std::vector<GeoSurface> surfaces;
    GPUMeshBuffers meshBuffers;
};


//forward declaration
class VulkanEngine;

// standard class that wraps a type, vector of mesh assets and allows for it to be errored/null
// since file loading can fail for many reasons
std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath);

AllocatedImage load_image_from_file(VulkanEngine* engine, std::filesystem::path filePath, bool mipmapped);
AllocatedImage load_cubemap_from_files(VulkanEngine* engine, std::string paths[6]);
AllocatedImage load_cubemap_from_files_hdr(VulkanEngine* engine, std::string paths[6]);

