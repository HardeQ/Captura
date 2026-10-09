#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "Capture.h"
#include "Settings.h"

struct RenderParams
{
    float time = 0;
    bool drawVideo = false;
    float backgroundAlpha = 0; // animated background drawn over (or instead of) the video
    bool crt = false;
    float power = 1;           // CRT turn-on/off animation, 0..1
    const Settings* settings = nullptr;
};

// D3D11 renderer: video with color controls, animated background and the CRT
// pass. Exposes a Direct2D context targeting the scene for UI drawing.
class Renderer
{
public:
    bool Init(HWND hwnd);
    const std::wstring& Error() const { return error_; }
    void Resize();
    bool CanRender() const { return width_ && height_; }

    void UploadFrame(const VideoFrame& frame);
    bool HasVideo() const { return hasVideo_; }

    // Draws video/background and opens the D2D context; EndFrame applies the
    // CRT (or plain) pass and presents.
    void BeginFrame(const RenderParams& params);
    HRESULT EndFrame(const RenderParams& params);

    ID2D1DeviceContext* D2D() const { return d2d_.Get(); }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

private:
    struct Constants
    {
        float rect[4];
        float resolution[2];
        float time;
        float bgAlpha;
        float brightness, contrast, saturation, hue;
        float gamma, sharpness, scanlines, curvature;
        float texel[2];
        float power;
        float pad;
        float bgLow[4], bgHigh[4], glow[4], light[4], accent[4];
    };

    bool InitImpl(HWND hwnd);
    bool CreateSizeDependent();
    void ReleaseSizeDependent();
    void DrawQuad(ID3D11RenderTargetView* target, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv,
                  const float rect[4], bool blend);

    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    std::wstring error_; // why Init failed (shader compiler output)
    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ComPtr<ID3D11Texture2D> scene_;
    ComPtr<ID3D11RenderTargetView> sceneRtv_;
    ComPtr<ID3D11ShaderResourceView> sceneSrv_;
    ComPtr<ID3D11Texture2D> video_;
    ComPtr<ID3D11ShaderResourceView> videoSrv_;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psVideo_, psBackground_, psCrt_, psCopy_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> blend_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2d_;
    ComPtr<ID2D1Bitmap1> sceneBitmap_;
    ComPtr<IDWriteFactory> dwrite_;

    UINT width_ = 0, height_ = 0;
    UINT videoWidth_ = 0, videoHeight_ = 0;
    uint64_t uploadedSequence_ = UINT64_MAX;
    bool hasVideo_ = false;
    Constants cb_{};
};
