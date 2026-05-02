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

    std::cout << "[PERF] " << titleA << std::endl;
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
        std::cout << "[INFO] timeEndPeriod(1) applied." << std::endl;
    }
}

void MIDIThreadProc() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        std::cerr << "[WARN] Failed to set MIDI thread priority to THREAD_PRIORITY_HIGHEST." << std::endl;
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

            while (g_NextNoteIndex < g_NoteStore->size() &&
                   g_NoteStore->startTimes[g_NextNoteIndex] <= dispatchTime) {
                const float noteStart = g_NoteStore->startTimes[g_NextNoteIndex];
                const uint8_t channel = g_NoteStore->channels[g_NextNoteIndex];
                const uint8_t key = g_NoteStore->keys[g_NextNoteIndex];
                const uint8_t velocity = g_NoteStore->velocities[g_NextNoteIndex];
                const float duration = g_NoteStore->durations[g_NextNoteIndex];

                g_MIDIPlayer->sendShortMsg(0x90 | (channel & 0x0F), key & 0x7F, velocity & 0x7F);

                const float noteOffTime = std::max(0.0f, noteStart + duration - kMidiDispatchLookaheadSec);
                g_ActiveNotes.push({ noteOffTime, channel, key });

                g_NextNoteIndex++;
            }

            while (!g_ActiveNotes.empty() && g_ActiveNotes.top().endTime <= currentTime) {
                const ActiveNote n = g_ActiveNotes.top();
                g_ActiveNotes.pop();
                g_MIDIPlayer->sendShortMsg(0x80 | (n.channel & 0x0F), n.key & 0x7F, 0);
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
        std::cout << "[INFO] timeBeginPeriod(1) applied." << std::endl;
    } else {
        g_HighResTimerEnabled.store(false, std::memory_order_release);
        std::cerr << "[WARN] timeBeginPeriod(1) failed." << std::endl;
    }

    g_MIDIThreadStop.store(false, std::memory_order_release);
    g_MIDIThread = std::thread(MIDIThreadProc);
    g_MIDIThreadRunning.store(true, std::memory_order_release);
}

void LoadMidiFile(const std::wstring& filePath) {
    if (!g_NoteStore || !g_Renderer || !g_Clock) return;

    std::wcout << L"[INFO] Loading MIDI file: " << filePath << std::endl;

    StopMIDIThread();
    if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();

    g_Clock->reset();
    g_PlaybackTimeSec.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
        g_NextNoteIndex = 0;
        g_NextTempoIndex = 0;
        g_ActiveNotes = decltype(g_ActiveNotes)();
    }

    if ([[maybe_unused]] MidiParser parser; MidiParser::parse(filePath, *g_NoteStore)) {
        g_Renderer->uploadNotes(*g_NoteStore);
        std::cout << "[INFO] Ready to play " << g_NoteStore->size() << " notes." << std::endl;

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
            g_NextTempoIndex = 0;
            g_ActiveNotes = decltype(g_ActiveNotes)();
        }

        StartMIDIThread();
    } else {
        std::wcerr << L"[ERROR] Failed to load MIDI file: " << filePath << std::endl;
    }
}

void UpdateTempoVisual(const float currentTime) {
    if (!g_NoteStore || !g_Renderer) return;

    while (g_NextTempoIndex + 1 < g_NoteStore->tempoMap.size() &&
           g_NoteStore->tempoMap[g_NextTempoIndex + 1].time <= currentTime) {
        g_NextTempoIndex++;
        g_Renderer->setBPM(g_NoteStore->tempoMap[g_NextTempoIndex].bpm);
        std::cout << "[INFO] Tempo change: " << g_NoteStore->tempoMap[g_NextTempoIndex].bpm << " BPM" << std::endl;
    }
}

auto WndProc(HWND hWnd, const UINT message, const WPARAM wParam, const LPARAM lParam) -> LRESULT {
    switch (message) {
        case WM_SIZE:
            if (g_Renderer) g_Renderer->onResize(LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_SPACE:
                    if (g_Clock) {
                        g_Clock->togglePause();
                        if (g_Clock->isPaused() && g_MIDIPlayer) {
                            g_MIDIPlayer->allNotesOff();
                            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
                            g_ActiveNotes = decltype(g_ActiveNotes)();
                        }
                    }
                    break;
                case 'R':
                    if (g_Clock) {
                        g_Clock->reset();
                        g_PlaybackTimeSec.store(0.0f, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lock(g_MIDIStateMutex);
                            g_NextNoteIndex = 0;
                            g_NextTempoIndex = 0;
                            g_ActiveNotes = decltype(g_ActiveNotes)();
                        }
                        if (g_MIDIPlayer) g_MIDIPlayer->allNotesOff();
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
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(WS_EX_ACCEPTFILES, CLASS_NAME, _T("nasu Midi Player"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);

    if (hWnd == nullptr) return 0;
    ShowWindow(hWnd, nShowCmd);

    g_Clock = std::make_unique<Clock>();
    g_NoteStore = std::make_unique<NoteDataStore>();
    g_Renderer = std::make_unique<Renderer>();
    g_MIDIPlayer = std::make_unique<MIDIPlayer>();

    std::cout << "[INFO] Initializing Renderer..." << std::endl;
    if (!g_Renderer->initialize(hWnd, 1280, 720)) {
        std::cerr << "[FATAL] Renderer initialization failed." << std::endl;
        MessageBox(hWnd, _T("Renderer initialization failed. Please check DirectX 11 support."), _T("Error"), MB_ICONERROR);
        return -1;
    }
    std::cout << "[INFO] Renderer initialized." << std::endl;

    std::cout << "[INFO] Opening MIDI Output Device (OmniMIDI)..." << std::endl;
    if (!g_MIDIPlayer->open()) {
        std::cerr << "[WARN] MIDI Output Device could not be opened. Audio will be disabled." << std::endl;
    } else {
        std::cout << "[INFO] MIDI Output Device opened." << std::endl;
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

    return static_cast<int>(msg.wParam);
}
