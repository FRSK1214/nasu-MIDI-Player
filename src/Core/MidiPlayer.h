#pragma once

#include <windows.h>
#include <tchar.h>
#include <iostream>

typedef BOOL (WINAPI *LPISKDMAPIAVAILABLE)();

typedef BOOL (WINAPI *LPINITIALIZEKDMAPISTREAM)();

typedef BOOL (WINAPI *LPTERMINATEKDMAPISTREAM)();

typedef VOID (WINAPI *LPRESETKDMAPISTREAM)();

typedef VOID (WINAPI *LPSENDDIRECTDATA)(DWORD);

namespace BlackMidi {
    class MIDIPlayer {
    public:
        MIDIPlayer() : hLib(nullptr), pIsAvailable(nullptr), pInitialize(nullptr),
                       pTerminate(nullptr), pReset(nullptr), pSendDirectData(nullptr) {
        }

        ~MIDIPlayer() { close(); }

        bool open() {
            hLib = LoadLibrary(_T("OmniMIDI.dll"));
            if (!hLib) {
                std::cerr << "[Audio] OmniMIDI.dll not found. Please install OmniMIDI driver." << std::endl;
                return false;
            }

            pIsAvailable = reinterpret_cast<LPISKDMAPIAVAILABLE>(GetProcAddress(hLib, "IsKDMAPIAvailable"));
            pInitialize = reinterpret_cast<LPINITIALIZEKDMAPISTREAM>(GetProcAddress(hLib, "InitializeKDMAPIStream"));
            pTerminate = reinterpret_cast<LPTERMINATEKDMAPISTREAM>(GetProcAddress(hLib, "TerminateKDMAPIStream"));
            pReset = reinterpret_cast<LPRESETKDMAPISTREAM>(GetProcAddress(hLib, "ResetKDMAPIStream"));
            pSendDirectData = reinterpret_cast<LPSENDDIRECTDATA>(GetProcAddress(hLib, "SendDirectData"));

            if (!pIsAvailable || !pInitialize || !pTerminate || !pReset || !pSendDirectData) {
                std::cerr << "[Audio] Failed to get KDMAPI function addresses." << std::endl;
                return false;
            }

            if (!pIsAvailable()) {
                std::cerr << "[Audio] KDMAPI is not available." << std::endl;
                return false;
            }

            if (!pInitialize()) {
                std::cerr << "[Audio] Failed to initialize KDMAPI stream." << std::endl;
                return false;
            }

            std::cout << "[Audio] OmniMIDI (KDMAPI) initialized successfully" << std::endl;
            return true;
        }

        void close() {
            if (pTerminate) pTerminate();
            if (hLib) {
                FreeLibrary(hLib);
                hLib = nullptr;
            }
            pIsAvailable = nullptr;
            pInitialize = nullptr;
            pTerminate = nullptr;
            pReset = nullptr;
            pSendDirectData = nullptr;
        }

        void sendShortMsg(const uint8_t status, const uint8_t data1, const uint8_t data2) const {
            if (!pSendDirectData) return;
            const DWORD msg = status | (data1 << 8) | (data2 << 16);
            pSendDirectData(msg);
        }

        void allNotesOff() const {
            if (pReset) pReset();
            for (int ch = 0; ch < 16; ++ch) {
                sendShortMsg(0xB0 | ch, 120, 0);
                sendShortMsg(0xB0 | ch, 123, 0);
                sendShortMsg(0xB0 | ch, 121, 0);
            }
        }

    private:
        HMODULE hLib;
        LPISKDMAPIAVAILABLE pIsAvailable;
        LPINITIALIZEKDMAPISTREAM pInitialize;
        LPTERMINATEKDMAPISTREAM pTerminate;
        LPRESETKDMAPISTREAM pReset;
        LPSENDDIRECTDATA pSendDirectData;
    };
}
