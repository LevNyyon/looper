// The one engine check. Record 4 bars, then loop slices of it, offline.
//   clang++ -std=c++17 -O1 engine/Engine.cpp engine/selfcheck.cpp -o build/selfcheck && build/selfcheck
#include "Engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace alk;

#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); std::exit (1); } } while (0)

static const int64_t bar = 1024, songLen = 13 * bar;

static Region rec (int64_t a, int64_t b, std::shared_ptr<Take> t) { Region r; r.start = a; r.end = b; r.record = true; r.take = t; return r; }
static Region play (int64_t a, int64_t b, std::shared_ptr<Take> t, int64_t s, int64_t len)
{
    Region r; r.start = a; r.end = b; r.take = t; r.sliceStart = s; r.sliceLen = len; return r;
}

int main()
{
    for (int64_t latency : { 0, 7 })
    {
        auto audio = std::make_shared<Take> (4 * bar, false);
        auto midi = std::make_shared<Take> (4 * bar, true);

        auto arr = std::make_unique<Arrangement>();
        arr->length = songLen;
        arr->tracks.resize (2);
        arr->tracks[0].regions = {
            rec (0, 4 * bar, audio),
            play (4 * bar, 8 * bar, audio, 0, 2 * bar),               // bars 1 to 2, twice
            play (8 * bar, 10 * bar, audio, 1 * bar, 2 * bar),        // bars 2 to 3
            play (10 * bar, 12 * bar, audio, 2 * bar, 2 * bar),       // bars 3 to 4
            play (12 * bar, 13 * bar, audio, bar / 2, bar / 16),      // a 16th, stuttered for a bar
        };
        arr->tracks[1].regions = { rec (0, 4 * bar, midi), play (4 * bar, 8 * bar, midi, 1 * bar, 1 * bar) };   // bar 2, four times

        Engine e;
        e.setArrangement (std::move (arr));
        e.latency = latency;
        e.mode = Engine::Recording;

        // what the performer plays at song position i is perf (i); it reaches the input `latency` samples late
        auto perf = [] (int64_t i) { return i < 0 ? 0.f : (float) (i + 1); };
        struct Live { int64_t at; uint8_t d[3]; };
        const Live live[] = { { 1100, { 0x90, 60, 100 } }, { 1500, { 0x80, 60, 0 } },
                              { 2000, { 0x90, 62, 100 } }, { 2500, { 0x80, 62, 0 } },     // 62 is held past the slice end
                              { 4000, { 0x90, 64, 100 } }, { 4300, { 0x80, 64, 0 } } };   // 64 is released after the record loop ended

        std::vector<float> outL ((size_t) songLen), outR ((size_t) songLen);
        std::vector<MidiEvent> midiOut;

        for (int64_t done = 0; done < songLen;)
        {
            const int n = e.beginBlock (100);
            if (e.mode != Engine::Recording) break;
            float inL[100], inR[100];
            for (int k = 0; k < n; ++k) { inL[k] = perf (done + k - latency); inR[k] = -inL[k]; }
            e.processAudio (0, inL, inR, outL.data() + done, outR.data() + done, n);

            MidiEvent in[8], out[64];
            int numIn = 0;
            for (auto& l : live) if (l.at >= done && l.at < done + n) { in[numIn].time = l.at - done; for (int b = 0; b < 3; ++b) in[numIn].data[b] = l.d[b]; ++numIn; }
            const int numOut = e.processMidi (1, in, numIn, out, 64, n);
            for (int k = 0; k < numOut; ++k) { out[k].time += done; midiOut.push_back (out[k]); }

            e.endBlock (n);
            done += n;
        }
        CHECK (e.pos == songLen);

        // the take holds the performance, lined up with the grid whatever the latency
        for (int64_t k = 0; k < 4 * bar; ++k) CHECK (audio->L[(size_t) k] == perf (k) && audio->R[(size_t) k] == -perf (k));

        // while recording you hear your input
        for (int64_t p = 200; p < 4 * bar; ++p) CHECK (outL[(size_t) p] == perf (p - latency));

        // each play loop plays its slice, looped, sample exact away from the declick fades
        struct Exp { int64_t start, end, s, len; };
        for (Exp x : { Exp { 4 * bar, 8 * bar, 0, 2 * bar }, Exp { 8 * bar, 10 * bar, bar, 2 * bar },
                       Exp { 10 * bar, 12 * bar, 2 * bar, 2 * bar }, Exp { 12 * bar, 13 * bar, bar / 2, bar / 16 } })
        {
            const int64_t fi = x.len >= 128 ? 32 : x.len / 4, fo = x.len >= 512 ? 128 : x.len / 4;
            for (int64_t p = x.start + 200; p < x.end; ++p)      // +200: past the monitor fade-out
            {
                const int64_t off = (p - x.start) % x.len;
                if (off >= fi && x.len - off > fo && x.end - p > fo)
                    CHECK (outL[(size_t) p] == perf (x.s + off) && outR[(size_t) p] == -perf (x.s + off));
            }
        }
        // and the fades are really there: first and last sample of a slice repeat
        CHECK (outL[(size_t) (6 * bar)] == perf (0) * (1.f / 33.f));
        CHECK (outL[(size_t) (6 * bar - 1)] == perf (2 * bar - 1) * (1.f / 129.f));

        // a note released after its record loop ended gets its note-off wrapped into the loop (4300 - 4096 = 204)
        bool wrapped = false;
        for (int k = 0; k < midi->midiCount; ++k) wrapped |= midi->midi[(size_t) k].time == 204 && midi->midi[(size_t) k].data[0] == 0x80 && midi->midi[(size_t) k].data[1] == 64;
        CHECK (wrapped);
        midiOut.erase (std::remove_if (midiOut.begin(), midiOut.end(), [] (const MidiEvent& m) { return m.data[1] == 64; }), midiOut.end());

        // midi: 4 live events pass through, then bar 2 repeats four times with the held note cut at each wrap
        CHECK (midiOut.size() == 4 + 4 * 4);
        for (int rep = 0; rep < 4; ++rep)
        {
            const MidiEvent* m = &midiOut[(size_t) (4 + rep * 4)];
            const int64_t base = 4 * bar + rep * bar;
            CHECK (m[0].time == base + 76 && m[0].data[0] == 0x90 && m[0].data[1] == 60);
            CHECK (m[1].time == base + 476 && m[1].data[0] == 0x80 && m[1].data[1] == 60);
            CHECK (m[2].time == base + 976 && m[2].data[0] == 0x90 && m[2].data[1] == 62);
            CHECK (m[3].time == base + 1023 && m[3].data[0] == 0x80 && m[3].data[1] == 62);
        }
    }
    std::printf ("engine selfcheck ok\n");
    return 0;
}
