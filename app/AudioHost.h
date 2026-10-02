// Owns the song, the engine, the audio device and the plugins. Everything the UI calls lives here.
#pragma once
#include "Song.h"

struct AudioHost : juce::AudioIODeviceCallback, juce::ChangeListener, juce::Timer
{
    AudioHost();
    ~AudioHost() override;

    Song song;
    alk::Engine engine;
    juce::AudioDeviceManager dm;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    juce::ApplicationProperties props;

    double sr = 48000;
    int blockSize = 512;
    std::atomic<int> keyTrack { -1 };       // index of the track the on-screen keys and pads play
    bool dirty = false;
    double loopA = 0, loopB = 0;            // global loop in beats, on when loopB > loopA
    bool loopRange = false;                 // Prefs: the loop range in the ruler is available at all
    void setLoopRange (bool available);
    std::function<void()> onChange;         // transport state or the whole song changed

    void openDevices (bool microphoneAllowed);
    juce::String deviceError;               // why there is no audio device, shown in the track panel
    juce::StringArray channelNames (bool input) const;

    // call after editing the song
    void regionsChanged();                  // regions, tempo, takes
    void paramsChanged();                   // gain, pan, power, solo, audition, bypass, quantize
    void structureChanged();                // tracks, plugins, io
    void pushUndo();                        // before a region edit
    void undo();
    void redo();

    bool rolling() const { return engine.mode != alk::Engine::Stopped; }
    bool recording() const { return engine.mode == alk::Engine::Recording; }
    double spb() const { return sr * 60.0 / song.bpm; }
    double posBeats() const { return (double) engine.pos / spb(); }
    void play();
    void record();
    void stop();
    void jump (double beat);
    void panic();
    void setLoop (double a, double b);

    void setPlugin (int trackId, int slot, const juce::PluginDescription&);   // slot 0 = instrument, past the end = append
    void removePlugin (int trackId, int slot);
    void showEditor (int trackId, int slot);

    void ensureClickTrack();
    void newSong();
    bool openSong (const juce::File& folder);
    void saveSong (const juce::File& folder);
    void autosave();

    void injectMidi (juce::MidiMessage);            // from the on-screen keyboard, same path as a real device
    struct Controller { int id; float value; };     // id: cc number, or 128 + note
    bool popController (Controller&);
    float meter (int trackIndex);

    double latencyOffsetMs = 0;             // added to what the device reports
    void updateLatency();
    juce::String latencyText() const;
    void calibrate (std::function<void (const juce::String&)> done);

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& e) override { juce::Logger::writeToLog ("audio device error: " + e); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

private:
    static constexpr int kMaxFx = 16, kMaxEv = 512;
    struct RtTrack      // what the audio thread sees of a track
    {
        int type = 0, input = 0, output = 0, midiChannel = 0;
        bool stereoIn = false, drums = false;
        std::vector<std::shared_ptr<juce::AudioPluginInstance>> chain;    // instrument first
        std::atomic<float> gain { 1.f }, pan { 0.f }, peak { 0.f };
        std::atomic<bool> power { true }, solo { false }, bypass[kMaxFx + 1];
        float lastL = 0.f, lastR = 0.f;
        juce::AudioBuffer<float> buf;
        juce::MidiBuffer midi;
    };

    void rebuild();
    void finishStop();

    juce::CriticalSection lock;                         // guards rt's structure
    std::vector<std::unique_ptr<RtTrack>> rt;
    std::vector<float> zeros;
    juce::MidiMessageCollector collector, screen;       // midi devices, and the on-screen keys
    juce::MidiDeviceListConnection midiWatch;
    juce::MidiBuffer midiIn, screenIn;
    juce::AbstractFifo ctrlFifo { 256 };
    Controller ctrlBuf[256];
    std::atomic<double> rtBeatLen { 24000.0 };
    std::atomic<int> rtNum { 4 };
    std::atomic<bool> learned[256];                     // controllers owned by automation tracks, kept away from instruments
    int autosaveTicks = 0;
    bool registered = false;                            // device and midi callbacks are added once
    struct { int left = 0, total = 1; double phase = 0, inc = 0; } click;
    struct { std::atomic<int> state { 0 }; juce::int64 counter = 0, peakAt = 0; float peak = 0; } calib;
    std::function<void (const juce::String&)> calibDone;

    int uiMode = alk::Engine::Stopped;                  // what the UI thread last asked for
    using Snapshot = std::vector<std::vector<Region>>;
    std::vector<Snapshot> undoStack, redoStack;
    Snapshot snapshot() const;
    void restore (const Snapshot&);
    juce::OwnedArray<juce::DocumentWindow> editors;
};
