#include "Engine.h"
#include <algorithm>
#include <climits>
#include <cstring>

namespace alk {

// Declick at slice wraps and region ends. Short fade-in so drum transients survive.
static constexpr int64_t kFadeIn = 32, kFadeOut = 128;
static constexpr float kMonStep = 1.f / 128.f;

static const Region* regionAt (const Track& t, int64_t p)
{
    for (auto& r : t.regions) if (p >= r.start && p < r.end) return &r;
    return nullptr;
}

static int64_t nextBoundary (const Track& t, int64_t p)
{
    int64_t b = INT64_MAX;
    for (auto& r : t.regions)
    {
        if (r.start > p) b = std::min (b, r.start);
        if (r.end > p) b = std::min (b, r.end);
    }
    return b;
}

static int64_t sliceLength (const Region& r)
{
    const int64_t s = std::clamp<int64_t> (r.sliceStart, 0, r.take->length);
    const int64_t len = r.sliceLen > 0 ? r.sliceLen : r.take->length - s;
    return std::min (len, r.take->length - s);
}

Engine::Engine() : state (kMaxTracks)
{
    for (auto& a : audition) a.store (false);
    for (auto& q : quantGrid) q.store (0);
    for (auto& s : state) { s.qDelta.assign (16 * 128, 0); s.held.resize (16 * 128); }
}

Engine::~Engine()
{
    collectGarbage();
    delete pending.load();
    delete cur;
}

void Engine::setArrangement (std::unique_ptr<Arrangement> a)
{
    delete pending.exchange (a.release());   // an arrangement the audio thread never picked up
    collectGarbage();
}

void Engine::collectGarbage()
{
    for (int t = rTail.load(); t != rHead.load(); t = (t + 1) % kRetire)
    {
        delete retired[t];
        rTail.store ((t + 1) % kRetire);
    }
}

void Engine::discontinuity (int64_t p)
{
    recFloor = p;   // input older than a jump belongs to where we came from, drop it
    for (auto& s : state)
    {
        s.flush = true;
        if (s.anyHeld) { for (auto& h : s.held) h.take = nullptr; s.anyHeld = false; }     // their note-offs belong to the pass we left
    }
}

template <typename Emit>
void Engine::flushNotes (TrackState& st, int64_t time, Emit&& emit)
{
    if (st.sustain.any())
    {
        for (int ch = 0; ch < 16; ++ch)
            if (st.sustain[(size_t) ch]) { const uint8_t up[3] = { (uint8_t) (0xB0 | ch), 64, 0 }; emit (time, up); }
        st.sustain.reset();
    }
    if (! st.anyActive) return;
    for (int ch = 0; ch < 16; ++ch)
        for (int note = 0; note < 128; ++note)
            if (st.active[ch][(size_t) note])
            {
                const uint8_t off[3] = { (uint8_t) (0x80 | ch), (uint8_t) note, 0 };
                emit (time, off);
            }
    for (auto& a : st.active) a.reset();
    st.anyActive = false;
}

int Engine::beginBlock (int maxN)
{
    const int head = rHead.load();
    if ((head + 1) % kRetire != rTail.load())           // room to retire the old one
        if (auto* a = pending.exchange (nullptr))
        {
            // A play loop edited or deleted under the playhead must not leave its notes hanging.
            // Tracks whose loop did not change keep ringing (Panic or an edit elsewhere must not cut them).
            const int64_t at = pos.load();
            for (size_t i = 0; i < state.size(); ++i)
            {
                const Region* was = cur != nullptr && i < cur->tracks.size() ? regionAt (cur->tracks[i], at) : nullptr;
                const Region* now = i < a->tracks.size() ? regionAt (a->tracks[i], at) : nullptr;
                const bool same = (was == nullptr && now == nullptr)
                               || (was != nullptr && now != nullptr && was->take == now->take && was->record == now->record
                                   && was->start == now->start && was->end == now->end
                                   && was->sliceStart == now->sliceStart && was->sliceLen == now->sliceLen);
                if (! same) state[i].flush = true;
            }
            if (cur != nullptr) { retired[head] = cur; rHead.store ((head + 1) % kRetire); }
            cur = a;
        }

    blockMode = cur != nullptr ? mode.load() : (int) Stopped;
    blockLatency = latency.load();
    blockMidiLatency = midiLatency.load();

    int64_t p = pos.load();
    const int64_t j = jumpTo.exchange (-1);
    if (j >= 0) { p = j; pos.store (p); discontinuity (p); }
    if (blockMode != lastMode) { discontinuity (p); lastMode = blockMode; }
    blockPos = p;

    if (blockMode == Stopped) return maxN;

    int64_t n = maxN;
    const int64_t ls = loopStart.load(), le = loopEnd.load();
    if (le > ls && p < le) n = std::min (n, le - p);
    n = std::min (n, cur->length - p);
    if (n <= 0)                                          // song end
    {
        mode.store (Stopped);
        blockMode = lastMode = Stopped;
        discontinuity (p);
        return maxN;
    }
    return (int) n;
}

void Engine::endBlock (int n)
{
    if (blockMode == Stopped) return;
    int64_t p = blockPos + n;
    const int64_t ls = loopStart.load(), le = loopEnd.load();
    if (le > ls && p == le) { p = ls; discontinuity (p); }
    pos.store (p);
}

void Engine::processAudio (int ti, const float* inL, const float* inR, float* outL, float* outR, int n)
{
    auto& st = state[(size_t) ti];
    const Track* t = cur != nullptr && ti < (int) cur->tracks.size() ? &cur->tracks[(size_t) ti] : nullptr;
    const bool rolling = blockMode != Stopped && t != nullptr;
    const bool aud = audition[ti].load (std::memory_order_relaxed);

    // 1. record. The sample arriving now was played against what the performer heard
    //    one round trip ago, so it belongs `latency` samples back.
    // ponytail: the last `latency` samples of a record loop are lost when the transport
    // stops or wraps right at its end. Keep a tail clock if that is ever audible.
    if (blockMode == Recording && t != nullptr)
    {
        const int64_t w0 = blockPos - blockLatency;
        for (auto& r : t->regions)
        {
            if (! r.record || r.take == nullptr || r.take->L.empty()) continue;
            const int64_t a = std::max ({ w0, r.start, recFloor });
            const int64_t b = std::min ({ w0 + n, r.end, r.start + r.take->length });
            if (a >= b) continue;
            r.take->touched.store (true, std::memory_order_relaxed);
            std::copy (inL + (a - w0), inL + (b - w0), r.take->L.data() + (a - r.start));
            std::copy (inR + (a - w0), inR + (b - w0), r.take->R.data() + (a - r.start));
        }
    }

    // 2. output = monitored input + loop playback, in runs between region boundaries
    for (int i = 0; i < n;)
    {
        const int64_t p = blockPos + i;
        const Region* r = rolling ? regionAt (*t, p) : nullptr;
        const int m = rolling ? (int) std::min<int64_t> (n - i, nextBoundary (*t, p) - p) : n - i;

        // while recording you hear yourself only inside record loops
        const float target = blockMode == Recording ? (r != nullptr && r->record ? 1.f : 0.f) : (aud ? 1.f : 0.f);
        for (int k = i; k < i + m; ++k)
        {
            st.mon += std::clamp (target - st.mon, -kMonStep, kMonStep);
            outL[k] = inL[k] * st.mon;
            outR[k] = inR[k] * st.mon;
        }

        if (r != nullptr && ! r->record && r->take != nullptr && ! r->take->L.empty())
        {
            const int64_t len = sliceLength (*r);
            if (len > 0)
            {
                const int64_t s = std::clamp<int64_t> (r->sliceStart, 0, r->take->length);
                const int64_t fi = std::min (kFadeIn, len / 4), fo = std::min (kFadeOut, len / 4);
                const float* srcL = r->take->L.data() + s;
                const float* srcR = r->take->R.data() + s;
                for (int k = 0; k < m; ++k)
                {
                    const int64_t off = (p + k - r->start) % len;
                    const int64_t left = std::min (len - off, r->end - (p + k));   // samples to wrap or region end
                    float g = 1.f;
                    if (off < fi) g = (float) (off + 1) / (float) (fi + 1);
                    if (left <= fo) g *= (float) left / (float) (fo + 1);
                    outL[i + k] += srcL[off] * g;
                    outR[i + k] += srcR[off] * g;
                }
            }
        }
        i += m;
    }
}

int Engine::processMidi (int ti, const MidiEvent* in, int numIn, MidiEvent* out, int outCap, int n)
{
    int num = 0;
    auto emit = [&] (int64_t time, const uint8_t* d)
    {
        if (num < outCap) { out[num].time = time; std::memcpy (out[num].data, d, 3); ++num; }
    };

    auto& st = state[(size_t) ti];
    const Track* t = cur != nullptr && ti < (int) cur->tracks.size() ? &cur->tracks[(size_t) ti] : nullptr;
    const bool rolling = blockMode != Stopped && t != nullptr;
    const bool aud = audition[ti].load (std::memory_order_relaxed);

    if (st.flush) { flushNotes (st, 0, emit); st.flush = false; }

    // 1. live input. A closed gate only blocks note-ons, so held notes can still end.
    for (int e = 0; e < numIn; ++e)
    {
        const Region* r = rolling ? regionAt (*t, blockPos + in[e].time) : nullptr;
        const bool open = blockMode == Recording ? (r != nullptr && r->record) : aud;
        const bool isOn = (in[e].data[0] & 0xF0) == 0x90 && in[e].data[2] > 0;
        if (open || ! isOn) emit (in[e].time, in[e].data);
    }

    // 2. record
    if (blockMode == Recording && t != nullptr)
    {
        const int64_t grid = quantGrid[ti].load (std::memory_order_relaxed);
        const int64_t w0 = blockPos - blockMidiLatency;

        // a new pass over a record loop replaces what was in it
        for (auto& r : t->regions)
            if (r.record && r.take != nullptr && ! r.take->midi.empty())
            {
                // (after a jump there is no early-note window before the landing point)
                const int64_t resetAt = std::max (r.start - grid / 2, std::min (recFloor, r.start));
                if (resetAt >= w0 && resetAt < w0 + n) r.take->midiCount.store (0);
                if (r.start < w0 + n && r.end > w0) r.take->touched.store (true, std::memory_order_relaxed);
            }

        for (int e = 0; e < numIn; ++e)
        {
            const int64_t w = w0 + in[e].time;
            if (w < recFloor) continue;
            const int status = in[e].data[0] & 0xF0, slot = (in[e].data[0] & 0x0F) * 128 + (in[e].data[1] & 0x7F);
            const bool isOn = status == 0x90 && in[e].data[2] > 0;
            const bool isOff = status == 0x80 || (status == 0x90 && in[e].data[2] == 0);

            // with quantize on, a note played up to half a grid early still lands on the loop's first beat
            const Region* r = regionAt (*t, w);
            if ((r == nullptr || ! r->record) && grid > 0 && isOn) r = regionAt (*t, w + grid / 2);
            Take* take = r != nullptr && r->record && r->take != nullptr && ! r->take->midi.empty() ? r->take.get() : nullptr;

            int64_t q = 0;
            auto& held = st.held[(size_t) slot];
            if (isOff && held.take != nullptr && held.take != take)
            {
                // The key is released after its record loop ended. Wrap the note-off into that loop,
                // so the note rings across the repeat instead of being cut at the loop end.
                take = nullptr;
                for (auto& o : t->regions) if (o.record && o.take.get() == held.take) take = o.take.get();
                q = (w - held.start + (grid > 0 ? st.qDelta[(size_t) slot] : 0) + held.len) % held.len;
                held.take = nullptr;
            }
            else if (take != nullptr)
            {
                const int64_t len = r->end - r->start;
                q = w - r->start;
                if (grid > 0 && isOn)
                {
                    const int64_t snapped = ((q + grid / 2) / grid) * grid;
                    st.qDelta[(size_t) slot] = (int32_t) ((snapped >= len ? 0 : snapped) - q);
                    q = snapped >= len ? 0 : snapped;      // snapping to the loop end means its first beat
                }
                else if (grid > 0 && isOff)
                    q = std::clamp<int64_t> (q + st.qDelta[(size_t) slot], 0, len - 1);
                if (isOn) { held = { take, r->start, len }; st.anyHeld = true; }
                else if (isOff) held.take = nullptr;
            }
            if (take == nullptr || q < 0) continue;

            auto& tk = *take;
            const int c = tk.midiCount.load();
            if (c >= Take::kMaxMidi) continue;
            int j = c;
            for (; j > 0 && tk.midi[(size_t) j - 1].time > q; --j) tk.midi[(size_t) j] = tk.midi[(size_t) j - 1];
            tk.midi[(size_t) j].time = q;
            std::memcpy (tk.midi[(size_t) j].data, in[e].data, 3);
            tk.midiCount.store (c + 1);
        }
    }

    // 3. loop playback
    for (int i = 0; i < n && rolling;)
    {
        const int64_t p = blockPos + i;
        const Region* r = regionAt (*t, p);
        const int m = (int) std::min<int64_t> (n - i, nextBoundary (*t, p) - p);

        if (r != nullptr && ! r->record && r->take != nullptr && ! r->take->midi.empty())
        {
            const int64_t len = sliceLength (*r);
            const int64_t s = std::clamp<int64_t> (r->sliceStart, 0, r->take->length);
            const MidiEvent* ev = r->take->midi.data();
            const int count = r->take->midiCount.load();
            const bool whole = s == 0 && len == r->take->length;   // the full loop: held notes ring across the repeat

            for (int done = 0; done < m && len > 0;)
            {
                const int64_t off = (p + done - r->start) % len;
                const int c = (int) std::min<int64_t> (m - done, len - off);
                const int64_t from = s + off;

                const MidiEvent* e = std::lower_bound (ev, ev + count, from,
                                                       [] (const MidiEvent& a, int64_t v) { return a.time < v; });
                for (; e < ev + count && e->time < from + c; ++e)
                {
                    const int64_t at = i + done + (e->time - from);
                    const int status = e->data[0] & 0xF0, ch = e->data[0] & 0x0F, note = e->data[1] & 0x7F;
                    if (status == 0x90 && e->data[2] > 0)
                    {
                        if (st.active[ch][(size_t) note])      // still ringing from the last repeat: end it, then retrigger
                        {
                            const uint8_t offMsg[3] = { (uint8_t) (0x80 | ch), (uint8_t) note, 0 };
                            emit (at, offMsg);
                        }
                        st.active[ch].set ((size_t) note);
                        st.anyActive = true;
                    }
                    else if (status == 0x80 || status == 0x90) st.active[ch].reset ((size_t) note);
                    else if (status == 0xB0 && note == 64) st.sustain[(size_t) ch] = e->data[2] >= 64;
                    emit (at, e->data);
                }

                done += c;
                // A slice cuts its notes at every repeat. The whole loop only cuts them when the play loop ends.
                if (p + done == r->end || (off + c == len && ! whole)) flushNotes (st, i + done - 1, emit);
            }
        }
        i += m;
    }
    return num;
}

} // namespace alk
