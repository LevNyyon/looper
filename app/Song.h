// The document: tracks, regions, takes. Positions in beats (quarter notes).
// Plain copyable data. compile() turns it into the engine's sample-based arrangement.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"

struct AppTake : alk::Take
{
    using alk::Take::Take;
    int id = 0;                 // names its file on disk
    double sampleRate = 48000;
};
using TakePtr = std::shared_ptr<AppTake>;

// What an automation region drives.
struct Target
{
    enum Type { None, Play, Record, Panic, Position, TrackPower, TrackVolume, PluginBypass, PluginParam };
    int type = None, trackId = 0, slot = 0, param = 0;   // slot 0 = instrument, 1.. = effects
};

struct Region
{
    enum Kind { Record, Play, Auto, Marker };
    int id = 0;
    Kind kind = Record;
    double start = 0, length = 4;

    // Record and Marker
    juce::String name;
    std::vector<TakePtr> takes;             // newest last, at most 8
    TakePtr current, reference, pending;    // pending: being recorded in this pass

    // Play
    int source = 0;                         // id of a record loop on the same track
    double sliceStart = 0, sliceLength = 0; // window into the source that gets looped. 0 length = to its end

    // Auto (command and control tracks)
    bool captured = false;                  // captured: driven live by the learned controller. scripted: by the region
    Target target;
    float value = 1.f;                      // command: the value sent
    int action = 0;                         // command: 0 on / set value, 1 off, 2 toggle
    int trigger = 0;                        // captured command: 0 immediately, 1 next beat, 2 next bar, 3 end of region
    float lo = 0.f, hi = 1.f;               // captured control: fader range
    std::vector<juce::Point<float>> curve;  // scripted control: x and y in 0..1
    bool revert = false;                    // on exit put the target back to its value on entry
};

struct PluginSlot
{
    juce::PluginDescription desc;
    juce::MemoryBlock state;
    bool bypass = false;
    std::shared_ptr<juce::AudioPluginInstance> instance;   // created by AudioHost
};

struct Track
{
    // Click: the metronome as a track, so it can go to any output. Talkback: an input passed straight
    // to an output, never looped. (New types go at the end, the number is saved in song files.)
    // Marker: named sections on the timeline, to jump between.
    enum Type { Audio, Instrument, Command, Control, Click, Talkback, Marker };
    int id = 0;
    Type type = Audio;
    juce::String name;
    bool power = true, solo = false, audition = false, locked = false;
    float gainDb = 0.f, pan = 0.f;
    int input = 0, output = 0;              // first device channel. outputs are stereo pairs
    bool stereoIn = false;
    double quantize = 0;                    // instrument: record quantize grid in beats, 0 = off
    int midiChannel = 0;                    // instrument: listen to this midi channel only, 0 = all
    bool drums = false;                     // instrument: played from the drum pads, notes go out on channel 10 (the GM kit)
    PluginSlot instrument;
    std::vector<PluginSlot> fx;
    std::vector<int> sources;               // automation: learned controllers (cc number, or 128 + note)
    std::vector<Region> regions;

    bool isAutomation() const { return type == Command || type == Control; }
    bool hasLoops() const { return type == Audio || type == Instrument; }
    PluginSlot* slot (int s) { return s == 0 ? &instrument : s - 1 < (int) fx.size() ? &fx[(size_t) s - 1] : nullptr; }
    Region* find (int regionId) { for (auto& r : regions) if (r.id == regionId) return &r; return nullptr; }
    const Region* find (int regionId) const { for (auto& r : regions) if (r.id == regionId) return &r; return nullptr; }
};

struct Song
{
    double bpm = 120;
    int num = 4, den = 4, bars = 32;
    int nextId = 1;
    std::vector<Track> tracks;
    juce::File folder;                      // Name.looper/ holding song.json and takes/, empty until saved

    double beatsPerBar() const { return num * 4.0 / den; }
    double lengthBeats() const { return bars * beatsPerBar(); }

    Track* track (int id) { for (auto& t : tracks) if (t.id == id) return &t; return nullptr; }
    int indexOf (int trackId) const { for (size_t i = 0; i < tracks.size(); ++i) if (tracks[i].id == trackId) return (int) i; return -1; }
    Region* region (int id, Track** owner = nullptr);
    bool fits (const Track&, const Region&) const;      // inside the song and not overlapping its neighbours

    Track& addTrack (Track::Type);
    Region& addRegion (Track&, Region::Kind, double start, double length);

    std::unique_ptr<alk::Arrangement> compile (double sampleRate, bool recording) const;
    void arm (double sampleRate, double fromBeat);      // fresh takes for the record loops still ahead
    void commit();                                      // after a record pass: keep the takes that were played into
    bool panic (double atBeat);                         // swap the record loops under the playhead to their reference take

    void save (const juce::File& folder);
    bool load (const juce::File& folder);
};
