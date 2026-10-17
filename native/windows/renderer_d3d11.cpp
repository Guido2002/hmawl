#include "renderer_d3d11.h"

#include <d3dcompiler.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

struct Color {
    float r;
    float g;
    float b;
};

struct ScreenConstants {
    float width;
    float height;
    float padding[2];
};

constexpr UINT kMaxVertices = 192;

constexpr Color kSky = {0.40f, 0.69f, 0.88f};
constexpr Color kGround = {0.31f, 0.59f, 0.30f};
constexpr Color kDarkGreen = {0.12f, 0.34f, 0.19f};
constexpr Color kLeafGreen = {0.25f, 0.46f, 0.20f};
constexpr Color kWood = {0.58f, 0.31f, 0.17f};
constexpr Color kRoof = {0.43f, 0.20f, 0.16f};
constexpr Color kCream = {0.91f, 0.80f, 0.57f};
constexpr Color kWindow = {0.37f, 0.68f, 0.78f};
constexpr Color kSkin = {0.94f, 0.72f, 0.48f};
constexpr Color kShirt = {0.74f, 0.22f, 0.18f};
constexpr Color kPants = {0.14f, 0.24f, 0.55f};
constexpr Color kPath = {0.61f, 0.50f, 0.33f};
constexpr Color kWhite = {0.97f, 0.95f, 0.83f};

constexpr char kVertexShader[] = R"(
cbuffer ScreenBuffer : register(b0) {
    float2 screenSize;
    float2 padding;
};

struct VertexInput {
    float2 position : POSITION;
    float4 color : COLOR;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PixelInput VSMain(VertexInput input) {
    PixelInput output;
    output.position = float4(
        input.position.x / screenSize.x * 2.0f - 1.0f,
        1.0f - input.position.y / screenSize.y * 2.0f,
        0.0f, 1.0f);
    output.color = input.color;
    return output;
}
)";

constexpr char kPixelShader[] = R"(
float4 PSMain(float4 position : SV_POSITION, float4 color : COLOR) : SV_TARGET {
    return color;
}
)";

void SetGraphicsError(std::wstring& error, const wchar_t* action,
                      HRESULT result) {
    wchar_t message[192];
    std::swprintf(message, sizeof(message) / sizeof(message[0]),
                  L"[hmawl] %ls (Direct3D error 0x%08lX).", action,
                  static_cast<unsigned long>(result));
    error = message;
}

void AppendVertex(std::array<Vertex, kMaxVertices>& vertices, UINT& count,
                  float x, float y, Color color) {
    vertices[count++] = {x, y, color.r, color.g, color.b, 1.0f};
}

void AppendQuad(std::array<Vertex, kMaxVertices>& vertices, UINT& count,
                float left, float top, float right, float bottom,
                Color color) {
    AppendVertex(vertices, count, left, top, color);
    AppendVertex(vertices, count, right, top, color);
    AppendVertex(vertices, count, left, bottom, color);
    AppendVertex(vertices, count, left, bottom, color);
    AppendVertex(vertices, count, right, top, color);
    AppendVertex(vertices, count, right, bottom, color);
}

void AppendTriangle(std::array<Vertex, kMaxVertices>& vertices, UINT& count,
                    float x1, float y1, float x2, float y2, float x3,
                    float y3, Color color) {
    AppendVertex(vertices, count, x1, y1, color);
    AppendVertex(vertices, count, x2, y2, color);
    AppendVertex(vertices, count, x3, y3, color);
}

bool CompileShader(const char* source, const char* entryPoint,
                   const char* shaderModel, ID3DBlob** shader,
                   std::wstring& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    const HRESULT result =
        D3DCompile(source, std::char_traits<char>::length(source), nullptr,
                   nullptr, nullptr, entryPoint, shaderModel,
                   D3DCOMPILE_ENABLE_STRICTNESS, 0, shader,
                   diagnostics.GetAddressOf());
    if (FAILED(result)) {
        if (diagnostics != nullptr) {
            OutputDebugStringA(
                static_cast<const char*>(diagnostics->GetBufferPointer()));
        }
        SetGraphicsError(error, L"Could not compile the built-in renderer shader",
                         result);
        return false;
    }
    return true;
}

void DrawTree(std::array<Vertex, kMaxVertices>& vertices, UINT& count,
              float x, float ground, float scale) {
    AppendQuad(vertices, count, x - scale * 0.018f, ground - scale * 0.14f,
               x + scale * 0.018f, ground, kWood);
    AppendQuad(vertices, count, x - scale * 0.075f, ground - scale * 0.30f,
               x + scale * 0.075f, ground - scale * 0.16f, kDarkGreen);
    AppendQuad(vertices, count, x - scale * 0.055f, ground - scale * 0.38f,
               x + scale * 0.055f, ground - scale * 0.25f, kLeafGreen);
}

} // namespace

bool Renderer::Initialize(HWND window, std::wstring& error) {
    DXGI_SWAP_CHAIN_DESC swapChainDescription = {};
    swapChainDescription.BufferCount = 2;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.OutputWindow = window;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_10_0;

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
        static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
        &swapChainDescription, swapChain_.GetAddressOf(), device_.GetAddressOf(),
        &selectedFeatureLevel, context_.GetAddressOf());

    if (FAILED(result)) {
        OutputDebugStringW(
            L"[hmawl] Hardware renderer unavailable; trying the Direct3D WARP "
            L"software renderer.\n");
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
            static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
            &swapChainDescription, swapChain_.ReleaseAndGetAddressOf(),
            device_.ReleaseAndGetAddressOf(), &selectedFeatureLevel,
            context_.ReleaseAndGetAddressOf());
    }
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create a Direct3D 11 device", result);
        return false;
    }

    RECT clientArea;
    if (!GetClientRect(window, &clientArea)) {
        SetGraphicsError(error, L"Could not read the window size",
                         HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    width_ = static_cast<UINT>(clientArea.right - clientArea.left);
    height_ = static_cast<UINT>(clientArea.bottom - clientArea.top);

    if (width_ == 0 || height_ == 0) {
        error = L"The application window has no drawable area.";
        return false;
    }
    if (!CreateRenderTarget(error) || !CreatePipeline(error)) {
        return false;
    }

    wchar_t message[96];
    std::swprintf(message, sizeof(message) / sizeof(message[0]),
                  L"[hmawl] Direct3D feature level: 0x%X\n",
                  static_cast<unsigned int>(selectedFeatureLevel));
    OutputDebugStringW(message);
    return true;
}

bool Renderer::CreateRenderTarget(std::wstring& error) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = swapChain_->GetBuffer(
        0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not access the presentation buffer",
                         result);
        return false;
    }

    result =
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                        renderTarget_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the presentation target",
                         result);
        return false;
    }
    return true;
}

bool Renderer::Resize(UINT width, UINT height, std::wstring& error) {
    if (width == 0 || height == 0 || swapChain_ == nullptr) {
        return true;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();

    const HRESULT result =
        swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not resize the presentation buffers",
                         result);
        return false;
    }

    width_ = width;
    height_ = height;
    return CreateRenderTarget(error);
}

bool Renderer::CreatePipeline(std::wstring& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexByteCode;
    if (!CompileShader(kVertexShader, "VSMain", "vs_4_0",
                       vertexByteCode.GetAddressOf(), error)) {
        return false;
    }

    HRESULT result = device_->CreateVertexShader(
        vertexByteCode->GetBufferPointer(), vertexByteCode->GetBufferSize(),
        nullptr, vertexShader_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the vertex shader", result);
        return false;
    }

    constexpr D3D11_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 2 * sizeof(float),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    result = device_->CreateInputLayout(
        inputElements, static_cast<UINT>(std::size(inputElements)),
        vertexByteCode->GetBufferPointer(), vertexByteCode->GetBufferSize(),
        inputLayout_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the vertex input layout",
                         result);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> pixelByteCode;
    if (!CompileShader(kPixelShader, "PSMain", "ps_4_0",
                       pixelByteCode.GetAddressOf(), error)) {
        return false;
    }
    result = device_->CreatePixelShader(
        pixelByteCode->GetBufferPointer(), pixelByteCode->GetBufferSize(),
        nullptr, pixelShader_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the pixel shader", result);
        return false;
    }

    D3D11_BUFFER_DESC vertexDescription = {};
    vertexDescription.ByteWidth = kMaxVertices * sizeof(Vertex);
    vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(&vertexDescription, nullptr,
                                   vertexBuffer_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the renderer vertex buffer",
                         result);
        return false;
    }

    D3D11_BUFFER_DESC screenDescription = {};
    screenDescription.ByteWidth = sizeof(ScreenConstants);
    screenDescription.Usage = D3D11_USAGE_DYNAMIC;
    screenDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    screenDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(&screenDescription, nullptr,
                                   screenBuffer_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the screen-size buffer",
                         result);
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDescription = {};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    result = device_->CreateRasterizerState(
        &rasterizerDescription, rasterizerState_.GetAddressOf());
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not create the renderer rasterizer",
                         result);
        return false;
    }
    return true;
}

bool Renderer::Render(float playerX, float playerY, std::wstring& error) {
    const float width = static_cast<float>(width_);
    const float height = static_cast<float>(height_);
    const float horizon = height * 0.68f;
    const float scale = height;

    std::array<Vertex, kMaxVertices> vertices = {};
    UINT vertexCount = 0;

    AppendQuad(vertices, vertexCount, 0.0f, 0.0f, width, horizon, kSky);
    AppendQuad(vertices, vertexCount, 0.0f, horizon, width, height, kGround);

    const float sunX = width * 0.79f;
    const float sunY = height * 0.20f;
    const float sunRadius = scale * 0.055f;
    AppendQuad(vertices, vertexCount, sunX - sunRadius, sunY - sunRadius,
               sunX + sunRadius, sunY + sunRadius, kCream);
    AppendQuad(vertices, vertexCount, sunX - sunRadius * 0.35f,
               sunY - sunRadius * 1.45f, sunX + sunRadius * 0.35f,
               sunY - sunRadius, kWhite);
    AppendQuad(vertices, vertexCount, sunX - sunRadius * 0.35f,
               sunY + sunRadius, sunX + sunRadius * 0.35f,
               sunY + sunRadius * 1.45f, kWhite);

    const float treeGround = height * 0.73f;
    DrawTree(vertices, vertexCount, width * 0.085f, treeGround, scale);
    DrawTree(vertices, vertexCount, width * 0.89f, treeGround, scale * 1.10f);

    const float barnLeft = width * 0.22f;
    const float barnRight = width * 0.47f;
    const float barnRoof = height * 0.31f;
    const float barnWallTop = height * 0.41f;
    const float barnBottom = height * 0.72f;
    AppendTriangle(vertices, vertexCount, barnLeft - scale * 0.045f, barnWallTop,
                   (barnLeft + barnRight) * 0.5f, barnRoof, barnRight + scale * 0.045f,
                   barnWallTop, kRoof);
    AppendQuad(vertices, vertexCount, barnLeft, barnWallTop, barnRight,
               barnBottom, kWood);
    AppendQuad(vertices, vertexCount, barnLeft + scale * 0.085f,
               barnWallTop + scale * 0.085f,
               barnLeft + scale * 0.165f, barnWallTop + scale * 0.17f,
               kWindow);
    AppendQuad(vertices, vertexCount, barnRight - scale * 0.155f,
               barnWallTop + scale * 0.085f,
               barnRight - scale * 0.075f, barnWallTop + scale * 0.17f,
               kWindow);
    AppendQuad(vertices, vertexCount, width * 0.305f, height * 0.55f,
               width * 0.385f, barnBottom, kRoof);
    AppendQuad(vertices, vertexCount, width * 0.345f, height * 0.55f,
               width * 0.35f, barnBottom, kCream);

    AppendTriangle(vertices, vertexCount, width * 0.0f, height * 0.79f,
                   width * 0.40f, height * 0.75f, width * 0.64f, height,
                   kPath);
    AppendTriangle(vertices, vertexCount, width * 0.40f, height * 0.75f,
                   width * 0.59f, height * 0.75f, width * 0.64f, height,
                   kPath);

    const float playerCenterX = playerX * width;
    const float playerCenterY = playerY * height;
    const float playerWidth = scale * 0.038f;
    const float playerHeight = scale * 0.11f;
    AppendQuad(vertices, vertexCount,
               playerCenterX - playerWidth * 0.45f,
               playerCenterY - playerHeight * 0.5f,
               playerCenterX + playerWidth * 0.45f,
               playerCenterY - playerHeight * 0.25f, kSkin);
    AppendQuad(vertices, vertexCount,
               playerCenterX - playerWidth * 0.70f,
               playerCenterY - playerHeight * 0.25f,
               playerCenterX + playerWidth * 0.70f,
               playerCenterY + playerHeight * 0.22f, kShirt);
    AppendQuad(vertices, vertexCount,
               playerCenterX - playerWidth * 0.62f,
               playerCenterY + playerHeight * 0.22f,
               playerCenterX - playerWidth * 0.05f,
               playerCenterY + playerHeight * 0.5f, kPants);
    AppendQuad(vertices, vertexCount,
               playerCenterX + playerWidth * 0.05f,
               playerCenterY + playerHeight * 0.22f,
               playerCenterX + playerWidth * 0.62f,
               playerCenterY + playerHeight * 0.5f, kPants);

    if (vertexCount > kMaxVertices) {
        error = L"The scene exceeded the renderer vertex-buffer capacity.";
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT result = context_->Map(vertexBuffer_.Get(), 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not update the renderer vertex buffer",
                         result);
        return false;
    }
    std::memcpy(mapped.pData, vertices.data(), vertexCount * sizeof(Vertex));
    context_->Unmap(vertexBuffer_.Get(), 0);

    result = context_->Map(screenBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                           &mapped);
    if (FAILED(result)) {
        SetGraphicsError(error, L"Could not update the screen-size buffer",
                         result);
        return false;
    }
    const ScreenConstants screen = {width, height, {0.0f, 0.0f}};
    std::memcpy(mapped.pData, &screen, sizeof(screen));
    context_->Unmap(screenBuffer_.Get(), 0);

    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = width;
    viewport.Height = height;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizerState_.Get());

    const float clearColor[] = {kSky.r, kSky.g, kSky.b, 1.0f};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    ID3D11Buffer* vertexBuffer = vertexBuffer_.Get();
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    ID3D11Buffer* constants = screenBuffer_.Get();
    context_->VSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->Draw(vertexCount, 0);

    result = swapChain_->Present(1, 0);
    if (FAILED(result) && result != DXGI_STATUS_OCCLUDED) {
        SetGraphicsError(error, L"Could not present the rendered frame", result);
        return false;
    }
    return true;
}
