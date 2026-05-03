#include "Renderer.h"
#include <iostream>
#include <d3dcompiler.h>
#include <cstring>
#include <dxgi.h>

namespace BlackMidi {
    auto g_vsCode = R"(
struct Note {
    float startTime;
    float duration;
    uint key;
    uint velocity;
    uint channel;
    uint track;
};

struct VS_INPUT {
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

cbuffer ConstantBuffer : register(b0) {
    float4x4 g_ViewProjection;
    float g_CurrentTime;
    float g_SecondsPerScreen;
    float g_WindowWidth;
    float g_WindowHeight;
    uint  g_StartInstance;
    float3 _pad;
};

StructuredBuffer<Note> g_Notes : register(t0);

float3 hsv2rgb(float3 c) {
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float4 GetNoteColor(uint channel, uint track, uint velocity) {
    uint key = track * 17u + channel;
    float hue = frac((float)key * 0.381966f);
    float saturation = 0.85f;
    float value = 0.6f + 0.4f * ((float)velocity / 127.0f);
    float3 rgb = hsv2rgb(float3(hue, saturation, value));
    return float4(rgb, 1.0f);
}

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    Note note = g_Notes[input.instanceID + g_StartInstance];

    float timeDiff = note.startTime - g_CurrentTime;

    if (timeDiff < 0 - note.duration || timeDiff > g_SecondsPerScreen) {
        output.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        output.color = float4(0, 0, 0, 0);
        return output;
    }

    float xBase = ((float)note.key / 127.0f) * 1.9f - 0.95f;

    float yBase = -1.0f + (timeDiff / g_SecondsPerScreen) * 2.0f;

    float noteWidth = 0.8f / 127.0f;
    float noteHeight = max(0.002f, (note.duration / g_SecondsPerScreen) * 2.0f);

    float2 offset = float2(0, 0);
    if (input.vertexID == 0)      offset = float2(-noteWidth, 0);
    else if (input.vertexID == 1) offset = float2(noteWidth, 0);
    else if (input.vertexID == 2) offset = float2(-noteWidth, noteHeight);
    else if (input.vertexID == 3) offset = float2(noteWidth, noteHeight);

    output.position = float4(xBase + offset.x, yBase + offset.y, 0.5f, 1.0f);
    output.color = GetNoteColor(note.channel, note.track, note.velocity);
    return output;
}
)";

    auto g_psCode = R"(
struct PS_INPUT {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};
float4 main(PS_INPUT input) : SV_TARGET {
    return input.color;
}
)";

    void SetIdentityMatrix(float *m) {
        std::memset(m, 0, sizeof(float) * 16);
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
    }

    bool Renderer::initialize(HWND hWnd, const int width, const int height) {
        this->windowWidth = width;
        this->windowHeight = height;

        std::cout << "[INFO] Creating Device and SwapChain..." << std::endl;
        if (!createDeviceAndSwapChain(hWnd, width, height)) {
            std::cerr << "[ERROR] Failed to create D3D11 Device and SwapChain." << std::endl;
            return false;
        }

        std::cout << "[INFO] Creating Resources..." << std::endl;
        if (!createResources()) {
            std::cerr << "[ERROR] Failed to create D3D11 Resources." << std::endl;
            return false;
        }

        std::cout << "[INFO] Setting up Shaders..." << std::endl;
        if (!setupShaders()) {
            std::cerr << "[ERROR] Failed to setup Shaders." << std::endl;
            return false;
        }

        std::cout << "[INFO] Renderer initialization complete." << std::endl;
        return true;
    }

    bool Renderer::createDeviceAndSwapChain(HWND hWnd, const int width, const int height) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = width;
        sd.BufferDesc.Height = height;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;

        constexpr D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL featureLevel;

        constexpr UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels, 3,
            D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context
        );

        if (FAILED(hr)) {
            std::cout << "[WARN] Hardware D3D11 device creation failed (HRESULT: 0x" << std::hex << hr << std::dec <<
                    "). Retrying with WARP (Software)..." << std::endl;
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevels, 3,
                D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context
            );
        }

        if (FAILED(hr)) {
            std::cout << "[WARN] WARP device creation failed. Retrying with REFERENCE..." << std::endl;
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_REFERENCE, nullptr, 0, featureLevels, 3,
                D3D11_SDK_VERSION, &sd, &swapChain, &device, &featureLevel, &context
            );
        }

        if (FAILED(hr)) {
            std::cerr << "[ERROR] D3D11CreateDeviceAndSwapChain failed all attempts. HRESULT: 0x" << std::hex << hr <<
                    std::dec << std::endl;
            return false;
        }

        std::cout << "[INFO] D3D11 Device created. Feature Level: 0x" << std::hex << featureLevel << std::dec <<
                std::endl;

        ComPtr<ID3D11Texture2D> backBuffer;
        hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
        if (FAILED(hr)) {
            std::cerr << "[ERROR] Failed to get BackBuffer from SwapChain. HRESULT: 0x" << std::hex << hr << std::dec <<
                    std::endl;
            return false;
        }

        hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);
        if (FAILED(hr)) {
            std::cerr << "[ERROR] Failed to create RenderTargetView. HRESULT: 0x" << std::hex << hr << std::dec <<
                    std::endl;
            return false;
        }

        return true;
    }

    bool Renderer::createResources() {
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.ByteWidth = sizeof(ConstantBufferData);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device->CreateBuffer(&cbd, nullptr, &constantBuffer);
        if (FAILED(hr)) {
            std::cerr << "[ERROR] CreateBuffer (ConstantBuffer) failed with HRESULT: 0x" << std::hex << hr << std::dec
                    << std::endl;
            return false;
        }

        D3D11_RASTERIZER_DESC rDesc = {};
        rDesc.FillMode = D3D11_FILL_SOLID;
        rDesc.CullMode = D3D11_CULL_NONE;
        rDesc.DepthClipEnable = TRUE;
        hr = device->CreateRasterizerState(&rDesc, &rasterizerState);
        if (FAILED(hr)) {
            std::cerr << "[ERROR] CreateRasterizerState failed with HRESULT: 0x" << std::hex << hr << std::dec <<
                    std::endl;
            return false;
        }

        return true;
    }

    bool Renderer::uploadNotes(const NoteDataStore &store) {
        this->noteCount = static_cast<uint32_t>(store.size());

        noteStartTimes = &store.startTimes;
        noteDurations = &store.durations;
        maxNoteDuration = 0.0f;

        if (noteCount == 0) return true;

        std::vector<NoteInstance> instances(noteCount);
        for (uint32_t i = 0; i < noteCount; ++i) {
            instances[i].startTime = store.startTimes[i];
            instances[i].duration = store.durations[i];
            instances[i].key = static_cast<uint32_t>(store.keys[i]);
            instances[i].velocity = static_cast<uint32_t>(store.velocities[i]);
            instances[i].channel = static_cast<uint32_t>(store.channels[i]);
            instances[i].track = static_cast<uint32_t>(store.tracks[i]);
            if (store.durations[i] > maxNoteDuration) maxNoteDuration = store.durations[i];
        }

        noteSRV.Reset();
        noteStructuredBuffer.Reset();

        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(NoteInstance) * noteCount;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(NoteInstance);

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = instances.data();
        HRESULT hr = device->CreateBuffer(&bd, &initData, &noteStructuredBuffer);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = noteCount;
        hr = device->CreateShaderResourceView(noteStructuredBuffer.Get(), &srvDesc, &noteSRV);

        return SUCCEEDED(hr);
    }

    bool Renderer::setupShaders() {
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> errorBlob;

        HRESULT hr = D3DCompile(g_vsCode, strlen(g_vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob,
                                &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "[ERROR] Vertex Shader Compilation Failed: " << static_cast<char *>(errorBlob->
                    GetBufferPointer()) << std::endl;
            } else {
                std::cerr << "[ERROR] Vertex Shader Compilation Failed with HRESULT: 0x" << std::hex << hr << std::dec
                        << std::endl;
            }
            return false;
        }
        hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
        if (FAILED(hr)) return false;

        hr = D3DCompile(g_psCode, strlen(g_psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob,
                        &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "[ERROR] Pixel Shader Compilation Failed: " << static_cast<char *>(errorBlob->
                    GetBufferPointer()) << std::endl;
            } else {
                std::cerr << "[ERROR] Pixel Shader Compilation Failed with HRESULT: 0x" << std::hex << hr << std::dec <<
                        std::endl;
            }
            return false;
        }
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
        if (FAILED(hr)) return false;

        return true;
    }

    void Renderer::render(const float currentTime) {
        if (!renderTargetView || !context) return;

        context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);

        D3D11_VIEWPORT vp;
        vp.Width = static_cast<FLOAT>(windowWidth);
        vp.Height = static_cast<FLOAT>(windowHeight);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        context->RSSetViewports(1, &vp);

        constexpr float clearColor[4] = {0.05f, 0.05f, 0.1f, 1.0f};
        context->ClearRenderTargetView(renderTargetView.Get(), clearColor);

        if (vertexShader && pixelShader && noteSRV) {
            constexpr float kEpsilon = 1e-4f;
            const float windowEnd = currentTime + secondsPerScreen;

            if (noteStartTimes && noteDurations && !noteStartTimes->empty()) {
                const float searchStart = currentTime - maxNoteDuration - kEpsilon;
                const auto loIt = std::lower_bound(
                    noteStartTimes->begin(), noteStartTimes->end(), searchStart);
                visibleStart = static_cast<uint32_t>(
                    std::distance(noteStartTimes->begin(), loIt));

                while (visibleStart < noteCount &&
                       (*noteStartTimes)[visibleStart] + (*noteDurations)[visibleStart] < currentTime - kEpsilon) {
                    ++visibleStart;
                }

                const auto hiIt = std::upper_bound(
                    noteStartTimes->begin() + visibleStart,
                    noteStartTimes->end(),
                    windowEnd);
                const uint32_t visibleEnd = static_cast<uint32_t>(
                    std::distance(noteStartTimes->begin(), hiIt));

                visibleCount = (visibleEnd > visibleStart) ? (visibleEnd - visibleStart) : 0;
            } else {
                visibleStart = 0;
                visibleCount = 0;
            }

            D3D11_MAPPED_SUBRESOURCE mappedResource;
            if (SUCCEEDED(context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
                const auto cbData = static_cast<ConstantBufferData *>(mappedResource.pData);
                SetIdentityMatrix(cbData->viewProjection);
                cbData->currentTime = currentTime;
                cbData->secondsPerScreen = secondsPerScreen;
                cbData->windowWidth = static_cast<float>(windowWidth);
                cbData->windowHeight = static_cast<float>(windowHeight);
                cbData->startInstance = visibleStart;
                context->Unmap(constantBuffer.Get(), 0);
            }

            context->VSSetShader(vertexShader.Get(), nullptr, 0);
            context->PSSetShader(pixelShader.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
            context->VSSetShaderResources(0, 1, noteSRV.GetAddressOf());
            context->RSSetState(rasterizerState.Get());

            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

            if (visibleCount > 0) {
                context->DrawInstanced(4, visibleCount, 0, 0);
            }
        }

        swapChain->Present(1, 0);
    }

    void Renderer::onResize(const int width, const int height) {
        if (width == 0 || height == 0 || !swapChain) return;
        this->windowWidth = width;
        this->windowHeight = height;

        renderTargetView.Reset();
        swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView);
    }

    bool Renderer::queryGpuMemoryUsageMB(double& usedMB, double& budgetMB) const {
        usedMB = 0.0;
        budgetMB = 0.0;
        if (!device) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device.As(&dxgiDevice)) || !dxgiDevice) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) return false;

        ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3)) || !adapter3) return false;

        DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) return false;

        usedMB = static_cast<double>(memInfo.CurrentUsage) / (1024.0 * 1024.0);
        budgetMB = static_cast<double>(memInfo.Budget) / (1024.0 * 1024.0);
        return true;
    }
}
