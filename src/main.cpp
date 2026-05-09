#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tchar.h>
#include <shellapi.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <mmsystem.h>
#include <psapi.h>
#include <sstream>
#include <iomanip>
#include <array>
#include <cmath>
#include "Core/Clock.h"
#include "Core/MidiParser.h"
#include "Core/NoteDataStore.h"
#include "Core/MIDIPlayer.h"
#include "Graphics/Renderer.h"

using namespace BlackMidi;

std::unique_ptr<Renderer> g_Renderer;
std::unique_ptr<Clock> g_Clock;
std::unique_ptr<NoteDataStore> g_NoteStore;
std::unique_ptr<MIDIPlayer> g_MIDIPlayer;

size_t g_NextNoteIndex = 0;
size_t g_NextCCIndex = 0;
size_t g_NextPCIndex = 0;
size_t g_NextPBIndex = 0;
size_t g_NextTempoIndex = 0;
struct ActiveNote {
    float endTime;
    uint8_t channel;
    uint8_t key;
};

struct ActiveNoteMinHeapCompare {
    bool operator()(const ActiveNote& a, const ActiveNote& b) const {
        return a.endTime > b.endTime;
    }
};

std::priority_queue<ActiveNote, std::vector<ActiveNote>, ActiveNoteMinHeapCompare> g_ActiveNotes;

std::thread g_MIDIThread;
std::atomic<bool> g_MIDIThreadStop{false};
std::atomic<bool> g_MIDIThreadRunning{false};
std::atomic<bool> g_HighResTimerEnabled{false};
std::atomic<float> g_PlaybackTimeSec{0.0f};
std::mutex g_MIDIStateMutex;
HWND g_MainWnd = nullptr;
HWND g_KeyboardWnd = nullptr;
std::array<std::atomic<int>, 128> g_KeyDownCounts{};

constexpr uint8_t kKeyboardMinNote = 0;
constexpr uint8_t kKeyboardMaxNote = 127;

bool IsBlackKey(uint8_t midiKey);

int CountWhiteKeysInRange(const uint8_t minNote, const uint8_t maxNote) {
    int count = 0;
    for (int key = static_cast<int>(minNote); key <= static_cast<int>(maxNote); ++key) {
        if (!IsBlackKey(static_cast<uint8_t>(key))) {
            count++;
        }
    }
    return count;
}

bool IsBlackKey(const uint8_t midiKey) {
    switch (midiKey % 12) {
        case 1: case 3: case 6: case 8: case 10:
            return true;
        default:
            return false;
    }
}

void ClearKeyboardStates() {
    for (auto& c : g_KeyDownCounts) {
        c.store(0, std::memory_order_relaxed);
    }
}

void RegisterKeyOn(const uint8_t midiKey) {
    if (midiKey >= g_KeyDownCounts.size()) return;
    g_KeyDownCounts[midiKey].fetch_add(1, std::memory_order_relaxed);
}

void RegisterKeyOff(const uint8_t midiKey) {
    if (midiKey >= g_KeyDownCounts.size()) return;

    int current = g_KeyDownCounts[midiKey].load(std::memory_order_relaxed);
    while (current > 0) {
        if (g_KeyDownCounts[midiKey].compare_exchange_weak(
            current, current - 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
            break;
        }
    }
}

bool IsKeyPressed(const uint8_t midiKey) {
    if (midiKey >= g_KeyDownCounts.size()) return false;
    return g_KeyDownCounts[midiKey].load(std::memory_order_relaxed) > 0;
}

LRESULT CALLBACK KeyboardWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            SetTimer(hWnd, 1, 33, nullptr); // ~30 FPS
            return 0;
        case WM_TIMER:
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hWnd, 1);
            g_KeyboardWnd = nullptr;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient{};
            GetClientRect(hWnd, &rcClient);
            const int clientW = static_cast<int>(rcClient.right - rcClient.left);
            const int clientH = static_cast<int>(rcClient.bottom - rcClient.top);
            const int width = std::max(1, clientW);
            const int height = std::max(1, clientH);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

            HBRUSH bgBrush = CreateSolidBrush(RGB(210, 210, 210));
            RECT rcBack{0, 0, width, height};
            FillRect(memDC, &rcBack, bgBrush);
            DeleteObject(bgBrush);

            const int whiteCount = std::max(1, CountWhiteKeysInRange(kKeyboardMinNote, kKeyboardMaxNote));
            const double whiteW = static_cast<double>(width) / static_cast<double>(whiteCount);

            int whiteIndex = 0;
            for (int keyInt = static_cast<int>(kKeyboardMinNote); keyInt <= static_cast<int>(kKeyboardMaxNote); ++keyInt) {
                const uint8_t key = static_cast<uint8_t>(keyInt);
                if (IsBlackKey(key)) continue;

                int x0 = static_cast<int>(std::round(whiteIndex * whiteW));
                int x1 = static_cast<int>(std::round((whiteIndex + 1) * whiteW));
                if (x1 <= x0) x1 = x0 + 1;

                RECT rc{ x0, 0, x1, height };
                const bool pressed = IsKeyPressed(key);
                HBRUSH keyBrush = CreateSolidBrush(pressed ? RGB(170, 210, 255) : RGB(250, 250, 250));
                FillRect(memDC, &rc, keyBrush);
                DeleteObject(keyBrush);
                FrameRect(memDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

                whiteIndex++;
            }

            whiteIndex = 0;
            const int blackH = static_cast<int>(height * 0.62f);
            for (int keyInt = static_cast<int>(kKeyboardMinNote); keyInt <= static_cast<int>(kKeyboardMaxNote); ++keyInt) {
                const uint8_t key = static_cast<uint8_t>(keyInt);
                if (!IsBlackKey(key)) {
                    whiteIndex++;
                    continue;
                }

                const int centerX = static_cast<int>(std::round(whiteIndex * whiteW));
                const int blackW = std::max(4, static_cast<int>(std::round(whiteW * 0.6f)));
                int x0 = centerX - blackW / 2;
                int x1 = x0 + blackW;
                if (x0 < 0) { x1 -= x0; x0 = 0; }
                if (x1 > width) { x0 -= (x1 - width); x1 = width; }
                if (x1 <= x0) x1 = x0 + 1;

                RECT rc{ x0, 0, x1, blackH };
                const bool pressed = IsKeyPressed(key);
                HBRUSH keyBrush = CreateSolidBrush(pressed ? RGB(90, 140, 255) : RGB(20, 20, 20));
                FillRect(memDC, &rc, keyBrush);
                DeleteObject(keyBrush);
                FrameRect(memDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }

            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

static double BytesToMiB(const size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static std::string FormatMiB(const size_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << BytesToMiB(bytes) << " MiB";
    return oss.str();
}

static size_t EstimateMidiStoreBytes(const NoteDataStore& store) {
    size_t total = 0;
    total += store.startTimes.capacity() * sizeof(float);
    total += store.durations.capacity() * sizeof(float);
    total += store.keys.capacity() * sizeof(uint8_t);
    total += store.velocities.capacity() * sizeof(uint8_t);
    total += store.channels.capacity() * sizeof(uint8_t);
    total += store.tracks.capacity() * sizeof(uint8_t);

    total += store.ccTimes.capacity() * sizeof(float);
    total += store.ccChannels.capacity() * sizeof(uint8_t);
    total += store.ccNumbers.capacity() * sizeof(uint8_t);
    total += store.ccValues.capacity() * sizeof(uint8_t);

    total += store.pcTimes.capacity() * sizeof(float);
    total += store.pcChannels.capacity() * sizeof(uint8_t);
    total += store.pcPrograms.capacity() * sizeof(uint8_t);

    total += store.pbTimes.capacity() * sizeof(float);
    total += store.pbChannels.capacity() * sizeof(uint8_t);
    total += store.pbLSB.capacity() * sizeof(uint8_t);
    total += store.pbMSB.capacity() * sizeof(uint8_t);

    total += store.tempoMap.capacity() * sizeof(TempoEvent);
    return total;
}

static inline uint64_t FileTimeToUint64(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

bool QueryProcessCpuUsagePercent(double& outCpuPercent) {
    static bool initialized = false;
    static uint64_t prevProcTime = 0;
    static uint64_t prevWallTime = 0;
    static DWORD logicalProcessors = 1;

    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    if (!GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime)) {
        return false;
    }

    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);

    const uint64_t procNow = FileTimeToUint64(kernelTime) + FileTimeToUint64(userTime);
    const uint64_t wallNow = FileTimeToUint64(nowFt);

    if (!initialized) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        logicalProcessors = (si.dwNumberOfProcessors == 0) ? 1 : si.dwNumberOfProcessors;
        prevProcTime = procNow;
        prevWallTime = wallNow;
        initialized = true;
        outCpuPercent = 0.0;
        return true;
    }

    const uint64_t procDelta = procNow - prevProcTime;
    const uint64_t wallDelta = wallNow - prevWallTime;
    prevProcTime = procNow;
    prevWallTime = wallNow;

    if (wallDelta == 0) {
        outCpuPercent = 0.0;
        return true;
    }

    outCpuPercent = (static_cast<double>(procDelta) /
                     (static_cast<double>(wallDelta) * static_cast<double>(logicalProcessors))) * 100.0;
    if (outCpuPercent < 0.0) outCpuPercent = 0.0;
    return true;
}

bool QueryProcessMemoryUsageMB(double& workingSetMB, double& privateMB) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return false;
    }

    workingSetMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    privateMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    return true;
}

void UpdatePerformanceOverlay(HWND hWnd) {
    static auto lastUpdate = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastUpdate < std::chrono::milliseconds(500)) return;
    lastUpdate = now;

    double cpuPercent = 0.0;
    double ramWorkingMB = 0.0;
    double ramPrivateMB = 0.0;
    double gpuUsedMB = 0.0;
    double gpuBudgetMB = 0.0;

    const bool hasCpu = QueryProcessCpuUsagePercent(cpuPercent);
    const bool hasRam = QueryProcessMemoryUsageMB(ramWorkingMB, ramPrivateMB);
    const bool hasGpu = g_Renderer && g_Renderer->queryGpuMemoryUsageMB(gpuUsedMB, gpuBudgetMB);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "nasu Midi Player | ";

    if (hasCpu) oss << "CPU " << cpuPercent << "%";
    else oss << "CPU N/A";

    oss << " | ";
    if (hasRam) oss << "RAM WS " << ramWorkingMB << "MB / Private " << ramPrivateMB << "MB";
    else oss << "RAM N/A";

    oss << " | ";
    if (hasGpu) oss << "GPU VRAM " << gpuUsedMB << "MB / " << gpuBudgetMB << "MB";
    else oss << "GPU VRAM N/A";

    const std::string titleA = oss.str();
    std::wstring titleW(titleA.begin(), titleA.end());
    SetWindowTextW(hWnd, titleW.c_str());


}

void StopMIDIThread() {
    if (!g_MIDIThreadRunning.load(std::memory_order_acquire)) return;

    g_MIDIThreadStop.store(true, std::memory_order_release);
    if (g_MIDIThread.joinable()) {
        g_MIDIThread.join();
    }

    g_MIDIThreadRunning.store(false, std::memory_order_release);
    g_MIDIThreadStop.store(false, std::memory_order_release);

    if (g_HighResTimerEnabled.load(std::memory_order_acquire)) {
        timeEndPeriod(1);
        g_HighResTimerEnabled.store(false, std::memory_order_release);
        std::cout << "[Timing] timeEndPeriod(1) applied." << std::endl;
    }
}

void MIDIThreadProc() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        std::cerr << "[Audio] Warning: failed to set MIDI thread priority to THREAD_PRIORITY_HIGHEST." << std::endl;
    }

    constexpr float kMidiDispatchLookaheadSec = 0.010f;

    auto nextWake = std::chrono::steady_clock::now();

    while (!g_MIDIThreadStop.load(std::memory_order_acquire)) {
        if (!g_NoteStore || !g_MIDIPlayer) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const float currentTime = g_PlaybackTimeSec.load(std::memory_order_relaxed);
        const float dispatchTime = currentTime + kMidiDispatchLookaheadSec;

        {
            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);

            while (true) {
                bool hasEvent = false;
                float nextTime = 0.0f;
                int nextType = -1;

                if (g_NextPCIndex < g_NoteStore->pcTimes.size()) {
                    const float t = g_NoteStore->pcTimes[g_NextPCIndex];
                    if (t <= dispatchTime) {
                        hasEvent = true;
                        nextTime = t;
                        nextType = 0;
                    }
                }

                if (g_NextCCIndex < g_NoteStore->ccTimes.size()) {
                    const float t = g_NoteStore->ccTimes[g_NextCCIndex];
                    if (t <= dispatchTime && (!hasEvent || t < nextTime || (t == nextTime && nextType > 1))) {
                        hasEvent = true;
                        nextTime = t;
                        nextType = 1;
                    }
                }

                if (g_NextPBIndex < g_NoteStore->pbTimes.size()) {
                    const float t = g_NoteStore->pbTimes[g_NextPBIndex];
                    if (t <= dispatchTime && (!hasEvent || t < nextTime || (t == nextTime && nextType > 2))) {
                        hasEvent = true;
                        nextTime = t;
                        nextType = 2;
                    }
                }

                if (!hasEvent) break;

                if (nextType == 0) {
                    const uint8_t channel = g_NoteStore->pcChannels[g_NextPCIndex];
                    const uint8_t program = g_NoteStore->pcPrograms[g_NextPCIndex];
                    g_MIDIPlayer->sendShortMsg(0xC0 | (channel & 0x0F), program & 0x7F, 0);
                    g_NextPCIndex++;
                } else if (nextType == 1) {
                    const uint8_t channel = g_NoteStore->ccChannels[g_NextCCIndex];
                    const uint8_t ccNum = g_NoteStore->ccNumbers[g_NextCCIndex];
                    const uint8_t ccVal = g_NoteStore->ccValues[g_NextCCIndex];
                    g_MIDIPlayer->sendShortMsg(0xB0 | (channel & 0x0F), ccNum & 0x7F, ccVal & 0x7F);
                    g_NextCCIndex++;
                } else {
                    const uint8_t channel = g_NoteStore->pbChannels[g_NextPBIndex];
                    const uint8_t lsb = g_NoteStore->pbLSB[g_NextPBIndex];
                    const uint8_t msb = g_NoteStore->pbMSB[g_NextPBIndex];
                    g_MIDIPlayer->sendShortMsg(0xE0 | (channel & 0x0F), lsb & 0x7F, msb & 0x7F);
                    g_NextPBIndex++;
                }
            }

            while (g_NextNoteIndex < g_NoteStore->size() &&
                   g_NoteStore->startTimes[g_NextNoteIndex] <= dispatchTime) {
                const float noteStart = g_NoteStore->startTimes[g_NextNoteIndex];
                const uint8_t channel = g_NoteStore->channels[g_NextNoteIndex];
                const uint8_t key = g_NoteStore->keys[g_NextNoteIndex];
                const uint8_t velocity = g_NoteStore->velocities[g_NextNoteIndex];
                const float duration = g_NoteStore->durations[g_NextNoteIndex];

                g_MIDIPlayer->sendShortMsg(0x90 | (channel & 0x0F), key & 0x7F, velocity & 0x7F);
                RegisterKeyOn(key);

                const float noteOffTime = std::max(0.0f, noteStart + duration - kMidiDispatchLookaheadSec);
                g_ActiveNotes.push({ noteOffTime, channel, key });

                g_NextNoteIndex++;
            }

            while (!g_ActiveNotes.empty() && g_ActiveNotes.top().endTime <= currentTime) {
                const ActiveNote n = g_ActiveNotes.top();
                g_ActiveNotes.pop();
                g_MIDIPlayer->sendShortMsg(0x80 | (n.channel & 0x0F), n.key & 0x7F, 0);
                RegisterKeyOff(n.key);
            }
        }

        nextWake += std::chrono::milliseconds(1);
        std::this_thread::sleep_until(nextWake);
        const auto now = std::chrono::steady_clock::now();
        if (now > nextWake + std::chrono::milliseconds(4)) {
            nextWake = now;
        }
    }
}

void StartMIDIThread() {
    StopMIDIThread();

    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        g_HighResTimerEnabled.store(true, std::memory_order_release);
        std::cout << "[Timing] timeBeginPeriod(1) applied." << std::endl;
    } else {
        g_HighResTimerEnabled.store(false, std::memory_order_release);
        std::cerr << "[Timing] Warning: timeBeginPeriod(1) failed." << std::endl;
    }

    g_MIDIThreadStop.store(false, std::memory_order_release);
    g_MIDIThread = std::thread(MIDIThreadProc);
    g_MIDIThreadRunning.store(true, std::memory_order_release);
}

void LoadMidiFile(const std::wstring& filePath) {
    if (!g_NoteStore || !g_Renderer || !g_Clock) return;

    if (filePath != L"initial_dummy") {
        std::wcout << L"[2D] Loading MIDI: " << filePath << std::endl;

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER li{};
            li.HighPart = fad.nFileSizeHigh;
            li.LowPart = fad.nFileSizeLow;
            std::cout << "Loading MIDI file: " << li.QuadPart << " bytes (memory-mapped)" << std::endl;
        } else {
            std::cout << "Loading MIDI file (memory-mapped)" << std::endl;
        }
    }

    StopMIDIThread();
    if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();
    ClearKeyboardStates();

    g_Clock->reset();
    g_PlaybackTimeSec.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
        g_NextNoteIndex = 0;
        g_NextCCIndex = 0;
        g_NextPCIndex = 0;
        g_NextPBIndex = 0;
        g_NextTempoIndex = 0;
        g_ActiveNotes = decltype(g_ActiveNotes)();
    }

    if ([[maybe_unused]] MidiParser parser; MidiParser::parse(filePath, *g_NoteStore)) {
        g_Renderer->uploadNotes(*g_NoteStore);
        std::cout << "Successfully mapped and parsed MIDI file" << std::endl;

        const size_t noteBytes = g_NoteStore->startTimes.capacity() * sizeof(float)
            + g_NoteStore->durations.capacity() * sizeof(float)
            + g_NoteStore->keys.capacity() * sizeof(uint8_t)
            + g_NoteStore->velocities.capacity() * sizeof(uint8_t)
            + g_NoteStore->channels.capacity() * sizeof(uint8_t)
            + g_NoteStore->tracks.capacity() * sizeof(uint8_t);
        const size_t ccBytes = g_NoteStore->ccTimes.capacity() * sizeof(float)
            + g_NoteStore->ccChannels.capacity() * sizeof(uint8_t)
            + g_NoteStore->ccNumbers.capacity() * sizeof(uint8_t)
            + g_NoteStore->ccValues.capacity() * sizeof(uint8_t);
        const size_t pbBytes = g_NoteStore->pbTimes.capacity() * sizeof(float)
            + g_NoteStore->pbChannels.capacity() * sizeof(uint8_t)
            + g_NoteStore->pbLSB.capacity() * sizeof(uint8_t)
            + g_NoteStore->pbMSB.capacity() * sizeof(uint8_t);
        const size_t pcBytes = g_NoteStore->pcTimes.capacity() * sizeof(float)
            + g_NoteStore->pcChannels.capacity() * sizeof(uint8_t)
            + g_NoteStore->pcPrograms.capacity() * sizeof(uint8_t);
        const size_t tempoBytes = g_NoteStore->tempoMap.capacity() * sizeof(TempoEvent);
        const size_t totalBytes = EstimateMidiStoreBytes(*g_NoteStore);
        const size_t totalEvents = g_NoteStore->size()
            + g_NoteStore->ccTimes.size()
            + g_NoteStore->pcTimes.size()
            + g_NoteStore->pbTimes.size()
            + g_NoteStore->tempoMap.size();

        std::cout << "[midi::Player] Loaded MIDI: " << totalEvents
                  << " events, approx " << FormatMiB(totalBytes)
                  << " allocated (notes " << FormatMiB(noteBytes)
                  << ", cc " << FormatMiB(ccBytes)
                  << ", pitch " << FormatMiB(pbBytes)
                  << ", prog " << FormatMiB(pcBytes)
                  << ", tempo " << FormatMiB(tempoBytes) << ")" << std::endl;

        if (!g_NoteStore->tempoMap.empty()) {
            g_Renderer->setBPM(g_NoteStore->tempoMap[0].bpm);
        } else {
            g_Renderer->setBPM(120.0f);
        }

        if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();

        g_Clock->reset();
        g_PlaybackTimeSec.store(0.0f, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
            g_NextNoteIndex = 0;
            g_NextCCIndex = 0;
            g_NextPCIndex = 0;
            g_NextPBIndex = 0;
            g_NextTempoIndex = 0;
            g_ActiveNotes = decltype(g_ActiveNotes)();
        }

        std::cout << "[midi::Player] Parsed events: CC " << g_NoteStore->ccTimes.size()
                  << " / PC " << g_NoteStore->pcTimes.size()
                  << " / PB " << g_NoteStore->pbTimes.size() << std::endl;

        StartMIDIThread();
    } else {
        std::wcerr << L"[MIDI] Failed to load MIDI file: " << filePath << std::endl;
    }
}

void UpdateTempoVisual(const float currentTime) {
    if (!g_NoteStore || !g_Renderer) return;

    while (g_NextTempoIndex + 1 < g_NoteStore->tempoMap.size() &&
           g_NoteStore->tempoMap[g_NextTempoIndex + 1].time <= currentTime) {
        g_NextTempoIndex++;
        g_Renderer->setBPM(g_NoteStore->tempoMap[g_NextTempoIndex].bpm);
        std::cout << "[Timeline] Tempo change: " << g_NoteStore->tempoMap[g_NextTempoIndex].bpm << " BPM" << std::endl;
    }
}

auto WndProc(HWND hWnd, const UINT message, const WPARAM wParam, const LPARAM lParam) -> LRESULT {
    switch (message) {
        case WM_SIZE:
            if (g_Renderer) g_Renderer->onResize(LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_DESTROY:
            if (hWnd == g_MainWnd) {
                PostQuitMessage(0);
            }
            break;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_SPACE:
                    if (g_Clock) {
                        g_Clock->togglePause();
                        if (g_Clock->isPaused() && g_MIDIPlayer) {
                            g_MIDIPlayer->allNotesOff();
                            ClearKeyboardStates();
                            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
                            g_ActiveNotes = decltype(g_ActiveNotes)();
                        }
                    }
                    break;
                case 'R':
                    if (g_Clock) {
                        StopMIDIThread();
                        if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();
                        ClearKeyboardStates();

                        g_Clock->reset();
                        g_PlaybackTimeSec.store(0.0f, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
                            g_NextNoteIndex = 0;
                            g_NextCCIndex = 0;
                            g_NextPCIndex = 0;
                            g_NextPBIndex = 0;
                            g_NextTempoIndex = 0;
                            g_ActiveNotes = decltype(g_ActiveNotes)();
                        }

                        if (g_Renderer && g_NoteStore && !g_NoteStore->tempoMap.empty()) {
                            g_Renderer->setBPM(g_NoteStore->tempoMap[0].bpm);
                        }

                        StartMIDIThread();
                    }
                    break;
                case VK_UP:
                        g_Renderer->setSecondsPerScreen(
                            std::max(0.5f, g_Renderer->getSecondsPerScreen() / 1.1f));
                        break;
                case VK_DOWN:
                        g_Renderer->setSecondsPerScreen(
                            std::min(30.0f, g_Renderer->getSecondsPerScreen() * 1.1f));
                        break;
                default: ;
            }
            break;
        case WM_DROPFILES:
            {
                const auto hDrop = reinterpret_cast<HDROP>(wParam);
                WCHAR filePath[MAX_PATH];
                if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
                    LoadMidiFile(filePath);
                }
                DragFinish(hDrop);
            }
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, const int nShowCmd) {
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);

    constexpr TCHAR CLASS_NAME[] = _T("BlackMidiPlayerWindowClass");
    constexpr TCHAR KEYBOARD_CLASS_NAME[] = _T("BlackMidiKeyboardWindowClass");

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    WNDCLASS kbdWc = {};
    kbdWc.lpfnWndProc = KeyboardWndProc;
    kbdWc.hInstance = hInstance;
    kbdWc.lpszClassName = KEYBOARD_CLASS_NAME;
    kbdWc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&kbdWc);

    HWND hWnd = CreateWindowEx(WS_EX_ACCEPTFILES, CLASS_NAME, _T("nasu Midi Player"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);

    if (hWnd == nullptr) return 0;
    g_MainWnd = hWnd;
    ShowWindow(hWnd, nShowCmd);

    g_KeyboardWnd = CreateWindowEx(
        0,
        KEYBOARD_CLASS_NAME,
        _T("nasu Midi Player - Keyboard"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 220,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );
    if (g_KeyboardWnd) {
        ShowWindow(g_KeyboardWnd, SW_SHOW);
        UpdateWindow(g_KeyboardWnd);
    }

    ClearKeyboardStates();

    g_Clock = std::make_unique<Clock>();
    g_NoteStore = std::make_unique<NoteDataStore>();
    g_Renderer = std::make_unique<Renderer>();
    g_MIDIPlayer = std::make_unique<MIDIPlayer>();

    std::cout << "[App] BlackMidiPlayer - C++ standard: " << __cplusplus << std::endl;
    std::cout << "[Renderer] Initializing..." << std::endl;
    if (!g_Renderer->initialize(hWnd, 1280, 720)) {
        std::cerr << "[Renderer] Fatal: initialization failed." << std::endl;
        MessageBox(hWnd, _T("Renderer initialization failed. Please check DirectX 11 support."), _T("Error"), MB_ICONERROR);
        return -1;
    }
    std::cout << "[Renderer] Initialized." << std::endl;

    std::cout << "[Audio] Opening MIDI output device (OmniMIDI)..." << std::endl;
    if (!g_MIDIPlayer->open()) {
        std::cerr << "[Audio] Warning: MIDI output device could not be opened. Audio will be disabled." << std::endl;
    } else {
        std::cout << "[Audio] MIDI output device opened." << std::endl;
    }

    g_Renderer->setSecondsPerScreen(0.5f);

    LoadMidiFile(L"initial_dummy");

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            g_Clock->tick();
            const auto time = static_cast<float>(g_Clock->getTotalTime());

            g_PlaybackTimeSec.store(time, std::memory_order_relaxed);
            UpdateTempoVisual(time);
            g_Renderer->render(time);
            UpdatePerformanceOverlay(hWnd);
        }
    }

    StopMIDIThread();
    if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();
    ClearKeyboardStates();

    return static_cast<int>(msg.wParam);
}
