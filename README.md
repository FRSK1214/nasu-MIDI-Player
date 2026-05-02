# nasu-MIDI-Player

A high-performance Black MIDI player for Windows.
It visualizes notes with DirectX 11 and sends real-time MIDI output via OmniMIDI.

---

## Features

- **Fast MIDI parsing**
- **Optimized rendering**
- **Low-latency MIDI output**
- **Runtime performance telemetry**

---

## Requirements

- OS: **Windows**
- Graphics API: **DirectX 11**
- MIDI output: **OmniMIDI** (`OmniMIDI.dll`)
- Compiler: MinGW g++ / Clang-cl / MSVC (C++20-level features recommended)

---

## Build (MinGW g++ example)

```BlackMidiPlayer/README.md#L1-1
g++ -std=c++2a -O3 -mwindows -DUNICODE -D_UNICODE -DUSE_SKA_SORT -I. src/main.cpp src/Core/MidiParser.cpp src/Graphics/Renderer.cpp -o BlackMidiPlayer.exe -fopenmp -ld3d11 -ldxgi -ld3dcompiler -ldxguid -lwinmm -lpsapi -lgomp -static -static-libstdc++ -static-libgcc
```

> `-lpsapi` is required for `GetProcessMemoryInfo`.

---

## Usage

1. Launch `nasuMidiPlayer.exe`
2. Drag & drop a MIDI file onto the window
3. Playback and visualization will start

---

## Controls

- `Space` : Pause / Resume
- `R` : Reset to beginning
- `↑` : Zoom in (shorter time span on screen)
- `↓` : Zoom out (longer time span on screen)

---

## Troubleshooting

- **No sound output**
  - Make sure OmniMIDI is installed
  - Make sure `OmniMIDI.dll` is accessible at runtime
- **`GetProcessMemoryInfo` undefined reference at link time**
  - Add `-lpsapi` to linker flags
- **Incorrect tempo on some files**
  - This project now scans tempo events across all tracks for accuracy
- **Rendering is too heavy**
  - Adjust visible time range using `↑` / `↓`

---

## Disclaimer

This project is under active development.
Very large MIDI files may still cause high CPU/GPU load depending on your hardware.
