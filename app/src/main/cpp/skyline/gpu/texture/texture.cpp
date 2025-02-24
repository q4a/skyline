// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <kernel/memory.h>
#include <common/trace.h>
#include <kernel/types/KProcess.h>
#include "texture.h"
#include "layout.h"
#include "adreno_aliasing.h"
#include "bc_decoder.h"
#include "format.h"

namespace skyline::gpu {
    u32 GuestTexture::GetLayerStride() {
        if (layerStride)
            return layerStride;

        switch (tileConfig.mode) {
            case texture::TileMode::Linear:
                return layerStride = static_cast<u32>(format->GetSize(dimensions));

            case texture::TileMode::Pitch:
                return layerStride = dimensions.height * tileConfig.pitch;

            case texture::TileMode::Block:
                return layerStride = static_cast<u32>(texture::GetBlockLinearLayerSize(dimensions, format->blockHeight, format->blockWidth, format->bpb, tileConfig.blockHeight, tileConfig.blockDepth, mipLevelCount, layerCount > 1));
        }
    }

    vk::ImageType GuestTexture::GetImageType() const {
        switch (viewType) {
            case vk::ImageViewType::e1D:
            case vk::ImageViewType::e1DArray:
                return vk::ImageType::e1D;
            case vk::ImageViewType::e2D:
            case vk::ImageViewType::e2DArray:
                // If depth is > 1 this is a 2D view into a 3D texture so the underlying image needs to be created as 3D yoo
                if (dimensions.depth > 1)
                    return vk::ImageType::e3D;
                else
                    return vk::ImageType::e2D;
            case vk::ImageViewType::eCube:
            case vk::ImageViewType::eCubeArray:
                return vk::ImageType::e2D;
            case vk::ImageViewType::e3D:
                return vk::ImageType::e3D;
        }
    }

    u32 GuestTexture::GetViewLayerCount() const {
        if (GetImageType() == vk::ImageType::e3D && viewType != vk::ImageViewType::e3D)
            return dimensions.depth;
        else
            return layerCount;
    }

    u32 GuestTexture::GetViewDepth() const {
        if (GetImageType() == vk::ImageType::e3D && viewType != vk::ImageViewType::e3D)
            return layerCount;
        else
            return dimensions.depth;
    }

    size_t GuestTexture::GetSize() {
        return GetLayerStride() * (layerCount - baseArrayLayer);
    }

    TextureView::TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format format, vk::ComponentMapping mapping) : texture(std::move(texture)), type(type), format(format), mapping(mapping), range(range) {}

    Texture::TextureViewStorage::TextureViewStorage(vk::ImageViewType type, texture::Format format, vk::ComponentMapping mapping, vk::ImageSubresourceRange range, vk::raii::ImageView &&vkView) : type(type), format(format), mapping(mapping), range(range), vkView(std::move(vkView)) {}

    vk::ImageView TextureView::GetView() {
        if (vkView)
            return vkView;

        auto it{std::find_if(texture->views.begin(), texture->views.end(), [this](const Texture::TextureViewStorage &view) {
            return view.type == type && view.format == format && view.mapping == mapping && view.range == range;
        })};
        if (it == texture->views.end()) {
            vk::ImageViewCreateInfo createInfo{
                .image = texture->GetBacking(),
                .viewType = type,
                .format = format ? *format : *texture->format,
                .components = mapping,
                .subresourceRange = range,
            };

            it = texture->views.emplace(texture->views.end(), type, format, mapping, range, vk::raii::ImageView{texture->gpu.vkDevice, createInfo});
        }

        return vkView = *it->vkView;
    }

    void TextureView::lock() {
        auto backing{std::atomic_load(&texture)};
        while (true) {
            backing->lock();

            auto latestBacking{std::atomic_load(&texture)};
            if (backing == latestBacking)
                return;

            backing->unlock();
            backing = latestBacking;
        }
    }

    void TextureView::unlock() {
        texture->unlock();
    }

    bool TextureView::try_lock() {
        auto backing{std::atomic_load(&texture)};
        while (true) {
            bool success{backing->try_lock()};

            auto latestBacking{std::atomic_load(&texture)};
            if (backing == latestBacking)
                // We want to ensure that the try_lock() was on the latest backing and not on an outdated one
                return success;

            if (success)
                // We only unlock() if the try_lock() was successful and we acquired the mutex
                backing->unlock();
            backing = latestBacking;
        }
    }

    void Texture::SetupGuestMappings() {
        auto &mappings{guest->mappings};
        if (mappings.size() == 1) {
            auto mapping{mappings.front()};
            u8 *alignedData{util::AlignDown(mapping.data(), PAGE_SIZE)};
            size_t alignedSize{static_cast<size_t>(util::AlignUp(mapping.data() + mapping.size(), PAGE_SIZE) - alignedData)};

            alignedMirror = gpu.state.process->memory.CreateMirror(span<u8>{alignedData, alignedSize});
            mirror = alignedMirror.subspan(static_cast<size_t>(mapping.data() - alignedData), mapping.size());
        } else {
            std::vector<span<u8>> alignedMappings;

            const auto &frontMapping{mappings.front()};
            u8 *alignedData{util::AlignDown(frontMapping.data(), PAGE_SIZE)};
            alignedMappings.emplace_back(alignedData, (frontMapping.data() + frontMapping.size()) - alignedData);

            size_t totalSize{frontMapping.size()};
            for (auto it{std::next(mappings.begin())}; it != std::prev(mappings.end()); ++it) {
                auto mappingSize{it->size()};
                alignedMappings.emplace_back(it->data(), mappingSize);
                totalSize += mappingSize;
            }

            const auto &backMapping{mappings.back()};
            totalSize += backMapping.size();
            alignedMappings.emplace_back(backMapping.data(), util::AlignUp(backMapping.size(), PAGE_SIZE));

            alignedMirror = gpu.state.process->memory.CreateMirrors(alignedMappings);
            mirror = alignedMirror.subspan(static_cast<size_t>(frontMapping.data() - alignedData), totalSize);
        }

        trapHandle = gpu.state.nce->TrapRegions(mappings, true, [this] {
            std::scoped_lock lock{*this};
            SynchronizeGuest(true); // We can skip trapping since the caller will do it
            WaitOnFence();
        }, [this] {
            std::scoped_lock lock{*this};
            SynchronizeGuest(true);
            dirtyState = DirtyState::CpuDirty; // We need to assume the texture is dirty since we don't know what the guest is writing
            WaitOnFence();
        });
    }

    std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl(const std::shared_ptr<FenceCycle> &pCycle) {
        if (!guest)
            throw exception("Synchronization of host textures requires a valid guest texture to synchronize from");
        else if (guest->dimensions != dimensions)
            throw exception("Guest and host dimensions being different is not supported currently");

        auto pointer{mirror.data()};

        WaitOnBacking();

        u8 *bufferData;
        auto stagingBuffer{[&]() -> std::shared_ptr<memory::StagingBuffer> {
            if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
                // We need a staging buffer for all optimal copies (since we aren't aware of the host optimal layout) and linear textures which we cannot map on the CPU since we do not have access to their backing VkDeviceMemory
                auto stagingBuffer{gpu.memory.AllocateStagingBuffer(surfaceSize)};
                bufferData = stagingBuffer->data();
                return stagingBuffer;
            } else if (tiling == vk::ImageTiling::eLinear) {
                // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly into it rather than a staging buffer
                if (layout == vk::ImageLayout::eUndefined)
                    TransitionLayout(vk::ImageLayout::eGeneral);
                bufferData = std::get<memory::Image>(backing).data();
                if (cycle.lock() != pCycle)
                    WaitOnFence();
                return nullptr;
            } else {
                throw exception("Guest -> Host synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
            }
        }()};

        std::vector<u8> deswizzleBuffer;
        u8 *deswizzleOutput;
        if (guest->format != format) {
            deswizzleBuffer.resize(deswizzledSurfaceSize);
            deswizzleOutput = deswizzleBuffer.data();
        } else [[likely]] {
            deswizzleOutput = bufferData;
        }

        auto guestLayerStride{guest->GetLayerStride()};
        if (levelCount == 1) {
            auto outputLayer{deswizzleOutput};
            for (size_t layer{}; layer < layerCount; layer++) {
                if (guest->tileConfig.mode == texture::TileMode::Block)
                    texture::CopyBlockLinearToLinear(*guest, pointer, outputLayer);
                else if (guest->tileConfig.mode == texture::TileMode::Pitch)
                    texture::CopyPitchLinearToLinear(*guest, pointer, outputLayer);
                else if (guest->tileConfig.mode == texture::TileMode::Linear)
                    std::memcpy(outputLayer, pointer, surfaceSize);
                pointer += guestLayerStride;
                outputLayer += deswizzledLayerStride;
            }
        } else if (levelCount > 1 && guest->tileConfig.mode == texture::TileMode::Block) {
            // We need to generate a buffer that has all layers for a given mip level while Tegra X1 layout holds all mip levels for a given layer
            for (size_t layer{}; layer < layerCount; layer++) {
                auto inputLevel{pointer}, outputLevel{deswizzleOutput};
                for (const auto &level : mipLayouts) {
                    texture::CopyBlockLinearToLinear(
                        level.dimensions,
                        guest->format->blockWidth, guest->format->blockHeight, guest->format->bpb,
                        level.blockHeight, level.blockDepth,
                        inputLevel, outputLevel + (layer * level.linearSize) // Offset into the current layer relative to the start of the current mip level
                    );

                    inputLevel += level.blockLinearSize; // Skip over the current mip level as we've deswizzled it
                    outputLevel += layerCount * level.linearSize; // We need to offset the output buffer by the size of the previous mip level
                }

                pointer += guestLayerStride; // We need to offset the input buffer by the size of the previous guest layer, this can differ from inputLevel's value due to layer end padding or guest RT layer stride
            }
        } else if (levelCount != 0) {
            throw exception("Mipmapped textures with tiling mode '{}' aren't supported", static_cast<int>(tiling));
        }

        if (!deswizzleBuffer.empty()) {
            for (const auto &level : mipLayouts) {
                size_t levelHeight{level.dimensions.height * layerCount}; //!< The height of an image representing all layers in the entire level
                switch (guest->format->vkFormat) {
                    case vk::Format::eBc1RgbaUnormBlock:
                    case vk::Format::eBc1RgbaSrgbBlock:
                        bcn::DecodeBc1(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, true);
                        break;

                    case vk::Format::eBc2UnormBlock:
                    case vk::Format::eBc2SrgbBlock:
                        bcn::DecodeBc2(deswizzleOutput, bufferData, level.dimensions.width, levelHeight);
                        break;

                    case vk::Format::eBc3UnormBlock:
                    case vk::Format::eBc3SrgbBlock:
                        bcn::DecodeBc3(deswizzleOutput, bufferData, level.dimensions.width, levelHeight);
                        break;

                    case vk::Format::eBc4UnormBlock:
                        bcn::DecodeBc4(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, false);
                        break;
                    case vk::Format::eBc4SnormBlock:
                        bcn::DecodeBc4(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, true);
                        break;

                    case vk::Format::eBc5UnormBlock:
                        bcn::DecodeBc5(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, false);
                        break;
                    case vk::Format::eBc5SnormBlock:
                        bcn::DecodeBc5(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, true);
                        break;

                    case vk::Format::eBc6HUfloatBlock:
                        bcn::DecodeBc6(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, false);
                        break;
                    case vk::Format::eBc6HSfloatBlock:
                        bcn::DecodeBc6(deswizzleOutput, bufferData, level.dimensions.width, levelHeight, true);
                        break;

                    case vk::Format::eBc7UnormBlock:
                    case vk::Format::eBc7SrgbBlock:
                        bcn::DecodeBc7(deswizzleOutput, bufferData, level.dimensions.width, levelHeight);
                        break;

                    default:
                        throw exception("Unsupported guest format '{}'", vk::to_string(guest->format->vkFormat));
                }

                deswizzleOutput += level.linearSize * layerCount;
                bufferData += level.targetLinearSize * layerCount;
            }
        }

        if (stagingBuffer && cycle.lock() != pCycle)
            WaitOnFence();

        return stagingBuffer;
    }

    boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies() {
        boost::container::small_vector<vk::BufferImageCopy, 10> bufferImageCopies;

        auto pushBufferImageCopyWithAspect{[&](vk::ImageAspectFlagBits aspect) {
            vk::DeviceSize bufferOffset{};
            u32 mipLevel{};
            for (auto &level : mipLayouts) {
                bufferImageCopies.emplace_back(
                    vk::BufferImageCopy{
                        .bufferOffset = bufferOffset,
                        .imageSubresource = {
                            .aspectMask = aspect,
                            .mipLevel = mipLevel++,
                            .layerCount = layerCount,
                        },
                        .imageExtent = level.dimensions,
                    }
                );
                bufferOffset += level.targetLinearSize * layerCount;
            }
        }};

        if (format->vkAspect & vk::ImageAspectFlagBits::eColor)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eColor);
        if (format->vkAspect & vk::ImageAspectFlagBits::eDepth)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eDepth);
        if (format->vkAspect & vk::ImageAspectFlagBits::eStencil)
            pushBufferImageCopyWithAspect(vk::ImageAspectFlagBits::eStencil);

        return bufferImageCopies;
    }

    void Texture::CopyFromStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        auto image{GetBacking()};
        if (layout == vk::ImageLayout::eUndefined)
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                .image = image,
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = std::exchange(layout, vk::ImageLayout::eGeneral),
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .subresourceRange = {
                    .aspectMask = format->vkAspect,
                    .levelCount = levelCount,
                    .layerCount = layerCount,
                },
            });

        auto bufferImageCopies{GetBufferImageCopies()};
        commandBuffer.copyBufferToImage(stagingBuffer->vkBuffer, image, layout, vk::ArrayProxy(static_cast<u32>(bufferImageCopies.size()), bufferImageCopies.data()));
    }

    void Texture::CopyIntoStagingBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<memory::StagingBuffer> &stagingBuffer) {
        auto image{GetBacking()};
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
            .image = image,
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = layout,
            .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .subresourceRange = {
                .aspectMask = format->vkAspect,
                .levelCount = levelCount,
                .layerCount = layerCount,
            },
        });

        auto bufferImageCopies{GetBufferImageCopies()};
        commandBuffer.copyImageToBuffer(image, layout, stagingBuffer->vkBuffer, vk::ArrayProxy(static_cast<u32>(bufferImageCopies.size()), bufferImageCopies.data()));

        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {}, vk::BufferMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = stagingBuffer->vkBuffer,
            .offset = 0,
            .size = stagingBuffer->size(),
        }, {});
    }

    void Texture::CopyToGuest(u8 *hostBuffer) {
        auto guestOutput{mirror.data()};

        auto guestLayerStride{guest->GetLayerStride()};
        if (levelCount == 1) {
            for (size_t layer{}; layer < layerCount; layer++) {
                if (guest->tileConfig.mode == texture::TileMode::Block)
                    texture::CopyLinearToBlockLinear(*guest, hostBuffer, guestOutput);
                else if (guest->tileConfig.mode == texture::TileMode::Pitch)
                    texture::CopyLinearToPitchLinear(*guest, hostBuffer, guestOutput);
                else if (guest->tileConfig.mode == texture::TileMode::Linear)
                    std::memcpy(hostBuffer, guestOutput, layerStride);
                guestOutput += guestLayerStride;
                hostBuffer += layerStride;
            }
        } else if (levelCount > 1 && guest->tileConfig.mode == texture::TileMode::Block) {
            // We need to copy into the Tegra X1 layout holds all mip levels for a given layer while the input buffer has all layers for a given mip level
            // Note: See SynchronizeHostImpl for additional comments
            for (size_t layer{}; layer < layerCount; layer++) {
                auto outputLevel{guestOutput}, inputLevel{hostBuffer};
                for (const auto &level : mipLayouts) {
                    texture::CopyLinearToBlockLinear(
                        level.dimensions,
                        guest->format->blockWidth, guest->format->blockHeight, guest->format->bpb,
                        level.blockHeight, level.blockDepth,
                        outputLevel, inputLevel + (layer * level.linearSize)
                    );

                    outputLevel += level.blockLinearSize;
                    inputLevel += layerCount * level.linearSize;
                }

                guestOutput += guestLayerStride;
            }
        } else if (levelCount != 0) {
            throw exception("Mipmapped textures with tiling mode '{}' aren't supported", static_cast<int>(tiling));
        }
    }

    Texture::TextureBufferCopy::TextureBufferCopy(std::shared_ptr<Texture> texture, std::shared_ptr<memory::StagingBuffer> stagingBuffer) : texture(std::move(texture)), stagingBuffer(std::move(stagingBuffer)) {}

    Texture::TextureBufferCopy::~TextureBufferCopy() {
        TRACE_EVENT("gpu", "Texture::TextureBufferCopy");
        texture->CopyToGuest(stagingBuffer ? stagingBuffer->data() : std::get<memory::Image>(texture->backing).data());
    }

    Texture::Texture(GPU &gpu, BackingType &&backing, texture::Dimensions dimensions, texture::Format format, vk::ImageLayout layout, vk::ImageTiling tiling, vk::ImageCreateFlags flags, vk::ImageUsageFlags usage, u32 levelCount, u32 layerCount, vk::SampleCountFlagBits sampleCount)
        : gpu(gpu),
          backing(std::move(backing)),
          dimensions(dimensions),
          format(format),
          layout(layout),
          tiling(tiling),
          flags(flags),
          usage(usage),
          levelCount(levelCount),
          layerCount(layerCount),
          sampleCount(sampleCount) {}

    texture::Format ConvertHostCompatibleFormat(texture::Format format, const TraitManager &traits) {
        auto bcnSupport{traits.bcnSupport};
        if (bcnSupport.all())
            return format;

        switch (format->vkFormat) {
            case vk::Format::eBc1RgbaUnormBlock:
                return bcnSupport[0] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc1RgbaSrgbBlock:
                return bcnSupport[0] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc2UnormBlock:
                return bcnSupport[1] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc2SrgbBlock:
                return bcnSupport[1] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc3UnormBlock:
                return bcnSupport[2] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc3SrgbBlock:
                return bcnSupport[2] ? format : format::R8G8B8A8Srgb;

            case vk::Format::eBc4UnormBlock:
                return bcnSupport[3] ? format : format::R8Unorm;
            case vk::Format::eBc4SnormBlock:
                return bcnSupport[3] ? format : format::R8Snorm;

            case vk::Format::eBc5UnormBlock:
                return bcnSupport[4] ? format : format::R8G8Unorm;
            case vk::Format::eBc5SnormBlock:
                return bcnSupport[4] ? format : format::R8G8Snorm;

            case vk::Format::eBc6HUfloatBlock:
            case vk::Format::eBc6HSfloatBlock:
                return bcnSupport[5] ? format : format::R16G16B16A16Float; // This is a signed 16-bit FP format, we don't have an unsigned 16-bit FP format

            case vk::Format::eBc7UnormBlock:
                return bcnSupport[6] ? format : format::R8G8B8A8Unorm;
            case vk::Format::eBc7SrgbBlock:
                return bcnSupport[6] ? format : format::R8G8B8A8Srgb;

            default:
                return format;
        }
    }

    size_t CalculateLevelStride(const std::vector<texture::MipLevelLayout> &mipLayouts) {
        size_t surfaceSize{};
        for (const auto &level : mipLayouts)
            surfaceSize += level.linearSize;
        return surfaceSize;
    }

    size_t CalculateTargetLevelStride(const std::vector<texture::MipLevelLayout> &mipLayouts) {
        size_t surfaceSize{};
        for (const auto &level : mipLayouts)
            surfaceSize += level.targetLinearSize;
        return surfaceSize;
    }

    Texture::Texture(GPU &pGpu, GuestTexture pGuest)
        : gpu(pGpu),
          guest(std::move(pGuest)),
          dimensions(guest->dimensions),
          format(ConvertHostCompatibleFormat(guest->format, gpu.traits)),
          layout(vk::ImageLayout::eUndefined),
          tiling(vk::ImageTiling::eOptimal), // Force Optimal due to not adhering to host subresource layout during Linear synchronization
          layerCount(guest->layerCount),
          deswizzledLayerStride(static_cast<u32>(guest->format->GetSize(dimensions))),
          layerStride(format == guest->format ? deswizzledLayerStride : static_cast<u32>(format->GetSize(dimensions))),
          levelCount(guest->mipLevelCount),
          mipLayouts(
              texture::GetBlockLinearMipLayout(
                  guest->dimensions,
                  guest->format->blockHeight, guest->format->blockWidth, guest->format->bpb,
                  format->blockHeight, format->blockWidth, format->bpb,
                  guest->tileConfig.blockHeight, guest->tileConfig.blockDepth,
                  guest->mipLevelCount
              )
          ),
          deswizzledSurfaceSize(CalculateLevelStride(mipLayouts) * layerCount),
          surfaceSize(format == guest->format ? deswizzledSurfaceSize : (CalculateTargetLevelStride(mipLayouts) * layerCount)),
          sampleCount(vk::SampleCountFlagBits::e1),
          flags(gpu.traits.quirks.vkImageMutableFormatCostly ? vk::ImageCreateFlags{} : vk::ImageCreateFlagBits::eMutableFormat),
          usage(vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled) {
        if ((format->vkAspect & vk::ImageAspectFlagBits::eColor) && !format->IsCompressed())
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (format->vkAspect & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil))
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;

        auto imageType{guest->GetImageType()};
        if (imageType == vk::ImageType::e2D && dimensions.width == dimensions.height && layerCount >= 6)
            flags |= vk::ImageCreateFlagBits::eCubeCompatible;
        else if (imageType == vk::ImageType::e3D)
            flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;

        vk::ImageCreateInfo imageCreateInfo{
            .flags = flags,
            .imageType = imageType,
            .format = *format,
            .extent = dimensions,
            .mipLevels = levelCount,
            .arrayLayers = layerCount,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = tiling,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &gpu.vkQueueFamilyIndex,
            .initialLayout = layout,
        };
        backing = tiling != vk::ImageTiling::eLinear ? gpu.memory.AllocateImage(imageCreateInfo) : gpu.memory.AllocateMappedImage(imageCreateInfo);

        SetupGuestMappings();
    }

    Texture::~Texture() {
        std::scoped_lock lock{*this};
        if (trapHandle)
            gpu.state.nce->DeleteTrap(*trapHandle);
        SynchronizeGuest(true);
        if (alignedMirror.valid())
            munmap(alignedMirror.data(), alignedMirror.size());
    }

    void Texture::MarkGpuDirty() {
        if (dirtyState == DirtyState::GpuDirty || !guest || format != guest->format)
            return; // In addition to other checks, we also need to skip GPU dirty if the host format and guest format differ as we don't support re-encoding compressed textures which is when this generally occurs
        gpu.state.nce->RetrapRegions(*trapHandle, false);
        dirtyState = DirtyState::GpuDirty;
    }

    bool Texture::WaitOnBacking() {
        TRACE_EVENT("gpu", "Texture::WaitOnBacking");

        if (GetBacking()) [[likely]] {
            return false;
        } else {
            std::unique_lock lock(mutex, std::adopt_lock);
            backingCondition.wait(lock, [&]() -> bool { return GetBacking(); });
            lock.release();
            return true;
        }
    }

    void Texture::WaitOnFence() {
        TRACE_EVENT("gpu", "Texture::WaitOnFence");

        auto lCycle{cycle.lock()};
        if (lCycle) {
            lCycle->Wait();
            cycle.reset();
        }
    }

    void Texture::SwapBacking(BackingType &&pBacking, vk::ImageLayout pLayout) {
        WaitOnFence();

        backing = std::move(pBacking);
        layout = pLayout;
        if (GetBacking())
            backingCondition.notify_all();
    }

    void Texture::TransitionLayout(vk::ImageLayout pLayout) {
        WaitOnBacking();
        WaitOnFence();

        TRACE_EVENT("gpu", "Texture::TransitionLayout");

        if (layout != pLayout) {
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = GetBacking(),
                    .srcAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .dstAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .oldLayout = std::exchange(layout, pLayout),
                    .newLayout = pLayout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = {
                        .aspectMask = format->vkAspect,
                        .levelCount = levelCount,
                        .layerCount = layerCount,
                    },
                });
            })};
            lCycle->AttachObject(shared_from_this());
            cycle = lCycle;
        }
    }

    void Texture::SynchronizeHost(bool rwTrap) {
        if (dirtyState != DirtyState::CpuDirty || !guest)
            return; // If the texture has not been modified on the CPU or has no mappings, there is no need to synchronize it

        TRACE_EVENT("gpu", "Texture::SynchronizeHost");

        auto stagingBuffer{SynchronizeHostImpl(nullptr)};
        if (stagingBuffer) {
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                CopyFromStagingBuffer(commandBuffer, stagingBuffer);
            })};
            lCycle->AttachObjects(stagingBuffer, shared_from_this());
            cycle = lCycle;
        }

        if (rwTrap) {
            gpu.state.nce->RetrapRegions(*trapHandle, false);
            dirtyState = DirtyState::GpuDirty;
        } else {
            gpu.state.nce->RetrapRegions(*trapHandle, true); // Trap any future CPU writes to this texture
            dirtyState = DirtyState::Clean;
        }
    }

    void Texture::SynchronizeHostWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &pCycle, bool rwTrap) {
        if (dirtyState != DirtyState::CpuDirty || !guest)
            return;

        TRACE_EVENT("gpu", "Texture::SynchronizeHostWithBuffer");

        auto stagingBuffer{SynchronizeHostImpl(pCycle)};
        if (stagingBuffer) {
            CopyFromStagingBuffer(commandBuffer, stagingBuffer);
            pCycle->AttachObjects(stagingBuffer, shared_from_this());
            cycle = pCycle;
        }

        if (rwTrap) {
            gpu.state.nce->RetrapRegions(*trapHandle, false);
            dirtyState = DirtyState::GpuDirty;
        } else {
            gpu.state.nce->RetrapRegions(*trapHandle, true); // Trap any future CPU writes to this texture
            dirtyState = DirtyState::Clean;
        }
    }

    void Texture::SynchronizeGuest(bool skipTrap) {
        if (dirtyState != DirtyState::GpuDirty || layout == vk::ImageLayout::eUndefined || !guest) {
            // We can skip syncing in three cases:
            // * If the texture has not been used on the GPU, there is no need to synchronize it
            // * If the state of the host texture is undefined then so can the guest
            // * If there is no guest texture to synchronise
            return;
        }

        if (layout == vk::ImageLayout::eUndefined || format != guest->format) {
            // If the state of the host texture is undefined then so can the guest
            // If the texture has differing formats on the guest and host, we don't support converting back in that case as it may involve recompression of a decompressed texture
            if (!skipTrap)
                gpu.state.nce->RetrapRegions(*trapHandle, true);
            dirtyState = DirtyState::Clean;
            return;
        }

        TRACE_EVENT("gpu", "Texture::SynchronizeGuest");

        WaitOnBacking();
        WaitOnFence();

        if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(surfaceSize)};

            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
            })};
            lCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this(), stagingBuffer));
            cycle = lCycle;
        } else if (tiling == vk::ImageTiling::eLinear) {
            // We can optimize linear texture sync on a UMA by mapping the texture onto the CPU and copying directly from it rather than using a staging buffer
            CopyToGuest(std::get<memory::Image>(backing).data());
        } else {
            throw exception("Host -> Guest synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
        }

        if (!skipTrap)
            gpu.state.nce->RetrapRegions(*trapHandle, true);
        dirtyState = DirtyState::Clean;
    }

    void Texture::SynchronizeGuestWithBuffer(const vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &pCycle) {
        if (dirtyState != DirtyState::GpuDirty || !guest)
            return;

        if (layout == vk::ImageLayout::eUndefined || format != guest->format) {
            // If the state of the host texture is undefined then so can the guest
            // If the texture has differing formats on the guest and host, we don't support converting back in that case as it may involve recompression of a decompressed texture
            dirtyState = DirtyState::Clean;
            return;
        }

        TRACE_EVENT("gpu", "Texture::SynchronizeGuestWithBuffer");

        WaitOnBacking();
        if (cycle.lock() != pCycle)
            WaitOnFence();

        if (tiling == vk::ImageTiling::eOptimal || !std::holds_alternative<memory::Image>(backing)) {
            auto stagingBuffer{gpu.memory.AllocateStagingBuffer(surfaceSize)};

            CopyIntoStagingBuffer(commandBuffer, stagingBuffer);
            pCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this(), stagingBuffer));
            cycle = pCycle;
        } else if (tiling == vk::ImageTiling::eLinear) {
            CopyToGuest(std::get<memory::Image>(backing).data());
            pCycle->AttachObject(std::make_shared<TextureBufferCopy>(shared_from_this()));
            cycle = pCycle;
        } else {
            throw exception("Host -> Guest synchronization of images tiled as '{}' isn't implemented", vk::to_string(tiling));
        }

        dirtyState = DirtyState::Clean;
    }

    std::shared_ptr<TextureView> Texture::GetView(vk::ImageViewType type, vk::ImageSubresourceRange range, texture::Format pFormat, vk::ComponentMapping mapping) {
        if (!pFormat || pFormat == guest->format)
            pFormat = format; // We want to use the texture's format if it isn't supplied or if the requested format matches the guest format then we want to use the host format just in case it is host incompatible and the host format differs from the guest format

        auto viewFormat{pFormat->vkFormat}, textureFormat{format->vkFormat};
        if (gpu.traits.quirks.vkImageMutableFormatCostly && viewFormat != textureFormat && (!gpu.traits.quirks.adrenoRelaxedFormatAliasing || !texture::IsAdrenoAliasCompatible(viewFormat, textureFormat)))
            Logger::Warn("Creating a view of a texture with a different format without mutable format: {} - {}", vk::to_string(viewFormat), vk::to_string(textureFormat));

        return std::make_shared<TextureView>(shared_from_this(), type, range, pFormat, mapping);
    }

    void Texture::CopyFrom(std::shared_ptr<Texture> source, const vk::ImageSubresourceRange &subresource) {
        WaitOnBacking();
        WaitOnFence();

        source->WaitOnBacking();
        source->WaitOnFence();

        if (source->layout == vk::ImageLayout::eUndefined)
            throw exception("Cannot copy from image with undefined layout");
        else if (source->dimensions != dimensions)
            throw exception("Cannot copy from image with different dimensions");
        else if (source->format != format)
            throw exception("Cannot copy from image with different format");

        TRACE_EVENT("gpu", "Texture::CopyFrom");

        auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
            auto sourceBacking{source->GetBacking()};
            if (source->layout != vk::ImageLayout::eTransferSrcOptimal) {
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = sourceBacking,
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                    .oldLayout = source->layout,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });
            }

            auto destinationBacking{GetBacking()};
            if (layout != vk::ImageLayout::eTransferDstOptimal) {
                commandBuffer.pipelineBarrier(layout != vk::ImageLayout::eUndefined ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = destinationBacking,
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = layout,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });

                if (layout == vk::ImageLayout::eUndefined)
                    layout = vk::ImageLayout::eTransferDstOptimal;
            }

            vk::ImageSubresourceLayers subresourceLayers{
                .aspectMask = subresource.aspectMask,
                .mipLevel = subresource.baseMipLevel,
                .baseArrayLayer = subresource.baseArrayLayer,
                .layerCount = subresource.layerCount == VK_REMAINING_ARRAY_LAYERS ? layerCount - subresource.baseArrayLayer : subresource.layerCount,
            };
            for (; subresourceLayers.mipLevel < (subresource.levelCount == VK_REMAINING_MIP_LEVELS ? levelCount - subresource.baseMipLevel : subresource.levelCount); subresourceLayers.mipLevel++)
                commandBuffer.copyImage(sourceBacking, vk::ImageLayout::eTransferSrcOptimal, destinationBacking, vk::ImageLayout::eTransferDstOptimal, vk::ImageCopy{
                    .srcSubresource = subresourceLayers,
                    .dstSubresource = subresourceLayers,
                    .extent = dimensions,
                });

            if (layout != vk::ImageLayout::eTransferDstOptimal)
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = destinationBacking,
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });

            if (layout != vk::ImageLayout::eTransferSrcOptimal)
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier{
                    .image = sourceBacking,
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = source->layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .subresourceRange = subresource,
                });
        })};
        lCycle->AttachObjects(std::move(source), shared_from_this());
        cycle = lCycle;
    }
}
