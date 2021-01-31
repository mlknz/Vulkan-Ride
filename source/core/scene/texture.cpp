#include "texture.hpp"

#include "core/log_assert.hpp"
#include "render/graphics_result.hpp"
#include "render/vulkan/vulkan_buffer.hpp"
#include "render/vulkan/vulkan_image.hpp"

namespace ez
{
TextureCreationInfo TextureCreationInfo::CreateFromData(unsigned char* data,
                                                        uint32_t width,
                                                        uint32_t height,
                                                        uint32_t channelsCount,
                                                        const TextureSampler& textureSampler)
{
    TextureCreationInfo ci;

    ci.bufferSize = static_cast<vk::DeviceSize>(width * height * 4);
    ci.buffer.resize(ci.bufferSize);

    unsigned char* copyTo = ci.buffer.data();
    unsigned char* copyFrom = data;
    for (uint32_t i = 0; i < width * height; ++i)
    {
        for (uint32_t j = 0; j < channelsCount; ++j) { copyTo[j] = copyFrom[j]; }
        copyTo += 4;
        copyFrom += channelsCount;
    }

    ci.width = width;
    ci.height = height;
    ci.mipLevels =
        static_cast<uint32_t>(std::floor(std::log2(std::max(ci.width, ci.height))) + 1.0);

    ci.textureSampler = textureSampler;

    return ci;
}

bool TextureCreationInfo::IsValid() const
{
    return width > 0 && height > 0 && bufferSize > 0 && !buffer.empty();
}

bool Texture::LoadToGpu(vk::Device aLogicalDevice,
                        vk::PhysicalDevice physicalDevice,
                        vk::Queue graphicsQueue,
                        vk::CommandPool graphicsCommandPool)
{
    if (loadedToGpu)
    {
        EZASSERT(false, "Trying to load texture to GPU but it is already loaded");
        return true;
    }
    if (!creationInfo.IsValid())
    {
        EZASSERT(false, "Can't load Texture with incomplete TextureCreationInfo");
        return false;
    }
    // notice CPU texture data in creationInfo is not freed after load

    width = creationInfo.width;
    height = creationInfo.height;
    mipLevels = creationInfo.mipLevels;

    logicalDevice = aLogicalDevice;

    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    vk::FormatProperties formatProperties;

    physicalDevice.getFormatProperties(format, &formatProperties);

    EZASSERT(static_cast<bool>(formatProperties.optimalTilingFeatures &
                               vk::FormatFeatureFlagBits::eBlitSrc));
    EZASSERT(static_cast<bool>(formatProperties.optimalTilingFeatures &
                               vk::FormatFeatureFlagBits::eBlitDst));

    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingMemory;

    vk::BufferCreateInfo bufferCI{};
    bufferCI.setSize(creationInfo.bufferSize);
    bufferCI.setUsage(vk::BufferUsageFlags(vk::BufferUsageFlagBits::eTransferSrc));
    bufferCI.setSharingMode(vk::SharingMode::eExclusive);

    CheckVkResult(logicalDevice.createBuffer(&bufferCI, nullptr, &stagingBuffer));

    vk::MemoryRequirements memReqs{};
    vk::MemoryAllocateInfo memAllocInfo{};
    logicalDevice.getBufferMemoryRequirements(stagingBuffer, &memReqs);

    const uint32_t memoryTypeIndex = VulkanBuffer::FindMemoryType(
        physicalDevice,
        memReqs.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    memAllocInfo.setAllocationSize(memReqs.size);
    memAllocInfo.setMemoryTypeIndex(memoryTypeIndex);

    CheckVkResult(logicalDevice.allocateMemory(&memAllocInfo, nullptr, &stagingMemory));
    CheckVkResult(logicalDevice.bindBufferMemory(stagingBuffer, stagingMemory, 0));

    uint8_t* data;
    CheckVkResult(logicalDevice.mapMemory(
        stagingMemory, 0, memReqs.size, vk::MemoryMapFlags{}, reinterpret_cast<void**>(&data)));
    memcpy(data, creationInfo.buffer.data(), creationInfo.bufferSize);
    logicalDevice.unmapMemory(stagingMemory);

    // /////////////////////////////

    ResultValue<vk::Image> imageRV = Image::CreateImage2D(
        logicalDevice,
        format,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eSampled,
        creationInfo.mipLevels,
        width,
        height);
    if (imageRV.result != GraphicsResult::Ok) { return false; }
    image = imageRV.value;

    logicalDevice.getImageMemoryRequirements(image, &memReqs);
    const uint32_t imageLocalMemoryTypeIndex = VulkanBuffer::FindMemoryType(
        physicalDevice, memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memAllocInfo.allocationSize = memReqs.size;
    memAllocInfo.memoryTypeIndex = imageLocalMemoryTypeIndex;
    CheckVkResult(logicalDevice.allocateMemory(&memAllocInfo, nullptr, &deviceMemory));
    CheckVkResult(logicalDevice.bindImageMemory(image, deviceMemory, 0));

    vk::CommandBufferAllocateInfo allocInfo = {};  // todo: one-time CB create-submit helper
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandPool = graphicsCommandPool;
    allocInfo.commandBufferCount = 1;

    vk::CommandBuffer copyCmd;
    CheckVkResult(logicalDevice.allocateCommandBuffers(&allocInfo, &copyCmd));

    vk::CommandBufferBeginInfo beginInfo = {};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    CheckVkResult(copyCmd.begin(&beginInfo));

    vk::ImageSubresourceRange subresourceRange = {};
    subresourceRange.setAspectMask(vk::ImageAspectFlagBits::eColor);
    subresourceRange.setLevelCount(1);
    subresourceRange.setLayerCount(1);

    {
        vk::ImageMemoryBarrier imageMemoryBarrier{};
        imageMemoryBarrier.setOldLayout(vk::ImageLayout::eUndefined);
        imageMemoryBarrier.setNewLayout(vk::ImageLayout::eTransferDstOptimal);
        imageMemoryBarrier.setSrcAccessMask(vk::AccessFlags{});
        imageMemoryBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        imageMemoryBarrier.setImage(image);
        imageMemoryBarrier.setSubresourceRange(subresourceRange);
        copyCmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eAllCommands,
                                vk::DependencyFlags{},
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &imageMemoryBarrier);
    }

    vk::BufferImageCopy bufferCopyRegion = {};
    bufferCopyRegion.imageSubresource.setAspectMask(vk::ImageAspectFlagBits::eColor);
    bufferCopyRegion.imageSubresource.setMipLevel(0);
    bufferCopyRegion.imageSubresource.setBaseArrayLayer(0);
    bufferCopyRegion.imageSubresource.setLayerCount(1);
    bufferCopyRegion.imageExtent.setWidth(width);
    bufferCopyRegion.imageExtent.setHeight(height);
    bufferCopyRegion.imageExtent.setDepth(1);

    copyCmd.copyBufferToImage(
        stagingBuffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &bufferCopyRegion);

    {
        vk::ImageMemoryBarrier imageMemoryBarrier{};
        imageMemoryBarrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal);
        imageMemoryBarrier.setNewLayout(vk::ImageLayout::eTransferSrcOptimal);
        imageMemoryBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
        imageMemoryBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        imageMemoryBarrier.setImage(image);
        imageMemoryBarrier.setSubresourceRange(subresourceRange);
        copyCmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eAllCommands,
                                vk::DependencyFlags{},
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &imageMemoryBarrier);
    }

    CheckVkResult(copyCmd.end());

    vk::SubmitInfo submitInfo = {};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &copyCmd;

    CheckVkResult(graphicsQueue.submit(1, &submitInfo, nullptr));
    CheckVkResult(graphicsQueue.waitIdle());

    logicalDevice.freeCommandBuffers(graphicsCommandPool, 1, &copyCmd);
    logicalDevice.destroyBuffer(stagingBuffer);
    logicalDevice.freeMemory(stagingMemory);

    // //////////////////////////////////////////////

    // Generate the mip chain (glTF uses jpg and png, so we need to create this manually)
    vk::CommandBuffer blitCmd;
    CheckVkResult(logicalDevice.allocateCommandBuffers(&allocInfo, &blitCmd));
    CheckVkResult(blitCmd.begin(&beginInfo));

    for (uint32_t i = 1; i < mipLevels; i++)
    {
        vk::ImageBlit imageBlit{};

        imageBlit.srcSubresource.setAspectMask(vk::ImageAspectFlagBits::eColor);
        imageBlit.srcSubresource.setLayerCount(1);
        imageBlit.srcSubresource.setMipLevel(i - 1);
        imageBlit.setSrcOffsets(
            { vk::Offset3D{},
              vk::Offset3D(int32_t(width >> (i - 1)), int32_t(height >> (i - 1)), 1) });

        imageBlit.dstSubresource.setAspectMask(vk::ImageAspectFlagBits::eColor);
        imageBlit.dstSubresource.setLayerCount(1);
        imageBlit.dstSubresource.setMipLevel(i);
        imageBlit.setDstOffsets(
            { vk::Offset3D{}, vk::Offset3D(int32_t(width >> i), int32_t(height >> i), 1) });

        vk::ImageSubresourceRange mipSubRange = {};
        mipSubRange.setAspectMask(vk::ImageAspectFlagBits::eColor);
        mipSubRange.setBaseMipLevel(i);
        mipSubRange.setLevelCount(1);
        mipSubRange.setLayerCount(1);

        {
            vk::ImageMemoryBarrier imageMemoryBarrier{};
            imageMemoryBarrier.setOldLayout(vk::ImageLayout::eUndefined);
            imageMemoryBarrier.setNewLayout(vk::ImageLayout::eTransferDstOptimal);
            imageMemoryBarrier.setSrcAccessMask(vk::AccessFlags{});
            imageMemoryBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            imageMemoryBarrier.setImage(image);
            imageMemoryBarrier.setSubresourceRange(mipSubRange);
            blitCmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eTransfer,
                                    vk::DependencyFlags{},
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &imageMemoryBarrier);
        }
        blitCmd.blitImage(image,
                          vk::ImageLayout::eTransferSrcOptimal,
                          image,
                          vk::ImageLayout::eTransferDstOptimal,
                          1,
                          &imageBlit,
                          vk::Filter::eLinear);

        {
            vk::ImageMemoryBarrier imageMemoryBarrier{};
            imageMemoryBarrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal);
            imageMemoryBarrier.setNewLayout(vk::ImageLayout::eTransferSrcOptimal);
            imageMemoryBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
            imageMemoryBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            imageMemoryBarrier.setImage(image);
            imageMemoryBarrier.setSubresourceRange(mipSubRange);

            blitCmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eTransfer,
                                    vk::DependencyFlags{},
                                    0,
                                    nullptr,
                                    0,
                                    nullptr,
                                    1,
                                    &imageMemoryBarrier);
        }
    }

    subresourceRange.setLevelCount(mipLevels);
    imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    {
        vk::ImageMemoryBarrier imageMemoryBarrier{};
        imageMemoryBarrier.setOldLayout(vk::ImageLayout::eTransferSrcOptimal);
        imageMemoryBarrier.setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
        imageMemoryBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
        imageMemoryBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        imageMemoryBarrier.setImage(image);
        imageMemoryBarrier.setSubresourceRange(subresourceRange);
        blitCmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                vk::PipelineStageFlagBits::eAllCommands,
                                vk::DependencyFlags{},
                                0,
                                nullptr,
                                0,
                                nullptr,
                                1,
                                &imageMemoryBarrier);
    }

    CheckVkResult(blitCmd.end());

    submitInfo = vk::SubmitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &blitCmd;

    CheckVkResult(graphicsQueue.submit(1, &submitInfo, nullptr));
    CheckVkResult(graphicsQueue.waitIdle());

    logicalDevice.freeCommandBuffers(graphicsCommandPool, 1, &blitCmd);

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.setMagFilter(creationInfo.textureSampler.magFilter);
    samplerInfo.setMinFilter(creationInfo.textureSampler.minFilter);
    samplerInfo.setMipmapMode(vk::SamplerMipmapMode::eLinear);
    samplerInfo.setAddressModeU(creationInfo.textureSampler.addressModeU);
    samplerInfo.setAddressModeV(creationInfo.textureSampler.addressModeV);
    samplerInfo.setAddressModeW(creationInfo.textureSampler.addressModeW);
    samplerInfo.setCompareOp(vk::CompareOp::eNever);
    samplerInfo.setBorderColor(vk::BorderColor::eFloatOpaqueWhite);
    samplerInfo.setMaxLod(static_cast<float>(mipLevels));
    samplerInfo.setMaxAnisotropy(8.0f);
    samplerInfo.setAnisotropyEnable(VK_TRUE);
    CheckVkResult(logicalDevice.createSampler(&samplerInfo, nullptr, &sampler));

    ResultValue<vk::ImageView> imageViewRV = Image::CreateImageView2D(
        logicalDevice, image, format, vk::ImageAspectFlagBits::eColor, mipLevels);
    if (imageViewRV.result != GraphicsResult::Ok) { return false; }

    descriptor.sampler = sampler;
    descriptor.imageView = imageViewRV.value;
    descriptor.imageLayout = imageLayout;

    loadedToGpu = true;
    return true;
}

Texture::~Texture() { Destroy(); }

void Texture::Destroy()
{
    if (logicalDevice)
    {
        logicalDevice.destroyImageView(imageView);
        logicalDevice.destroyImage(image);
        logicalDevice.freeMemory(deviceMemory);
        logicalDevice.destroySampler(sampler);
    }
}

}  // namespace ez
