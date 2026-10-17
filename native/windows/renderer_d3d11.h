#ifndef HMAWL_RENDERER_D3D11_H
#define HMAWL_RENDERER_D3D11_H

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <string>

class Renderer {
public:
    bool Initialize(HWND window, std::wstring& error);
    bool Resize(UINT width, UINT height, std::wstring& error);
    bool Render(float playerX, float playerY, std::wstring& error);

private:
    bool CreateRenderTarget(std::wstring& error);
    bool CreatePipeline(std::wstring& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> screenBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    UINT width_ = 0;
    UINT height_ = 0;
};

#endif
