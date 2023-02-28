#include <array>
#include <cstring>

#include "impl.h"
#include "util.h"

namespace atfix {

/** Hooking-related stuff */
using PFN_ID3D11Device_CreateBuffer = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_ID3D11Device_CreateDeferredContext = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  UINT, ID3D11DeviceContext**);
using PFN_ID3D11Device_GetImmediateContext = void(STDMETHODCALLTYPE*) (ID3D11Device*, ID3D11DeviceContext**);
using PFN_ID3D11Device_CreateTexture1D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using PFN_ID3D11Device_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_ID3D11Device_CreateTexture3D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);

struct DeviceProcs {
  PFN_ID3D11Device_CreateBuffer                         CreateBuffer                  = nullptr;
  PFN_ID3D11Device_CreateDeferredContext                CreateDeferredContext         = nullptr;
  PFN_ID3D11Device_GetImmediateContext                  GetImmediateContext           = nullptr;
  PFN_ID3D11Device_CreateTexture1D                      CreateTexture1D               = nullptr;
  PFN_ID3D11Device_CreateTexture2D                      CreateTexture2D               = nullptr;
  PFN_ID3D11Device_CreateTexture3D                      CreateTexture3D               = nullptr;
};

static mutex  g_hookMutex;
static mutex  g_globalMutex;

DeviceProcs   g_deviceProcs;

constexpr uint32_t HOOK_DEVICE  = (1u << 0);

uint32_t      g_installedHooks = 0u;

const DeviceProcs* getDeviceProcs(ID3D11Device* pDevice) {
  return &g_deviceProcs;
}

/** Metadata */
static const GUID IID_StagingShadowResource = {0xe2728d91,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

struct ATFIX_RESOURCE_INFO {
  D3D11_RESOURCE_DIMENSION Dim;
  DXGI_FORMAT Format;
  uint32_t Width;
  uint32_t Height;
  uint32_t Depth;
  uint32_t Layers;
  uint32_t Mips;
  D3D11_USAGE Usage;
  uint32_t BindFlags;
  uint32_t MiscFlags;
  uint32_t CPUFlags;
};

void* ptroffset(void* base, ptrdiff_t offset) {
  auto address = reinterpret_cast<uintptr_t>(base) + offset;
  return reinterpret_cast<void*>(address);
}

uint32_t getFormatPixelSize(
        DXGI_FORMAT               Format) {
  struct FormatRange {
    DXGI_FORMAT MinFormat;
    DXGI_FORMAT MaxFormat;
    uint32_t FormatSize;
  };

  static const std::array<FormatRange, 7> s_ranges = {{
    { DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_SINT,    16u },
    { DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_SINT,       12u },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R32G32_SINT,          8u  },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R32_SINT,             4u  },
    { DXGI_FORMAT_B8G8R8A8_UNORM,         DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,  4u  },
    { DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R16_SINT,             2u  },
    { DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_A8_UNORM,             1u  },
  }};

  for (const auto& range : s_ranges) {
    if (Format >= range.MinFormat && Format <= range.MaxFormat)
      return range.FormatSize;
  }

  log("Unhandled format ", Format);
  return 1u;
}

bool getResourceInfo(
        ID3D11Resource*           pResource,
        ATFIX_RESOURCE_INFO*      pInfo) {
  pResource->GetType(&pInfo->Dim);

  switch (pInfo->Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      pInfo->Format = DXGI_FORMAT_UNKNOWN;
      pInfo->Width = desc.ByteWidth;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = 1;
      pInfo->Mips = 1;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = desc.Depth;
      pInfo->Layers = 1;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    default:
      log("Unhandled resource dimension ", pInfo->Dim);
      return false;
  }
}

D3D11_BOX getResourceBox(
  const ATFIX_RESOURCE_INFO*      pInfo,
        UINT                      Subresource) {
  uint32_t mip = Subresource % pInfo->Mips;

  uint32_t w = std::max(pInfo->Width >> mip, 1u);
  uint32_t h = std::max(pInfo->Height >> mip, 1u);
  uint32_t d = std::max(pInfo->Depth >> mip, 1u);

  return D3D11_BOX { 0, 0, 0, w, h, d };
}

bool isImmediatecontext(
        ID3D11DeviceContext*      pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

bool isCpuWritableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING || pInfo->Usage == D3D11_USAGE_DYNAMIC)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_WRITE)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

bool isCpuReadableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_READ)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

ID3D11Resource* createShadowResourceLocked(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  ID3D11Device* device = nullptr;
  pContext->GetDevice(&device);

  ATFIX_RESOURCE_INFO resourceInfo = { };
  getResourceInfo(pBaseResource, &resourceInfo);

  ID3D11Resource* shadowResource = nullptr;
  HRESULT hr;

  switch (resourceInfo.Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
      desc.StructureByteStride = 0;

      ID3D11Buffer* shadowBuffer = nullptr;
      hr = device->CreateBuffer(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture1D* shadowBuffer = nullptr;
      hr = device->CreateTexture1D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture2D* shadowBuffer = nullptr;
      hr = device->CreateTexture2D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture3D* shadowBuffer = nullptr;
      hr = device->CreateTexture3D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    default:
      log("Unhandled resource dimension ", resourceInfo.Dim);
      hr = E_INVALIDARG;
  }

  if (SUCCEEDED(hr)) {
    pContext->CopyResource(shadowResource, pBaseResource);
    pBaseResource->SetPrivateDataInterface(IID_StagingShadowResource, shadowResource);
  } else
    log("Failed to create shadow resource, hr ", std::hex, hr);

  device->Release();
  return shadowResource;
}

ID3D11Resource* getShadowResourceLocked(
        ID3D11Resource*           pBaseResource) {
  ID3D11Resource* shadowResource = nullptr;
  UINT resultSize = sizeof(shadowResource);
  
  if (SUCCEEDED(pBaseResource->GetPrivateData(IID_StagingShadowResource, &resultSize, &shadowResource)))
    return shadowResource;

  return nullptr;
}

ID3D11Resource* getShadowResource(
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  return getShadowResourceLocked(pBaseResource);
}

ID3D11Resource* getOrCreateShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  ID3D11Resource* shadowResource = getShadowResourceLocked(pBaseResource);

  if (!shadowResource)
    shadowResource = createShadowResourceLocked(pContext, pBaseResource);

  return shadowResource;
}

void updateViewShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11View*               pView) {

  ID3D11Resource* baseResource;
  pView->GetResource(&baseResource);

  ID3D11Resource* shadowResource = getShadowResource(baseResource);

  if (shadowResource) {
    ATFIX_RESOURCE_INFO resourceInfo = { };
    getResourceInfo(baseResource, &resourceInfo);

    uint32_t mipLevel = 0;
    uint32_t layerIndex = 0;
    uint32_t layerCount = 1;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;

    if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&rtv)))) {
      D3D11_RENDER_TARGET_VIEW_DESC desc = { };
      rtv->GetDesc(&desc);
      rtv->Release();

      switch (desc.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled RTV dimension ", desc.ViewDimension);
      }
    } else if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&uav)))) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc = { };
      uav->GetDesc(&desc);
      uav->Release();

      switch (desc.ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER:
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled UAV dimension ", desc.ViewDimension);
      }
    } else {
      log("Unhandled view type");
    }

    for (uint32_t i = 0; i < layerCount; i++) {
      uint32_t subresource = D3D11CalcSubresource(mipLevel, layerIndex + i, resourceInfo.Mips);

      pContext->CopySubresourceRegion(
        shadowResource, subresource, 0, 0, 0,
        baseResource,   subresource, nullptr);
    }

    shadowResource->Release();
  }

  baseResource->Release();
}

void updateRtvShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11RenderTargetView*, 8> rtvs;
  pContext->OMGetRenderTargets(rtvs.size(), rtvs.data(), nullptr);

  for (ID3D11RenderTargetView* rtv : rtvs) {
    if (rtv) {
      updateViewShadowResource(pContext, rtv);
      rtv->Release();
    }
  }
}

void updateUavShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11UnorderedAccessView*, 8> uavs;
  pContext->CSGetUnorderedAccessViews(0, uavs.size(), uavs.data());

  for (ID3D11UnorderedAccessView* uav : uavs) {
    if (uav) {
      updateViewShadowResource(pContext, uav);
      uav->Release();
    }
  }
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateBuffer(
        ID3D11Device*             pDevice,
  const D3D11_BUFFER_DESC*        pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Buffer**            ppBuffer) {
  auto procs = getDeviceProcs(pDevice);
  D3D11_BUFFER_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateBuffer(pDevice, pDesc, pData, ppBuffer);
}

void STDMETHODCALLTYPE ID3D11Device_GetImmediateContext(
        ID3D11Device*             pDevice,
        ID3D11DeviceContext**     ppImmediateContext) {
  auto procs = getDeviceProcs(pDevice);
  procs->GetImmediateContext(pDevice, ppImmediateContext);
  *ppImmediateContext = hookContext(*ppImmediateContext);
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDeferredContext(
        ID3D11Device*             pDevice,
        UINT                      Flags,
        ID3D11DeviceContext**     ppDeferredContext) {
  auto procs = getDeviceProcs(pDevice);
  HRESULT hr = procs->CreateDeferredContext(pDevice, Flags, ppDeferredContext);

  if (SUCCEEDED(hr) && ppDeferredContext)
    *ppDeferredContext = hookContext(*ppDeferredContext);
  return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture1D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE1D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture1D**         ppTexture) {
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE1D_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateTexture1D(pDevice, pDesc, pData, ppTexture);
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture2D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE2D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture2D**         ppTexture) {
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE2D_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateTexture2D(pDevice, pDesc, pData, ppTexture);
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateTexture3D(
        ID3D11Device*             pDevice,
  const D3D11_TEXTURE3D_DESC*     pDesc,
  const D3D11_SUBRESOURCE_DATA*   pData,
        ID3D11Texture3D**         ppTexture) {
  auto procs = getDeviceProcs(pDevice);
  D3D11_TEXTURE3D_DESC desc;

  if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
    desc = *pDesc;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    pDesc = &desc;
  }

  return procs->CreateTexture3D(pDevice, pDesc, pData, ppTexture);
}

HRESULT tryCpuCopy(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  ATFIX_RESOURCE_INFO dstInfo = { };
  getResourceInfo(pDstResource, &dstInfo);

  if (!isCpuWritableResource(&dstInfo))
    return E_INVALIDARG;

  /* Compute source region for the given copy */
  ATFIX_RESOURCE_INFO srcInfo = { };
  getResourceInfo(pSrcResource, &srcInfo);

  D3D11_BOX srcBox = getResourceBox(&srcInfo, SrcSubresource);
  D3D11_BOX dstBox = getResourceBox(&dstInfo, DstSubresource);

  if (pSrcBox)
    srcBox = *pSrcBox;

  uint32_t w = std::min(srcBox.right - srcBox.left, dstBox.right - DstX);
  uint32_t h = std::min(srcBox.bottom - srcBox.top, dstBox.bottom - DstY);
  uint32_t d = std::min(srcBox.back - srcBox.front, dstBox.back - DstZ);

  srcBox = { srcBox.left,     srcBox.top,     srcBox.front,
             srcBox.left + w, srcBox.top + h, srcBox.front + d };

  dstBox = { DstX,     DstY,     DstZ,
             DstX + w, DstY + h, DstZ + d };

  if (!w || !h || !d)
    return S_OK;

  /* Check if we can map the destination resource immediately. The
   * engine creates all buffers that cause the severe stalls right
   * before mapping them, so this should succeed. */
  D3D11_MAPPED_SUBRESOURCE dstSr;
  D3D11_MAPPED_SUBRESOURCE srcSr;
  HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;

  if (dstInfo.Usage == D3D11_USAGE_DYNAMIC) {
    /* Don't bother with dynamic images etc., haven't seen a situation where it's relevant */
    if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER && w == dstInfo.Width)
      hr = pContext->Map(pDstResource, DstSubresource, D3D11_MAP_WRITE_DISCARD, 0, &dstSr);
  } else {
    hr = pContext->Map(pDstResource, DstSubresource, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &dstSr);
  }

  if (FAILED(hr)) {
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
      log("Failed to map destination resource, hr 0x", std::hex, hr);
      log("Resource dim ", dstInfo.Dim, ", size ", dstInfo.Width , "x", dstInfo.Height, ", usage ", dstInfo.Usage);
    }
    return hr;
  }

  ID3D11Resource* shadowResource = nullptr;

  if (!isCpuReadableResource(&srcInfo)) {
    shadowResource = getOrCreateShadowResource(pContext, pSrcResource);
    hr = pContext->Map(shadowResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      shadowResource->Release();

      log("Failed to map shadow resource, hr 0x", std::hex, hr);
      pContext->Unmap(pDstResource, DstSubresource);
      return hr;
    }
  } else {
    hr = pContext->Map(pSrcResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      log("Failed to map source resource, hr 0x", std::hex, hr);
      log("Resource dim ", srcInfo.Dim, ", size ", srcInfo.Width , "x", srcInfo.Height, ", usage ", srcInfo.Usage);
      pContext->Unmap(pDstResource, DstSubresource);
      return hr;
    }
  }

  /* Do the copy */
  if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    std::memcpy(
      ptroffset(dstSr.pData, dstBox.left),
      ptroffset(srcSr.pData, srcBox.left), w);
  } else {
    uint32_t formatSize = getFormatPixelSize(dstInfo.Format);

    for (uint32_t z = 0; z < d; z++) {
      for (uint32_t y = 0; y < h; y++) {
        uint32_t dstOffset = (dstBox.left) * formatSize
                           + (dstBox.top + y) * dstSr.RowPitch
                           + (dstBox.front + z) * dstSr.DepthPitch;
        uint32_t srcOffset = (srcBox.left) * formatSize
                           + (srcBox.top + y) * srcSr.RowPitch
                           + (srcBox.front + z) * srcSr.DepthPitch;
        std::memcpy(
          ptroffset(dstSr.pData, dstOffset),
          ptroffset(srcSr.pData, srcOffset),
          w * formatSize);
      }
    }
  }

  pContext->Unmap(pDstResource, DstSubresource);

  if (shadowResource) {
    pContext->Unmap(shadowResource, SrcSubresource);
    shadowResource->Release();
  } else {
    pContext->Unmap(pSrcResource, SrcSubresource);
  }

  return S_OK;
}

#define HOOK_PROC(iface, object, table, index, proc) \
  hookProc(object, #iface "::" #proc, &table->proc, &iface ## _ ## proc, index)

template<typename T>
void hookProc(void* pObject, const char* pName, T** ppOrig, T* pHook, uint32_t index) {
  void** vtbl = *reinterpret_cast<void***>(pObject);

  MH_STATUS mh = MH_CreateHook(vtbl[index],
    reinterpret_cast<void*>(pHook),
    reinterpret_cast<void**>(ppOrig));

  if (mh) {
    if (mh != MH_ERROR_ALREADY_CREATED)
      log("Failed to create hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  mh = MH_EnableHook(vtbl[index]);

  if (mh) {
    log("Failed to enable hook for ", pName, ": ", MH_StatusToString(mh));
    return;
  }

  log("Created hook for ", pName, " @ ", reinterpret_cast<void*>(pHook));
}

void hookDevice(ID3D11Device* pDevice) {
  std::lock_guard lock(g_hookMutex);

  if (g_installedHooks & HOOK_DEVICE)
    return;

  log("Hooking device ", pDevice);

  DeviceProcs* procs = &g_deviceProcs;
  HOOK_PROC(ID3D11Device, pDevice, procs, 3,  CreateBuffer);
  HOOK_PROC(ID3D11Device, pDevice, procs, 27, CreateDeferredContext);
  HOOK_PROC(ID3D11Device, pDevice, procs, 40, GetImmediateContext);
  HOOK_PROC(ID3D11Device, pDevice, procs, 4,  CreateTexture1D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 5,  CreateTexture2D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 6,  CreateTexture3D);

  g_installedHooks |= HOOK_DEVICE;
}

class ContextWrapper final : public ID3D11DeviceContext {
  LONG refcnt;
  ID3D11DeviceContext* ctx;

public:
  ContextWrapper(ID3D11DeviceContext* ctx_) : refcnt(1), ctx(ctx_) {}

  // IUnknown
  HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
    LPOLESTR iidstr;
    if (StringFromIID(riid, &iidstr) == S_OK) {
      char buf[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, iidstr, -1, buf, sizeof(buf), nullptr, nullptr);
      log("ID3D11DeviceContext QueryInterface ", buf);
      CoTaskMemFree(iidstr);
    } else {
      log("ID3D11DeviceContext QueryInterface <failed to get iid str>");
    }
    return ctx->QueryInterface(riid, ppvObject);
  }
  ULONG AddRef() override { return InterlockedAdd(&refcnt, 1); }
  ULONG Release() override {
    ULONG res = InterlockedAdd(&refcnt, -1);
    if (res == 0) {
      ctx->Release();
      delete this;
    }
    return res;
  }

  // ID3D11DeviceChild
  void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override { ctx->GetDevice(ppDevice); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override { return ctx->GetPrivateData(guid, pDataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override { return ctx->SetPrivateData(guid, DataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override { return ctx->SetPrivateDataInterface(guid, pData); }

  // ID3D11DeviceContext
  void VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances); }
  void PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->PSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances); }
  void DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override { ctx->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation); }
  void Draw(UINT VertexCount, UINT StartVertexLocation) override { ctx->Draw(VertexCount, StartVertexLocation); }
  HRESULT Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override { return ctx->Map(pResource, Subresource, MapType, MapFlags, pMappedResource); }
  void Unmap(ID3D11Resource* pResource, UINT Subresource) override { ctx->Unmap(pResource, Subresource); }
  void PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void IASetInputLayout(ID3D11InputLayout* pInputLayout) override { ctx->IASetInputLayout(pInputLayout); }
  void IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override { ctx->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
  void IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override { ctx->IASetIndexBuffer(pIndexBuffer, Format, Offset); }
  void DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override { ctx->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
  void DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override { ctx->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
  void GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->GSSetShader(pShader, ppClassInstances, NumClassInstances); }
  void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override { ctx->IASetPrimitiveTopology(Topology); }
  void VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->VSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void Begin(ID3D11Asynchronous* pAsync) override { ctx->Begin(pAsync); }
  void End(ID3D11Asynchronous* pAsync) override { ctx->End(pAsync); }
  HRESULT GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) override { return ctx->GetData(pAsync, pData, DataSize, GetDataFlags); }
  void SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override { ctx->SetPredication(pPredicate, PredicateValue); }
  void GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->GSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override { ctx->OMSetBlendState(pBlendState, BlendFactor, SampleMask); }
  void OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override { ctx->OMSetDepthStencilState(pDepthStencilState, StencilRef); }
  void SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) override { ctx->SOSetTargets(NumBuffers, ppSOTargets, pOffsets); }
  void DrawAuto() override { ctx->DrawAuto(); }
  void DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override { ctx->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs); }
  void DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override { ctx->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs); }
  void RSSetState(ID3D11RasterizerState* pRasterizerState) override { ctx->RSSetState(pRasterizerState); }
  void RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override { ctx->RSSetViewports(NumViewports, pViewports); }
  void RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override { ctx->RSSetScissorRects(NumRects, pRects); }
  void CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) override { ctx->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView); }
  void ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override { ctx->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil); }
  void GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override { ctx->GenerateMips(pShaderResourceView); }
  void SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override { ctx->SetResourceMinLOD(pResource, MinLOD); }
  FLOAT GetResourceMinLOD(ID3D11Resource* pResource) override { return ctx->GetResourceMinLOD(pResource); }
  void ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override { ctx->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
  void ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override { ctx->ExecuteCommandList(pCommandList, RestoreContextState); }
  void HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->HSSetShader(pHullShader, ppClassInstances, NumClassInstances); }
  void HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->HSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances); }
  void DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->DSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override { ctx->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts); }
  void CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances); }
  void CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->CSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }
  void PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }
  void PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override { ctx->IAGetInputLayout(ppInputLayout); }
  void IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override { ctx->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
  void IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override { ctx->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
  void GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }
  void IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override { ctx->IAGetPrimitiveTopology(pTopology); }
  void VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override { ctx->GetPredication(ppPredicate, pPredicateValue); }
  void GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView) override { ctx->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView); }
  void OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override { ctx->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews); }
  void OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override { ctx->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }
  void OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override { ctx->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }
  void SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) override { ctx->SOGetTargets(NumBuffers, ppSOTargets); }
  void RSGetState(ID3D11RasterizerState** ppRasterizerState) override { ctx->RSGetState(ppRasterizerState); }
  void RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override { ctx->RSGetViewports(pNumViewports, pViewports); }
  void RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override { ctx->RSGetScissorRects(pNumRects, pRects); }
  void HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void HSGetShader(ID3D11HullShader** ppHullShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }
  void HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void DSGetShader(ID3D11DomainShader** ppDomainShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }
  void DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override { ctx->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews); }
  void CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }
  void CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void ClearState() override { ctx->ClearState(); }
  void Flush() override { ctx->Flush(); }
  D3D11_DEVICE_CONTEXT_TYPE GetType() override { return ctx->GetType(); }
  UINT GetContextFlags() override { return ctx->GetContextFlags(); }
  HRESULT FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override { return ctx->FinishCommandList(RestoreDeferredContextState, ppCommandList); }

  void ClearRenderTargetView(
          ID3D11RenderTargetView*   pRTV,
    const FLOAT                     pColor[4]) override {
    ctx->ClearRenderTargetView(pRTV, pColor);
    if (pRTV)
      updateViewShadowResource(ctx, pRTV);
  }

  void ClearUnorderedAccessViewFloat(
          ID3D11UnorderedAccessView* pUAV,
    const FLOAT                     pColor[4]) override {
    ctx->ClearUnorderedAccessViewFloat(pUAV, pColor);
    if (pUAV)
      updateViewShadowResource(ctx, pUAV);
  }

  void ClearUnorderedAccessViewUint(
          ID3D11UnorderedAccessView* pUAV,
    const UINT                      pColor[4]) override {
    ctx->ClearUnorderedAccessViewUint(pUAV, pColor);
    if (pUAV)
      updateViewShadowResource(ctx, pUAV);
  }

  void CopyResource(
          ID3D11Resource*           pDstResource,
          ID3D11Resource*           pSrcResource) override {
    ID3D11Resource* dstShadow = getShadowResource(pDstResource);

    bool needsBaseCopy = true;
    bool needsShadowCopy = true;

    if (isImmediatecontext(ctx)) {
      HRESULT hr = tryCpuCopy(ctx, pDstResource,
        0, 0, 0, 0, pSrcResource, 0, nullptr);
      needsBaseCopy = FAILED(hr);

      if (!needsBaseCopy && dstShadow) {
        hr = tryCpuCopy(ctx, dstShadow,
          0, 0, 0, 0, pSrcResource, 0, nullptr);
        needsShadowCopy = FAILED(hr);
      }
    }

    if (needsBaseCopy)
      ctx->CopyResource(pDstResource, pSrcResource);

    if (dstShadow) {
      if (needsShadowCopy)
        ctx->CopyResource(dstShadow, pSrcResource);

      dstShadow->Release();
    }
  }

  void CopySubresourceRegion(
          ID3D11Resource*           pDstResource,
          UINT                      DstSubresource,
          UINT                      DstX,
          UINT                      DstY,
          UINT                      DstZ,
          ID3D11Resource*           pSrcResource,
          UINT                      SrcSubresource,
    const D3D11_BOX*                pSrcBox) override {

    ID3D11Resource* dstShadow = getShadowResource(pDstResource);

    bool needsBaseCopy = true;
    bool needsShadowCopy = true;

    if (isImmediatecontext(ctx)) {
      HRESULT hr = tryCpuCopy(ctx,
        pDstResource, DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
      needsBaseCopy = FAILED(hr);

      if (!needsBaseCopy && dstShadow) {
        hr = tryCpuCopy(ctx,
          dstShadow,    DstSubresource, DstX, DstY, DstZ,
          pSrcResource, SrcSubresource, pSrcBox);
        needsShadowCopy = FAILED(hr);
      }
    }

    if (needsBaseCopy) {
      ctx->CopySubresourceRegion(
        pDstResource, DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
    }

    if (dstShadow) {
      if (needsShadowCopy) {
        ATFIX_RESOURCE_INFO srcInfo = { };
        getResourceInfo(pSrcResource, &srcInfo);

        ctx->CopySubresourceRegion(
          dstShadow,    DstSubresource, DstX, DstY, DstZ,
          pSrcResource, SrcSubresource, pSrcBox);
      }

      dstShadow->Release();
    }
  }

  void Dispatch(
          UINT                      X,
          UINT                      Y,
          UINT                      Z) override {
    ctx->Dispatch(X, Y, Z);
    updateUavShadowResources(ctx);
  }

  void DispatchIndirect(
          ID3D11Buffer*             pParameterBuffer,
          UINT                      pParameterOffset) override {
    ctx->DispatchIndirect(pParameterBuffer, pParameterOffset);
    updateUavShadowResources(ctx);
  }

  void OMSetRenderTargets(
          UINT                      RTVCount,
          ID3D11RenderTargetView* const* ppRTVs,
          ID3D11DepthStencilView*   pDSV) override {
    updateRtvShadowResources(ctx);
    ctx->OMSetRenderTargets(RTVCount, ppRTVs, pDSV);
  }

  void OMSetRenderTargetsAndUnorderedAccessViews(
          UINT                      RTVCount,
          ID3D11RenderTargetView* const* ppRTVs,
          ID3D11DepthStencilView*   pDSV,
          UINT                      UAVIndex,
          UINT                      UAVCount,
          ID3D11UnorderedAccessView* const* ppUAVs,
    const UINT*                     pUAVClearValues) override {
    updateRtvShadowResources(ctx);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(
      RTVCount, ppRTVs, pDSV, UAVIndex, UAVCount, ppUAVs, pUAVClearValues);
  }

  void UpdateSubresource(
          ID3D11Resource*           pResource,
          UINT                      Subresource,
    const D3D11_BOX*                pBox,
    const void*                     pData,
          UINT                      RowPitch,
          UINT                      SlicePitch) override {
    ctx->UpdateSubresource(
      pResource, Subresource, pBox, pData, RowPitch, SlicePitch);

    ID3D11Resource* shadowResource = getShadowResource(pResource);
    if (shadowResource) {
      ctx->UpdateSubresource(
        shadowResource, Subresource, pBox, pData, RowPitch, SlicePitch);
      shadowResource->Release();
    }
  }
};

ID3D11DeviceContext* hookContext(ID3D11DeviceContext* pContext) {
  log("Hooking context ", pContext);
  return new ContextWrapper(pContext);
}

}