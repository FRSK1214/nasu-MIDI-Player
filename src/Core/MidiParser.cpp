#include "MidiParser.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <numeric>
#include <thread>
#include <atomic>
#include <windows.h>
#include <utility>

#ifdef USE_SKA_SORT
#include "ska_sort.hpp"
#endif

namespace BlackMidi {

struct MappedFile {
    HANDLE hFile    = INVALID_HANDLE_VALUE;
    HANDLE hMapping = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;

    explicit MappedFile(const std::wstring& path) {
        hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Failed to open file.");

        LARGE_INTEGER fs;
        if (!GetFileSizeEx(hFile, &fs)) { CloseHandle(hFile); throw std::runtime_error("GetFileSizeEx failed."); }
        size = static_cast<size_t>(fs.QuadPart);

        hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) { CloseHandle(hFile); throw std::runtime_error("CreateFileMapping failed."); }

        data = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
        if (!data) { CloseHandle(hMapping); CloseHandle(hFile); throw std::runtime_error("MapViewOfFile failed."); }
    }

    ~MappedFile() {
        if (data)    UnmapViewOfFile(data);
        if (hMapping) CloseHandle(hMapping);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

static uint32_t Read32BE(const uint8_t*& p) {
    const uint32_t v = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p+=4; return v;
}
static uint16_t Read16BE(const uint8_t*& p) {
    const uint16_t v = (p[0]<<8)|p[1]; p+=2; return v;
}
static inline uint32_t ReadVarLen(const uint8_t*& p) {
    uint32_t v = 0; uint8_t b;
    do { b = *p++; v = (v<<7)|(b&0x7F); } while (b&0x80);
    return v;
}

struct TempoSegment {
    uint32_t tick;
    double   seconds;
    double   ticksToSec;
};

// tick→秒変換: tempoSegs は tick 昇順であることが前提。
// parseTrack 内でカーソルを前に進めるだけなので呼び出しコストは O(1) 均償。
static inline float TickToSecAtCursor(
    const uint32_t tick,
    const std::vector<TempoSegment>& segs,
    size_t& cursor)
{
    // カーソルを前に進める（tick は単調増加なので巻き戻しは不要）
    while (cursor + 1 < segs.size() && segs[cursor + 1].tick <= tick)
        ++cursor;
    return static_cast<float>(segs[cursor].seconds + (tick - segs[cursor].tick) * segs[cursor].ticksToSec);
}

struct TrackSoA {
    std::vector<float> startTimes;
    std::vector<float> durations;
    std::vector<uint8_t> keys;
    std::vector<uint8_t> velocities;
    std::vector<uint8_t> channels;
    std::vector<uint8_t> tracks;

    void reserve(const size_t n) {
        startTimes.reserve(n);
        durations.reserve(n);
        keys.reserve(n);
        velocities.reserve(n);
        channels.reserve(n);
        tracks.reserve(n);
    }

    [[nodiscard]] size_t size() const { return startTimes.size(); }
};

static void CollectTempoEventsFromTrack(
    const uint8_t* start,
    const uint32_t size,
    std::vector<std::pair<uint32_t, uint32_t>>& outTempo)
{
    const uint8_t* tp   = start;
    const uint8_t* tend = start + size;
    uint32_t curTick = 0;
    uint8_t runStat = 0;

    while (tp < tend) {
        curTick += ReadVarLen(tp);
        if (tp >= tend) break;

        uint8_t st = *tp++;
        if (!(st & 0x80)) { st = runStat; tp--; } else { runStat = st; }

        if (st == 0xFF) {
            if (tp >= tend) break;
            const uint8_t meta = *tp++;
            const uint32_t len = ReadVarLen(tp);
            if (meta == 0x51 && len >= 3 && tp + 3 <= tend) {
                const uint32_t uspqn = (tp[0] << 16) | (tp[1] << 8) | tp[2];
                outTempo.emplace_back(curTick, uspqn);
            }
            tp += len;
        } else if ((st & 0xF0) == 0xF0) {
            if (st == 0xF0 || st == 0xF7) tp += ReadVarLen(tp);
        } else {
            tp += ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
        }
    }
}

static TrackSoA parseTrack(
    const uint8_t* data, const uint32_t size, const int trackIndex,
    const std::vector<TempoSegment>& tempoSegs)
{
    TrackSoA local;
    local.reserve(size / 3);

    const uint8_t* tp   = data;
    const uint8_t* tend = data + size;
    uint32_t curTick = 0;
    uint8_t  runStat = 0;

    // テンポセグメントカーソル: tick は単調増加なので前進のみ → O(1) 均償
    size_t tempoCursor = 0;

    struct Pending { float start; uint8_t vel; bool active; };
    Pending pending[16][128] = {};

    while (tp < tend) {
        curTick += ReadVarLen(tp);
        if (tp >= tend) break;

        uint8_t st = *tp++;
        if (!(st & 0x80)) { st = runStat; tp--; } else { runStat = st; }

        const uint8_t type = st & 0xF0;
        const uint8_t ch   = st & 0x0F;

        if (type == 0x90 || type == 0x80) {
            const uint8_t key = *tp++ & 0x7F;
            const uint8_t vel = *tp++ & 0x7F;
            // 毎回全探索していた TickToSec をカーソル方式に変更
            const float   sec = TickToSecAtCursor(curTick, tempoSegs, tempoCursor);
            if (type == 0x90 && vel > 0) {
                pending[ch][key] = {sec, vel, true};
            } else {
                if (pending[ch][key].active) {
                    float dur = sec - pending[ch][key].start;
                    if (dur <= 0.0f) dur = 0.01f;
                    local.startTimes.push_back(pending[ch][key].start);
                    local.durations.push_back(dur);
                    local.keys.push_back(key);
                    local.velocities.push_back(pending[ch][key].vel);
                    local.channels.push_back(ch);
                    local.tracks.push_back(static_cast<uint8_t>(trackIndex));
                    pending[ch][key].active = false;
                }
            }
        } else if (st == 0xFF) {
            tp++;
            tp += ReadVarLen(tp);
        } else if (type == 0xC0 || type == 0xD0) {
            tp++;
        } else if (type == 0xA0 || type == 0xB0 || type == 0xE0) {
            tp += 2;
        } else if (st == 0xF0 || st == 0xF7) {
            tp += ReadVarLen(tp);
        }
    }
    return local;
}

static void sortNoteStore(NoteDataStore& store) {
    const size_t n = store.size();
    if (n == 0) return;

#ifdef USE_SKA_SORT
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    ska_sort(idx.begin(), idx.end(),
             [&](const size_t i) { return store.startTimes[i]; });
#else
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [&](size_t a, size_t b){ return store.startTimes[a] < store.startTimes[b]; });
#endif

    std::vector<float>   st(n), dur(n);
    std::vector<uint8_t> key(n), vel(n), ch(n), trk(n);
    for (size_t i = 0; i < n; ++i) {
        st[i]  = store.startTimes[idx[i]];
        dur[i] = store.durations[idx[i]];
        key[i] = store.keys[idx[i]];
        vel[i] = store.velocities[idx[i]];
        ch[i]  = store.channels[idx[i]];
        trk[i] = store.tracks[idx[i]];
    }
    store.startTimes = std::move(st);
    store.durations  = std::move(dur);
    store.keys       = std::move(key);
    store.velocities = std::move(vel);
    store.channels   = std::move(ch);
    store.tracks     = std::move(trk);
}

bool MidiParser::parse(const std::wstring& filePath, NoteDataStore& outStore) {
    outStore.startTimes.clear();
    outStore.durations.clear();
    outStore.keys.clear();
    outStore.velocities.clear();
    outStore.channels.clear();
    outStore.tracks.clear();
    outStore.tempoMap.clear();

    if (filePath == L"initial_dummy") {
        outStore.tempoMap.push_back({0.0f, 120.0f});
        constexpr int N = 100000;
        outStore.startTimes.resize(N); outStore.durations.resize(N);
        outStore.keys.resize(N);       outStore.velocities.resize(N);
        outStore.channels.resize(N);   outStore.tracks.resize(N);
        for (int i = 0; i < N; ++i) {
            outStore.startTimes[i]  = static_cast<float>(i) * 0.001f;
            outStore.durations[i]   = 0.1f;
            outStore.keys[i]        = static_cast<uint8_t>(21 + i % 88);
            outStore.velocities[i]  = 16;
            outStore.channels[i]    = static_cast<uint8_t>(i % 16);
            outStore.tracks[i]      = 0;
        }
        return true;
    }

    const auto t0 = std::chrono::steady_clock::now();

    std::unique_ptr<MappedFile> mapped;
    try {
        mapped = std::make_unique<MappedFile>(filePath);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return false;
    }

    const uint8_t* p   = mapped->data;
    const uint8_t* end = mapped->data + mapped->size;

    if (p + 14 > end || std::memcmp(p, "MThd", 4) != 0) return false;
    p += 4;
    const uint32_t hdrSize  = Read32BE(p);
    Read16BE(p); // format type
    const uint16_t numTracks = Read16BE(p);
    const uint16_t division  = Read16BE(p);
    p = mapped->data + 8 + hdrSize;

    std::cout << "[INFO] TPQN:" << division << " Tracks:" << numTracks << std::endl;

    struct TrackInfo { const uint8_t* start; uint32_t size; };
    std::vector<TrackInfo> tracks;
    tracks.reserve(numTracks);
    for (int i = 0; i < numTracks && p + 8 <= end; ++i) {
        if (std::memcmp(p, "MTrk", 4) != 0) break;
        p += 4;
        uint32_t sz = Read32BE(p);
        if (p + sz > end) sz = static_cast<uint32_t>(end - p);
        tracks.push_back({p, sz});
        p += sz;
    }

    std::vector<std::pair<uint32_t, uint32_t>> tempoEvents;
    tempoEvents.reserve(256);
    tempoEvents.emplace_back(0u, 500000u);

    // テンポ定義は本来どのトラックにも置けるため、全トラックを走査して正確性を優先する。
    // （一部ファイルは track 0 以外にテンポイベントを持つ）
    for (const auto& [start, size] : tracks)
        CollectTempoEventsFromTrack(start, size, tempoEvents);

    std::sort(tempoEvents.begin(), tempoEvents.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 同tickの重複は最後のイベントを採用
    std::vector<std::pair<uint32_t, uint32_t>> dedupTempo;
    dedupTempo.reserve(tempoEvents.size());
    for (const auto& ev : tempoEvents) {
        if (!dedupTempo.empty() && dedupTempo.back().first == ev.first) {
            dedupTempo.back().second = ev.second;
        } else {
            dedupTempo.push_back(ev);
        }
    }

    std::vector<TempoSegment> tempoSegs;
    tempoSegs.reserve(dedupTempo.size());
    {
        double sec = 0.0;
        uint32_t prevTick = 0;
        uint32_t prevUSPQN = 500000;
        for (const auto& [tick, uspqn] : dedupTempo) {
            sec += static_cast<double>(tick - prevTick) * prevUSPQN / (division * 1000000.0);
            tempoSegs.push_back({tick, sec, static_cast<double>(uspqn) / (division * 1000000.0)});
            outStore.tempoMap.push_back({static_cast<float>(sec), 60000000.0f / static_cast<float>(uspqn)});
            prevTick = tick;
            prevUSPQN = uspqn;
        }
    }

    const int nTracks = static_cast<int>(tracks.size());
    std::vector<TrackSoA> results(nTracks);

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < nTracks; ++i)
        results[i] = parseTrack(tracks[i].start, tracks[i].size, i, tempoSegs);
#else
    if (nTracks <= 1) {
        if (nTracks == 1)
            results[0] = parseTrack(tracks[0].start, tracks[0].size, 0, tempoSegs);
    } else {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const int workerCount = std::min(nTracks, static_cast<int>(hw));
        std::atomic<int> nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (int w = 0; w < workerCount; ++w) {
            workers.emplace_back([&]() {
                while (true) {
                    const int i = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (i >= nTracks) break;
                    results[i] = parseTrack(tracks[i].start, tracks[i].size, i, tempoSegs);
                }
            });
        }

        for (auto& th : workers) th.join();
    }
#endif

    std::vector<size_t> offsets(nTracks + 1, 0);
    for (int i = 0; i < nTracks; ++i)
        offsets[i+1] = offsets[i] + results[i].size();
    const size_t total = offsets[nTracks];

    outStore.startTimes.resize(total);
    outStore.durations.resize(total);
    outStore.keys.resize(total);
    outStore.velocities.resize(total);
    outStore.channels.resize(total);
    outStore.tracks.resize(total);

#ifdef _OPENMP
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < nTracks; ++i) {
#else
    for (int i = 0; i < nTracks; ++i) {
#endif
        const size_t dst = offsets[i];
        const auto& local = results[i];
        const size_t cnt = local.size();
        if (cnt == 0) continue;

        std::memcpy(outStore.startTimes.data() + dst, local.startTimes.data(), cnt * sizeof(float));
        std::memcpy(outStore.durations.data() + dst, local.durations.data(), cnt * sizeof(float));
        std::memcpy(outStore.keys.data() + dst, local.keys.data(), cnt * sizeof(uint8_t));
        std::memcpy(outStore.velocities.data() + dst, local.velocities.data(), cnt * sizeof(uint8_t));
        std::memcpy(outStore.channels.data() + dst, local.channels.data(), cnt * sizeof(uint8_t));
        std::memcpy(outStore.tracks.data() + dst, local.tracks.data(), cnt * sizeof(uint8_t));
    }

    sortNoteStore(outStore);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "[INFO] Parse complete: " << total << " notes (" << ms << " ms)" << std::endl;
    return true;
}

}