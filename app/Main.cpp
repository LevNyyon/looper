#include "Ui.h"

// ---------------------------------------------------------------- main component

MainComponent::MainComponent (AudioHost& h) : host (h)
{
    setWantsKeyboardFocus (true);
    for (juce::Component* c : { (juce::Component*) &transport, (juce::Component*) &headers, (juce::Component*) &arrange,
                                (juce::Component*) &trackPanel, (juce::Component*) &inspector })
        addAndMakeVisible (c);
    pads = makeDrumPads (*this);
    addChildComponent (*pads);
    addChildComponent (keyboard);
    keyboard.setLowestVisibleKey (36);
    keyboard.clearKeyMappings();                // typing is handled in keyPressed, so no click on the piano is needed first
    keyboard.setWantsKeyboardFocus (false);
    keyboard.setKeyWidth (22.f);
    keyState.addListener (this);
    inspector.setVisible (false);
    host.onChange = [this] { refresh(); };
    if (! host.song.tracks.empty()) selTrack = host.song.tracks.front().id;
    setSize (1400, 820);
    startTimer (10);
}

MainComponent::~MainComponent() { keyState.removeListener (this); host.onChange = nullptr; }

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transport.setBounds (r.removeFromTop (40));
    if (inspector.isVisible()) inspector.setBounds (r.removeFromBottom (150));
    if (keyboard.isVisible() || pads->isVisible())
    {
        const auto strip = r.removeFromBottom (58);
        keyboard.setBounds (strip);
        pads->setBounds (strip);
    }
    trackPanel.setBounds (r.removeFromRight (240));
    const auto left = r.removeFromLeft (208);
    arrange.setBounds (r);          // first: the header rows take their height from the timeline
    headers.setBounds (left);
    headers.rebuild();
}

void MainComponent::handleAsyncUpdate()
{
    if (host.song.track (selTrack) == nullptr) selTrack = host.song.tracks.empty() ? 0 : host.song.tracks.front().id;
    selection.erase (std::remove_if (selection.begin(), selection.end(), [this] (int id) { return host.song.region (id) == nullptr; }), selection.end());
    if (host.song.region (selRegion) == nullptr) selRegion = selection.empty() ? 0 : selection.back();
    auto* sel = track();
    const bool instrument = sel != nullptr && sel->type == Track::Instrument;
    const bool keys = instrument && ! sel->drums, drums = instrument && sel->drums;
    host.keyTrack = instrument ? host.song.indexOf (selTrack) : -1;     // the keys and pads play the selected track only
    const bool inspect = region() != nullptr;        // the inspector only takes space while a region is selected
    if (keys != keyboard.isVisible() || drums != pads->isVisible() || inspect != inspector.isVisible())
    {
        keyboard.setVisible (keys);
        pads->setVisible (drums);
        inspector.setVisible (inspect);
        resized();
    }
    transport.rebuild();
    headers.rebuild();
    trackPanel.rebuild();
    inspector.rebuild();
    repaint();
}

void MainComponent::select (int trackId, int regionId)
{
    if (trackId != 0) selTrack = trackId;
    selRegion = regionId;
    selection.clear();
    if (regionId != 0) selection.push_back (regionId);
    refresh();
}

void MainComponent::timerCallback()
{
    if (! heldKeys.empty()) keyStateChanged (false);    // a key let go while the window was not looking (Cmd-Tab, a dialog)
    events.clear();
    for (AudioHost::Controller c; host.popController (c);) events.push_back (c);

    if (learnTrack != 0)
        for (auto& e : events)
            if (e.value > 0)
            {
                if (auto* t = host.song.track (learnTrack); t != nullptr && std::find (t->sources.begin(), t->sources.end(), e.id) == t->sources.end())
                    t->sources.push_back (e.id);
                learnTrack = 0;
                host.paramsChanged();
                refresh();
                events.clear();     // the press that teaches must not also fire the region under the playhead
                break;
            }

    if (automation.tick (events)) refresh();

    if (++ticks % 3 != 0) return;

    // While recording, the instrument track whose record region is coming up becomes the selected one,
    // so its piano or drum pads and key map are in place without touching the mouse.
    if (host.recording())
    {
        const double pos = host.posBeats();
        auto live = [pos] (const Track& t)
        {
            if (t.type != Track::Instrument) return false;
            for (auto& r : t.regions)
                if (r.kind == Region::Record && r.pending != nullptr && pos + 0.5 >= r.start && pos < r.start + r.length) return true;
            return false;
        };
        if (auto* sel = track(); sel == nullptr || ! live (*sel))
            for (auto& t : host.song.tracks)
                if (live (t)) { select (t.id, 0); break; }
    }

    arrange.playheadMoved();
    if (transport.pos != nullptr) transport.pos->setText (barBeat (host.song, std::floor (host.posBeats() * host.song.den / 4.0) * 4.0 / host.song.den), juce::dontSendNotification);
    for (size_t i = 0; i < headers.levels.size(); ++i) headers.levels[i] = juce::jmax (host.meter ((int) i), headers.levels[i] * 0.8f);
    headers.repaint (headers.getWidth() - 8, 0, 8, headers.getHeight());
    if (ticks % 30 == 0 && transport.cpu != nullptr)
        transport.cpu->setText ("CPU " + juce::String (juce::roundToInt (host.dm.getCpuUsage() * 100)) + "%"
                                    + (host.song.folder != juce::File() ? "   " + host.song.folder.getFileNameWithoutExtension() : juce::String()),
                                juce::dontSendNotification);
}

void MainComponent::drum (int note, bool down)
{
    host.injectMidi (down ? juce::MidiMessage::noteOn (10, note, (juce::uint8) 100) : juce::MidiMessage::noteOff (10, note));
}

bool MainComponent::keyStateChanged (bool)
{
    for (auto it = heldKeys.begin(); it != heldKeys.end();)
        if (! juce::KeyPress::isKeyCurrentlyDown (it->first))
        {
            if (it->second.second) drum (it->second.first, false); else keyState.noteOff (1, it->second.first, 0.f);
            it = heldKeys.erase (it);
        }
        else ++it;
    return false;
}

void MainComponent::promptName (const juce::String& title, const juce::String& current, std::function<void (const juce::String&)> done)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", current);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int res)
    {
        if (res == 1) done (w->getTextEditorContents ("name"));
    }), true);
}

void MainComponent::renameTrack (int trackId)
{
    auto* t = host.song.track (trackId);
    if (t == nullptr) return;
    promptName ("Name this track", t->name, [this, trackId] (const juce::String& name)
    {
        if (auto* tr = host.song.track (trackId)) { tr->name = name; host.dirty = true; refresh(); }
    });
}

void MainComponent::deleteSelected()
{
    if (selection.empty()) return;
    host.pushUndo();
    for (int id : selection)
    {
        Track* t = nullptr;
        if (auto* r = host.song.region (id, &t)) t->regions.erase (t->regions.begin() + (r - t->regions.data()));
    }
    selection.clear();
    selRegion = 0;
    host.regionsChanged();
    refresh();
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto c = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) k.getKeyCode());

    if (k.isKeyCode (juce::KeyPress::spaceKey)) { if (host.rolling()) host.stop(); else host.play(); return true; }
    if (k.isKeyCode (juce::KeyPress::returnKey)) { if (host.recording()) host.stop(); else host.record(); return true; }
    if (k.isKeyCode (juce::KeyPress::backspaceKey) || k.isKeyCode (juce::KeyPress::deleteKey)) { deleteSelected(); return true; }
    if (mods.isCommandDown())
    {
        if (c == 's') { if (host.song.folder == juce::File() || mods.isShiftDown()) saveSongAs(); else host.saveSong (host.song.folder); return true; }
        if (c == 'o') { openSong(); return true; }
        if (c == 'z') { if (mods.isShiftDown()) host.redo(); else host.undo(); return true; }
        if (c == ',') { showPrefs(); return true; }
        return false;
    }
    // With an instrument track selected the letter keys play it: the drum map, or one octave of piano from middle C.
    // The piano keys stay clear of the shortcuts. The drum map takes Z and Q while a drum track is selected.
    if (auto* t = track(); t != nullptr && t->type == Track::Instrument)
    {
        int note = -1;
        if (t->drums) { for (auto& d : drumMap) if (c == (juce::juce_wchar) d.key) note = d.note; }
        else if (const int i = juce::String ("awsedftgyhujk").indexOfChar (c); i >= 0) note = 60 + i;
        if (note >= 0)
        {
            if (heldKeys.count (k.getKeyCode()) == 0)
            {
                heldKeys[k.getKeyCode()] = { note, t->drums };
                if (t->drums) drum (note, true); else keyState.noteOn (1, note, 0.8f);
            }
            return true;        // also swallows key repeat while it is held
        }
    }

    switch (c)
    {
        case 'z': if (mods.isShiftDown()) host.redo(); else host.undo(); return true;
        case 'r': host.jump (host.loopB > host.loopA ? host.loopA : 0.0); return true;
        case ',': host.jump (0.0); return true;
        case '.':       // on to the next marker
        {
            double next = -1;
            for (auto& t : host.song.tracks)
                for (auto& r : t.regions)
                    if (r.kind == Region::Marker && r.start > host.posBeats() + 0.01 && (next < 0 || r.start < next)) next = r.start;
            if (next >= 0) host.jump (next);
            return true;
        }
        case 'p': host.panic(); return true;
        case 'q':       // loop the selected region, or clear the loop
            if (! host.loopRange) return true;
            if (auto* r = region(); r != nullptr && ! (juce::approximatelyEqual (host.loopA, r->start) && juce::approximatelyEqual (host.loopB, r->start + r->length))) host.setLoop (r->start, r->start + r->length);
            else host.setLoop (0, 0);
            arrange.repaint();
            return true;
        case 'l':
            if (auto* t = track(); t != nullptr && t->hasLoops()) { t->locked = ! t->locked; host.regionsChanged(); refresh(); }
            return true;
        default: return false;
    }
}

void MainComponent::addTrackMenu (juce::Component* target)
{
    juce::PopupMenu m;
    m.addItem (1, "Audio track");
    m.addItem (2, "Instrument track");
    m.addItem (3, "Command track (pedals, switches)");
    m.addItem (4, "Control track (faders, knobs, curves)");
    m.addItem (5, "Talkback track (a mic straight to an output)");
    m.addItem (6, "Drum track (pads on the computer keyboard)");
    m.addItem (7, "Marker track (named sections, the . key jumps to the next)");
    const auto where = target != nullptr ? juce::PopupMenu::Options().withTargetComponent (target) : juce::PopupMenu::Options().withMousePosition();
    m.showMenuAsync (where, [this] (int res)
    {
        if (res == 0 || (int) host.song.tracks.size() >= alk::Engine::kMaxTracks) return;
        const Track::Type kinds[] = { Track::Audio, Track::Instrument, Track::Command, Track::Control, Track::Talkback, Track::Instrument, Track::Marker };
        selTrack = host.newTrack (kinds[res - 1], res == 6).id;
        host.structureChanged();
    });
}

void MainComponent::pluginMenu (juce::Component& target, int trackId, int slot, bool instruments)
{
    auto list = std::make_shared<juce::Array<juce::PluginDescription>>();
    juce::PopupMenu m;
    for (auto& d : host.known.getTypes())
        if (d.isInstrument == instruments) { list->add (d); m.addItem (list->size(), d.name + "  (" + d.pluginFormatName + ")"); }
    if (list->isEmpty()) m.addItem (20000, "No plugins found. Scan for them in Prefs.", false);
    auto* t = host.song.track (trackId);
    if (t != nullptr && slot <= (int) t->fx.size() && t->slot (slot)->desc.name.isNotEmpty())
    {
        m.addSeparator();
        m.addItem (10000, "Remove");
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [this, list, trackId, slot] (int res)
    {
        if (res == 10000) host.removePlugin (trackId, slot);
        else if (res > 0 && res <= list->size()) host.setPlugin (trackId, slot, list->getReference (res - 1));
    });
}

void MainComponent::targetMenu (juce::Component& target, int regionId)
{
    Track* owner = nullptr;
    if (host.song.region (regionId, &owner) == nullptr) return;
    const bool command = owner->type == Track::Command;

    auto list = std::make_shared<std::vector<Target>>();
    auto item = [&] (juce::PopupMenu& m, const juce::String& name, Target tg) { list->push_back (tg); m.addItem ((int) list->size(), name); };

    juce::PopupMenu m;
    if (command)
    {
        juce::PopupMenu tr;
        item (tr, "Play / Stop", { Target::Play });
        item (tr, "Record / Stop", { Target::Record });
        item (tr, "Panic", { Target::Panic });
        item (tr, "Jump to bar (set Value to the bar number)", { Target::Position });
        m.addSubMenu ("Transport", tr);
    }
    for (auto& t : host.song.tracks)
    {
        if (t.isAutomation() || t.type == Track::Marker) continue;
        juce::PopupMenu tm;
        if (command) item (tm, "Power", { Target::TrackPower, t.id });
        item (tm, "Volume", { Target::TrackVolume, t.id });
        for (int s = 0; s <= (int) t.fx.size(); ++s)
        {
            auto* ps = t.slot (s);
            if (ps->instance == nullptr) continue;
            juce::PopupMenu pm;
            if (command) item (pm, "On / off", { Target::PluginBypass, t.id, s });
            auto& params = ps->instance->getParameters();
            for (int p = 0; p < juce::jmin (300, params.size()); ++p) item (pm, params[p]->getName (40), { Target::PluginParam, t.id, s, p });
            tm.addSubMenu (ps->desc.name, pm);
        }
        m.addSubMenu (t.name, tm);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [this, list, regionId] (int res)
    {
        if (auto* r = host.song.region (regionId); r != nullptr && res > 0)
        {
            r->target = (*list)[(size_t) res - 1];
            host.dirty = true;
            refresh();
        }
    });
}

static juce::File songsDir() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Looper"); }

void MainComponent::openSong()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a song folder (.looper)", songsDir());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f != juce::File() && ! host.openSong (f))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a song", f.getFileName() + " has no song.json inside.");
    });
}

void MainComponent::saveSongAs()
{
    songsDir().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save song", songsDir().getChildFile ("Song.looper"));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode, [this] (const juce::FileChooser& fc)
    {
        if (fc.getResult() != juce::File()) host.saveSong (fc.getResult().withFileExtension ("looper"));
    });
}

// ---------------------------------------------------------------- preferences

struct AudioPage : juce::Component, juce::Timer
{
    explicit AudioPage (AudioHost& h) : host (h), selector (h.dm, 0, 256, 0, 256, true, false, false, false)
    {
        for (juce::Component* c : { (juce::Component*) &selector, (juce::Component*) &info, (juce::Component*) &offsetLabel,
                                    (juce::Component*) &offset, (juce::Component*) &calibrate, (juce::Component*) &result, (juce::Component*) &hideLoop })
            addAndMakeVisible (c);
        hideLoop.setToggleState (! host.loopRange, juce::dontSendNotification);
        hideLoop.onClick = [this] { host.setLoopRange (! hideLoop.getToggleState()); };
        offsetLabel.setText ("Extra recording offset", juce::dontSendNotification);
        offset.setRange (-100, 100, 0.1);
        offset.setTextValueSuffix (" ms");
        offset.setValue (host.latencyOffsetMs, juce::dontSendNotification);
        offset.onValueChange = [this]
        {
            host.latencyOffsetMs = offset.getValue();
            host.props.getUserSettings()->setValue ("latencyOffsetMs", host.latencyOffsetMs);
            host.updateLatency();
        };
        calibrate.onClick = [this]
        {
            result.setText ("Listening...", juce::dontSendNotification);
            host.calibrate ([safe = juce::Component::SafePointer<AudioPage> (this)] (const juce::String& msg)
            {
                if (safe == nullptr) return;
                safe->result.setText (msg, juce::dontSendNotification);
                safe->offset.setValue (safe->host.latencyOffsetMs, juce::dontSendNotification);
            });
        };
        result.setText ("Loops landing early or late? Connect an output to an input with a cable and measure, or nudge the offset by ear.", juce::dontSendNotification);
        startTimerHz (2);
    }
    void timerCallback() override { info.setText (host.latencyText(), juce::dontSendNotification); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto bottom = r.removeFromBottom (150);
        hideLoop.setBounds (bottom.removeFromBottom (28));
        selector.setBounds (r);
        info.setBounds (bottom.removeFromTop (36));
        auto row = bottom.removeFromTop (26);
        offsetLabel.setBounds (row.removeFromLeft (150));
        calibrate.setBounds (row.removeFromRight (190));
        offset.setBounds (row.reduced (6, 0));
        result.setBounds (bottom);
    }

    AudioHost& host;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label info, offsetLabel, result;
    juce::Slider offset { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton calibrate { "Measure with loopback cable" };
    juce::ToggleButton hideLoop { "Hide the loop range (the blue bar in the ruler that repeats one section, and its Q shortcut)" };
};

struct Prefs : juce::TabbedComponent
{
    explicit Prefs (AudioHost& h)
        : TabbedComponent (juce::TabbedButtonBar::TabsAtTop), audio (h),
          plugins (h.formats, h.known, h.props.getUserSettings()->getFile().getSiblingFile ("PluginScanCrashed"), h.props.getUserSettings(), true)
    {
        addTab ("Audio and MIDI", col::panel, &audio, false);
        addTab ("Plugins", col::panel, &plugins, false);
        setSize (680, 640);
    }
    AudioPage audio;
    juce::PluginListComponent plugins;
};

void MainComponent::showPrefs()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new Prefs (host));
    o.dialogTitle = "Preferences";
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

// ---------------------------------------------------------------- app

// A made-up song so `--snapshot` has something to draw. Not used in normal runs.
// The audio take is recorded through the real device callback, then saved and reloaded,
// so the picture also shows that record, commit, save and load work.
static void demoSong (AudioHost& host, const juce::File& folder)
{
    auto& song = host.song;
    song.bars = 16;
    song.folder = folder;
    auto add = [&] (Track& t, double start, double len, double s, double sl)
    {
        auto& r = song.addRegion (t, Region::Play, start, len);
        r.sliceStart = s;
        r.sliceLength = sl;
    };

    song.tracks[0].name = "Guitar";
    song.addRegion (song.tracks[0], Region::Record, 0, 16).name = "Riff";
    add (song.tracks[0], 16, 16, 0, 8);         // bars 1 to 2
    add (song.tracks[0], 32, 8, 4, 8);          // bars 2 to 3
    add (song.tracks[0], 40, 8, 8, 8);          // bars 3 to 4
    add (song.tracks[0], 48, 4, 2, 0.25);       // a 16th, stuttered
    add (song.tracks[0], 52, 12, 0, 0);         // the whole loop

    const int keys = song.addTrack (Track::Instrument).id;
    song.track (keys)->name = "Keys";
    song.addRegion (*song.track (keys), Region::Record, 16, 16).name = "Chords";
    add (*song.track (keys), 32, 16, 0, 4);
    add (*song.track (keys), 48, 16, 8, 8);

    auto& pedal = song.addTrack (Track::Command);
    pedal.name = "Pedal";
    pedal.sources = { 64 };
    auto& panic = song.addRegion (pedal, Region::Auto, 0, 32);
    panic.captured = true;
    panic.target = { Target::Panic };
    song.addRegion (pedal, Region::Auto, 40, 8).target = { Target::TrackPower, keys };

    auto& fader = song.addTrack (Track::Control);
    fader.name = "Fader";
    auto& sweep = song.addRegion (fader, Region::Auto, 32, 16);
    sweep.target = { Target::TrackVolume, song.tracks[0].id };
    sweep.curve = { { 0.f, 0.2f }, { 0.5f, 1.f }, { 1.f, 0.4f } };

    // Apple's built-in delay and synth, so the plugin chain runs too
    juce::KnownPluginList scratch;      // not the user's plugin list
    for (auto* f : host.formats.getFormats())
        if (f->getName() == "AudioUnit")
        {
            juce::OwnedArray<juce::PluginDescription> found;
            scratch.scanAndAddFile ("AudioUnit:Effects/aufx,dely,appl", true, found, *f);
            scratch.scanAndAddFile ("AudioUnit:Synths/aumu,dls ,appl", true, found, *f);
            for (auto* d : found)
            {
                PluginSlot slot;
                slot.desc = *d;
                if (d->isInstrument) song.track (keys)->instrument = slot; else song.tracks[0].fx.push_back (slot);
            }
        }

    for (auto& t : song.tracks) if (t.type == Track::Click) t.power = false;
    host.structureChanged();
    const int n = 512;
    const double spb = host.spb();
    std::vector<float> in ((size_t) n), outL ((size_t) n), outR ((size_t) n);
    const float* ins[] = { in.data() };
    float* outs[] = { outL.data(), outR.data() };
    host.record();
    for (int64_t done = 0; done < (int64_t) (34 * spb); done += n)
    {
        for (int k = 0; k < n; ++k)     // a plucked note per beat
        {
            const double beat = (double) (done + k) / spb, ph = beat - std::floor (beat);
            in[(size_t) k] = (float) (std::sin ((double) (done + k) * 0.02 * (1 + (int) beat % 4)) * std::exp (-ph * 5) * 0.9);
        }
        host.audioDeviceIOCallbackWithContext (ins, 1, outs, 2, n, {});
    }
    if (auto take = song.track (keys)->regions[0].pending)      // no midi device here, so write the notes in
        for (int i = 0; i < 16; ++i)
        {
            const uint8_t note = (uint8_t) (48 + (i * 7) % 24);
            take->midi[(size_t) i * 2] = { (int64_t) (i * spb), { 0x90, note, 100 } };
            take->midi[(size_t) i * 2 + 1] = { (int64_t) ((i + 0.8) * spb), { 0x80, note, 0 } };
            take->midiCount = (i + 1) * 2;
        }
    host.stop();                // commits the takes and autosaves into `folder`
    host.openSong (folder);     // plugins come back from their saved state
    host.setLoop (32, 48);
}

bool microphoneAllowed();                                       // MicPermission.mm
void requestMicrophone (std::function<void (bool)> done);

struct App : juce::JUCEApplication
{
    const juce::String getApplicationName() override { return "Looper"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        logger.reset (juce::FileLogger::createDefaultAppLogger ("Looper", "Looper.log", "Looper started"));    // ~/Library/Logs/Looper
        juce::Logger::setCurrentLogger (logger.get());
        host = std::make_unique<AudioHost>();
        const auto args = getCommandLineParameterArray();

        // `Looper --snapshot out.png [region index]` draws the window to a file and quits, no audio device opened
        if (const int i = args.indexOf ("--snapshot"); i >= 0 && i + 1 < args.size())
        {
            const auto dir = juce::File::getCurrentWorkingDirectory();
            dir.getChildFile ("demo.looper").deleteRecursively();
            const auto lastSong = host->props.getUserSettings()->getValue ("lastSong");
            demoSong (*host, dir.getChildFile ("demo.looper"));
            host->props.getUserSettings()->setValue ("lastSong", lastSong);    // the demo is not the user's song
            MainComponent ui (*host);
            ui.arrange.pxPerBeat = 14;
            const int pick = args[i + 2].getIntValue();
            int n = 0;
            for (auto& t : host->song.tracks) for (auto& r : t.regions) if (n++ == pick) { ui.selTrack = t.id; ui.selRegion = r.id; ui.selection = { r.id }; }
            ui.handleAsyncUpdate();
            juce::FileOutputStream out (dir.getChildFile (args[i + 1]));
            out.setPosition (0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream (ui.createComponentSnapshot (ui.getLocalBounds()), out);
            quit();
            return;
        }

        const juce::File last (host->props.getUserSettings()->getValue ("lastSong"));
        if (last.getChildFile ("song.json").existsAsFile()) host->openSong (last);
        host->dirty = false;

        window = std::make_unique<juce::DocumentWindow> ("Looper", col::bg, juce::DocumentWindow::allButtons);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (new MainComponent (*host), true);
        window->setResizable (true, false);
        window->centreWithSize (window->getWidth(), window->getHeight());
        window->setVisible (true);
        window->getContentComponent()->grabKeyboardFocus();

        // after the window is up: macOS may hold this call while it asks for microphone access
        // Outputs and MIDI work straight away. Inputs join once macOS has an answer about the microphone,
        // so a pending prompt never holds the app up.
        host->openDevices (microphoneAllowed());
        host->dirty = false;
        if (host->onChange) host->onChange();       // the track panel lists the device's channels
        if (! microphoneAllowed())
            requestMicrophone ([this] (bool allowed)
            {
                if (host == nullptr) return;
                if (allowed) host->openDevices (true);
                else host->deviceError = "Microphone access is off, so audio tracks record silence. Allow Looper in System Settings, Privacy and Security, Microphone, then relaunch.";
                if (host->onChange) host->onChange();
            });
    }

    void systemRequestedQuit() override { quit(); }

    void shutdown() override
    {
        bool hasContent = false;
        if (host != nullptr) for (auto& t : host->song.tracks) hasContent |= ! t.regions.empty();
        if (host != nullptr && host->dirty && hasContent && window != nullptr) host->autosave();
        window = nullptr;
        host = nullptr;
        juce::Logger::setCurrentLogger (nullptr);
    }

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<AudioHost> host;
    std::unique_ptr<juce::DocumentWindow> window;
};

START_JUCE_APPLICATION (App)
