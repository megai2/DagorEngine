#include "device.h"
#include "frontend_state.h"

#include "d3dformat.h"

#include <ioSys/dag_memIo.h>
#include <image/dag_texPixel.h>
#include <math/dag_adjpow2.h>
#include <3d/dag_drv3d_pc.h>
#include <EASTL/tuple.h>
#include <basetexture.h>
#include <validateUpdateSubRegion.h>


#if 0
#define VERBOSE_DEBUG debug
#else
#define VERBOSE_DEBUG(...)
#endif

using namespace drv3d_dx12;

namespace
{
D3DFORMAT texfmt_to_d3dformat(uint32_t fmt)
{
  switch (fmt)
  {
    case TEXFMT_DEFAULT: return D3DFMT_A8R8G8B8;
    case TEXFMT_A2B10G10R10: return D3DFMT_A2B10G10R10;
    case TEXFMT_A16B16G16R16: return D3DFMT_A16B16G16R16;
    case TEXFMT_A16B16G16R16F: return D3DFMT_A16B16G16R16F;
    case TEXFMT_A32B32G32R32F: return D3DFMT_A32B32G32R32F;
    case TEXFMT_G16R16: return D3DFMT_G16R16;
    case TEXFMT_V16U16: return D3DFMT_V16U16;
    case TEXFMT_L16: return D3DFMT_L16;
    case TEXFMT_A8: return D3DFMT_A8;
    case TEXFMT_R8: return D3DFMT_L8;
    case TEXFMT_A8L8: return D3DFMT_A8L8;
    case TEXFMT_G16R16F: return D3DFMT_G16R16F;
    case TEXFMT_G32R32F: return D3DFMT_G32R32F;
    case TEXFMT_R16F: return D3DFMT_R16F;
    case TEXFMT_R32F: return D3DFMT_R32F;
    case TEXFMT_DXT1: return D3DFMT_DXT1;
    case TEXFMT_DXT3: return D3DFMT_DXT3;
    case TEXFMT_DXT5: return D3DFMT_DXT5;
    case TEXFMT_A4R4G4B4: return D3DFMT_A4R4G4B4; // dxgi 1.2
    case TEXFMT_A1R5G5B5: return D3DFMT_A1R5G5B5;
    case TEXFMT_R5G6B5: return D3DFMT_R5G6B5;
    case TEXFMT_ATI1N: return static_cast<D3DFORMAT>(_MAKE4C('ATI1'));
    case TEXFMT_ATI2N: return static_cast<D3DFORMAT>(_MAKE4C('ATI2'));
  }
  G_ASSERTF(0, "can't convert tex format: %d", fmt);
  return D3DFMT_A8R8G8B8;
}

uint32_t auto_mip_levels_count(uint32_t w, uint32_t h, uint32_t mnsz)
{
  uint32_t lev = 1;
  while (w > mnsz && h > mnsz)
  {
    lev++;
    w >>= 1;
    h >>= 1;
  }
  return lev;
}

eastl::tuple<int32_t, int> fixup_tex_params(int w, int h, int32_t flg, int levels)
{
  const bool rt = (TEXCF_RTARGET & flg) != 0;
  auto fmt = TEXFMT_MASK & flg;
  if (TEXFMT_DEFAULT == fmt)
  {
    VERBOSE_DEBUG("DX12: Texture format was DEFAULT, retargeting to ARGB8888");
    fmt = TEXFMT_A8R8G8B8; // -V1048 (in case TEXFMT_DEFAULT == TEXFMT_A8R8G8B8)
  }

  if (rt)
  {
    if (0 != (TEXCF_SRGBWRITE & flg))
    {
      if ((TEXFMT_A8R8G8B8 != fmt))
      {
        // TODO verify this requirement
        if (0 != (TEXCF_SRGBREAD & flg))
        {
          VERBOSE_DEBUG("DX12: Adding TEXCF_SRGBREAD to texture flags, because chosen format needs it");
          flg |= TEXCF_SRGBREAD;
        }
      }
    }
  }

  if (0 == levels)
  {
    levels = auto_mip_levels_count(w, h, rt ? 1 : 4);
    VERBOSE_DEBUG("DX12: Auto compute for texture mip levels yielded %d", levels);
  }

  // update format info if it had changed
  return eastl::make_tuple((flg & ~TEXFMT_MASK) | fmt, levels);
}

namespace
{
bool needs_subresource_tracking(uint32_t cflags)
{
  // In addition to RT, UA and copy target, dynamic textures also need state tracking, as they can (and will) be
  // updated multiple times during the same frame and we need to update them during GPU timeline.
  return 0 != (cflags & (TEXCF_RTARGET | TEXCF_UNORDERED | TEXCF_UPDATE_DESTINATION | TEXCF_DYNAMIC));
}
} // namespace

bool create_tex2d(D3DTextures &tex, BaseTex *bt_in, uint32_t w, uint32_t h, uint32_t levels, bool cube,
  BaseTex::ImageMem *initial_data, int array_size = 1, BaseTex *baseTexture = nullptr, bool temp_alloc = false)
{
  auto &device = get_device();

  uint32_t &flg = bt_in->cflg;
  G_ASSERT(!((flg & TEXCF_SAMPLECOUNT_MASK) && initial_data != nullptr));
  G_ASSERT(!((flg & TEXCF_LOADONCE) && (flg & (TEXCF_DYNAMIC | TEXCF_RTARGET))));

  if (flg & TEXCF_VARIABLE_RATE)
  {
    // Check rules for VRS textures
    G_ASSERTF_RETURN(TEXFMT_R8UI == (flg & TEXFMT_MASK), false, "Variable Rate Textures can only use R8 UINT format");
    G_ASSERTF_RETURN(0 == (flg & TEXCF_RTARGET), false, "Variable Rate Textures can not be used as render target");
    G_ASSERTF_RETURN(0 == (flg & TEXCF_SAMPLECOUNT_MASK), false, "Variable Rate Textures can not be multisampled");
    G_ASSERTF_RETURN(1 == array_size, false, "Variable Rate Textures can not be a arrayed texture");
    G_ASSERTF_RETURN(false == cube, false, "Variable Rate Texture can not be a cube map");
    G_ASSERTF_RETURN(1 == levels, false, "Variable Rate Texture can only have one mip level");
  }

  ImageInfo desc;
  desc.type = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.size.width = w;
  desc.size.height = h;
  desc.size.depth = 1;
  desc.mips = MipMapCount::make(levels);
  desc.arrays = ArrayLayerCount::make((cube ? 6 : 1) * array_size);
  desc.format = FormatStore::fromCreateFlags(flg);
  desc.memoryLayout = (flg & TEXCF_TILED_RESOURCE) ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.allocateSubresourceIDs = needs_subresource_tracking(flg);
  desc.sampleDesc.Count = get_sample_count(flg);

  flg = BaseTex::update_flags_for_linear_layout(flg, desc.format);
  desc.memoryClass = BaseTex::get_memory_class(flg);
#if _TARGET_XBOX
  if (TEXCF_LINEAR_LAYOUT & flg)
  {
    desc.memoryLayout = TEXTURE_LINEAR_LAYOUT;
  }
#endif

  tex.realMipLevels = levels;

  if (flg & TEXCF_SIMULTANEOUS_MULTI_QUEUE_USE)
  {
    G_ASSERTF_RETURN(0 != ((TEXCF_RTARGET | TEXCF_UNORDERED) & flg), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be render targets "
      "and/or uav textures");
    G_ASSERTF_RETURN(!desc.format.isDepth() && !desc.format.isStencil(), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be used with color "
      "texture formats");

    desc.usage |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  }

#if _TARGET_XBOXONE
  if (flg & TEXCF_RTARGET) // Fast clear doesn't work, clears only a top half of a thick textures with incorrect values, or clears
                           // areas adjacent to thin textures.
    desc.usage |= RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA;
#endif

  const bool isDepth = desc.format.isDepth();
  const bool isRT = flg & TEXCF_RTARGET;
  const bool isUAV = flg & TEXCF_UNORDERED;
  if (isRT)
  {
    reinterpret_cast<uint32_t &>(desc.usage) |=
      isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
  reinterpret_cast<uint32_t &>(desc.usage) |= (flg & TEXCF_UNORDERED) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 0;

  G_ASSERT(!(isDepth && (flg & TEXCF_READABLE)));

  if (!temp_alloc)
    TEXQL_PRE_CLEAN(bt_in->ressize());

#if DX12_USE_ESRAM
  if (baseTexture)
  {
    auto baseEsram = baseTexture->cflg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    auto selfEsram = flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    if (baseEsram != (selfEsram & baseEsram))
    {
      logwarn("DX12: Alias texture has not matching ESRam flags to its base %08X != %08X", baseEsram, selfEsram);
    }
    if (baseEsram & TEXCF_MOVABLE_ESRAM)
    {
      logwarn("DX12: Aliased ESRam texture uses TEXCF_MOVABLE_ESRAM which has issues");
    }
    // when base texture is esram, than alias has to be also esram
    flg = (flg & ~(TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM)) | baseEsram;
  }
  if (flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM))
  {
    tex.image = device.createEsramBackedImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
  }
  if (!tex.image)
  {
#endif
    tex.image = device.createImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
#if DX12_USE_ESRAM
  }
  else if ((flg & TEXCF_MOVABLE_ESRAM) && tex.image->isEsramTexture())
  {
    EsramResource &resource = tex.image->getEsramResource();
    // no support for aliasing, this causes lots of problems
    resource.dramStorage = device.createImage(desc, nullptr, bt_in->getResName());
    device.registerMovableESRAMTexture(tex.image);
  }
#endif
  if (!tex.image)
  {
    if (tex.stagingMemory)
    {
      device.getContext().freeMemory(tex.stagingMemory);
      tex.stagingMemory = HostDeviceSharedMemoryRegion{};
    }
    return false;
  }
  if (flg & TEXCF_SYSMEM)
  {
    uint64_t size = calculate_texture_staging_buffer_size(*tex.image, tex.image->getSubresourceRange());
    tex.stagingMemory = device.allocatePersistentBidirectionalMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    G_ASSERT(!isRT && !isDepth);
  }

  if ((flg & (TEXCF_READABLE | TEXCF_SYSMEM)) == TEXCF_READABLE)
  {
    if (!tex.stagingMemory)
    {
      uint64_t size = calculate_texture_staging_buffer_size(*tex.image, tex.image->getSubresourceRange());
      tex.stagingMemory = device.allocatePersistentBidirectionalMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }
  }

  if (initial_data)
  {
    auto stage = tex.stagingMemory;
#if _TARGET_XBOX
    XGTextureAddressComputer *uploader = nullptr;
    if (!stage)
    {
      uploader = tex.image->getAccessComputer();
    }

    if (uploader)
    {
      for (auto i : desc.arrays)
      {
        for (auto j : desc.mips)
        {
          BaseTex::ImageMem &src = initial_data[desc.mips.count() * i.index() + j.index()];
          auto subResInfo = calculate_texture_mip_info(*tex.image, j);
          uploader->CopyIntoSubresource(tex.image->getMemory().asPointer(), 0,
            calculate_subresource_index(j.index(), i.index(), 0, desc.mips.count(), desc.arrays.count()), src.ptr, src.rowPitch,
            src.rowPitch * subResInfo.rowCount);
        }
      }
      // only need end access as begin is implicit after create
      device.getContext().endCPUTextureAccess(tex.image);
      // usually we only need this once
      tex.image->releaseAccessComputer();
    }
    else
#endif
    {
      if (!stage)
      {
        auto size = calculate_texture_staging_buffer_size(*tex.image, tex.image->getSubresourceRange());
        stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!stage)
        {
          return false;
        }
      }

      BufferImageCopy copies[MAX_MIPMAPS];

      uint32_t offset = 0;
      for (auto i : desc.arrays)
      {
        uint32_t flushStart = offset;
        for (auto j : desc.mips)
        {
          BaseTex::ImageMem &src = initial_data[desc.mips.count() * i.index() + j.index()];
          auto subResInfo = calculate_texture_mip_info(*tex.image, j);
          G_ASSERT(src.rowPitch > 0);
          BufferImageCopy &copy = copies[j.index()];
          copy.subresourceIndex = calculate_subresource_index(j.index(), i.index(), 0, desc.mips.count(), desc.arrays.count());
          copy.layout.Footprint = subResInfo.footprint;
          copy.layout.Offset = offset;
          G_ASSERT(copy.layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
          copy.imageOffset.x = 0;
          copy.imageOffset.y = 0;
          copy.imageOffset.z = 0;
          auto srcPtr = reinterpret_cast<const uint8_t *>(src.ptr);
          for (uint32_t r = 0; r < subResInfo.rowCount; ++r)
          {
            memcpy(&stage.pointer[offset], srcPtr, src.rowPitch);
            offset += copy.layout.Footprint.RowPitch;
            srcPtr += src.rowPitch;
          }
          offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        }
        stage.flushRegion(ValueRange<size_t>{flushStart, offset});
        device.getContext().uploadToImage(tex.image, copies, desc.mips.count(), stage, DeviceQueueType::UPLOAD, false);
      }

      if (!tex.stagingMemory)
      {
        device.getContext().freeMemory(stage);
      }
    }
  }
  else if (isRT)
  {
    // render target always clear to zero on create.
    ImageSubresourceRange area;
    area.mipMapRange = desc.mips;
    area.arrayLayerRange = desc.arrays;
    if (desc.format.isColor())
    {
      ClearColorValue cv = {};
      device.getContext().clearColorImage(tex.image, area, cv);
    }
    else
    {
      ClearDepthStencilValue cv = {};
      device.getContext().clearDepthStencilImage(tex.image, area, cv);
    }
  }
  else if (flg & TEXCF_CLEAR_ON_CREATE)
  {
    if (isUAV)
    {
      float clearValF[4] = {
        0.0f,
      };
      unsigned clearValI[4] = {0};
      ImageViewState viewState;
      viewState.isCubemap = cube;
      viewState.isArray = array_size > 1;
      viewState.setArrayRange(desc.arrays);
      viewState.setMipCount(1);
      viewState.setUAV();
      viewState.setFormat(desc.format.getLinearVariant());
      for (auto level : desc.mips)
      {
        viewState.setMipBase(level);
        if (desc.format.isFloat())
        {
          device.getContext().clearUAVTexture(tex.image, viewState, clearValF);
        }
        else
        {
          device.getContext().clearUAVTexture(tex.image, viewState, clearValI);
        }
      }
    }
    else
    {
      auto stage = tex.stagingMemory;
#if _TARGET_XBOX
      XGTextureAddressComputer *uploader = nullptr;
      if (!stage)
      {
        uploader = tex.image->getAccessComputer();
      }
      if (uploader)
      {
        memset(tex.image->getMemory().asPointer(), 0, tex.image->getMemory().size());
        device.getContext().endCPUTextureAccess(tex.image);
        tex.image->releaseAccessComputer();
      }
      else
#endif
      {
        if (!stage)
        {
          auto size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, 1));
          stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
          if (!stage)
            return 0;
        }

        BufferImageCopy copies[MAX_MIPMAPS];
        memset(stage.pointer, 0, stage.range.size());
        stage.flush();
        for (auto i : desc.arrays)
        {
          for (auto j : desc.mips)
          {
            BufferImageCopy &copy = copies[j.index()];
            copy.subresourceIndex = calculate_subresource_index(j.index(), i.index(), 0, desc.mips.count(), desc.arrays.count());
            auto subResInfo = calculate_texture_mip_info(*tex.image, j);
            copy.layout.Footprint = subResInfo.footprint;
            copy.layout.Offset = 0;
            copy.imageOffset.x = 0;
            copy.imageOffset.y = 0;
            copy.imageOffset.z = 0;
          }
          device.getContext().uploadToImage(tex.image, copies, desc.mips.count(), stage, DeviceQueueType::UPLOAD, false);
        }
        if (!tex.stagingMemory)
          device.getContext().freeMemory(stage);
      }
    }
  }

  tex.memSize = bt_in->ressize();
  bt_in->updateTexName();
  TEXQL_ON_ALLOC(bt_in);
  return true;
}

bool create_tex3d(D3DTextures &tex, BaseTex *bt_in, uint32_t w, uint32_t h, uint32_t d, uint32_t flg, uint32_t levels,
  BaseTex::ImageMem *initial_data, BaseTex *baseTexture = nullptr)
{
  auto &device = get_device();
  auto &ctx = device.getContext();

  G_ASSERT((flg & TEXCF_SAMPLECOUNT_MASK) == 0);
  G_ASSERT(!((flg & TEXCF_LOADONCE) && (flg & TEXCF_DYNAMIC)));
  G_ASSERTF_RETURN(0 == (flg & TEXCF_VARIABLE_RATE), false, "Variable Rate Texture can not be a volumetric texture");

  ImageInfo desc;
  desc.type = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.size.width = w;
  desc.size.height = h;
  desc.size.depth = d;
  desc.mips = MipMapCount::make(levels);
  desc.arrays = ArrayLayerCount::make(1);
  desc.format = FormatStore::fromCreateFlags(flg);
  desc.memoryLayout = (flg & TEXCF_TILED_RESOURCE) ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.allocateSubresourceIDs = needs_subresource_tracking(flg);

  flg = BaseTex::update_flags_for_linear_layout(flg, desc.format);
  desc.memoryClass = BaseTex::get_memory_class(flg);
#if _TARGET_XBOX
  if (TEXCF_LINEAR_LAYOUT & flg)
  {
    desc.memoryLayout = TEXTURE_LINEAR_LAYOUT;
  }
#endif

  tex.realMipLevels = levels;

  if (flg & TEXCF_SIMULTANEOUS_MULTI_QUEUE_USE)
  {
    G_ASSERTF_RETURN(0 != ((TEXCF_RTARGET | TEXCF_UNORDERED) & flg), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be render targets "
      "and/or uav textures");
    G_ASSERTF_RETURN(!desc.format.isDepth() && !desc.format.isStencil(), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be used with color "
      "texture formats");

    desc.usage |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  }

  const bool isRT = flg & TEXCF_RTARGET;
  const bool isUAV = flg & TEXCF_UNORDERED;
  if (flg & TEXCF_UNORDERED)
    reinterpret_cast<uint32_t &>(desc.usage) |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  TEXQL_PRE_CLEAN(bt_in->ressize());

  const bool isDepth = desc.format.isDepth();
  if (isRT)
  {
    reinterpret_cast<uint32_t &>(desc.usage) |=
      isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
#if DX12_USE_ESRAM
  if (baseTexture)
  {
    auto baseEsram = baseTexture->cflg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    auto selfEsram = flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    if (baseEsram != (selfEsram & baseEsram))
    {
      logwarn("DX12: Alias texture has not matching ESRam flags to its base %08X != %08X", baseEsram, selfEsram);
    }
    if (baseEsram & TEXCF_MOVABLE_ESRAM)
    {
      logwarn("DX12: Aliased ESRam texture uses TEXCF_MOVABLE_ESRAM which has issues");
    }
    // when base texture is esram, than alias has to be also esram
    flg = (flg & ~(TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM)) | baseEsram;
  }
  if (flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM))
  {
    tex.image = device.createEsramBackedImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
  }
  if (!tex.image)
  {
#endif
    tex.image = device.createImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
#if DX12_USE_ESRAM
  }
  else if ((flg & TEXCF_MOVABLE_ESRAM) && tex.image->isEsramTexture())
  {
    EsramResource &resource = tex.image->getEsramResource();
    // no support for aliasing, this causes lots of problems
    resource.dramStorage = device.createImage(desc, nullptr, bt_in->getResName());
    device.registerMovableESRAMTexture(tex.image);
  }
#endif
  if (!tex.image)
    return false;

  if (initial_data)
  {
#if _TARGET_XBOX
    XGTextureAddressComputer *uploader = tex.image->getAccessComputer();
    if (uploader)
    {
      for (auto j : desc.mips)
      {
        auto &src = initial_data[j.index()];
        uploader->CopyIntoSubresource(tex.image->getMemory().asPointer(), 0,
          calculate_subresource_index(j.index(), 0, 0, desc.mips.count(), 1), src.ptr, src.rowPitch, src.slicePitch);
      }
      // only need end access as begin is implicit after create
      device.getContext().endCPUTextureAccess(tex.image);
      // usually we only need this once
      tex.image->releaseAccessComputer();
    }
    else
#endif
    {
      uint64_t size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, desc.mips.count()));
      auto stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      if (stage)
      {
        BufferImageCopy copies[MAX_MIPMAPS];
        uint64_t offset = 0;

        for (auto j : desc.mips)
        {
          BufferImageCopy &copy = copies[j.index()];
          copy.subresourceIndex = calculate_subresource_index(j.index(), 0, 0, desc.mips.count(), 1);
          auto subResInfo = calculate_texture_mip_info(*tex.image, j);
          copy.layout.Footprint = subResInfo.footprint;
          copy.layout.Offset = offset;
          G_ASSERT(copy.layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
          copy.imageOffset.x = 0;
          copy.imageOffset.y = 0;
          copy.imageOffset.z = 0;
          auto &initSource = initial_data[j.index()];
          auto srcPtr = reinterpret_cast<const uint8_t *>(initSource.ptr);
          for (uint32_t z = 0; z < copy.layout.Footprint.Depth; ++z)
          {
            auto slicePtr = srcPtr;
            for (uint32_t r = 0; r < subResInfo.rowCount; ++r)
            {
              memcpy(&stage.pointer[offset], slicePtr, initSource.rowPitch);
              offset += copy.layout.Footprint.RowPitch;
              slicePtr += initSource.rowPitch;
            }
            srcPtr += initSource.slicePitch;
          }
          offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        }
        stage.flush();
        ctx.uploadToImage(tex.image, copies, levels, stage, DeviceQueueType::UPLOAD, false);
        ctx.freeMemory(stage);
      }
      else
      {
        return 0;
      }
    }
  }
  else if (isRT)
  {
    // init render target to a known state
    ImageSubresourceRange area;
    area.mipMapRange = desc.mips;
    area.arrayLayerRange = desc.arrays;
    if (desc.format.isColor())
    {
      ClearColorValue cv = {};
      ctx.clearColorImage(tex.image, area, cv);
    }
    else
    {
      ClearDepthStencilValue cv = {};
      ctx.clearDepthStencilImage(tex.image, area, cv);
    }
  }
  else if (flg & TEXCF_CLEAR_ON_CREATE)
  {
    if (isUAV)
    {
      float clearValF[4] = {
        0.0f,
      };
      unsigned clearValI[4] = {0};
      for (auto mip : desc.mips)
      {
        ImageViewState viewState;
        viewState.isCubemap = 0;
        viewState.isArray = 0;
        viewState.setDepthLayerRange(0, max(1u, desc.size.depth >> mip.index()));
        viewState.setSingleMipMapRange(mip);
        viewState.setUAV();
        viewState.setFormat(desc.format.getLinearVariant());
        if (desc.format.isFloat())
        {
          device.getContext().clearUAVTexture(tex.image, viewState, clearValF);
        }
        else
        {
          device.getContext().clearUAVTexture(tex.image, viewState, clearValI);
        }
      }
    }
    else
    {
#if _TARGET_XBOX
      XGTextureAddressComputer *uploader = tex.image->getAccessComputer();
      if (uploader)
      {
        memset(tex.image->getMemory().asPointer(), 0, tex.image->getMemory().size());
        device.getContext().endCPUTextureAccess(tex.image);
        tex.image->releaseAccessComputer();
      }
      else
#endif
      {
        uint64_t size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, 1));
        auto stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (stage)
        {
          memset(stage.pointer, 0, size);
          stage.flush();

          BufferImageCopy copies[MAX_MIPMAPS];
          for (auto j : desc.mips)
          {
            BufferImageCopy &copy = copies[j.index()];
            copy.subresourceIndex = calculate_subresource_index(j.index(), 0, 0, desc.mips.count(), 1);
            auto subResInfo = calculate_texture_mip_info(*tex.image, j);
            copy.layout.Footprint = subResInfo.footprint;
            copy.layout.Offset = 0;
            copy.imageOffset.x = 0;
            copy.imageOffset.y = 0;
            copy.imageOffset.z = 0;
          }
          ctx.uploadToImage(tex.image, copies, levels, stage, DeviceQueueType::UPLOAD, false);
          ctx.freeMemory(stage);
        }
        else
        {
          return 0;
        }
      }
    }
  }

  tex.memSize = bt_in->ressize();
  bt_in->updateTexName();
  TEXQL_ON_ALLOC(bt_in);
  return true;
}
} // namespace

ImageViewState BaseTex::getViewInfoUAV(MipMapIndex mip, ArrayLayerIndex layer, bool as_uint) const
{
  ImageViewState result;
  if (resType == RES3D_TEX)
  {
    result.isCubemap = 0;
    result.isArray = 0;
    result.setSingleArrayRange(ArrayLayerIndex::make(0));
    G_ASSERTF(layer.index() == 0, "UAV view for layer/face %u requested, but texture was 2d and has no layers/faces", layer);
  }
  else if (resType == RES3D_CUBETEX)
  {
    result.isCubemap = 1;
    result.isArray = 0;
    result.setArrayRange(getArrayCount().front(layer));
    G_ASSERTF(layer < getArrayCount(),
      "UAV view for layer/face %u requested, but texture was cubemap and has only 6 "
      "faces",
      layer);
  }
  else if (resType == RES3D_ARRTEX)
  {
    result.isArray = 1;
    result.isCubemap = isArrayCube();
    result.setArrayRange(getArrayCount().front(layer));
    G_ASSERTF(layer < getArrayCount(), "UAV view for layer/face %u requested, but texture has only %u layers", layer, getArrayCount());
  }
  else if (resType == RES3D_VOLTEX)
  {
    result.isArray = 0;
    result.isCubemap = 0;
    result.setDepthLayerRange(0, max<uint16_t>(1, depth >> mip.index()));
    G_ASSERTF(layer.index() == 0, "UAV view for layer/face %u requested, but texture was 3d and has no layers/faces", layer);
  }
  if (as_uint)
  {
    G_ASSERT(getFormat().getBytesPerPixelBlock() == 4 ||
             (getFormat().getBytesPerPixelBlock() == 8 && d3d::get_driver_desc().caps.hasShader64BitIntegerResources));
    if (getFormat().getBytesPerPixelBlock() == 4)
      result.setFormat(FormatStore::fromCreateFlags(TEXFMT_R32UI));
    else if (getFormat().getBytesPerPixelBlock() == 8)
      result.setFormat(FormatStore::fromCreateFlags(TEXFMT_R32G32UI));
  }
  else
  {
    result.setFormat(getFormat().getLinearVariant());
  }
  result.setSingleMipMapRange(mip);
  result.setUAV();
  return result;
}

ImageViewState BaseTex::getViewInfoRenderTarget(MipMapIndex mip, ArrayLayerIndex layer, bool as_const) const
{
  FormatStore format = allowSrgbWrite() ? getFormat() : getFormat().getLinearVariant();
  ImageViewState result;
  result.isArray = resType == RES3D_ARRTEX;
  result.isCubemap = resType == RES3D_CUBETEX;
  result.setFormat(format);
  result.setSingleMipMapRange(mip);

  if (layer.index() < d3d::RENDER_TO_WHOLE_ARRAY)
  {
    result.setSingleArrayRange(layer);
  }
  else
  {
    if (RES3D_VOLTEX == resType)
    {
      result.setDepthLayerRange(0, max<uint16_t>(1, depth >> mip.index()));
    }
    else
    {
      result.setArrayRange(getArrayCount());
    }
  }

  if (format.isColor())
  {
    result.setRTV();
  }
  else
  {
    result.setDSV(as_const);
  }

  return result;
}

ImageViewState BaseTex::getViewInfo() const
{
  ImageViewState result;
  result.setFormat(allowSrgbRead() ? getFormat() : getFormat().getLinearVariant());
  result.isArray = resType == RES3D_ARRTEX ? 1 : 0;
  result.isCubemap = resType == RES3D_CUBETEX ? 1 : (resType == RES3D_ARRTEX ? int(isArrayCube()) : 0);
  int32_t baseMip = clamp<int32_t>(maxMipLevel, 0, max(0, (int32_t)tex.realMipLevels - 1));
  int32_t mipCount = (minMipLevel - maxMipLevel) + 1;
  if (mipCount <= 0 || baseMip + mipCount > tex.realMipLevels)
  {
    mipCount = tex.realMipLevels - baseMip;
  }
  if (isStub())
  {
    baseMip = 0;
    mipCount = 1;
  }
  result.setMipBase(MipMapIndex::make(baseMip));
  result.setMipCount(max(mipCount, 1));
  result.setArrayRange(getArrayCount());
  result.setSRV();
  result.sampleStencil = sampleStencil();
  return result;
}

FormatStore BaseTex::getFormat() const { return tex.image ? tex.image->getFormat() : fmt; }

void BaseTex::updateDeviceSampler()
{
  sampler = get_device().getSampler(samplerState);
  lastSamplerState = samplerState;
}

D3D12_CPU_DESCRIPTOR_HANDLE BaseTex::getDeviceSampler()
{
  if (!sampler.ptr || samplerState != lastSamplerState)
  {
    updateDeviceSampler();
  }

  return sampler;
}

Extent3D BaseTex::getMipmapExtent(uint32_t level) const
{
  Extent3D result;
  result.width = max(width >> level, 1);
  result.height = max(height >> level, 1);
  result.depth = resType == RES3D_VOLTEX ? max(depth >> level, 1) : 1;
  return result;
}

void BaseTex::updateTexName()
{
  // don't propagate down to stub images
  if (isStub())
    return;
  if (tex.image)
  {
    get_device().setTexName(tex.image, getResName());
  }
}

void BaseTex::notifySamplerChange()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_sampler(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::notifySRViewChange()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::notifyTextureReplaceFinish()
{
  // if we are ending up here, we are still holding the lock of the state tracker
  // to avoid a deadlock by reentering we have to do the dirtying without a lock
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv_and_sampler_no_lock(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::dirtyBoundSRVsNoLock()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv_no_lock(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::dirtyBoundUAVsNoLock()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (uavBindingStages[s].any())
    {
      dirty_uav_no_lock(this, s, uavBindingStages[s]);
    }
  }
}

void BaseTex::setUAVBinding(uint32_t stage, uint32_t index, bool s)
{
  uavBindingStages[stage].set(index, s);
  stateBitSet.set(acitve_binding_used_offset);
  if (s)
  {
    stateBitSet.reset(active_binding_was_copied_to_stage_offset);
  }
}

void BaseTex::setSRVBinding(uint32_t stage, uint32_t index, bool s)
{
  srvBindingStages[stage].set(index, s);
  stateBitSet.set(acitve_binding_used_offset);
}

void BaseTex::setRTVBinding(uint32_t index, bool s)
{
  G_ASSERT(index < Driver3dRenderTarget::MAX_SIMRT);
  stateBitSet.set(active_binding_rtv_offset + index, s);
  stateBitSet.set(acitve_binding_used_offset);
  if (s)
  {
    stateBitSet.reset(active_binding_was_copied_to_stage_offset);
    stateBitSet.set(active_binding_dirty_rt);
  }
}

void BaseTex::setDSVBinding(bool s)
{
  stateBitSet.set(active_binding_dsv_offset, s);
  stateBitSet.set(acitve_binding_used_offset);
  if (s)
  {
    stateBitSet.reset(active_binding_was_copied_to_stage_offset);
    stateBitSet.set(active_binding_dirty_rt);
  }
}

eastl::bitset<Driver3dRenderTarget::MAX_SIMRT> BaseTex::getRTVBinding() const
{
  eastl::bitset<Driver3dRenderTarget::MAX_SIMRT> ret;
  ret.from_uint64((stateBitSet >> active_binding_rtv_offset).to_uint64());
  return ret;
}

void BaseTex::setUsedWithBindless()
{
  stateBitSet.set(active_binding_bindless_used_offset);
  stateBitSet.set(acitve_binding_used_offset);
}

void BaseTex::setParams(int w, int h, int d, int levels, const char *stat_name)
{
  G_ASSERT(levels > 0);
  fmt = FormatStore::fromCreateFlags(cflg);
  mipLevels = levels;
  width = w;
  height = h;
  depth = d;
  maxMipLevel = 0;
  minMipLevel = levels - 1;
  setTexName(stat_name);
}

ArrayLayerCount BaseTex::getArrayCount() const
{
  if (resType == RES3D_CUBETEX)
  {
    return ArrayLayerCount::make(6);
  }
  else if (resType == RES3D_ARRTEX)
  {
    return ArrayLayerCount::make((isArrayCube() ? 6 : 1) * depth);
  }
  return ArrayLayerCount::make(1);
}

void BaseTex::setResApiName(const char *name) const
{
  G_UNUSED(name);
#if DX12_DOES_SET_DEBUG_NAMES
  wchar_t stringBuf[MAX_OBJECT_NAME_LENGTH];
  DX12_SET_DEBUG_OBJ_NAME(tex.image->getHandle(), lazyToWchar(name, stringBuf, MAX_OBJECT_NAME_LENGTH));
#endif
}

BaseTex::BaseTex(int res_type, uint32_t cflg_) :
  resType(res_type), cflg(cflg_), minMipLevel(20), maxMipLevel(0), lockFlags(0), depth(1), width(0), height(0)
{
  samplerState.setBias(default_lodbias);
  samplerState.setAniso(default_aniso);
  samplerState.setW(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setV(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setU(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setMip(D3D12_FILTER_TYPE_LINEAR);
  samplerState.setFilter(D3D12_FILTER_TYPE_LINEAR);

  if (RES3D_CUBETEX == resType)
  {
    samplerState.setV(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplerState.setU(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
  }
}

BaseTex::~BaseTex() { setRld(nullptr); }

void BaseTex::resolve(Image *dst) { get_device().getContext().resolveMultiSampleImage(tex.image, dst); }

BaseTexture *BaseTex::makeTmpTexResCopy(int w, int h, int d, int l, bool staging_tex)
{
  STORE_RETURN_ADDRESS();
  if (resType != RES3D_ARRTEX && resType != RES3D_VOLTEX && resType != RES3D_CUBEARRTEX)
    d = 1;
  BaseTex *clonedTex = get_device().newTextureObject(resType, cflg | (staging_tex ? TEXCF_SYSMEM : 0));
  if (!staging_tex)
    clonedTex->tidXored = tidXored, clonedTex->stubTexIdx = stubTexIdx;
  clonedTex->setParams(w, h, d, l, String::mk_str_cat(staging_tex ? "stg:" : "tmp:", getTexName()));
  clonedTex->setPreallocBeforeLoad(true);
  clonedTex->setDelayedCreate(true);
  if (!clonedTex->allocateTex())
    del_d3dres(clonedTex);
  return clonedTex;
}
void BaseTex::replaceTexResObject(BaseTexture *&other_tex)
{
  {
    OSSpinlockScopedLock resourceBindingLock(drv3d_dx12::get_resource_binding_guard());
    BaseTex *other = getbasetex(other_tex);
    G_ASSERT_RETURN(other, );

    // swap texture objects
    char tmp[sizeof(tex)];
    memcpy(tmp, &tex, sizeof(tmp));
    memcpy(&tex, &other->tex, sizeof(tmp));
    memcpy(&other->tex, tmp, sizeof(tmp));
    // swap dimensions
    eastl::swap(width, other->width);
    eastl::swap(height, other->height);
    eastl::swap(depth, other->depth);
    eastl::swap(mipLevels, other->mipLevels);
    eastl::swap(minMipLevel, other->minMipLevel);
    eastl::swap(maxMipLevel, other->maxMipLevel);

    other->setWasUsed();
  }
  del_d3dres(other_tex);
}

bool BaseTex::downSize(int new_width, int new_height, int new_depth, int new_mips, unsigned start_src_level, unsigned level_offset)
{
  auto rep = makeTmpTexResCopy(new_width, new_height, new_depth, new_mips, false);
  if (!rep)
  {
    return false;
  }

  auto repTex = getbasetex(rep);

  unsigned sourceLevel = max<unsigned>(level_offset, start_src_level);
  unsigned sourceLevelEnd = min<unsigned>(mipLevels, new_mips + level_offset);

  rep->texmiplevel(sourceLevel - level_offset, sourceLevelEnd - level_offset - 1);

  STORE_RETURN_ADDRESS();
  get_device().getContext().resizeImageMipMapTransfer(getDeviceImage(), repTex->getDeviceImage(),
    MipMapRange::make(sourceLevel, sourceLevelEnd - sourceLevel), 0, level_offset);

  replaceTexResObject(rep);
  return true;
}

bool BaseTex::upSize(int new_width, int new_height, int new_depth, int new_mips, unsigned start_src_level, unsigned level_offset)
{
  auto rep = makeTmpTexResCopy(new_width, new_height, new_depth, new_mips, false);
  if (!rep)
  {
    return false;
  }

  auto repTex = getbasetex(rep);

  unsigned destinationLevel = level_offset + start_src_level;
  unsigned destinationLevelEnd = min<unsigned>(mipLevels + level_offset, new_mips);

  rep->texmiplevel(destinationLevel, destinationLevelEnd - 1);

  STORE_RETURN_ADDRESS();
  get_device().getContext().resizeImageMipMapTransfer(getDeviceImage(), repTex->getDeviceImage(),
    MipMapRange::make(destinationLevel, destinationLevelEnd - destinationLevel), level_offset, 0);

  replaceTexResObject(rep);
  return true;
}

bool BaseTex::allocateTex()
{
  if (tex.image)
    return true;
  STORE_RETURN_ADDRESS();
  switch (resType)
  {
    case RES3D_VOLTEX: return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, nullptr);
    case RES3D_TEX:
    case RES3D_CUBETEX: return create_tex2d(tex, this, width, height, mipLevels, resType == RES3D_CUBETEX, nullptr, 1);
    case RES3D_CUBEARRTEX:
    case RES3D_ARRTEX: return create_tex2d(tex, this, width, height, mipLevels, resType == RES3D_CUBEARRTEX, nullptr, depth);
  }
  return false;
}

void BaseTex::discardTex()
{
  if (stubTexIdx >= 0)
  {
    releaseTex();
    recreate();
  }
}

bool BaseTex::recreate()
{
  STORE_RETURN_ADDRESS();
  if (RES3D_TEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "rt|dyn");
      return create_tex2d(tex, this, width, height, mipLevels, false, NULL, 1);
    }

    if (!preallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "empty");
      return create_tex2d(tex, this, width, height, mipLevels, false, NULL, 1);
    }

    setDelayedCreate(preallocBeforeLoad());
    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex2d(tex, this, 4, 4, 1, false, NULL, 1);
  }

  if (RES3D_CUBETEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "rt|dyn");
      return create_tex2d(tex, this, width, height, mipLevels, true, NULL, 1);
    }

    if (!preallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "empty");
      return create_tex2d(tex, this, width, height, mipLevels, true, NULL, 1);
    }

    setDelayedCreate(preallocBeforeLoad());
    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex2d(tex, this, 4, 4, 1, true, NULL, 1);
  }

  if (RES3D_VOLTEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "rt|dyn");
      return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, NULL);
    }

    if (!preallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "empty");
      return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, NULL);
    }

    setDelayedCreate(preallocBeforeLoad());
    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex3d(tex, this, 4, 4, 1, cflg, 1, NULL);
  }

  if (RES3D_ARRTEX == resType)
  {
    VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "rt|dyn");
    return create_tex2d(tex, this, width, height, mipLevels, isArrayCube(), NULL, depth);
  }

  return false;
}

uint32_t BaseTex::update_flags_for_linear_layout(uint32_t cflags, FormatStore format)
{
#if _TARGET_XBOX
#if !_TARGET_XBOXONE
  if ((TEXCF_RTARGET | TEXCF_LINEAR_LAYOUT) == (cflags & (TEXCF_RTARGET | TEXCF_LINEAR_LAYOUT)))
  {
    // can't use linear layout on render targets, GPU will just hang if we try it
    cflags &= ~(TEXCF_LINEAR_LAYOUT | TEXCF_CPU_CACHED_MEMORY);
  }
#endif
  if (TEXCF_LINEAR_LAYOUT & cflags)
  {
    if (format.isBlockCompressed())
    {
      cflags &= ~(TEXCF_LINEAR_LAYOUT | TEXCF_CPU_CACHED_MEMORY);
    }
  }
#endif
  G_UNUSED(format);
  return cflags;
}

DeviceMemoryClass BaseTex::get_memory_class(uint32_t cflags)
{
#if _TARGET_XBOX
  if (TEXCF_LINEAR_LAYOUT & cflags)
  {
    if (TEXCF_CPU_CACHED_MEMORY & cflags)
    {
      return DeviceMemoryClass::HOST_RESIDENT_HOST_READ_WRITE_IMAGE;
    }
  }
#endif
  if (TEXCF_TILED_RESOURCE & cflags)
  {
    return DeviceMemoryClass::RESERVED_RESOURCE;
  }

  G_UNUSED(cflags);
  return DeviceMemoryClass::DEVICE_RESIDENT_IMAGE;
}

void BaseTex::destroyObject()
{
  releaseTex();
  // engine deletes textures that where just loaded...
  // if not deferred we will end up crashing when it was loaded via streaming load
  get_device().getContext().deleteTexture(this);
}

#if DAGOR_DBGLEVEL < 1
#define G_ASSERT_RETURN_AND_LOG(expression, rv, ...) \
  do                                                 \
  {                                                  \
    if (DAGOR_UNLIKELY(!(expression)))               \
    {                                                \
      logerr(__VA_ARGS__);                           \
      return rv;                                     \
    }                                                \
  } while (0)
#else
#define G_ASSERT_RETURN_AND_LOG(expr, rv, ...)               \
  do                                                         \
  {                                                          \
    const bool g_assert_result_do_ = !!(expr);               \
    G_ASSERTF_EX(g_assert_result_do_, #expr, ##__VA_ARGS__); \
    if (DAGOR_UNLIKELY(!g_assert_result_do_))                \
      return rv;                                             \
  } while (0)
#endif

namespace
{
bool can_be_copy_updated(uint32_t cflg) { return 0 != (cflg & (TEXCF_UPDATE_DESTINATION | TEXCF_RTARGET | TEXCF_UNORDERED)); }
} // namespace

int BaseTex::update(BaseTexture *src)
{
  if (!can_be_copy_updated(cflg))
  {
    logerr("DX12: used update method on texture <%s> that does not support it, the texture needs either TEXCF_UPDATE_DESTINATION, "
           "TEXCF_RTARGET or TEXCF_UNORDERED create flags specified",
      getResName());
    return 0;
  }
  BaseTex *sTex = getbasetex(src);
  if (!sTex)
  {
    logerr("DX12: BaseTex::update for <%s> called with null as source", getTexName());
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (isStub())
  {
    logerr("DX12: BaseTex::update for <%s> is in stub state: stubTexIdx=%d", getTexName(), stubTexIdx);
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (sTex->isStub())
  {
    logerr("DX12: BaseTex::update for <%s>, source <%s> is in stub state: stubTexIdx=%d", getTexName(), sTex->getTexName(),
      sTex->stubTexIdx);
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if ((RES3D_TEX != resType) && (RES3D_CUBETEX != resType) && (RES3D_VOLTEX != resType))
  {
    logerr("DX12: BaseTex::update for <%s> with invalid resType %u", getTexName(), resType);
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  auto srcImage = sTex->tex.image;
  auto dstImage = tex.image;

  if (!srcImage || !dstImage)
  {
    logerr("DX12: BaseTex::update for <%s> and <%s> at least one of the image objects (%p, %p) was "
           "null",
      getTexName(), sTex->getTexName(), dstImage, srcImage);
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (srcImage->getMipLevelRange() != dstImage->getMipLevelRange())
  {
    logerr("DX12: BaseTex::update source <%s> mipmaps %u does not match dest <%s> mipmaps %u", sTex->getTexName(),
      srcImage->getMipLevelRange(), getTexName(), dstImage->getMipLevelRange());
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (srcImage->getArrayLayers() != dstImage->getArrayLayers())
  {
    logerr("DX12: BaseTex::update source <%s> array layers %u does not match dst <%s> "
           "array layers %u",
      sTex->getTexName(), srcImage->getArrayLayers(), getTexName(), dstImage->getArrayLayers());
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  const auto srcFmt = srcImage->getFormat();
  const auto dstFmt = dstImage->getFormat();

  G_ASSERT_RETURN_AND_LOG(srcFmt.isCopyConvertible(dstFmt), 0,
    "DX12: BaseTex::update source <%s> format %s can not be copied to dest <%s> "
    "format %s",
    sTex->getTexName(), srcFmt.getNameString(), getTexName(), dstFmt.getNameString());

  auto sExt = srcImage->getBaseExtent();
  auto dExt = dstImage->getBaseExtent();

  uint32_t sBlockX, sBlockY, dBlockX, dBlockY;
  srcFmt.getBytesPerPixelBlock(&sBlockX, &sBlockY);
  dstFmt.getBytesPerPixelBlock(&dBlockX, &dBlockY);

  sExt.width *= sBlockX;
  sExt.height *= sBlockY;
  dExt.width *= dBlockX;
  dExt.height *= dBlockY;

  if (sExt != dExt)
  {
    logerr("DX12: BaseTex::update source <%s> extent %u %u %u does not match dest <%s> extent %u "
           "%u %u",
      sTex->getTexName(), sExt.width, sExt.height, sExt.depth, getTexName(), dExt.width, dExt.height, dExt.depth);
    G_ASSERTF_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

#if DAGOR_DBGLEVEL > 0
  sTex->setWasUsed();
#endif
  STORE_RETURN_ADDRESS();
  ScopedCommitLock ctxLock{get_device().getContext()};
  if (sTex->isMultisampled())
  {
    sTex->resolve(tex.image);
  }
  else
  {
    // passing no regions is fast whole resource copy
    get_device().getContext().copyImage(sTex->tex.image, tex.image, make_whole_resource_copy_info());
  }
  return 1;
}

int BaseTex::updateSubRegion(BaseTexture *src, int src_subres_idx, int src_x, int src_y, int src_z, int src_w, int src_h, int src_d,
  int dest_subres_idx, int dest_x, int dest_y, int dest_z)
{
  if (!can_be_copy_updated(cflg))
  {
    logerr("DX12: used updateSubRegion method on texture <%s> that does not support it, the texture needs either "
           "TEXCF_UPDATE_DESTINATION, TEXCF_RTARGET or TEXCF_UNORDERED create flags specified",
      getResName());
    return 0;
  }
  STORE_RETURN_ADDRESS();
  if (isStub())
  {
    logerr("updateSubRegion() called for tex=<%s> in stub state: stubTexIdx=%d", getTexName(), stubTexIdx);
    return 0;
  }
  if (src)
    if (BaseTex *stex = getbasetex(src))
      if (stex->isStub())
      {
        logerr("updateSubRegion() called with src tex=<%s> in stub state: stubTexIdx=%d", src->getTexName(), stex->stubTexIdx);
        return 0;
      }

  if ((RES3D_TEX == resType) || (RES3D_CUBETEX == resType) || (RES3D_VOLTEX == resType) || (RES3D_ARRTEX == resType))
  {
    BaseTex *stex = getbasetex(src);
    if (stex && stex->tex.image)
    {
      if (!validate_update_sub_region_params(src, src_subres_idx, src_x, src_y, src_z, src_w, src_h, src_d, this, dest_subres_idx,
            dest_x, dest_y, dest_z))
        return 0;

      // we have to check if the copy offsets and sizes are multiples of the block size
      // of the texture format and round them up.
      // If we don't do that, those copies will reset the device.
      auto sfmt = stex->getFormat();
      auto dfmt = getFormat();

      G_ASSERT_RETURN_AND_LOG(sfmt.isCopyConvertible(dfmt), 0,
        "DX12: BaseTex::updateSubRegion source <%s> format %s can not be copied "
        "to dest <%s> format %s",
        stex->getTexName(), sfmt.getNameString(), getTexName(), dfmt.getNameString());

      uint32_t sbx, sby, dbx, dby;
      sfmt.getBytesPerPixelBlock(&sbx, &sby);
      dfmt.getBytesPerPixelBlock(&dbx, &dby);

      auto fix_block_size = [](auto value, auto block, auto name) {
        if (auto delta = value % block)
        {
          decltype(value) adjusted = value + block - delta;
          debug("DX12: %s = %d is not multiples of %u, adjusting to %u", name, value, block, adjusted);
          return adjusted;
        }
        return value;
      };

      src_x = fix_block_size(src_x, sbx, "src_x");
      src_y = fix_block_size(src_y, sby, "src_y");

      src_w = fix_block_size(src_w, sbx, "src_w");
      src_h = fix_block_size(src_h, sby, "src_h");

      dest_x = fix_block_size(dest_x, dbx, "dest_x");
      dest_y = fix_block_size(dest_y, dby, "dest_y");

      ImageCopy region;
      region.srcSubresource = SubresourceIndex::make(src_subres_idx);
      region.dstSubresource = SubresourceIndex::make(dest_subres_idx);
      region.dstOffset.x = dest_x;
      region.dstOffset.y = dest_y;
      region.dstOffset.z = dest_z;
      region.srcBox.left = src_x;
      region.srcBox.top = src_y;
      region.srcBox.front = src_z;
      region.srcBox.right = src_x + src_w;
      region.srcBox.bottom = src_y + src_h;
      region.srcBox.back = src_z + src_d;

      ScopedCommitLock ctxLock{get_device().getContext()};
      get_device().getContext().copyImage(stex->tex.image, tex.image, region);
#if DAGOR_DBGLEVEL > 0
      stex->setWasUsed();
#endif
      return 1;
    }
  }
  return 0;
}

void BaseTex::destroy()
{
  release();
#if DAGOR_DBGLEVEL > 1
  if (!wasUsed())
    logwarn("texture %p, of size %dx%dx%d total=%dbytes, name=%s was destroyed but was never used "
            "in rendering",
      this, width, height, depth, tex.memSize, getResName());
#elif DAGOR_DBGLEVEL > 0
  if (!wasUsed())
    debug("texture %p, of size %dx%dx%d total=%dbytes, name=%s was destroyed but was never used in "
          "rendering",
      this, width, height, depth, tex.memSize, getResName());
#endif
  destroyObject();
}

int BaseTex::generateMips()
{
  STORE_RETURN_ADDRESS();
  if (mipLevels > 1)
  {
    get_device().getContext().generateMipmaps(tex.image);
  }
  return 1;
}

bool BaseTex::setReloadCallback(IReloadData *_rld)
{
  setRld(_rld);
  return true;
}

static constexpr int TEX_COPIED = 1 << 30;

void D3DTextures::release(uint64_t progress)
{
  auto &device = get_device();
  auto &ctx = device.getContext();

  if (stagingMemory)
  {
    ctx.freeMemory(stagingMemory);
    stagingMemory = HostDeviceSharedMemoryRegion{};
  }

  if (progress)
  {
    ctx.waitForProgress(progress);
  }

  if (image)
  {
    ctx.destroyImage(image);
    image = nullptr;
  }
}

void BaseTex::resetTex()
{
  sampler.ptr = 0;
  if (!isStub() && tex.image)
    TEXQL_ON_RELEASE(this);
  tex.stagingMemory = HostDeviceSharedMemoryRegion{};
  waitProgress = 0;
  tex.image = nullptr;
}

void BaseTex::releaseTex()
{
  STORE_RETURN_ADDRESS();
  notify_delete(this, srvBindingStages, uavBindingStages, getRTVBinding(), getDSVBinding());
  sampler.ptr = 0;
  if (isStub())
    tex.image = nullptr;
  else if (tex.image)
    TEXQL_ON_RELEASE(this);
  tex.release(waitProgress);
  waitProgress = 0;
}

#if _TARGET_XBOX
#if _TARGET_XBOXONE
// wrapper, which wraps call to XGTextureAddressComputer::GetTexelElementOffsetBytes as on scarlett
// it has one extra parameter that returns the bit offset
uint64_t get_texel_element_offset_bytes(XGTextureAddressComputer *computer, uint32_t plane, uint32_t mip_level, uint64_t x, uint32_t y,
  uint32_t z_or_slice, uint32_t sample)
{
  return computer->GetTexelElementOffsetBytes(plane, mip_level, x, y, z_or_slice, sample);
}
#else
// wrapper, which wraps call to XGTextureAddressComputer::GetTexelElementOffsetBytes as on scarlett
// it has one extra parameter that returns the bit offset
uint64_t get_texel_element_offset_bytes(XGTextureAddressComputer *computer, uint32_t plane, uint32_t mip_level, uint64_t x, uint32_t y,
  uint32_t z_or_slice, uint32_t sample)
{
  return computer->GetTexelElementOffsetBytes(plane, mip_level, x, y, z_or_slice, sample, nullptr);
}
#endif
#endif

int BaseTex::lockimg(void **p, int &stride, int lev, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_TEX != resType)
    return 0;

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    if (p)
    {
      if ((flags & TEXLOCK_READ) && (cflg & (TEXCF_RTARGET | TEXCF_UNORDERED)))
      {
        if (waitProgress)
        {
          ctx.waitForProgress(waitProgress);
          waitProgress = 0;
        }
        else
        {
          debug("DX12: Blocking read back of <%s>", getTexName());
          ctx.beginCPUTextureAccess(tex.image);
          // not previously waited on, need to block
          ctx.wait();
        }
      }
      else
      {
        ctx.beginCPUTextureAccess(tex.image);
      }
      auto mem = tex.image->getMemory();
      auto memAccessComputer = tex.image->getAccessComputer();
      auto offset = get_texel_element_offset_bytes(memAccessComputer, 0, lev, 0, 0, 0, 0);
      auto offset2 = get_texel_element_offset_bytes(memAccessComputer, 0, lev, 0, 1, 0, 0);
      stride = offset2 - offset;
      *p = mem.asPointer() + offset;
      lockFlags = flags;
    }
    else
    {
      ctx.beginCPUTextureAccess(tex.image);

      if (flags & TEXLOCK_READ)
      {
        waitProgress = ctx.getRecordingFenceProgress();
      }
    }
    return 1;
  }
#endif

  if (!tex.image && (flags & TEXLOCK_WRITE) && !(flags & TEXLOCK_READ))
    if (!create_tex2d(tex, this, width, height, mipLevels, false, nullptr))
    {
      logerr("failed to auto-create tex.tex2D on lockImg");
      return 0;
    }
  stride = 0;

  uint32_t prevFlags = lockFlags;
  if (cflg & (TEXCF_RTARGET | TEXCF_UNORDERED))
  {
#if DAGOR_DBGLEVEL > 0
    setWasUsed();
#endif
    bool resourceCopied = (lockFlags == TEX_COPIED);
    lockFlags = 0;
    if ((getFormat().isDepth()) && !(flags & TEXLOCK_COPY_STAGING))
    {
      logerr("can't lock depth format");
      return 0;
    }

    DeviceQueueType readBackQueue = DeviceQueueType::READ_BACK;
    if (is_swapchain_color_image(tex.image))
    {
      readBackQueue = DeviceQueueType::GRAPHICS;
    }
    if (!tex.stagingMemory)
    {
      uint64_t size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, mipLevels));
      tex.stagingMemory = device.allocatePersistentBidirectionalMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }

    if (tex.stagingMemory)
    {
      BufferImageCopy copies[MAX_MIPMAPS];
      uint64_t offset = 0;

      // TODO why copy all mips?
      for (uint32_t j = 0; j < mipLevels; ++j)
      {
        BufferImageCopy &copy = copies[j];
        copy.subresourceIndex = calculate_subresource_index(j, 0, 0, mipLevels, 1);
        auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(j));
        copy.layout.Footprint = subResInfo.footprint;
        copy.layout.Offset = offset;
        G_ASSERT(copy.layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
        copy.imageOffset.x = 0;
        copy.imageOffset.y = 0;
        copy.imageOffset.z = 0;
        if (j == lev)
        {
          lockMsr.slicePitch = copy.layout.Footprint.RowPitch * subResInfo.rowCount;
          lockMsr.memSize = copy.layout.Footprint.RowPitch * subResInfo.rowCount;
          lockMsr.ptr = &tex.stagingMemory.pointer[offset];
          lockMsr.rowPitch = copy.layout.Footprint.RowPitch;
        }
        offset += subResInfo.rowCount * copy.layout.Footprint.RowPitch;
        offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      }
      if (flags & TEXLOCK_COPY_STAGING)
      {
        ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, copies, mipLevels, tex.image, readBackQueue));
        tex.stagingMemory.invalidate();
        lockFlags = TEXLOCK_COPY_STAGING;
        return 1;
      }
      if (!p)
      {
        // on some cases the requested copy is never used
        if (waitProgress)
        {
          device.getContext().waitForProgress(waitProgress);
          waitProgress = 0;
        }
        waitProgress = ctx.readBackFromImage(tex.stagingMemory, copies, mipLevels, tex.image, readBackQueue);
      }
      else if (!resourceCopied)
      {
        ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, copies, mipLevels, tex.image, readBackQueue));
        tex.stagingMemory.invalidate();
        *p = lockMsr.ptr;

        stride = lockMsr.rowPitch;
        lockFlags = flags;
        lockedSubRes = calculate_subresource_index(lev, 0, 0, mipLevels, 1);
        return 1;
      }
    }
    if (!p)
    {
      lockFlags = TEX_COPIED;
      stride = 0;
      return 1;
    }
  }
  else
  {
    lockFlags = flags;
    if (p == nullptr)
    {
      if (flags & TEXLOCK_RWMASK)
      {
        logerr("nullptr in lockimg");
        return 0;
      }
    }
    else
      *p = 0;
  }

  if (flags & TEXLOCK_RWMASK)
  {
    G_ASSERT_RETURN(tex.image, 0);
    uint32_t offset = 0;

    lockedSubRes = calculate_subresource_index(lev, 0, 0, mipLevels, 1);

    if (flags & TEXLOCK_DISCARD)
    {
      if (tex.stagingMemory)
      {
        ctx.freeMemory(tex.stagingMemory);
        tex.stagingMemory = HostDeviceSharedMemoryRegion{};
      }
      auto size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(lockedSubRes, 1)); // -V522 12 line above
                                                                                                              // tex.image checked
      tex.stagingMemory = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

      offset = 0;
    }
    else
    {
      if (!tex.stagingMemory)
      {
        auto size = calculate_texture_staging_buffer_size(*tex.image, tex.image->getSubresourceRange());
        tex.stagingMemory = device.allocatePersistentBidirectionalMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }

      offset = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, lockedSubRes));
    }

    if (!tex.stagingMemory)
      return 0;

    auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(lev));
    lockMsr.rowPitch = subResInfo.footprint.RowPitch;
    lockMsr.slicePitch = subResInfo.footprint.RowPitch * subResInfo.rowCount;
    lockMsr.memSize = lockMsr.slicePitch;
    lockMsr.ptr = &tex.stagingMemory.pointer[offset];

    // fast check is ok here
    if (waitProgress)
    {
      if (flags & TEXLOCK_NOSYSLOCK)
      {
        if (waitProgress > ctx.getCompletedFenceProgress())
        {
          if (prevFlags == TEX_COPIED)
            lockFlags = TEX_COPIED;
          return 0;
        }
      }
      else
      {
        ctx.waitForProgress(waitProgress);
      }
      waitProgress = 0;
    }

    *p = lockMsr.ptr;

    stride = lockMsr.rowPitch;
    lockFlags = flags;
    return 1;
  }
  lockFlags = flags;
  return 1;
}

int BaseTex::unlockimg()
{
  STORE_RETURN_ADDRESS();
  auto &device = get_device();
  auto &ctx = device.getContext();
#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    ctx.endCPUTextureAccess(tex.image);
    lockFlags = 0;
    return 1;
  }
#endif
  if (RES3D_TEX == resType)
  {
    // only ever used when texture is RTV or UAV and staging is assumed to be fully written
    if (lockFlags == TEXLOCK_COPY_STAGING)
    {
      BufferImageCopy copies[MAX_MIPMAPS];
      uint64_t offset = 0;

      for (uint32_t j = 0; j < mipLevels; ++j)
      {
        BufferImageCopy &copy = copies[j];
        copy.subresourceIndex = calculate_subresource_index(j, 0, 0, mipLevels, 1);
        auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(j));
        copy.layout.Footprint = subResInfo.footprint;
        copy.layout.Offset = offset;
        G_ASSERT(copy.layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
        copy.imageOffset.x = 0;
        copy.imageOffset.y = 0;
        copy.imageOffset.z = 0;
        offset += subResInfo.rowCount * copy.layout.Footprint.RowPitch;
        offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      }
      tex.stagingMemory.flush();
      ctx.uploadToImage(tex.image, copies, mipLevels, tex.stagingMemory, DeviceQueueType::UPLOAD, false);
      return 1;
    }

    if ((cflg & (TEXCF_RTARGET | TEXCF_UNORDERED)) && (lockFlags == TEX_COPIED))
      return 1;

    ddsx::Header *hdr = (ddsx::Header *)texCopy.data();
    if ((cflg & TEXCF_SYSTEXCOPY) && lockMsr.ptr && hdr && !hdr->compressionType())
    {
      auto rpitch = hdr->getSurfacePitch(lockedSubRes); // for normal tex subres id and mip level are the same
      auto h = hdr->getSurfaceScanlines(lockedSubRes);
      uint8_t *src = (uint8_t *)lockMsr.ptr;
      uint8_t *dest = texCopy.data() + sizeof(ddsx::Header);

      for (uint32_t i = 0; i < lockedSubRes; i++)
        dest += hdr->getSurfacePitch(i) * hdr->getSurfaceScanlines(i);
      G_ASSERT(dest < texCopy.data() + sizeof(ddsx::Header) + hdr->memSz);

      G_ASSERTF(rpitch <= lockMsr.rowPitch, "%dx%d: tex.pitch=%d copy.pitch=%d, lev=%d", width, height, lockMsr.rowPitch, rpitch,
        lockedSubRes);
      for (int y = 0; y < h; y++, dest += rpitch)
        memcpy(dest, src + y * lockMsr.rowPitch, rpitch);
      VERBOSE_DEBUG("%s %dx%d updated DDSx for TEXCF_SYSTEXCOPY", getResName(), hdr->w, hdr->h, data_size(texCopy));
    }

    if (tex.stagingMemory)
    {
      if (tex.image)
      {
        if (lockFlags & TEXLOCK_DISCARD)
        {
          BufferImageCopy copy{};

          copy.subresourceIndex = lockedSubRes;
          auto subResInfo = calculate_texture_subresource_info(*tex.image, SubresourceIndex::make(lockedSubRes));
          copy.layout.Footprint = subResInfo.footprint;
          copy.layout.Offset = 0;
          copy.imageOffset.x = 0;
          copy.imageOffset.y = 0;
          copy.imageOffset.z = 0;
          tex.stagingMemory.flush();
          // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
          // execute the upload on the upload queue, it will move it to the graphics queue.
          ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, true);

          ctx.freeMemory(tex.stagingMemory);
          tex.stagingMemory = HostDeviceSharedMemoryRegion{};
        }
        else if ((lockFlags & (TEXLOCK_RWMASK | TEXLOCK_UPDATEFROMSYSTEX)) != 0 && !(lockFlags & TEXLOCK_DONOTUPDATEON9EXBYDEFAULT) &&
                 !(cflg & TEXCF_RTARGET))
        {
          // TODO: copy only locked level? (check VK too)
          BufferImageCopy copies[MAX_MIPMAPS];
          uint64_t offset = 0;

          for (uint32_t j = 0; j < mipLevels; ++j)
          {
            BufferImageCopy &copy = copies[j];
            copy.subresourceIndex = calculate_subresource_index(j, 0, 0, mipLevels, 1);
            auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(j));
            copy.layout.Footprint = subResInfo.footprint;
            copy.layout.Offset = offset;
            G_ASSERT(copy.layout.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
            copy.imageOffset.x = 0;
            copy.imageOffset.y = 0;
            copy.imageOffset.z = 0;
            offset += subResInfo.rowCount * copy.layout.Footprint.RowPitch;
            offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
          }
          tex.stagingMemory.flush();
          ctx.uploadToImage(tex.image, copies, mipLevels, tex.stagingMemory, DeviceQueueType::UPLOAD, false);
        }
      }
    }

    if (tex.stagingMemory && (lockFlags & TEXLOCK_DELSYSMEMCOPY) && !(cflg & TEXCF_DYNAMIC))
    {
      ctx.freeMemory(tex.stagingMemory);
      tex.stagingMemory = HostDeviceSharedMemoryRegion{};
    }

    lockFlags = 0;
    return 1;
  }
  else if (RES3D_CUBETEX == resType)
  {
    if (tex.stagingMemory)
    {
      if (lockFlags & TEXLOCK_WRITE)
      {
        auto subResInfo = calculate_texture_subresource_info(*tex.image, SubresourceIndex::make(lockedSubRes));
        BufferImageCopy copy{};
        copy.subresourceIndex = lockedSubRes;
        copy.layout.Footprint = subResInfo.footprint;

        tex.stagingMemory.flush();
        // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
        // execute the upload on the upload queue, it will move it to the graphics queue.
        ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, 0 != (lockFlags & TEXLOCK_DISCARD));
      }
      ctx.freeMemory(tex.stagingMemory);
      tex.stagingMemory = HostDeviceSharedMemoryRegion{};
    }

    lockFlags = 0;
    return 1;
  }
  return 0;
}

int BaseTex::lockimg(void **p, int &stride, int face, int level, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_CUBETEX != resType)
    return 0;

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    if (p)
    {
      if ((flags & TEXLOCK_READ) && (cflg & (TEXCF_RTARGET | TEXCF_UNORDERED)))
      {
        if (waitProgress)
        {
          ctx.waitForProgress(waitProgress);
          waitProgress = 0;
        }
        else
        {
          debug("DX12: Blocking read back of <%s>", getTexName());
          ctx.beginCPUTextureAccess(tex.image);
          // not previously waited on, need to block
          ctx.wait();
        }
      }
      else
      {
        ctx.beginCPUTextureAccess(tex.image);
      }
      auto mem = tex.image->getMemory();
      auto memAccessComputer = tex.image->getAccessComputer();
      auto offset = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, face, 0);
      auto offset2 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 1, face, 0);
      stride = offset2 - offset;
      *p = mem.asPointer() + offset;
      lockFlags = flags;
    }
    else
    {
      ctx.beginCPUTextureAccess(tex.image);

      if (flags & TEXLOCK_READ)
      {
        waitProgress = ctx.getRecordingFenceProgress();
      }
    }
    return 1;
  }
#endif

  G_ASSERTF(!(cflg & TEXCF_SYSTEXCOPY), "cube texture with system copy not implemented yet");
  G_ASSERTF(!(flags & TEXLOCK_DISCARD), "Discard for cube texture is not implemented yet");
  G_ASSERTF(!((flags & TEXCF_RTARGET) && (flags & TEXLOCK_WRITE)), "you can not write to a render "
                                                                   "target");

  if ((flags & TEXLOCK_RWMASK))
  {
    lockedSubRes = calculate_subresource_index(level, face, 0, mipLevels, 6);
    auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(level));

    lockFlags = flags;

    if (!tex.stagingMemory)
    {
      tex.stagingMemory =
        device.allocatePersistentBidirectionalMemory(subResInfo.totalByteSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      if (!tex.stagingMemory)
        return 0;

      if (flags & TEXLOCK_READ)
      {
        BufferImageCopy copy{};
        copy.layout.Footprint = subResInfo.footprint;
        copy.subresourceIndex = lockedSubRes;
        if (p)
        {
          logwarn("DX12: blocking texture readback issued");
          ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, &copy, 1, tex.image, DeviceQueueType::READ_BACK));
        }
        else
        {
          waitProgress = ctx.readBackFromImage(tex.stagingMemory, &copy, 1, tex.image, DeviceQueueType::READ_BACK);
        }
      }

      if (p)
      {
        if (waitProgress)
        {
          ctx.waitForProgress(waitProgress);
          waitProgress = 0;
        }

        tex.stagingMemory.invalidate();
        *p = tex.stagingMemory.pointer;
        stride = subResInfo.footprint.RowPitch;
      }
    }
    return 1;
  }
  return 0;
}

int BaseTex::lockbox(void **data, int &row_pitch, int &slice_pitch, int level, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_VOLTEX != resType)
  {
    logwarn("DX12: called lockbox on a non volume texture");
    return 0;
  }

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    if (data)
    {
      if (flags & TEXLOCK_READ)
      {
        if (waitProgress)
        {
          ctx.waitForProgress(waitProgress);
          waitProgress = 0;
        }
        else
        {
          debug("DX12: Blocking read back of <%s>", getTexName());
          ctx.beginCPUTextureAccess(tex.image);
          // not previously waited on, need to block
          ctx.wait();
        }
      }
      else
      {
        ctx.beginCPUTextureAccess(tex.image);
      }
      auto mem = tex.image->getMemory();
      auto memAccessComputer = tex.image->getAccessComputer();
      auto offset = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, 0, 0);
      auto offset2 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 1, 0, 0);
      auto offset3 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, 1, 0);
      row_pitch = offset2 - offset;
      slice_pitch = offset3 - offset;
      *data = mem.asPointer() + offset;
      lockFlags = flags;
    }
    else
    {
      ctx.beginCPUTextureAccess(tex.image);

      if (flags & TEXLOCK_READ)
      {
        waitProgress = ctx.getRecordingFenceProgress();
      }
    }
    return 1;
  }
#endif

  G_ASSERTF(!(flags & TEXLOCK_DISCARD), "Discard for volume texture is not implemented yet");
  G_ASSERTF(data != nullptr, "for lockbox you need to provide a output pointer");
  G_ASSERTF(!((cflg & TEXCF_RTARGET) && (flags & TEXLOCK_WRITE)), "you can not write to a render "
                                                                  "target");

  if ((flags & TEXLOCK_RWMASK) && data)
  {
    lockedSubRes = level;
    lockFlags = flags;

    G_ASSERT(!tex.stagingMemory);

    auto subResInfo = calculate_texture_subresource_info(*tex.image, SubresourceIndex::make(lockedSubRes));
    // only get a buffer large enough to hold the locked level
    tex.stagingMemory = device.allocatePersistentBidirectionalMemory(subResInfo.totalByteSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    if (!tex.stagingMemory)
      return 0;

    if (flags & TEXLOCK_READ)
    {
      BufferImageCopy copy{};
      copy.layout.Footprint = subResInfo.footprint;
      copy.subresourceIndex = lockedSubRes;
      logwarn("DX12: blocking texture readback issued");
      ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, &copy, 1, tex.image, DeviceQueueType::READ_BACK));
      tex.stagingMemory.invalidate();
    }
    *data = lockMsr.ptr = tex.stagingMemory.pointer;
    row_pitch = lockMsr.rowPitch = subResInfo.footprint.RowPitch;
    slice_pitch = lockMsr.slicePitch = subResInfo.footprint.RowPitch * subResInfo.rowCount;
    lockMsr.memSize = static_cast<uint32_t>(subResInfo.totalByteSize);
    return 1;
  }
  else
  {
    logwarn("DX12: lockbox called with no effective action (either no read/write flag or null "
            "pointer passed)");
    return 0;
  }
}

int BaseTex::unlockbox()
{
  STORE_RETURN_ADDRESS();
  if (RES3D_VOLTEX != resType)
    return 0;

  G_ASSERTF(lockFlags != 0, "Unlock without any lock before?");

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    ctx.endCPUTextureAccess(tex.image);
    lockFlags = 0;
    return 1;
  }
#endif

  if ((cflg & TEXCF_SYSTEXCOPY) && data_size(texCopy) && lockMsr.ptr)
  {
    ddsx::Header &hdr = *(ddsx::Header *)texCopy.data();
    G_ASSERT(!hdr.compressionType());

    auto rpitch = hdr.getSurfacePitch(lockedSubRes); // for vol tex sub res index is the same as mip level
    auto h = hdr.getSurfaceScanlines(lockedSubRes);
    auto d = max<uint32_t>(depth >> lockedSubRes, 1);
    uint8_t *src = (uint8_t *)lockMsr.ptr;
    uint8_t *dest = texCopy.data() + sizeof(ddsx::Header);

    for (int i = 0; i < lockedSubRes; i++)
      dest += hdr.getSurfacePitch(i) * hdr.getSurfaceScanlines(i) * max(depth >> i, 1);
    G_ASSERT(dest < texCopy.data() + sizeof(ddsx::Header) + hdr.memSz);

    G_ASSERTF(rpitch <= lockMsr.rowPitch && rpitch * h <= lockMsr.slicePitch, "%dx%dx%d: tex.pitch=%d,%d copy.pitch=%d,%d, lev=%d",
      width, height, depth, lockMsr.rowPitch, lockMsr.slicePitch, rpitch, rpitch * h, lockedSubRes);
    for (int di = 0; di < d; di++, src += lockMsr.slicePitch)
      for (int y = 0; y < h; y++, dest += rpitch)
        memcpy(dest, src + y * lockMsr.rowPitch, rpitch);
    VERBOSE_DEBUG("%s %dx%dx%d updated DDSx for TEXCF_SYSTEXCOPY", getResName(), hdr.w, hdr.h, hdr.depth, data_size(texCopy));
  }
  lockMsr.ptr = nullptr;

  if (tex.stagingMemory)
  {
    if (lockFlags & TEXLOCK_WRITE)
    {
      BufferImageCopy copy{};
      auto subResInfo = calculate_texture_subresource_info(*tex.image, SubresourceIndex::make(lockedSubRes));
      copy.layout.Footprint = subResInfo.footprint;
      copy.subresourceIndex = lockedSubRes;
      tex.stagingMemory.flush();
      // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
      // execute the upload on the upload queue, it will move it to the graphics queue.
      ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, 0 != (lockFlags & TEXLOCK_DISCARD));
    }

    ctx.freeMemory(tex.stagingMemory);
    tex.stagingMemory = HostDeviceSharedMemoryRegion{};
  }

  lockFlags = 0;
  return 1;
}

int BaseTex::ressize() const
{
  if ((cflg & TEXCF_TILED_RESOURCE) != 0)
    return 0;
  if (tex.image && tex.image->isAliased())
    return 0;
  if (isStub())
    return 0;

  Extent3D ext{width, height, resType == RES3D_VOLTEX ? depth : 1u};
  return static_cast<int>(
    calculate_texture_staging_buffer_size(ext, MipMapCount::make(mipLevels), fmt, SubresourceRange::make(0, mipLevels)) *
    getArrayCount().count());
}

int BaseTex::getinfo(TextureInfo &ti, int level) const
{
  level = clamp<int>(level, 0, mipLevels - 1);

  ti.w = max<uint32_t>(1u, width >> level);
  ti.h = max<uint32_t>(1u, height >> level);
  switch (resType)
  {
    case RES3D_CUBETEX:
      ti.d = 1;
      ti.a = 6;
      break;
    case RES3D_CUBEARRTEX:
    case RES3D_ARRTEX:
      ti.d = 1;
      ti.a = getArrayCount().count();
      break;
    case RES3D_VOLTEX:
      ti.d = max<uint32_t>(1u, getDepthSlices() >> level);
      ti.a = 1;
      break;
    default:
      ti.d = 1;
      ti.a = 1;
      break;
  }

  ti.mipLevels = mipLevels;
  ti.resType = resType;
  ti.cflg = cflg;
  return 1;
}

int BaseTex::texaddr(int a)
{
  samplerState.setW(translate_texture_address_mode_to_dx12(a));
  samplerState.setV(translate_texture_address_mode_to_dx12(a));
  samplerState.setU(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddru(int a)
{
  samplerState.setU(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddrv(int a)
{
  samplerState.setV(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddrw(int a)
{
  if (RES3D_VOLTEX == resType)
  {
    samplerState.setW(translate_texture_address_mode_to_dx12(a));
    notifySamplerChange();
    return 1;
  }
  return 0;
}

int BaseTex::texbordercolor(E3DCOLOR c)
{
  samplerState.setBorder(c);
  notifySamplerChange();
  return 1;
}

int BaseTex::texfilter(int m)
{
  samplerState.isCompare = m == TEXFILTER_COMPARE;
  samplerState.setFilter(translate_filter_type_to_dx12(m));
  notifySamplerChange();
  return 1;
}

int BaseTex::texmipmap(int m)
{
  samplerState.setMip(translate_mip_filter_type_to_dx12(m));
  notifySamplerChange();
  return 1;
}

int BaseTex::texlod(float mipmaplod)
{
  samplerState.setBias(mipmaplod);
  notifySamplerChange();
  return 1;
}

int BaseTex::texmiplevel(int minlevel, int maxlevel)
{
  maxMipLevel = (minlevel >= 0) ? minlevel : 0;
  minMipLevel = (maxlevel >= 0) ? maxlevel : (mipLevels - 1);
  notifySRViewChange();
  return 1;
}

int BaseTex::setAnisotropy(int level)
{
  samplerState.setAniso(clamp<int>(level, 1, 16));
  notifySamplerChange();
  return 1;
}

static Texture *create_tex_internal(TexImage32 *img, int w, int h, int flg, int levels, const char *stat_name, Texture *baseTexture)
{
  G_ASSERT_RETURN(d3d::check_texformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    logerr("create_tex: can not create dynamic render target");
    return nullptr;
  }
  if (img)
  {
    w = img->w;
    h = img->h;
  }

  const Driver3dDesc &dd = d3d::get_driver_desc();
  w = clamp<int>(w, dd.mintexw, dd.maxtexw);
  h = clamp<int>(h, dd.mintexh, dd.maxtexh);

  eastl::tie(flg, levels) = fixup_tex_params(w, h, flg, levels);

  if (img)
  {
    levels = 1;

    if (0 == w && 0 == h)
    {
      w = img->w;
      h = img->h;
    }

    if ((w != img->w) || (h != img->h))
    {
      logerr("create_tex: image size differs from texture size (%dx%d != %dx%d)", img->w, img->h, w, h);
      img = nullptr; // abort copying
    }

    if (FormatStore::fromCreateFlags(flg).getBytesPerPixelBlock() != 4)
      img = nullptr;
  }

  // TODO: check for preallocated RT (with requested, not adjusted tex dimensions)

  auto tex = get_device().newTextureObject(RES3D_TEX, flg);

  tex->setParams(w, h, 1, levels, stat_name);
  if (tex->cflg & TEXCF_SYSTEXCOPY)
  {
    if (img)
    {
      uint32_t memSz = w * h * 4;
      clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + memSz);

      ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
      memset(&hdr, 0, sizeof(hdr));
      hdr.label = _MAKE4C('DDSx');
      hdr.d3dFormat = D3DFMT_A8R8G8B8;
      hdr.flags |= ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0) ? hdr.FLG_GAMMA_EQ_1 : 0;
      hdr.w = w;
      hdr.h = h;
      hdr.levels = 1;
      hdr.bitsPerPixel = 32;
      hdr.memSz = memSz;
      /*sysCopyQualityId*/ hdr.hqPartLevels = 0;

      memcpy(tex->texCopy.data() + sizeof(hdr), img + 1, memSz);
      VERBOSE_DEBUG("%s %dx%d stored DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
    }
    else if (tex->cflg & TEXCF_LOADONCE)
    {
      uint32_t memSz = tex->ressize();
      clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + memSz);
      mem_set_0(tex->texCopy);

      ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
      hdr.label = _MAKE4C('DDSx');
      hdr.d3dFormat = texfmt_to_d3dformat(tex->cflg & TEXFMT_MASK);
      hdr.flags |= ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0) ? hdr.FLG_GAMMA_EQ_1 : 0;
      hdr.w = w;
      hdr.h = h;
      hdr.levels = levels;
      hdr.bitsPerPixel = tex->getFormat().getBytesPerPixelBlock();
      if (hdr.d3dFormat == D3DFMT_DXT1)
        hdr.dxtShift = 3;
      else if (hdr.d3dFormat == D3DFMT_DXT3 || hdr.d3dFormat == D3DFMT_DXT5)
        hdr.dxtShift = 4;
      if (hdr.dxtShift)
        hdr.bitsPerPixel = 0;
      hdr.memSz = memSz;
      /*sysCopyQualityId*/ hdr.hqPartLevels = 0;

      VERBOSE_DEBUG("%s %dx%d reserved DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
    }
    else
      tex->cflg &= ~TEXCF_SYSTEXCOPY;
  }

  // only 1 mip
  BaseTex::ImageMem idata;
  if (img)
  {
    idata.ptr = img + 1;
    idata.rowPitch = w * 4;
    idata.slicePitch = 0; // -V1048
    idata.memSize = w * h * 4;
  }

  if (!create_tex2d(tex->tex, tex, w, h, levels, false, img ? &idata : nullptr, 1, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

Texture *d3d::create_tex(TexImage32 *img, int w, int h, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_tex_internal(img, w, h, flg, levels, stat_name, nullptr);
}

static CubeTexture *create_cubetex_internal(int size, int flg, int levels, const char *stat_name, CubeTexture *baseTexture)
{
  G_ASSERT_RETURN(d3d::check_cubetexformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    logerr("create_cubtex: can not create dynamic render target");
    return nullptr;
  }

  const Driver3dDesc &dd = d3d::get_driver_desc();
  size = get_bigger_pow2(clamp<int>(size, dd.mincubesize, dd.maxcubesize));

  eastl::tie(flg, levels) = fixup_tex_params(size, size, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_CUBETEX, flg);
  tex->setParams(size, size, 1, levels, stat_name);

  if (!create_tex2d(tex->tex, tex, size, size, levels, true, nullptr, 1, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

CubeTexture *d3d::create_cubetex(int size, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cubetex_internal(size, flg, levels, stat_name, nullptr);
}

static VolTexture *create_voltex_internal(int w, int h, int d, int flg, int levels, const char *stat_name, VolTexture *baseTexture)
{
  G_ASSERT_RETURN(d3d::check_voltexformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    logerr("create_voltex: can not create dynamic render target");
    return nullptr;
  }

  eastl::tie(flg, levels) = fixup_tex_params(w, h, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_VOLTEX, flg);
  tex->setParams(w, h, d, levels, stat_name);

  if (!create_tex3d(tex->tex, tex, w, h, d, flg, levels, nullptr, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();
  if (tex->cflg & TEXCF_SYSTEXCOPY)
  {
    clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + tex->tex.memSize);
    mem_set_0(tex->texCopy);

    ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
    hdr.label = _MAKE4C('DDSx');
    hdr.d3dFormat = texfmt_to_d3dformat(tex->cflg & TEXFMT_MASK);
    hdr.w = w;
    hdr.h = h;
    hdr.depth = d;
    hdr.levels = levels;
    hdr.bitsPerPixel = tex->getFormat().getBytesPerPixelBlock();
    if (hdr.d3dFormat == D3DFMT_DXT1)
      hdr.dxtShift = 3;
    else if (hdr.d3dFormat == D3DFMT_DXT3 || hdr.d3dFormat == D3DFMT_DXT5)
      hdr.dxtShift = 4;
    if (hdr.dxtShift)
      hdr.bitsPerPixel = 0;
    hdr.flags = hdr.FLG_VOLTEX;
    if ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0)
      hdr.flags |= hdr.FLG_GAMMA_EQ_1;
    hdr.memSz = tex->tex.memSize;
    /*sysCopyQualityId*/ hdr.hqPartLevels = 0;
    VERBOSE_DEBUG("%s %dx%d reserved DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
  }

  return tex;
}

VolTexture *d3d::create_voltex(int w, int h, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_voltex_internal(w, h, d, flg, levels, stat_name, nullptr);
}

static ArrayTexture *create_array_tex_internal(int w, int h, int d, int flg, int levels, const char *stat_name,
  ArrayTexture *baseTexture)
{
  G_ASSERT_RETURN(d3d::check_texformat(flg), nullptr);

  eastl::tie(flg, levels) = fixup_tex_params(w, h, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_ARRTEX, flg);
  tex->setParams(w, h, d, levels, stat_name);

  if (!create_tex2d(tex->tex, tex, w, h, levels, false, nullptr, d, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

ArrayTexture *d3d::create_array_tex(int w, int h, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_array_tex_internal(w, h, d, flg, levels, stat_name, nullptr);
}

static ArrayTexture *create_cube_array_tex_internal(int side, int d, int flg, int levels, const char *stat_name,
  ArrayTexture *baseTexture)
{
  G_ASSERT_RETURN(d3d::check_cubetexformat(flg), nullptr);

  eastl::tie(flg, levels) = fixup_tex_params(side, side, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_ARRTEX, flg);
  tex->setParams(side, side, d, levels, stat_name);
  tex->setIsArrayCube(true);

  if (!create_tex2d(tex->tex, tex, side, side, levels, true, nullptr, d, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

ArrayTexture *d3d::create_cube_array_tex(int side, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cube_array_tex_internal(side, d, flg, levels, stat_name, nullptr);
}

// load compressed texture
BaseTexture *d3d::create_ddsx_tex(IGenLoad &crd, int flg, int quality_id, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  ddsx::Header hdr;
  if (!crd.readExact(&hdr, sizeof(hdr)) || !hdr.checkLabel())
  {
    debug("invalid DDSx format");
    return nullptr;
  }

  BaseTexture *tex = alloc_ddsx_tex(hdr, flg, quality_id, levels, stat_name);
  if (tex)
  {
    BaseTex *bt = (BaseTex *)tex;
    int st_pos = crd.tell();

    G_ASSERTF_AND_DO(hdr.hqPartLevels == 0, bt->cflg &= ~TEXCF_SYSTEXCOPY,
      "cannot use TEXCF_SYSTEXCOPY with base part of split texture!");
    if (bt->cflg & TEXCF_SYSTEXCOPY)
    {
      auto data_sz = hdr.packedSz ? hdr.packedSz : hdr.memSz;
      clear_and_resize(bt->texCopy, sizeof(hdr) + data_sz);
      memcpy(bt->texCopy.data(), &hdr, sizeof(hdr));
      /*sysCopyQualityId*/ ((ddsx::Header *)bt->texCopy.data())->hqPartLevels = quality_id;
      if (!crd.readExact(bt->texCopy.data() + sizeof(hdr), data_sz))
      {
        logerr_ctx("inconsistent input tex data, data_sz=%d tex=%s", data_sz, stat_name);
        del_d3dres(tex);
        return NULL;
      }
      VERBOSE_DEBUG("%s %dx%d stored DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(bt->texCopy));
      InPlaceMemLoadCB mcrd(bt->texCopy.data() + sizeof(hdr), data_sz);
      if (load_ddsx_tex_contents(tex, hdr, mcrd, quality_id))
        return tex;
    }
    else if (load_ddsx_tex_contents(tex, hdr, crd, quality_id))
      return tex;

    if (!is_main_thread())
    {
      for (unsigned attempt = 0, tries = 5, f = dagor_frame_no(); attempt < tries;)
        if (dagor_frame_no() < f + 1)
          sleep_msec(1);
        else
        {
          crd.seekto(st_pos);
          if (load_ddsx_tex_contents(tex, hdr, crd, quality_id))
          {
            debug("finally loaded %s (attempt=%d)", stat_name, attempt + 1);
            return tex;
          }
          f = dagor_frame_no();
          attempt++;
        }
      return tex;
    }
    del_d3dres(tex);
  }
  return nullptr;
}

BaseTexture *d3d::alloc_ddsx_tex(const ddsx::Header &hdr, int flg, int q_id, int levels, const char *stat_name, int stub_tex_idx)
{
  flg = implant_d3dformat(flg, hdr.d3dFormat);
  if (hdr.d3dFormat == D3DFMT_A4R4G4B4 || hdr.d3dFormat == D3DFMT_X4R4G4B4 || hdr.d3dFormat == D3DFMT_R5G6B5)
    flg = implant_d3dformat(flg, D3DFMT_A8R8G8B8);
  G_ASSERT((flg & TEXCF_RTARGET) == 0);
  flg |= (hdr.flags & hdr.FLG_GAMMA_EQ_1) ? 0 : TEXCF_SRGBREAD;

  if (levels <= 0)
    levels = hdr.levels;

  int resType;
  if (hdr.flags & ddsx::Header::FLG_CUBTEX)
    resType = RES3D_CUBETEX;
  else if (hdr.flags & ddsx::Header::FLG_VOLTEX)
    resType = RES3D_VOLTEX;
  else if (hdr.flags & ddsx::Header::FLG_ARRTEX)
    resType = RES3D_ARRTEX;
  else
    resType = RES3D_TEX;

  auto bt = drv3d_dx12::get_device().newTextureObject(resType, flg);

  int skip_levels = hdr.getSkipLevels(hdr.getSkipLevelsFromQ(q_id), levels);
  int w = max(hdr.w >> skip_levels, 1), h = max(hdr.h >> skip_levels, 1), d = max(hdr.depth >> skip_levels, 1);
  if (!(hdr.flags & hdr.FLG_VOLTEX))
    d = (hdr.flags & hdr.FLG_ARRTEX) ? hdr.depth : 1;

  bt->setParams(w, h, d, levels, stat_name);
  bt->stubTexIdx = stub_tex_idx;
  bt->setPreallocBeforeLoad(true);
  bt->setDelayedCreate(true);

  if (stub_tex_idx >= 0)
  {
    // static analysis says this could be null
    auto subtex = bt->getStubTex();
    if (subtex)
      bt->tex.image = subtex->tex.image;
  }

  return bt;
}

#if _TARGET_PC_WIN
unsigned d3d::pcwin32::get_texture_format(BaseTexture *tex)
{
  auto bt = getbasetex(tex);
  if (!bt)
    return 0;
  return bt->getFormat();
}
const char *d3d::pcwin32::get_texture_format_str(BaseTexture *tex)
{
  auto bt = getbasetex(tex);
  if (!bt)
    return nullptr;
  return bt->getFormat().getNameString();
}
void *d3d::pcwin32::get_native_surface(BaseTexture *)
{
  return nullptr; // TODO:: ((BaseTex*)tex)->tex.tex2D;
}
#endif

bool d3d::set_tex_usage_hint(int, int, int, const char *, unsigned int)
{
  debug_ctx("n/a");
  return true;
}

eastl::pair<Image *, HostDeviceSharedMemoryRegion> TextureReplacer::update() const
{
  Image *resultImage = nullptr;
  HostDeviceSharedMemoryRegion resultMemory{};
  // ->> releaseTex
  // rep.target->releaseTex();
  if (target->isStub())
    target->tex.image = nullptr;
  else if (target->tex.image)
    TEXQL_ON_RELEASE(target);
  if (target->tex.stagingMemory)
  {
    resultMemory = target->tex.stagingMemory;
    target->tex.stagingMemory = HostDeviceSharedMemoryRegion{};
  }

  if (target->tex.image)
  {
    resultImage = target->tex.image;
    target->tex.image = nullptr;
  }
  // <<- releaseTex
  target->tex = newTex;
  target->updateTexName();
  // ensures that used textures are rebound on next flush
  target->notifyTextureReplaceFinish();

  return {resultImage, resultMemory};
}


Texture *d3d::alias_tex(Texture *baseTexture, TexImage32 *img, int w, int h, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_tex_internal(img, w, h, flg, levels, stat_name, static_cast<BaseTex *>(baseTexture));
}

CubeTexture *d3d::alias_cubetex(CubeTexture *baseTexture, int size, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cubetex_internal(size, flg, levels, stat_name, baseTexture);
}

VolTexture *d3d::alias_voltex(VolTexture *baseTexture, int w, int h, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_voltex_internal(w, h, d, flg, levels, stat_name, baseTexture);
}

ArrayTexture *d3d::alias_array_tex(ArrayTexture *baseTexture, int w, int h, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_array_tex_internal(w, h, d, flg, levels, stat_name, baseTexture);
}

ArrayTexture *d3d::alias_cube_array_tex(ArrayTexture *baseTexture, int side, int d, int flg, int levels, const char *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cube_array_tex_internal(side, d, flg, levels, stat_name, baseTexture);
}
