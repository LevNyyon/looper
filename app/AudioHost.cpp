#include "AudioHost.h"

namespace {

struct EditorWindow : juce::DocumentWindow
{
    EditorWindow (std::shared_ptr<juce::AudioPluginInstance> p, juce::OwnedArray<juce::DocumentWindow>& owner)
        : DocumentWindow (p->getName(), juce::Colours::darkgrey, closeButton), plugin (std::move (p)), list (owner)
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* ed = plugin->hasEditor() ? plugin->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*plugin);
        setContentOwned (ed, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    ~EditorWindow() override { clearContentComponent(); }       // the editor goes before the plugin
    void closeButtonPressed() override { list.removeObject (this); }

    std::shared_ptr<juce::AudioPluginInstance> plugin;          // keeps the plugin alive while its window is open
    juce::OwnedArray<juce::DocumentWindow>& list;
};

void prepare (juce::AudioPluginInstance& p, double sr, int bs, bool instrument)
{
    p.releaseResources();
    p.disableNonMainBuses();
    if (! instrument && p.getBusCount (true) > 0) p.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
    if (p.getBusCount (false) > 0) p.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());
    p.setRateAndBufferSizeDetails (sr, bs);
    p.prepareToPlay (sr, bs);
}

} // namespace

AudioHost::AudioHost()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Looper";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    props.setStorageParameters (o);
    latencyOffsetMs = props.getUserSettings()->getDoubleValue ("latencyOffsetMs", 0.0);
    loopRange = props.getUserSettings()->getBoolValue ("loopRange", false);

    juce::addDefaultFormatsToManager (formats);
    if (auto xml = props.getUserSettings()->getXmlValue ("plugins")) known.recreateFromXml (*xml);
    known.addChangeListener (this);

    // Apple's built-in synth is always installed, so an instrument track can make sound before any plugin scan
    bool hasSynth = false;
    for (auto& d : known.getTypes()) hasSynth |= d.isInstrument;
    for (auto* f : formats.getFormats())
        if (! hasSynth && f->getName() == "AudioUnit")
        {
            juce::OwnedArray<juce::PluginDescription> found;
            known.scanAndAddFile ("AudioUnit:Synths/aumu,dls ,appl", true, found, *f);
        }

    for (auto& l : learned) l = false;
    collector.reset (sr);
    screen.reset (sr);
    midiIn.ensureSize (kMaxEv * 16);
    screenIn.ensureSize (kMaxEv * 16);
    song.addTrack (Track::Audio);
    ensureClickTrack();
    rebuild();
    startTimerHz (30);
}

AudioHost::~AudioHost()
{
    stopTimer();
    dm.removeChangeListener (this);
    dm.removeAudioCallback (this);
    dm.removeMidiInputDeviceCallback ({}, &collector);
    known.removeChangeListener (this);
    editors.clear();
}

void AudioHost::openDevices (bool microphoneAllowed)
{
    auto saved = props.getUserSettings()->getXmlValue ("audio");
    deviceError = dm.initialise (microphoneAllowed ? 256 : 0, 256, saved.get(), true);
    if (dm.getCurrentAudioDevice() == nullptr && deviceError.isEmpty())
        deviceError = "No audio device could be opened. Pick one in Prefs.";
    juce::Logger::writeToLog ("openDevices: mic " + juce::String (microphoneAllowed ? "allowed" : "not allowed yet, outputs only") + ", result \"" + deviceError + "\"");
    if (registered) return;         // called again once the microphone is allowed
    registered = true;
    auto listenToAll = [this] { for (auto& d : juce::MidiInput::getAvailableDevices()) dm.setMidiInputDeviceEnabled (d.identifier, true); };
    listenToAll();
    midiWatch = juce::MidiDeviceListConnection::make (listenToAll);     // keyboards plugged in later
    dm.addMidiInputDeviceCallback ({}, &collector);
    dm.addAudioCallback (this);
    dm.addChangeListener (this);
}

juce::StringArray AudioHost::channelNames (bool input) const
{
    juce::StringArray names;    // only active channels reach the callback, in this order
    if (auto* dev = dm.getCurrentAudioDevice())
    {
        const auto all = input ? dev->getInputChannelNames() : dev->getOutputChannelNames();
        const auto active = input ? dev->getActiveInputChannels() : dev->getActiveOutputChannels();
        for (int i = 0; i < all.size(); ++i) if (active[i]) names.add (all[i]);
    }
    return names;
}

void AudioHost::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &known) { if (auto xml = known.createXml()) props.getUserSettings()->setValue ("plugins", xml.get()); }
    else if (auto xml = dm.createStateXml()) props.getUserSettings()->setValue ("audio", xml.get());
    if (source == &dm && onChange) onChange();
}

void AudioHost::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    sr = dev->getCurrentSampleRate();
    blockSize = dev->getCurrentBufferSizeSamples();
    screen.reset (sr);
    juce::Logger::writeToLog ("device started: " + dev->getName() + ", " + juce::String (sr) + " Hz, buffer " + juce::String (blockSize)
                              + ", inputs " + juce::String (dev->getActiveInputChannels().countNumberOfSetBits())
                              + ", outputs " + juce::String (dev->getActiveOutputChannels().countNumberOfSetBits()));
    collector.reset (sr);
    for (auto& t : song.tracks)
    {
        if (t.instrument.instance != nullptr) prepare (*t.instrument.instance, sr, blockSize, true);
        for (auto& s : t.fx) if (s.instance != nullptr) prepare (*s.instance, sr, blockSize, false);
    }
    rebuild();
    updateLatency();
    regionsChanged();
    setLoop (loopA, loopB);
}

void AudioHost::updateLatency()
{
    if (auto* dev = dm.getCurrentAudioDevice())
    {
        const int out = dev->getOutputLatencyInSamples() + blockSize;
        const int off = juce::roundToInt (latencyOffsetMs * sr / 1000.0);
        engine.latency = juce::jmax (0, dev->getInputLatencyInSamples() + out + off);
        // ponytail: an estimate. A key press reaches the callback one buffer late and is played against
        // sound that left one output latency ago. There is no loopback measurement for midi, add an offset knob if notes land late.
        engine.midiLatency = juce::jmax (0, out + blockSize);
    }
}

juce::String AudioHost::latencyText() const
{
    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr) return "No audio device";
    return dev->getName() + ", " + juce::String (sr, 0) + " Hz, buffer " + juce::String (blockSize)
         + ". Reported in " + juce::String (dev->getInputLatencyInSamples()) + " + out " + juce::String (dev->getOutputLatencyInSamples())
         + ". Recording shifted by " + juce::String ((juce::int64) engine.latency.load()) + " samples ("
         + juce::String (1000.0 * (double) engine.latency.load() / sr, 1) + " ms)";
}

void AudioHost::calibrate (std::function<void (const juce::String&)> done)
{
    calibDone = std::move (done);
    calib.counter = 0;
    calib.peak = 0;
    calib.state = 1;
}

// ---------------------------------------------------------------- song edits

void AudioHost::rebuild()
{
    juce::String failed;
    auto load = [&] (PluginSlot& s, bool instrument)
    {
        if (s.instance != nullptr || s.desc.name.isEmpty()) return;
        juce::String err;
        s.instance = formats.createPluginInstance (s.desc, sr, blockSize, err);
        if (s.instance == nullptr) { failed << s.desc.name << ": " << err << "\n"; return; }
        if (s.state.getSize() > 0) s.instance->setStateInformation (s.state.getData(), (int) s.state.getSize());
        prepare (*s.instance, sr, blockSize, instrument);
    };

    std::vector<std::unique_ptr<RtTrack>> fresh;
    for (auto& t : song.tracks)
    {
        load (t.instrument, true);
        for (auto& s : t.fx) load (s, false);

        auto r = std::make_unique<RtTrack>();
        r->type = t.type;
        r->input = t.input;
        r->output = t.output;
        r->stereoIn = t.stereoIn;
        r->midiChannel = t.midiChannel;
        r->drums = t.drums;
        int chans = 2;
        auto add = [&] (PluginSlot& s)
        {
            if (s.instance == nullptr || r->chain.size() > kMaxFx) return;
            chans = juce::jmax (chans, s.instance->getTotalNumInputChannels(), s.instance->getTotalNumOutputChannels());
            r->chain.push_back (s.instance);
        };
        if (t.type == Track::Instrument) add (t.instrument);
        for (auto& s : t.fx) add (s);
        r->buf.setSize (chans, blockSize);
        r->midi.ensureSize (kMaxEv * 16);       // room for a full block of events, so addEvent never allocates
        fresh.push_back (std::move (r));
    }
    {
        const juce::ScopedLock sl (lock);
        rt.swap (fresh);
        zeros.assign ((size_t) blockSize, 0.f);
    }
    paramsChanged();
    if (failed.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin did not load", failed);
}

void AudioHost::paramsChanged()
{
    for (size_t i = 0; i < song.tracks.size() && i < rt.size(); ++i)
    {
        auto& t = song.tracks[i];
        auto& r = *rt[i];
        r.gain = juce::Decibels::decibelsToGain (t.gainDb, -60.f);
        r.pan = t.pan;
        r.power = t.power;
        r.solo = t.solo;
        size_t k = 0;
        if (t.type == Track::Instrument && t.instrument.instance != nullptr) r.bypass[k++] = t.instrument.bypass;
        for (auto& s : t.fx) if (s.instance != nullptr && k <= kMaxFx) r.bypass[k++] = s.bypass;
        if (i < (size_t) alk::Engine::kMaxTracks)
        {
            engine.audition[i] = t.audition;
            engine.quantGrid[i] = (juce::int64) std::llround (t.quantize * spb());
        }
    }
    for (int id = 0; id < 256; ++id)
    {
        bool used = false;
        for (auto& t : song.tracks) used |= std::find (t.sources.begin(), t.sources.end(), id) != t.sources.end();
        learned[id] = used;
    }
    rtBeatLen = spb() * 4.0 / song.den;
    rtNum = song.num;
    dirty = true;
}

void AudioHost::regionsChanged()
{
    engine.setArrangement (song.compile (sr, uiMode == alk::Engine::Recording));
    paramsChanged();    // tempo moves the quantize grid and the metronome
}

void AudioHost::structureChanged()
{
    undoStack.clear();
    redoStack.clear();
    rebuild();
    regionsChanged();

    for (int i = editors.size(); --i >= 0;)     // close windows of plugins that are gone
    {
        auto* p = static_cast<EditorWindow*> (editors[i])->plugin.get();
        bool alive = false;
        for (auto& t : song.tracks)
        {
            alive |= t.instrument.instance.get() == p;
            for (auto& s : t.fx) alive |= s.instance.get() == p;
        }
        if (! alive) editors.remove (i);
    }
    if (onChange) onChange();
}

// ponytail: undo keeps whole copies of every track's regions (takes are shared, so it is cheap)
// and covers region edits only. Adding or removing a track or plugin clears it.
AudioHost::Snapshot AudioHost::snapshot() const
{
    Snapshot s;
    for (auto& t : song.tracks) s.push_back (t.regions);
    return s;
}

void AudioHost::restore (const Snapshot& s)
{
    if (s.size() != song.tracks.size()) return;
    for (size_t i = 0; i < s.size(); ++i)
    {
        song.tracks[i].regions = s[i];
        for (auto& r : song.tracks[i].regions) r.pending = nullptr;
    }
    regionsChanged();
    if (onChange) onChange();
}

void AudioHost::pushUndo()
{
    undoStack.push_back (snapshot());
    if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void AudioHost::undo()
{
    if (undoStack.empty() || rolling()) return;
    redoStack.push_back (snapshot());
    const auto s = undoStack.back();
    undoStack.pop_back();
    restore (s);
}

void AudioHost::redo()
{
    if (redoStack.empty() || rolling()) return;
    undoStack.push_back (snapshot());
    const auto s = redoStack.back();
    redoStack.pop_back();
    restore (s);
}

// ---------------------------------------------------------------- transport

void AudioHost::play()
{
    if (rolling()) stop();
    uiMode = alk::Engine::Playing;
    regionsChanged();
    engine.mode = alk::Engine::Playing;
    if (onChange) onChange();
}

void AudioHost::record()
{
    if (rolling()) stop();
    song.arm (sr, posBeats());      // take buffers are allocated here, never on the audio thread
    uiMode = alk::Engine::Recording;
    regionsChanged();
    engine.mode = alk::Engine::Recording;
    if (onChange) onChange();
}

void AudioHost::stop()
{
    engine.mode = alk::Engine::Stopped;
    finishStop();
}

void AudioHost::finishStop()
{
    const bool wasRecording = uiMode == alk::Engine::Recording;
    uiMode = alk::Engine::Stopped;
    if (wasRecording) song.commit();
    regionsChanged();
    if (wasRecording) autosave();
    if (onChange) onChange();
}

void AudioHost::jump (double beat)
{
    engine.jumpTo = (juce::int64) std::llround (juce::jlimit (0.0, song.lengthBeats(), beat) * spb());
}

void AudioHost::panic()
{
    if (recording() && song.panic (posBeats())) { regionsChanged(); if (onChange) onChange(); }
}

void AudioHost::setLoopRange (bool available)
{
    loopRange = available;
    props.getUserSettings()->setValue ("loopRange", available);
    if (! available) setLoop (0, 0);
    if (onChange) onChange();
}

void AudioHost::setLoop (double a, double b)
{
    if (! loopRange) a = b = 0;     // hidden in Prefs: the song always runs straight through

    loopA = a;
    loopB = b;
    engine.loopStart = (juce::int64) std::llround (a * spb());
    engine.loopEnd = b > a ? (juce::int64) std::llround (b * spb()) : (juce::int64) -1;
}

void AudioHost::timerCallback()
{
    engine.collectGarbage();
    if (uiMode != alk::Engine::Stopped && ! rolling()) finishStop();    // the engine stopped itself at the song end

    // crash protection: unsaved edits reach the disk within about 15 seconds, never mid-performance
    if (++autosaveTicks >= 450)
    {
        autosaveTicks = 0;
        bool hasContent = false;
        for (auto& t : song.tracks) hasContent |= ! t.regions.empty();
        if (dirty && hasContent && ! rolling() && dm.getCurrentAudioDevice() != nullptr) autosave();
    }

    if (calib.state == 2)
    {
        calib.state = 0;
        juce::String msg = "No signal came back. Connect an output to an input with a cable and try again.";
        if (auto* dev = dm.getCurrentAudioDevice(); dev != nullptr && calib.peak > 0.02f)
        {
            const auto reported = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples() + blockSize;
            latencyOffsetMs = (double) (calib.peakAt - reported) * 1000.0 / sr;
            props.getUserSettings()->setValue ("latencyOffsetMs", latencyOffsetMs);
            updateLatency();
            msg = "Measured round trip: " + juce::String (calib.peakAt) + " samples ("
                + juce::String (1000.0 * (double) calib.peakAt / sr, 1) + " ms). Recording now compensates for it.";
        }
        if (calibDone) calibDone (msg);
    }
}

// ---------------------------------------------------------------- plugins

void AudioHost::setPlugin (int trackId, int slot, const juce::PluginDescription& desc)
{
    auto* t = song.track (trackId);
    if (t == nullptr) return;
    PluginSlot s;
    s.desc = desc;
    if (auto* existing = slot == 0 || slot <= (int) t->fx.size() ? t->slot (slot) : nullptr) *existing = s;
    else if ((int) t->fx.size() < kMaxFx) t->fx.push_back (s);
    structureChanged();
}

void AudioHost::removePlugin (int trackId, int slot)
{
    auto* t = song.track (trackId);
    if (t == nullptr) return;
    if (slot == 0) t->instrument = {};
    else if (slot <= (int) t->fx.size()) t->fx.erase (t->fx.begin() + (slot - 1));
    structureChanged();
}

void AudioHost::showEditor (int trackId, int slot)
{
    auto* t = song.track (trackId);
    auto* s = t != nullptr ? t->slot (slot) : nullptr;
    if (s == nullptr || s->instance == nullptr) return;
    for (auto* w : editors)
        if (static_cast<EditorWindow*> (w)->plugin == s->instance) { w->toFront (true); return; }
    editors.add (new EditorWindow (s->instance, editors));
}

// ---------------------------------------------------------------- song files

void AudioHost::ensureClickTrack()
{
    for (auto& t : song.tracks) if (t.type == Track::Click) return;
    song.addTrack (Track::Click).name = "Click";
}

void AudioHost::newSong()
{
    stop();
    editors.clear();
    song = Song();
    song.addTrack (Track::Audio);
    ensureClickTrack();
    setLoop (0, 0);
    jump (0);
    structureChanged();
    dirty = false;
}

bool AudioHost::openSong (const juce::File& folder)
{
    Song s;
    if (! s.load (folder)) return false;
    stop();
    editors.clear();
    song = std::move (s);
    ensureClickTrack();
    setLoop (0, 0);
    jump (0);
    structureChanged();
    props.getUserSettings()->setValue ("lastSong", folder.getFullPathName());
    dirty = false;
    return true;
}

void AudioHost::saveSong (const juce::File& folder)
{
    song.save (folder);
    props.getUserSettings()->setValue ("lastSong", folder.getFullPathName());
    dirty = false;
}

void AudioHost::autosave()
{
    if (song.folder == juce::File())
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Looper");
        dir.createDirectory();
        song.folder = dir.getNonexistentChildFile ("Untitled", ".looper");
    }
    saveSong (song.folder);
}

// ---------------------------------------------------------------- audio thread

void AudioHost::injectMidi (juce::MidiMessage m)
{
    m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    screen.addMessageToQueue (m);
}

bool AudioHost::popController (Controller& c)
{
    int s1, n1, s2, n2;
    ctrlFifo.prepareToRead (1, s1, n1, s2, n2);
    if (n1 + n2 == 0) return false;
    c = ctrlBuf[n1 > 0 ? s1 : s2];
    ctrlFifo.finishedRead (1);
    return true;
}

float AudioHost::meter (int i)
{
    return i >= 0 && i < (int) rt.size() ? rt[(size_t) i]->peak.exchange (0.f) : 0.f;
}

void AudioHost::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                  int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals;
    for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear (out[c], numSamples);

    if (calib.state.load() == 1)    // loopback measurement: one click out, find it coming back in
    {
        for (int k = 0; k < numSamples; ++k)
        {
            if (calib.counter + k == 0) for (int c = 0; c < numOut; ++c) out[c][k] = 0.9f;
            for (int c = 0; c < numIn; ++c)
                if (std::abs (in[c][k]) > calib.peak) { calib.peak = std::abs (in[c][k]); calib.peakAt = calib.counter + k; }
        }
        calib.counter += numSamples;
        if (calib.counter > (juce::int64) sr) calib.state = 2;
        return;
    }

    // ponytail: the collector takes a lock it shares with the midi input thread (stock JUCE behaviour).
    // Replace it with a lock-free fifo if midi-heavy sets ever glitch.
    midiIn.clear();
    screenIn.clear();
    collector.removeNextBlockOfMessages (midiIn, numSamples);
    screen.removeNextBlockOfMessages (screenIn, numSamples);
    for (const auto m : midiIn)     // controller moves and note presses from devices, for learn and automation.
    {                               // (the computer keys and pads are for playing, they never trigger commands)
        const int status = m.data[0] & 0xF0;
        if (m.numBytes != 3 || (status != 0xB0 && status != 0x90 && status != 0x80)) continue;
        int s1, n1, s2, n2;
        ctrlFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0) break;
        ctrlBuf[n1 > 0 ? s1 : s2] = status == 0xB0 ? Controller { m.data[1], m.data[2] / 127.f }
                                                   : Controller { 128 + m.data[1], status == 0x90 && m.data[2] > 0 ? 1.f : 0.f };
        ctrlFifo.finishedWrite (1);
    }

    // ponytail: structural edits (add a track, add a plugin) hold this lock and cost one silent block.
    // Hand over a snapshot like the engine does if that is ever heard on stage.
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;

    bool anySolo = false;
    for (auto& t : rt) anySolo |= t->solo.load();
    const int maxBlock = (int) zeros.size();

    for (int done = 0; done < numSamples;)
    {
        const int n = engine.beginBlock (juce::jmin (numSamples - done, maxBlock));

        alk::MidiEvent ev[kMaxEv], evScreen[64], evOut[kMaxEv];
        int numEv = 0, numScreen = 0;
        for (const auto m : screenIn)
            if (m.samplePosition >= done && m.samplePosition < done + n && m.numBytes == 3 && numScreen < 64)
            {
                evScreen[numScreen].time = m.samplePosition - done;
                std::memcpy (evScreen[numScreen].data, m.data, 3);
                ++numScreen;
            }
        for (const auto m : midiIn)
            if (m.samplePosition >= done && m.samplePosition < done + n && m.numBytes == 3 && numEv < kMaxEv)
            {
                const int status = m.data[0] & 0xF0;    // a learned pedal or fader is not a note for the synths
                if ((status == 0xB0 && learned[m.data[1]].load()) || ((status == 0x90 || status == 0x80) && learned[128 + m.data[1]].load())) continue;
                ev[numEv].time = m.samplePosition - done;
                std::memcpy (ev[numEv].data, m.data, 3);
                ++numEv;
            }

        for (size_t ti = 0; ti < rt.size() && ti < (size_t) alk::Engine::kMaxTracks; ++ti)
        {
            auto& t = *rt[ti];
            if (t.type == Track::Command || t.type == Track::Control) continue;
            float* L = t.buf.getWritePointer (0);
            float* R = t.buf.getWritePointer (1);
            t.midi.clear();
            for (int c = 2; c < t.buf.getNumChannels(); ++c) t.buf.clear (c, 0, n);

            // the looper sits at the track input: loops are recorded dry and replayed through the plugins
            if (t.type == Track::Audio)
            {
                const float* iL = t.input < numIn ? in[t.input] + done : zeros.data();
                const float* iR = t.stereoIn && t.input + 1 < numIn ? in[t.input + 1] + done : iL;
                engine.processAudio ((int) ti, iL, iR, L, R, n);
            }
            else if (t.type == Track::Talkback)     // straight through, never looped. Power is the talk switch
            {
                const float* iL = t.input < numIn ? in[t.input] + done : zeros.data();
                const float* iR = t.stereoIn && t.input + 1 < numIn ? in[t.input + 1] + done : iL;
                juce::FloatVectorOperations::copy (L, iL, n);
                juce::FloatVectorOperations::copy (R, iR, n);
            }
            else if (t.type == Track::Click)
            {
                juce::FloatVectorOperations::clear (L, n);
                juce::FloatVectorOperations::clear (R, n);
                if (! engine.blockRolling()) click.left = 0;
                else
                {
                    const double beatLen = rtBeatLen.load();
                    const juce::int64 p = engine.blockStart();
                    const auto k = (juce::int64) std::ceil ((double) p / beatLen);
                    const auto at = (juce::int64) std::llround ((double) k * beatLen) - p;      // next beat, in this block if < n
                    for (int i = 0; i < n; ++i)
                    {
                        if (i == at)
                        {
                            click.total = click.left = (int) (0.03 * sr);
                            click.phase = 0;
                            click.inc = juce::MathConstants<double>::twoPi * (k % rtNum.load() == 0 ? 1760.0 : 1100.0) / sr;
                        }
                        if (click.left > 0)
                        {
                            const float env = (float) click.left / (float) click.total;
                            L[i] = R[i] = (float) std::sin (click.phase) * 0.3f * env * env;
                            click.phase += click.inc;
                            --click.left;
                        }
                    }
                }
            }
            else
            {
                // Devices reach every instrument track (filtered by channel). The computer keys and pads reach the
                // selected one. While recording they reach all of them, and each track's record regions decide
                // who sounds and who records, so a performance moves between instruments with no mouse.
                // Their note-offs always go everywhere, so a key released after the selection moved still ends its note.
                alk::MidiEvent mine[kMaxEv];
                int numMine = 0;
                for (int k = 0; k < numEv; ++k)
                    if (t.midiChannel == 0 || (ev[k].data[0] & 0x0F) == t.midiChannel - 1) mine[numMine++] = ev[k];
                const bool target = (int) ti == keyTrack.load() || engine.blockRecording();
                for (int k = 0; k < numScreen && numMine < kMaxEv; ++k)
                {
                    const bool isOn = (evScreen[k].data[0] & 0xF0) == 0x90 && evScreen[k].data[2] > 0;
                    if (! target && isOn) continue;
                    int j = numMine++;      // insert in time order, after anything already at that time
                    for (; j > 0 && mine[j - 1].time > evScreen[k].time; --j) mine[j] = mine[j - 1];
                    mine[j] = evScreen[k];
                }
                if (t.drums) for (int k = 0; k < numMine; ++k) mine[k].data[0] = (uint8_t) ((mine[k].data[0] & 0xF0) | 9);   // channel 10
                const int numOutEv = engine.processMidi ((int) ti, mine, numMine, evOut, kMaxEv, n);
                for (int k = 0; k < numOutEv; ++k) t.midi.addEvent (evOut[k].data, 3, (int) evOut[k].time);
                juce::FloatVectorOperations::clear (L, n);
                juce::FloatVectorOperations::clear (R, n);
            }

            for (size_t s = 0; s < t.chain.size(); ++s)
            {
                if (t.bypass[s].load()) continue;
                auto& p = *t.chain[s];
                const int chans = juce::jmin (t.buf.getNumChannels(), juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
                if (chans <= 0) continue;
                juce::AudioBuffer<float> view (t.buf.getArrayOfWritePointers(), chans, n);
                p.processBlock (view, t.midi);
                if (p.getTotalNumOutputChannels() == 1) juce::FloatVectorOperations::copy (R, L, n);
            }

            const bool cue = t.type == Track::Click || t.type == Track::Talkback;      // a solo elsewhere does not silence these
            const float g = t.power.load() && (cue || ! anySolo || t.solo.load()) ? t.gain.load() : 0.f;
            const float angle = (t.pan.load() + 1.f) * juce::MathConstants<float>::pi / 4.f;
            const float gl = g * std::cos (angle) * 1.41421356f, gr = g * std::sin (angle) * 1.41421356f;
            float* oL = t.output < numOut ? out[t.output] + done : nullptr;
            float* oR = t.output + 1 < numOut ? out[t.output + 1] + done : oL;
            float peak = 0.f;
            for (int k = 0; k < n && oL != nullptr; ++k)
            {
                const float a = (float) (k + 1) / (float) n;
                const float l = L[k] * (t.lastL + (gl - t.lastL) * a), r = R[k] * (t.lastR + (gr - t.lastR) * a);
                oL[k] += l;
                oR[k] += r;
                peak = juce::jmax (peak, std::abs (l), std::abs (r));
            }
            t.lastL = gl;
            t.lastR = gr;
            if (peak > t.peak.load()) t.peak = peak;
        }

        engine.endBlock (n);
        done += n;
    }
}
