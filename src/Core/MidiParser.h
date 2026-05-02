#pragma once

#include <string>
#include <vector>
#include <memory>
#include "NoteDataStore.h"

namespace BlackMidi {
    class MidiParser {
    public:
        MidiParser() = default;

        ~MidiParser() = default;

        static bool parse(const std::wstring &filePath, NoteDataStore &outStore);
    };
}
