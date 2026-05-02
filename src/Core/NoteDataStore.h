#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include "ska_sort.hpp"

namespace BlackMidi {
    struct TempoEvent {
        float time;
        float bpm;
    };

    struct NoteDataStore {
        std::vector<float>   startTimes;
        std::vector<float>   durations;
        std::vector<uint8_t> keys;
        std::vector<uint8_t> velocities;
        std::vector<uint8_t> channels;
        std::vector<uint8_t> tracks;
        std::vector<TempoEvent> tempoMap;

        void addNote(const float startTime, const float duration, const uint8_t key, const uint8_t velocity,
                     const uint8_t channel, const uint8_t track) {
            startTimes.push_back(startTime);
            durations.push_back(duration);
            keys.push_back(key);
            velocities.push_back(velocity);
            channels.push_back(channel);
            tracks.push_back(track);
        }

        [[nodiscard]] size_t size() const {
            return startTimes.size();
        }

        // ska_sort (radix sort) による高速ソート。
        // 一時 struct にまとめて ska_sort し、展開して書き戻す。
        void sort() {
            const size_t n = size();
            if (n == 0) return;

            // ---- 1. SoA → AoS (一時 struct に詰める) ----
            struct NoteEntry {
                float    startTime;
                float    duration;
                uint8_t  key;
                uint8_t  velocity;
                uint8_t  channel;
                uint8_t  track;
            };

            std::vector<NoteEntry> entries(n);
            for (size_t i = 0; i < n; ++i) {
                entries[i] = { startTimes[i], durations[i],
                               keys[i], velocities[i],
                               channels[i], tracks[i] };
            }

            // ---- 2. ska_sort: float キーでラジックスソート (O(n)) ----
            ska_sort(entries.begin(), entries.end(),
                     [](const NoteEntry &e) { return e.startTime; });

            // ---- 3. AoS → SoA に書き戻す ----
            for (size_t i = 0; i < n; ++i) {
                startTimes[i]  = entries[i].startTime;
                durations[i]   = entries[i].duration;
                keys[i]        = entries[i].key;
                velocities[i]  = entries[i].velocity;
                channels[i]    = entries[i].channel;
                tracks[i]      = entries[i].track;
            }
        }
    };
}
