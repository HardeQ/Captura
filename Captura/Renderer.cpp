#include "Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "Palette.h"
#include "Shaders.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr float kFullRect[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
constexpr float kPi = 3.14159265f;

std::wstring g_compileError;

HRESULT Compile(const char* entry, const char* target, ID3DBlob** blob)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "Captura.hlsl", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr) && errors)
    {
        const char* text = static_cast<const char*>(errors->GetBufferPointer());
        OutputDebugStringA(text);
        g_compileError = std::wstring(text, text + std::strlen(text));
    }
    return hr;
}
} // namespace

bool Renderer::Init(HWND hwnd)
{
    const bool ok = InitImpl(hwnd);
    if (!ok)
        error_ = g_compileError;
    return ok;
}

bool Renderer::InitImpl(HWND hwnd)
{
    hwnd_ = hwnd;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr))
        return false;

    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr, &swapChain_)))
        return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); // we handle Alt+Enter ourselves

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))) ||
        FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    d2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ComPtr<ID3DBlob> blob;
    if (FAILED(Compile("VSMain", "vs_4_0", &blob)) ||
        FAILED(device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_)))
        return false;
    const std::pair<const char*, ComPtr<ID3D11PixelShader>*> pixelShaders[] = {
        {"PSVideo", &psVideo_}, {"PSBackground", &psBackground_}, {"PSCrt", &psCrt_}, {"PSCopy", &psCopy_}};
    for (const auto& [entry, shader] : pixelShaders)
    {
        blob.Reset();
        if (FAILED(Compile(entry, "ps_4_0", &blob)) ||
            FAILED(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                              shader->GetAddressOf())))
            return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(Constants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, &constants_)) ||
        FAILED(device_->CreateSamplerState(&sampler, &sampler_)) ||
        FAILED(device_->CreateRasterizerState(&raster, &raster_)) || FAILED(device_->CreateBlendState(&blend, &blend_)))
        return false;

    return CreateSizeDependent();
}

bool Renderer::CreateSizeDependent()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(rc.right - rc.left);
    const UINT h = static_cast<UINT>(rc.bottom - rc.top);
    if (!w || !h)
        return true; // minimized; created on the next resize

    if (FAILED(swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        return false;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &backRtv_)))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &scene_)) ||
        FAILED(device_->CreateRenderTargetView(scene_.Get(), nullptr, &sceneRtv_)) ||
        FAILED(device_->CreateShaderResourceView(scene_.Get(), nullptr, &sceneSrv_)))
        return false;

    ComPtr<IDXGISurface> surface;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(scene_.As(&surface)) || FAILED(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &sceneBitmap_)))
        return false;
    d2d_->SetTarget(sceneBitmap_.Get());

    width_ = w;
    height_ = h;
    return true;
}

void Renderer::ReleaseSizeDependent()
{
    d2d_->SetTarget(nullptr);
    sceneBitmap_.Reset();
    sceneSrv_.Reset();
    sceneRtv_.Reset();
    scene_.Reset();
    backRtv_.Reset();
    context_->ClearState();
    context_->Flush();
    width_ = height_ = 0;
}

void Renderer::Resize()
{
    if (!swapChain_)
        return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (static_cast<UINT>(rc.right - rc.left) == width_ && static_cast<UINT>(rc.bottom - rc.top) == height_)
        return;
    ReleaseSizeDependent();
    CreateSizeDependent();
}

void Renderer::UploadFrame(const VideoFrame& frame)
{
    if (frame.sequence == uploadedSequence_)
        return;
    uploadedSequence_ = frame.sequence;

    if (!frame.width || !frame.height || frame.pixels.size() < static_cast<size_t>(frame.width) * frame.height)
    {
        hasVideo_ = false;
        return;
    }
    if (!video_ || frame.width != videoWidth_ || frame.height != videoHeight_)
    {
        video_.Reset();
        videoSrv_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = frame.width;
        desc.Height = frame.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &video_)) ||
            FAILED(device_->CreateShaderResourceView(video_.Get(), nullptr, &videoSrv_)))
        {
            video_.Reset();
            hasVideo_ = false;
            return;
        }
        videoWidth_ = frame.width;
        videoHeight_ = frame.height;
    }
    context_->UpdateSubresource(video_.Get(), 0, nullptr, frame.pixels.data(), frame.width * 4, 0);
    hasVideo_ = true;
}

void Renderer::DrawQuad(ID3D11RenderTargetView* target, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv,
                        const float rect[4], bool blend)
{
    std::copy(rect, rect + 4, cb_.rect);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &cb_, 0, 0);

    // Direct2D shares this context, so set the whole pipeline every time.
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ID3D11Buffer* cb = constants_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend ? blend_.Get() : nullptr, nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &cb);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->PSSetShader(ps, nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(4, 0);
}

void Renderer::BeginFrame(const RenderParams& params)
{
    const Settings defaults;
    const Settings& s = params.settings ? *params.settings : defaults;
    cb_.resolution[0] = static_cast<float>(width_);
    cb_.resolution[1] = static_cast<float>(height_);
    cb_.time = params.time;
    cb_.bgAlpha = params.backgroundAlpha;
    cb_.brightness = s.picture.brightness / 200.0f;
    cb_.contrast = s.picture.contrast / 100.0f;
    cb_.saturation = s.picture.saturation / 100.0f;
    cb_.hue = s.picture.hue * kPi / 180.0f;
    cb_.gamma = s.picture.gamma / 100.0f;
    cb_.sharpness = s.picture.sharpness / 100.0f;
    cb_.scanlines = s.scanlines / 100.0f;
    cb_.curvature = s.curvature / 100.0f;
    cb_.texel[0] = videoWidth_ ? 1.0f / videoWidth_ : 0.0f;
    cb_.texel[1] = videoHeight_ ? 1.0f / videoHeight_ : 0.0f;
    cb_.power = params.power;

    const Palette& pal = GetPalette(s.palette);
    auto setColor = [](float (&dst)[4], const Rgb& c) {
        dst[0] = c.r;
        dst[1] = c.g;
        dst[2] = c.b;
        dst[3] = 1.0f;
    };
    setColor(cb_.bgLow, pal.bgLow);
    setColor(cb_.bgHigh, pal.bgHigh);
    setColor(cb_.glow, pal.glow);
    setColor(cb_.light, pal.light);
    setColor(cb_.accent, pal.accent);

    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(sceneRtv_.Get(), black);

    if (params.drawVideo && hasVideo_)
    {
        // Letterbox the video inside the window.
        const float sw = static_cast<float>(width_), sh = static_cast<float>(height_);
        float dw = sw, dh = sw * videoHeight_ / videoWidth_;
        if (dh > sh)
        {
            dh = sh;
            dw = sh * videoWidth_ / videoHeight_;
        }
        const float x0 = (sw - dw) * 0.5f, y0 = (sh - dh) * 0.5f;
        const float rect[4] = {x0 / sw * 2 - 1, 1 - y0 / sh * 2, (x0 + dw) / sw * 2 - 1, 1 - (y0 + dh) / sh * 2};
        DrawQuad(sceneRtv_.Get(), psVideo_.Get(), videoSrv_.Get(), rect, false);
    }
    if (params.backgroundAlpha > 0.001f)
        DrawQuad(sceneRtv_.Get(), psBackground_.Get(), nullptr, kFullRect, true);

    d2d_->BeginDraw();
    d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
}

HRESULT Renderer::EndFrame(const RenderParams& params)
{
    d2d_->EndDraw();
    DrawQuad(backRtv_.Get(), params.crt ? psCrt_.Get() : psCopy_.Get(), sceneSrv_.Get(), kFullRect, false);
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none); // scene becomes a render target again next frame
    return swapChain_->Present(1, 0);
}
