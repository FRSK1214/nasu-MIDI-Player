#pragma once

#include <windows.h>

namespace BlackMidi {
    class Clock {
    public:
        Clock() {
            QueryPerformanceFrequency(&frequency);
            reset();
        }

        void reset() {
            QueryPerformanceCounter(&startTime);
            lastTime = startTime;
            currentTime = startTime;
            deltaTime = 0.0;
            totalTime = 0.0;
            paused = false;
        }

        void tick() {
            if (paused) {
                deltaTime = 0.0;
                return;
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            currentTime = now;

            deltaTime = static_cast<double>(currentTime.QuadPart - lastTime.QuadPart) / frequency.QuadPart;
            totalTime += deltaTime;
            lastTime = currentTime;
        }

        void togglePause() {
            paused = !paused;
        }

        double getTotalTime() const {
            return totalTime;
        }

        double getDeltaTime() const {
            return deltaTime;
        }

        bool isPaused() const {
            return paused;
        }

    private:
        LARGE_INTEGER frequency{};
        LARGE_INTEGER startTime{};
        LARGE_INTEGER lastTime{};
        LARGE_INTEGER currentTime{};
        double deltaTime{};
        double totalTime{};
        bool paused{};
    };
}
