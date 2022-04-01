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
using PFN_ID3D11Device_CreateTexture1D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using PFN_ID3D11Device_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_ID3D11Device_CreateTexture3D = HRESULT (STDMETHODCALLTYPE *) (ID3D11Device*,
  const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);

using PFN_ID3D11DeviceContext_ClearRenderTargetView = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11RenderTargetView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const FLOAT[4]);
using PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11UnorderedAccessView*, const UINT[4]);
using PFN_ID3D11DeviceContext_CopyResource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, ID3D11Resource*);
using PFN_ID3D11DeviceContext_CopySubresourceRegion = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_ID3D11DeviceContext_CopyStructureCount = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT, ID3D11UnorderedAccessView*);
using PFN_ID3D11DeviceContext_Dispatch = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, UINT, UINT);
using PFN_ID3D11DeviceContext_DispatchIndirect = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Buffer*, UINT);
using PFN_ID3D11DeviceContext_OMSetRenderTargets = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_ID3D11DeviceContext_UpdateSubresource = void (STDMETHODCALLTYPE *) (ID3D11DeviceContext*,
  ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

struct DeviceProcs {
  PFN_ID3D11Device_CreateBuffer                         CreateBuffer                  = nullptr;
  PFN_ID3D11Device_CreateDeferredContext                CreateDeferredContext         = nullptr;
  PFN_ID3D11Device_CreateTexture1D                      CreateTexture1D               = nullptr;
  PFN_ID3D11Device_CreateTexture2D                      CreateTexture2D               = nullptr;
  PFN_ID3D11Device_CreateTexture3D                      CreateTexture3D               = nullptr;
};

struct ContextProcs {
  PFN_ID3D11DeviceContext_ClearRenderTargetView         ClearRenderTargetView         = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewFloat ClearUnorderedAccessViewFloat = nullptr;
  PFN_ID3D11DeviceContext_ClearUnorderedAccessViewUint  ClearUnorderedAccessViewUint  = nullptr;
  PFN_ID3D11DeviceContext_CopyResource                  CopyResource                  = nullptr;
  PFN_ID3D11DeviceContext_CopySubresourceRegion         CopySubresourceRegion         = nullptr;
  PFN_ID3D11DeviceContext_CopyStructureCount            CopyStructureCount            = nullptr;
  PFN_ID3D11DeviceContext_Dispatch                      Dispatch                      = nullptr;
  PFN_ID3D11DeviceContext_DispatchIndirect              DispatchIndirect              = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargets            OMSetRenderTargets            = nullptr;
  PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews OMSetRenderTargetsAndUnorderedAccessViews = nullptr;
  PFN_ID3D11DeviceContext_UpdateSubresource             UpdateSubresource             = nullptr;
};

static mutex  g_hookMutex;
static mutex  g_globalMutex;

DeviceProcs   g_deviceProcs;
ContextProcs  g_immContextProcs;
ContextProcs  g_defContextProcs;

constexpr uint32_t HOOK_DEVICE  = (1u << 0);
constexpr uint32_t HOOK_IMM_CTX = (1u << 1);
constexpr uint32_t HOOK_DEF_CTX = (1u << 2);

uint32_t      g_installedHooks = 0u;

const DeviceProcs* getDeviceProcs(ID3D11Device* pDevice) {
  return &g_deviceProcs;
}

const ContextProcs* getContextProcs(ID3D11DeviceContext* pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_immContextProcs
    : &g_defContextProcs;
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
  auto procs = getContextProcs(pContext);

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
    procs->CopyResource(pContext, shadowResource, pBaseResource);
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
  auto procs = getContextProcs(pContext);

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

      procs->CopySubresourceRegion(pContext,
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

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDeferredContext(
        ID3D11Device*             pDevice,
        UINT                      Flags,
        ID3D11DeviceContext**     ppDeferredContext) {
  auto procs = getDeviceProcs(pDevice);
  HRESULT hr = procs->CreateDeferredContext(pDevice, Flags, ppDeferredContext);

  if (SUCCEEDED(hr) && ppDeferredContext)
    hookContext(*ppDeferredContext);

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

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearRenderTargetView(
        ID3D11DeviceContext*      pContext,
        ID3D11RenderTargetView*   pRTV,
  const FLOAT                     pColor[4]) {
  auto procs = getContextProcs(pContext);
  procs->ClearRenderTargetView(pContext, pRTV, pColor);

  if (pRTV)
    updateViewShadowResource(pContext, pRTV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearUnorderedAccessViewFloat(
        ID3D11DeviceContext*      pContext,
        ID3D11UnorderedAccessView* pUAV,
  const FLOAT                     pColor[4]) {
  auto procs = getContextProcs(pContext);
  procs->ClearUnorderedAccessViewFloat(pContext, pUAV, pColor);

  if (pUAV)
    updateViewShadowResource(pContext, pUAV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_ClearUnorderedAccessViewUint(
        ID3D11DeviceContext*      pContext,
        ID3D11UnorderedAccessView* pUAV,
  const UINT                      pColor[4]) {
  auto procs = getContextProcs(pContext);
  procs->ClearUnorderedAccessViewUint(pContext, pUAV, pColor);

  if (pUAV)
    updateViewShadowResource(pContext, pUAV);
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
  }

  return S_OK;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopyResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        ID3D11Resource*           pSrcResource) {
  auto procs = getContextProcs(pContext);

  ID3D11Resource* dstShadow = getShadowResource(pDstResource);

  bool needsBaseCopy = true;
  bool needsShadowCopy = true;

  if (isImmediatecontext(pContext)) {
    HRESULT hr = tryCpuCopy(pContext, pDstResource,
      0, 0, 0, 0, pSrcResource, 0, nullptr);
    needsBaseCopy = FAILED(hr);

    if (!needsBaseCopy && dstShadow) {
      hr = tryCpuCopy(pContext, dstShadow,
        0, 0, 0, 0, pSrcResource, 0, nullptr);
      needsShadowCopy = FAILED(hr);
    }
  }

  if (needsBaseCopy)
    procs->CopyResource(pContext, pDstResource, pSrcResource);

  if (dstShadow) {
    if (needsShadowCopy)
      procs->CopyResource(pContext, dstShadow, pSrcResource);

    dstShadow->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopySubresourceRegion(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  auto procs = getContextProcs(pContext);

  ID3D11Resource* dstShadow = getShadowResource(pDstResource);

  bool needsBaseCopy = true;
  bool needsShadowCopy = true;

  if (isImmediatecontext(pContext)) {
    HRESULT hr = tryCpuCopy(pContext,
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox);
    needsBaseCopy = FAILED(hr);

    if (!needsBaseCopy && dstShadow) {
      hr = tryCpuCopy(pContext,
        dstShadow,    DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
      needsShadowCopy = FAILED(hr);
    }
  }

  if (needsBaseCopy) {
    procs->CopySubresourceRegion(pContext,
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox);
  }

  if (dstShadow) {
    if (needsShadowCopy) {
      ATFIX_RESOURCE_INFO srcInfo = { };
      getResourceInfo(pSrcResource, &srcInfo);

      procs->CopySubresourceRegion(pContext,
        dstShadow,    DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
    }

    dstShadow->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_CopyStructureCount(
        ID3D11DeviceContext*      pContext,
        ID3D11Buffer*             pDstBuffer,
        UINT                      DstOffset,
        ID3D11UnorderedAccessView* pSrcUav) {
  auto procs = getContextProcs(pContext);
  procs->CopyStructureCount(pContext, pDstBuffer, DstOffset, pSrcUav);

  ID3D11Resource* shadowResource = getShadowResource(pDstBuffer);
  ID3D11Buffer*   shadowBuffer   = nullptr;

  if (shadowResource) {
    shadowResource->QueryInterface(IID_PPV_ARGS(&shadowBuffer));
    shadowResource->Release();

    procs->CopyStructureCount(pContext, shadowBuffer, DstOffset, pSrcUav);
    shadowBuffer->Release();
  }
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Dispatch(
        ID3D11DeviceContext*      pContext,
        UINT                      X,
        UINT                      Y,
        UINT                      Z) {
  auto procs = getContextProcs(pContext);
  procs->Dispatch(pContext, X, Y, Z);

  updateUavShadowResources(pContext);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DispatchIndirect(
        ID3D11DeviceContext*      pContext,
        ID3D11Buffer*             pParameterBuffer,
        UINT                      pParameterOffset) {
  auto procs = getContextProcs(pContext);
  procs->DispatchIndirect(pContext, pParameterBuffer, pParameterOffset);

  updateUavShadowResources(pContext);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargets(
        ID3D11DeviceContext*      pContext,
        UINT                      RTVCount,
        ID3D11RenderTargetView* const* ppRTVs,
        ID3D11DepthStencilView*   pDSV) {
  auto procs = getContextProcs(pContext);
  updateRtvShadowResources(pContext);

  procs->OMSetRenderTargets(pContext, RTVCount, ppRTVs, pDSV);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(
        ID3D11DeviceContext*      pContext,
        UINT                      RTVCount,
        ID3D11RenderTargetView* const* ppRTVs,
        ID3D11DepthStencilView*   pDSV,
        UINT                      UAVIndex,
        UINT                      UAVCount,
        ID3D11UnorderedAccessView* const* ppUAVs,
  const UINT*                     pUAVClearValues) {
  auto procs = getContextProcs(pContext);
  updateRtvShadowResources(pContext);

  procs->OMSetRenderTargetsAndUnorderedAccessViews(pContext,
    RTVCount, ppRTVs, pDSV, UAVIndex, UAVCount, ppUAVs, pUAVClearValues);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_UpdateSubresource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pResource,
        UINT                      Subresource,
  const D3D11_BOX*                pBox,
  const void*                     pData,
        UINT                      RowPitch,
        UINT                      SlicePitch) {
  auto procs = getContextProcs(pContext);

  procs->UpdateSubresource(pContext, pResource,
    Subresource, pBox, pData, RowPitch, SlicePitch);

  ID3D11Resource* shadowResource = getShadowResource(pResource);
  
  if (shadowResource) {
    procs->UpdateSubresource(pContext, shadowResource,
      Subresource, pBox, pData, RowPitch, SlicePitch);
    shadowResource->Release();
  }
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
  HOOK_PROC(ID3D11Device, pDevice, procs, 4,  CreateTexture1D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 5,  CreateTexture2D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 6,  CreateTexture3D);

  g_installedHooks |= HOOK_DEVICE;
}

void hookContext(ID3D11DeviceContext* pContext) {
  std::lock_guard lock(g_hookMutex);

  uint32_t flag = HOOK_IMM_CTX;
  ContextProcs* procs = &g_immContextProcs;

  if (!isImmediatecontext(pContext)) {
    flag = HOOK_DEF_CTX;
    procs = &g_defContextProcs;
  }

  if (g_installedHooks & flag)
    return;

  log("Hooking context ", pContext);

  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 50, ClearRenderTargetView);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 52, ClearUnorderedAccessViewFloat);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 51, ClearUnorderedAccessViewUint);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 47, CopyResource);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 46, CopySubresourceRegion);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 49, CopyStructureCount);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 41, Dispatch);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 42, DispatchIndirect);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 33, OMSetRenderTargets);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 34, OMSetRenderTargetsAndUnorderedAccessViews);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 48, UpdateSubresource);

  g_installedHooks |= flag;

  /* Immediate context and deferred context methods may share code */
  if (flag & HOOK_IMM_CTX)
    g_defContextProcs = g_immContextProcs;
}

}