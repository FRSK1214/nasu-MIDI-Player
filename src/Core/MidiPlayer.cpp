#include "MidiParser.h"
#include <iostream>
#include <vector>
#include <algorithm>

#ifdef USE_MIDIFILE
#include "MidiFile.h"
#endif

namespace BlackMidi {
    bool MidiParser::parse(const std::string &filePath, NoteDataStore &outStore) {
        outStore.startTimes.clear();
        outStore.durations.clear();
        outStore.keys.clear();
        outStore.velocities.clear();
        outStore.channels.clear();
        outStore.tracks.clear();

#ifndef USE_MIDIFILE
        std::cout << "[INFO] Generating test pattern..." << std::endl;
        for (int i = 0; i < 500000; ++i) {
            float startTime = static_cast<float>(i) * 0.01f;
            outStore.addNote(
                startTime,
                0.5f,
                static_cast<uint8_t>(21 + (i % 88)),
                100,
                static_cast<uint8_t>(i % 16),
                0
            );
        }
        outStore.sort();
        return true;
#else
        smf::MidiFile midifile;
        if (!midifile.read(filePath)) {
            std::cerr << "[ERROR] Could not read MIDI file: " << filePath << std::endl;
            return false;
        }

        midifile.doTimeAnalysis();
        midifile.linkNotePairs();

        for (int track = 0; track < midifile.getTrackCount(); ++track) {
            for (int event = 0; event < midifile[track].size(); ++event) {
                auto& midievent = midifile[track][event];
                if (midievent.isNoteOn()) {
                    float startTime = (float)midievent.seconds;
                    float duration = (float)midievent.getDurationInSeconds();
                    if (duration <= 0.0f) duration = 0.1f;
                    outStore.addNote(startTime, duration, (uint8_t)midievent.getKeyNumber(), (uint8_t)midievent.getVelocity(), (uint8_t)midievent.getChannel(), (uint8_t)track);
                }
            }
        }
        outStore.sort();
        std::cout << "[INFO] Successfully parsed " << outStore.size() << " notes." << std::endl;
        return true;
#endif
    }
}
