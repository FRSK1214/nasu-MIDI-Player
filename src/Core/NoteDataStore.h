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

        std::vector<float>   ccTimes;
        std::vector<uint8_t> ccChannels;
        std::vector<uint8_t> ccNumbers;
        std::vector<uint8_t> ccValues;

        std::vector<float>   pcTimes;
        std::vector<uint8_t> pcChannels;
        std::vector<uint8_t> pcPrograms;

        std::vector<float>   pbTimes;
        std::vector<uint8_t> pbChannels;
        std::vector<uint8_t> pbLSB;
        std::vector<uint8_t> pbMSB;

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

        void sort() {
            const size_t n = size();
            if (n == 0) return;

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

            ska_sort(entries.begin(), entries.end(),
                     [](const NoteEntry &e) { return e.startTime; });

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
