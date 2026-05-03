#include "MidiParser.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <iostream>
#include <vector>
#include <deque>
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

static inline float TickToSecAtCursor(
    const uint32_t tick,
    const std::vector<TempoSegment>& segs,
    size_t& cursor)
{
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

    std::vector<float> ccTimes;
    std::vector<uint8_t> ccChannels;
    std::vector<uint8_t> ccNumbers;
    std::vector<uint8_t> ccValues;

    std::vector<float> pcTimes;
    std::vector<uint8_t> pcChannels;
    std::vector<uint8_t> pcPrograms;

    std::vector<float> pbTimes;
    std::vector<uint8_t> pbChannels;
    std::vector<uint8_t> pbLSB;
    std::vector<uint8_t> pbMSB;

    void reserve(const size_t n) {
        startTimes.reserve(n);
        durations.reserve(n);
        keys.reserve(n);
        velocities.reserve(n);
        channels.reserve(n);
        tracks.reserve(n);

        ccTimes.reserve(n / 8 + 16);
        ccChannels.reserve(n / 8 + 16);
        ccNumbers.reserve(n / 8 + 16);
        ccValues.reserve(n / 8 + 16);

        pcTimes.reserve(n / 16 + 8);
        pcChannels.reserve(n / 16 + 8);
        pcPrograms.reserve(n / 16 + 8);

        pbTimes.reserve(n / 16 + 8);
        pbChannels.reserve(n / 16 + 8);
        pbLSB.reserve(n / 16 + 8);
        pbMSB.reserve(n / 16 + 8);
    }

    [[nodiscard]] size_t size() const { return startTimes.size(); }
    [[nodiscard]] size_t ccSize() const { return ccTimes.size(); }
    [[nodiscard]] size_t pcSize() const { return pcTimes.size(); }
    [[nodiscard]] size_t pbSize() const { return pbTimes.size(); }
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
        if (!(st & 0x80)) {
            st = runStat;
            tp--;
        } else if ((st & 0xF0) != 0xF0) {
            runStat = st;
        }

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

    size_t tempoCursor = 0;

    struct Pending { float start; uint8_t vel; };
    std::deque<Pending> pending[16][128];

    while (tp < tend) {
        curTick += ReadVarLen(tp);
        if (tp >= tend) break;

        uint8_t st = *tp++;
        if (!(st & 0x80)) {
            st = runStat;
            tp--;
        } else if ((st & 0xF0) != 0xF0) {
            runStat = st;
        }

        const uint8_t type = st & 0xF0;
        const uint8_t ch   = st & 0x0F;

        if (type == 0x90 || type == 0x80) {
            const uint8_t key = *tp++ & 0x7F;
            const uint8_t vel = *tp++ & 0x7F;
            const float   sec = TickToSecAtCursor(curTick, tempoSegs, tempoCursor);
            if (type == 0x90 && vel > 0) {
                pending[ch][key].push_back({sec, vel});
            } else {
                if (!pending[ch][key].empty()) {
                    const Pending noteOn = pending[ch][key].front();
                    pending[ch][key].pop_front();
                    float dur = sec - noteOn.start;
                    if (dur <= 0.0f) dur = 0.01f;
                    local.startTimes.push_back(noteOn.start);
                    local.durations.push_back(dur);
                    local.keys.push_back(key);
                    local.velocities.push_back(noteOn.vel);
                    local.channels.push_back(ch);
                    local.tracks.push_back(static_cast<uint8_t>(trackIndex));
                }
            }
        } else if (type == 0xB0) {
            const uint8_t ccNum = *tp++ & 0x7F;
            const uint8_t ccVal = *tp++ & 0x7F;
            const float sec = TickToSecAtCursor(curTick, tempoSegs, tempoCursor);
            local.ccTimes.push_back(sec);
            local.ccChannels.push_back(ch);
            local.ccNumbers.push_back(ccNum);
            local.ccValues.push_back(ccVal);
        } else if (type == 0xC0) {
            const uint8_t program = *tp++ & 0x7F;
            const float sec = TickToSecAtCursor(curTick, tempoSegs, tempoCursor);
            local.pcTimes.push_back(sec);
            local.pcChannels.push_back(ch);
            local.pcPrograms.push_back(program);
        } else if (type == 0xE0) {
            const uint8_t lsb = *tp++ & 0x7F;
            const uint8_t msb = *tp++ & 0x7F;
            const float sec = TickToSecAtCursor(curTick, tempoSegs, tempoCursor);
            local.pbTimes.push_back(sec);
            local.pbChannels.push_back(ch);
            local.pbLSB.push_back(lsb);
            local.pbMSB.push_back(msb);
        } else if (st == 0xFF) {
            tp++;
            tp += ReadVarLen(tp);
        } else if (type == 0xD0) {
            tp++;
        } else if (type == 0xA0) {
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
    outStore.ccTimes.clear();
    outStore.ccChannels.clear();
    outStore.ccNumbers.clear();
    outStore.ccValues.clear();
    outStore.pcTimes.clear();
    outStore.pcChannels.clear();
    outStore.pcPrograms.clear();
    outStore.pbTimes.clear();
    outStore.pbChannels.clear();
    outStore.pbLSB.clear();
    outStore.pbMSB.clear();

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
    const uint32_t hdrSize   = Read32BE(p);
    const uint16_t formatType = Read16BE(p);
    const uint16_t numTracks  = Read16BE(p);
    const uint16_t division   = Read16BE(p);
    p = mapped->data + 8 + hdrSize;

    const bool isTPQN = (division & 0x8000u) == 0;

    std::cout << "[INFO] Format:" << formatType << " Division:" << division
              << " (" << (isTPQN ? "TPQN" : "SMPTE") << ")"
              << " Tracks:" << numTracks << std::endl;

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

    std::vector<TempoSegment> tempoSegs;

    if (isTPQN) {
        std::vector<std::pair<uint32_t, uint32_t>> tempoEvents;
        tempoEvents.reserve(256);
        tempoEvents.emplace_back(0u, 500000u);

        if (formatType == 1 && !tracks.empty()) {
            const size_t before = tempoEvents.size();
            CollectTempoEventsFromTrack(tracks[0].start, tracks[0].size, tempoEvents);
            if (tempoEvents.size() == before) {
                for (const auto& [start, size] : tracks)
                    CollectTempoEventsFromTrack(start, size, tempoEvents);
            }
        } else {
            for (const auto& [start, size] : tracks)
                CollectTempoEventsFromTrack(start, size, tempoEvents);
        }

        std::stable_sort(tempoEvents.begin(), tempoEvents.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<std::pair<uint32_t, uint32_t>> dedupTempo;
        dedupTempo.reserve(tempoEvents.size());
        for (const auto& ev : tempoEvents) {
            if (!dedupTempo.empty() && dedupTempo.back().first == ev.first) {
                dedupTempo.back().second = ev.second;
            } else {
                dedupTempo.push_back(ev);
            }
        }

        tempoSegs.reserve(dedupTempo.size());
        double sec = 0.0;
        uint32_t prevTick = 0;
        uint32_t prevUSPQN = 500000;
        for (const auto& [tick, uspqn] : dedupTempo) {
            sec += static_cast<double>(tick - prevTick) * prevUSPQN /
                   (static_cast<double>(division) * 1000000.0);
            tempoSegs.push_back({tick, sec,
                static_cast<double>(uspqn) / (static_cast<double>(division) * 1000000.0)});
            outStore.tempoMap.push_back({static_cast<float>(sec), 60000000.0f / static_cast<float>(uspqn)});
            prevTick = tick;
            prevUSPQN = uspqn;
        }
    } else {
        const int8_t smpteCode = static_cast<int8_t>((division >> 8) & 0xFF);
        const uint8_t ticksPerFrame = static_cast<uint8_t>(division & 0xFF);

        double fps = 30.0;
        switch (-smpteCode) {
            case 24: fps = 24.0; break;
            case 25: fps = 25.0; break;
            case 29: fps = 29.97; break;
            case 30: fps = 30.0; break;
            default: fps = 30.0; break;
        }

        const double ticksToSec = (ticksPerFrame == 0)
            ? (1.0 / (30.0 * 80.0))
            : (1.0 / (fps * static_cast<double>(ticksPerFrame)));

        tempoSegs.push_back({0u, 0.0, ticksToSec});
        outStore.tempoMap.push_back({0.0f, 120.0f});

        std::cout << "[INFO] SMPTE timing: fps=" << fps
                  << " ticks/frame=" << static_cast<int>(ticksPerFrame)
                  << " sec/tick=" << ticksToSec << std::endl;
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

    std::vector<size_t> ccOffsets(nTracks + 1, 0);
    for (int i = 0; i < nTracks; ++i)
        ccOffsets[i + 1] = ccOffsets[i] + results[i].ccSize();
    const size_t totalCC = ccOffsets[nTracks];

    std::vector<size_t> pcOffsets(nTracks + 1, 0);
    for (int i = 0; i < nTracks; ++i)
        pcOffsets[i + 1] = pcOffsets[i] + results[i].pcSize();
    const size_t totalPC = pcOffsets[nTracks];

    std::vector<size_t> pbOffsets(nTracks + 1, 0);
    for (int i = 0; i < nTracks; ++i)
        pbOffsets[i + 1] = pbOffsets[i] + results[i].pbSize();
    const size_t totalPB = pbOffsets[nTracks];

    outStore.startTimes.resize(total);
    outStore.durations.resize(total);
    outStore.keys.resize(total);
    outStore.velocities.resize(total);
    outStore.channels.resize(total);
    outStore.tracks.resize(total);
    outStore.ccTimes.resize(totalCC);
    outStore.ccChannels.resize(totalCC);
    outStore.ccNumbers.resize(totalCC);
    outStore.ccValues.resize(totalCC);
    outStore.pcTimes.resize(totalPC);
    outStore.pcChannels.resize(totalPC);
    outStore.pcPrograms.resize(totalPC);
    outStore.pbTimes.resize(totalPB);
    outStore.pbChannels.resize(totalPB);
    outStore.pbLSB.resize(totalPB);
    outStore.pbMSB.resize(totalPB);

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

        const size_t ccDst = ccOffsets[i];
        const size_t ccCnt = local.ccSize();
        if (ccCnt > 0) {
            std::memcpy(outStore.ccTimes.data() + ccDst, local.ccTimes.data(), ccCnt * sizeof(float));
            std::memcpy(outStore.ccChannels.data() + ccDst, local.ccChannels.data(), ccCnt * sizeof(uint8_t));
            std::memcpy(outStore.ccNumbers.data() + ccDst, local.ccNumbers.data(), ccCnt * sizeof(uint8_t));
            std::memcpy(outStore.ccValues.data() + ccDst, local.ccValues.data(), ccCnt * sizeof(uint8_t));
        }

        const size_t pcDst = pcOffsets[i];
        const size_t pcCnt = local.pcSize();
        if (pcCnt > 0) {
            std::memcpy(outStore.pcTimes.data() + pcDst, local.pcTimes.data(), pcCnt * sizeof(float));
            std::memcpy(outStore.pcChannels.data() + pcDst, local.pcChannels.data(), pcCnt * sizeof(uint8_t));
            std::memcpy(outStore.pcPrograms.data() + pcDst, local.pcPrograms.data(), pcCnt * sizeof(uint8_t));
        }

        const size_t pbDst = pbOffsets[i];
        const size_t pbCnt = local.pbSize();
        if (pbCnt > 0) {
            std::memcpy(outStore.pbTimes.data() + pbDst, local.pbTimes.data(), pbCnt * sizeof(float));
            std::memcpy(outStore.pbChannels.data() + pbDst, local.pbChannels.data(), pbCnt * sizeof(uint8_t));
            std::memcpy(outStore.pbLSB.data() + pbDst, local.pbLSB.data(), pbCnt * sizeof(uint8_t));
            std::memcpy(outStore.pbMSB.data() + pbDst, local.pbMSB.data(), pbCnt * sizeof(uint8_t));
        }
    }

    if (!outStore.ccTimes.empty()) {
        std::vector<size_t> ccIdx(outStore.ccTimes.size());
        std::iota(ccIdx.begin(), ccIdx.end(), 0);
        std::stable_sort(ccIdx.begin(), ccIdx.end(),
                  [&](const size_t a, const size_t b) { return outStore.ccTimes[a] < outStore.ccTimes[b]; });

        std::vector<float> ccTimes(outStore.ccTimes.size());
        std::vector<uint8_t> ccCh(outStore.ccChannels.size());
        std::vector<uint8_t> ccNum(outStore.ccNumbers.size());
        std::vector<uint8_t> ccVal(outStore.ccValues.size());
        for (size_t i = 0; i < ccIdx.size(); ++i) {
            const size_t src = ccIdx[i];
            ccTimes[i] = outStore.ccTimes[src];
            ccCh[i] = outStore.ccChannels[src];
            ccNum[i] = outStore.ccNumbers[src];
            ccVal[i] = outStore.ccValues[src];
        }
        outStore.ccTimes = std::move(ccTimes);
        outStore.ccChannels = std::move(ccCh);
        outStore.ccNumbers = std::move(ccNum);
        outStore.ccValues = std::move(ccVal);
    }

    if (!outStore.pcTimes.empty()) {
        std::vector<size_t> pcIdx(outStore.pcTimes.size());
        std::iota(pcIdx.begin(), pcIdx.end(), 0);
        std::stable_sort(pcIdx.begin(), pcIdx.end(),
                  [&](const size_t a, const size_t b) { return outStore.pcTimes[a] < outStore.pcTimes[b]; });

        std::vector<float> pcTimes(outStore.pcTimes.size());
        std::vector<uint8_t> pcCh(outStore.pcChannels.size());
        std::vector<uint8_t> pcProg(outStore.pcPrograms.size());
        for (size_t i = 0; i < pcIdx.size(); ++i) {
            const size_t src = pcIdx[i];
            pcTimes[i] = outStore.pcTimes[src];
            pcCh[i] = outStore.pcChannels[src];
            pcProg[i] = outStore.pcPrograms[src];
        }
        outStore.pcTimes = std::move(pcTimes);
        outStore.pcChannels = std::move(pcCh);
        outStore.pcPrograms = std::move(pcProg);
    }

    if (!outStore.pbTimes.empty()) {
        std::vector<size_t> pbIdx(outStore.pbTimes.size());
        std::iota(pbIdx.begin(), pbIdx.end(), 0);
        std::stable_sort(pbIdx.begin(), pbIdx.end(),
                  [&](const size_t a, const size_t b) { return outStore.pbTimes[a] < outStore.pbTimes[b]; });

        std::vector<float> pbTimes(outStore.pbTimes.size());
        std::vector<uint8_t> pbCh(outStore.pbChannels.size());
        std::vector<uint8_t> pbLSB(outStore.pbLSB.size());
        std::vector<uint8_t> pbMSB(outStore.pbMSB.size());
        for (size_t i = 0; i < pbIdx.size(); ++i) {
            const size_t src = pbIdx[i];
            pbTimes[i] = outStore.pbTimes[src];
            pbCh[i] = outStore.pbChannels[src];
            pbLSB[i] = outStore.pbLSB[src];
            pbMSB[i] = outStore.pbMSB[src];
        }
        outStore.pbTimes = std::move(pbTimes);
        outStore.pbChannels = std::move(pbCh);
        outStore.pbLSB = std::move(pbLSB);
        outStore.pbMSB = std::move(pbMSB);
    }

    sortNoteStore(outStore);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "[INFO] Parse complete: " << total << " notes (" << ms << " ms)" << std::endl;
    return true;
}

}
