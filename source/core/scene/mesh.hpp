#pragma once

#include <array>
//#include <gli/gli.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include <vector>

#include "core/scene/material.hpp"
#include "core/scene/texture.hpp"
#include "core/scene/texture_sampler.hpp"
#include "render/vulkan/vulkan_graphics_pipeline.hpp"
#include "render/vulkan_include.hpp"

namespace tinygltf
{
class Node;
class Model;
}  // namespace tinygltf

namespace ez
{
struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv0;
    glm::vec2 uv1;

    static vk::VertexInputBindingDescription getBindingDescription()
    {
        vk::VertexInputBindingDescription bindingDescription = {};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = vk::VertexInputRate::eVertex;

        return bindingDescription;
    }

    static std::array<vk::VertexInputAttributeDescription, 4> getAttributeDescriptions()
    {
        std::array<vk::VertexInputAttributeDescription, 4> attributeDescriptions = {};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = vk::Format::eR32G32B32Sfloat;
        attributeDescriptions[0].offset = offsetof(Vertex, position);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = vk::Format::eR32G32B32Sfloat;
        attributeDescriptions[1].offset = offsetof(Vertex, normal);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = vk::Format::eR32G32Sfloat;
        attributeDescriptions[2].offset = offsetof(Vertex, uv0);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = vk::Format::eR32G32Sfloat;
        attributeDescriptions[3].offset = offsetof(Vertex, uv1);

        return attributeDescriptions;
    }
};

struct BoundingBox
{
    BoundingBox() {}
    BoundingBox(glm::vec3 min, glm::vec3 max) : min(min), max(max) {}
    BoundingBox GetAABB(glm::mat4 m);

    glm::vec3 min;
    glm::vec3 max;
    bool valid = false;
};

struct Primitive
{
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t vertexCount;
    bool hasIndices;

    BoundingBox bb;
    Material& material;

    Primitive(uint32_t firstIndex,
              uint32_t indexCount,
              uint32_t vertexCount,
              Material& material)
        : firstIndex(firstIndex)
        , indexCount(indexCount)
        , vertexCount(vertexCount)
        , material(material)
    {
        hasIndices = indexCount > 0;
    }

    void SetBoundingBox(glm::vec3 min, glm::vec3 max)
    {
        bb.min = min;
        bb.max = max;
        bb.valid = true;
    }
};

struct Mesh
{
    vk::Device device;

    std::vector<std::unique_ptr<Primitive>> primitives;

    BoundingBox bb;
    BoundingBox aabb;

    struct PushConstantsBlock final
    {
        glm::mat4 modelMatrix = glm::mat4(1.0f);
    } pushConstantsBlock;
    static constexpr uint32_t PushConstantsBlockSize = sizeof(Mesh::PushConstantsBlock);

    struct UniformBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        vk::DescriptorBufferInfo descriptor;
        vk::DescriptorSet descriptorSet;
        void* mapped;
    } uniformBuffer;

    Mesh(const glm::mat4& matrix) { this->pushConstantsBlock.modelMatrix = matrix; }

    void SetBoundingBox(glm::vec3 min, glm::vec3 max)
    {
        bb.min = min;
        bb.max = max;
        bb.valid = true;
    }
};

struct Node
{
    Node* parent = nullptr;
    uint32_t index;
    std::vector<std::unique_ptr<Node>> children;
    glm::mat4 matrix;
    std::string name;
    std::unique_ptr<Mesh> mesh;
    glm::vec3 translation{};
    glm::vec3 scale{ 1.0f };
    glm::quat rotation{};
    BoundingBox aabb;

    inline glm::mat4 ConstructLocalMatrix()
    {
        return glm::translate(glm::mat4(1.0f), translation) * glm::mat4(rotation) *
               glm::scale(glm::mat4(1.0f), scale) * matrix;
    }

    inline glm::mat4 GetMatrix()
    {
        glm::mat4 m = ConstructLocalMatrix();
        Node* p = parent;
        while (p)
        {
            m = p->ConstructLocalMatrix() * m;
            p = p->parent;
        }
        return m;
    }

    void Update()
    {
        if (mesh) { mesh->pushConstantsBlock.modelMatrix = glm::mat4(1.0f); /*GetMatrix()*/ }

        for (auto& child : children) { child->Update(); }
    }
};

struct Model
{
    Model() = delete;
    Model(const Model& other) = delete;

    Model(const std::string& gltfFilePath);
    Model(Model&& other) = default;
    ~Model();

    void SetLogicalDevice(vk::Device device) { logicalDevice = device; }
    bool CreateVertexBuffers(vk::PhysicalDevice physicalDevice,
                             vk::Queue graphicsQueue,
                             vk::CommandPool graphicsCommandPool);
    bool CreateDescriptorSet(vk::DescriptorPool descriptorPool,
                             vk::DescriptorSetLayout aDescriptorSetLayout,
                             size_t hardcodedGlobalUBOSize);

    vk::DescriptorSetLayout GetDescriptorSetLayout() const { return descriptorSetLayout; }

    std::string name;
    std::vector<std::unique_ptr<Node>> nodes;

    vk::Buffer vertexBuffer;
    vk::Buffer indexBuffer;
    vk::Buffer uniformBuffer;

    vk::DeviceMemory uniformBufferMemory;

    vk::DescriptorSetLayout descriptorSetLayout;
    vk::DescriptorSet descriptorSet;
    std::shared_ptr<VulkanGraphicsPipeline> graphicsPipeline;

    std::vector<TextureSampler> textureSamplers;
    std::vector<Texture> textures;
    std::vector<Material> materials;

   private:
    void LoadNodeFromGLTF(Node* parent,
                          const tinygltf::Node& node,
                          uint32_t nodeIndex,
                          const tinygltf::Model& model,
                          std::vector<uint32_t>& indexBuffer,
                          std::vector<Vertex>& vertexBuffer);

    void LoadMaterials(tinygltf::Model& model);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    vk::Device logicalDevice;

    vk::DeviceMemory vertexBufferMemory;
    vk::DeviceMemory indexBufferMemory;

    uint32_t uniformBufferMaxHackSize = 192;
};

}  // namespace ez
