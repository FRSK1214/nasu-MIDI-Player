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
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            currentTime = now;

            if (paused) {
                lastTime = currentTime;
                deltaTime = 0.0;
                return;
            }

            deltaTime = static_cast<double>(currentTime.QuadPart - lastTime.QuadPart) / frequency.QuadPart;
            totalTime += deltaTime;
            lastTime = currentTime;
        }

        void togglePause() {
            const bool wasPaused = paused;
            paused = !paused;

            if (wasPaused && !paused) {
                QueryPerformanceCounter(&lastTime);
                currentTime = lastTime;
                deltaTime = 0.0;
            }
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
