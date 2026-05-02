#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "../Core/NoteDataStore.h"

namespace BlackMidi {
    using Microsoft::WRL::ComPtr;

    struct NoteInstance {
        float startTime;
        float duration;
        uint32_t key;
        uint32_t velocity;
        uint32_t channel;
        uint32_t track;
    };

    struct ConstantBufferData {
        float viewProjection[16];
        float currentTime;
        float secondsPerScreen;
        float windowWidth;
        float windowHeight;
        uint32_t startInstance;
        float _pad[3];
    };

    static_assert(sizeof(ConstantBufferData) % 16 == 0,
                  "ConstantBufferData must be 16-byte aligned!");

    class Renderer {
    public:
        Renderer() = default;

        ~Renderer() = default;

        bool initialize(HWND hWnd, int width, int height);

        bool uploadNotes(const NoteDataStore &store);

        void render(float currentTime);

        void onResize(int width, int height);

        bool queryGpuMemoryUsageMB(double& usedMB, double& budgetMB) const;

    private:
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swapChain;
        ComPtr<ID3D11RenderTargetView> renderTargetView;

        ComPtr<ID3D11Buffer> noteStructuredBuffer;
        ComPtr<ID3D11ShaderResourceView> noteSRV;
        ComPtr<ID3D11Buffer> constantBuffer;
        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ComPtr<ID3D11RasterizerState> rasterizerState;

        uint32_t noteCount = 0;
        int windowWidth = 0;
        int windowHeight = 0;
        float currentBPM = 120.0f;
        float beatsPerScreen = 4.0f;
        float secondsPerScreen = 4.0f;

        const std::vector<float>* noteStartTimes = nullptr;
        const std::vector<float>* noteDurations = nullptr;

        uint32_t visibleStart = 0;
        uint32_t visibleCount = 0;
        float maxNoteDuration = 0.0f;

    public:
        void setSecondsPerScreen(const float sec) { secondsPerScreen = sec; }
        [[nodiscard]] float getSecondsPerScreen() const { return secondsPerScreen; }
        void setBPM(const float bpm) { currentBPM = bpm; }
        void setBeatsPerScreen(const float beats) { beatsPerScreen = beats; }
        [[nodiscard]] float getBeatsPerScreen() const { return beatsPerScreen; }

    private:
        bool createDeviceAndSwapChain(HWND hWnd, int width, int height);

        bool createResources();

        bool setupShaders();
    };
}
