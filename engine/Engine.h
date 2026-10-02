// The looper core: a script of record and play regions driving loop buffers.
// std only. No devices, no UI, no plugins. All positions are in samples.
#pragma once
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

namespace alk {

// time: samples from block start (live events) or from take start (recorded events)
struct MidiEvent { int64_t time = 0; uint8_t data[3] = { 0, 0, 0 }; };

// One performance of a record loop. Allocated off the audio thread, written on it.
struct Take
{
    static constexpr int kMaxMidi = 8192;

    Take (int64_t len, bool isMidi) : length (len)
    {
        if (isMidi) midi.resize (kMaxMidi);
        else { L.assign ((size_t) len, 0.f); R.assign ((size_t) len, 0.f); }
    }

    int64_t length;
    std::vector<float> L, R;            // audio takes
    std::vector<MidiEvent> midi;        // midi takes: sorted by time, midiCount valid
    std::atomic<int> midiCount { 0 };
    std::atomic<bool> touched { false };
};

struct Region
{
    int64_t start = 0, end = 0;         // song position, [start, end)
    bool record = false;                // record: capture input into take and monitor it. else: play take
    std::shared_ptr<Take> take;         // null plays silence
    int64_t sliceStart = 0;             // play only: window into the take that gets looped
    int64_t sliceLen = 0;               // 0 = to the end of the take
};

struct Track { std::vector<Region> regions; };   // sorted by start, never overlapping

struct Arrangement { std::vector<Track> tracks; int64_t length = 0; };

class Engine
{
public:
    enum Mode { Stopped, Playing, Recording };
    static constexpr int kMaxTracks = 64;

    Engine();
    ~Engine();

    // --- any thread
    std::atomic<int> mode { Stopped };
    std::atomic<int64_t> pos { 0 };                         // read only outside the audio thread, use jumpTo to move
    std::atomic<int64_t> jumpTo { -1 };
    std::atomic<int64_t> loopStart { -1 }, loopEnd { -1 };  // global loop, active when loopEnd > loopStart
    std::atomic<int64_t> latency { 0 };                     // audio record shift: round trip (in + out)
    std::atomic<int64_t> midiLatency { 0 };                 // midi record shift: output only
    std::atomic<bool> audition[kMaxTracks];                 // monitor input when not recording
    std::atomic<int64_t> quantGrid[kMaxTracks];             // midi record quantize, 0 = off

    // --- UI thread
    void setArrangement (std::unique_ptr<Arrangement>);     // lock-free handoff to the audio thread
    void collectGarbage();                                  // frees arrangements the audio thread is done with

    // --- audio thread. Per device callback:
    //   while (done < numSamples) { n = beginBlock (numSamples - done); process each track for n; endBlock (n); done += n; }
    int beginBlock (int maxN);   // returns how many samples can run before the next loop wrap or song end
    void processAudio (int track, const float* inL, const float* inR, float* outL, float* outR, int n);
    int processMidi (int track, const MidiEvent* in, int numIn, MidiEvent* out, int outCap, int n);
    void endBlock (int n);
    int64_t blockStart() const { return blockPos; }             // valid between beginBlock and endBlock
    bool blockRolling() const { return blockMode != Stopped; }
    bool blockRecording() const { return blockMode == Recording; }

private:
    struct TrackState
    {
        float mon = 0.f;                       // smoothed monitor gain
        std::bitset<128> active[16];           // notes sounding from loop playback
        std::bitset<16> sustain;               // channels where loop playback left the sustain pedal down
        bool anyActive = false, anyHeld = false, flush = false;
        std::vector<int32_t> qDelta;           // per note: how far quantize moved its note-on
        struct Held { const Take* take = nullptr; int64_t start = 0, len = 0; };
        std::vector<Held> held;                // per note: the record loop its note-on went into
    };

    void discontinuity (int64_t p);
    template <typename Emit> void flushNotes (TrackState&, int64_t time, Emit&&);

    std::atomic<Arrangement*> pending { nullptr };
    Arrangement* cur = nullptr;                // audio thread only

    static constexpr int kRetire = 64;         // audio thread pushes, UI thread pops
    Arrangement* retired[kRetire] {};
    std::atomic<int> rHead { 0 }, rTail { 0 };

    std::vector<TrackState> state;
    int blockMode = Stopped, lastMode = Stopped;
    int64_t blockPos = 0, blockLatency = 0, blockMidiLatency = 0, recFloor = 0;
};

} // namespace alk
