#include "Ui.h"

// ---------------------------------------------------------------- widget helpers

juce::Label& Panel::label (juce::Rectangle<int> r, const juce::String& text, juce::Colour c)
{
    auto& l = add<juce::Label> (r, juce::String(), text);
    l.setColour (juce::Label::textColourId, c);
    l.setFont (juce::Font (juce::FontOptions (12.5f)));
    l.setMinimumHorizontalScale (1.f);
    l.setBorderSize ({ 0, 2, 0, 2 });
    l.setInterceptsMouseClicks (false, false);
    return l;
}

juce::Label& Panel::field (juce::Rectangle<int> r, const juce::String& text, std::function<void (const juce::String&)> fn)
{
    auto& l = add<juce::Label> (r, juce::String(), text);
    l.setFont (juce::Font (juce::FontOptions (12.5f)));
    l.setEditable (true, true, false);
    l.setColour (juce::Label::backgroundColourId, col::bg);
    l.setColour (juce::Label::textColourId, col::text);
    l.setColour (juce::Label::outlineColourId, col::grid);
    l.onTextChange = [&l, fn] { fn (l.getText()); };
    return l;
}

juce::TextButton& Panel::button (juce::Rectangle<int> r, const juce::String& text, std::function<void()> fn)
{
    auto& b = add<juce::TextButton> (r, text);
    b.setWantsKeyboardFocus (false);        // space and return belong to the transport
    b.setColour (juce::TextButton::buttonColourId, col::grid);
    b.onClick = std::move (fn);
    return b;
}

juce::TextButton& Panel::toggle (juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour, std::function<void (bool)> fn)
{
    auto& b = button (r, text, {});
    b.setClickingTogglesState (true);
    b.setToggleState (on, juce::dontSendNotification);
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    b.onClick = [&b, fn] { fn (b.getToggleState()); };
    return b;
}

juce::ComboBox& Panel::combo (juce::Rectangle<int> r, const juce::StringArray& names, int selected, std::function<void (int)> fn)
{
    auto& c = add<juce::ComboBox> (r);
    c.setWantsKeyboardFocus (false);
    c.addItemList (names, 1);
    c.setSelectedItemIndex (selected, juce::dontSendNotification);
    c.onChange = [&c, fn] { fn (c.getSelectedItemIndex()); };
    return c;
}

juce::Slider& Panel::slider (juce::Rectangle<int> r, double lo, double hi, double value, const juce::String& suffix, std::function<void (double)> fn)
{
    auto& s = add<juce::Slider> (r, juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
    s.setWantsKeyboardFocus (false);
    s.setRange (lo, hi, 0.01);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 18);
    s.setTextValueSuffix (suffix);
    s.setValue (value, juce::dontSendNotification);
    s.onValueChange = [&s, fn] { fn (s.getValue()); };
    return s;
}

// ---------------------------------------------------------------- transport bar

static const juce::StringArray snapNames { "Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T", "Off" };
static double snapValue (int i, const Song& s)
{
    const double v[] = { s.beatsPerBar(), 2, 1, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6, 0 };
    return v[juce::jlimit (0, 8, i)];
}

void TransportBar::rebuild()
{
    items.clear();
    auto& host = ui.host;
    auto& song = host.song;
    ui.snapBeats = snapValue (ui.snapIndex, song);

    int x = 8;
    const int y = 7, h = getHeight() - 14;
    auto next = [&] (int w, int gap = 4) { juce::Rectangle<int> r (x, y, w, h); x += w + gap; return r; };
    const bool playing = host.rolling() && ! host.recording();

    button (next (40), "Rew", [this] { ui.host.jump (ui.host.loopB > ui.host.loopA ? ui.host.loopA : 0.0); });
    toggle (next (48), playing ? "Stop" : "Play", playing, col::play, [this] (bool on) { if (on) ui.host.play(); else ui.host.stop(); });
    toggle (next (48, 10), host.recording() ? "Stop" : "Rec", host.recording(), col::rec, [this] (bool on) { if (on) ui.host.record(); else ui.host.stop(); });
    pos = &label (next (58, 8), "1.1", col::text);
    pos->setFont (juce::Font (juce::FontOptions (16.f)));

    label (next (32, 0), "BPM");
    field (next (48, 8), juce::String (song.bpm, 1), [this] (const juce::String& s)
    {
        if (! ui.host.rolling()) { ui.host.song.bpm = juce::jlimit (30.0, 300.0, s.getDoubleValue()); ui.host.regionsChanged(); }
        ui.refresh();
    });
    label (next (26, 0), "Sig");
    field (next (38, 8), juce::String (song.num) + "/" + juce::String (song.den), [this] (const juce::String& s)
    {
        const int num = s.upToFirstOccurrenceOf ("/", false, false).getIntValue(), den = s.fromFirstOccurrenceOf ("/", false, false).getIntValue();
        if (! ui.host.rolling() && num >= 1 && num <= 32 && (den == 2 || den == 4 || den == 8 || den == 16))
        {
            ui.host.song.num = num;
            ui.host.song.den = den;
            ui.host.regionsChanged();
        }
        ui.refresh();
    });
    label (next (32, 0), "Bars");
    field (next (40, 8), juce::String (song.bars), [this] (const juce::String& s)
    {
        if (! ui.host.rolling()) { ui.host.song.bars = juce::jlimit (1, 2000, s.getIntValue()); ui.host.regionsChanged(); }
        ui.refresh();
    });
    label (next (36, 0), "Snap");
    combo (next (70, 8), snapNames, ui.snapIndex, [this] (int i) { ui.snapIndex = i; ui.refresh(); });
    bool clickOn = false;
    for (auto& t : song.tracks) clickOn |= t.type == Track::Click && t.power;
    toggle (next (44, 10), "Click", clickOn, col::loop, [this] (bool on)
    {
        for (auto& t : ui.host.song.tracks) if (t.type == Track::Click) t.power = on;      // same switch as the click track's P
        ui.host.paramsChanged();
        ui.refresh();
    });

    auto& addTrack = button (next (62, 10), "+ Track", {});
    addTrack.onClick = [this, &addTrack] { ui.addTrackMenu (&addTrack); };
    button (next (40), "New", [this] { ui.host.newSong(); });
    button (next (44), "Open", [this] { ui.openSong(); });
    button (next (44, 10), "Save", [this] { if (ui.host.song.folder == juce::File()) ui.saveSongAs(); else ui.host.saveSong (ui.host.song.folder); });
    button (next (46, 10), "Prefs", [this] { ui.showPrefs(); });
    cpu = &label ({ x, y, juce::jmax (0, getWidth() - x - 6), h }, {});
}

// ---------------------------------------------------------------- track headers

void Headers::rebuild()
{
    items.clear();
    auto& song = ui.host.song;
    levels.resize (song.tracks.size(), 0.f);
    const int th = ui.arrange.trackH();

    for (int i = 0; i < (int) song.tracks.size(); ++i)
    {
        auto& t = song.tracks[(size_t) i];
        const int id = t.id, y = ArrangeView::rulerH + i * th + (th - 22) / 2;

        auto& name = field ({ 12, y, t.hasLoops() ? 96 : 118, 22 }, t.name, [this, id] (const juce::String& s)
        {
            if (auto* tr = ui.host.song.track (id)) { tr->name = s; ui.host.dirty = true; ui.refresh(); }
        });
        name.setEditable (false, true, false);      // double click renames, single click selects the track
        name.addMouseListener (this, false);
        name.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        name.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

        if (t.isAutomation() || t.type == Track::Marker)
        {
            label ({ 132, y, 66, 22 }, t.type == Track::Command ? "command" : t.type == Track::Control ? "control" : "markers");
            continue;
        }
        if (! t.hasLoops()) label ({ 136, y, 62, 22 }, t.type == Track::Click ? "click" : "talkback");
        struct B { const char* text; bool on; juce::Colour c; int which; };
        const B bs[] = { { "P", t.power, col::play, 0 }, { "S", t.solo, col::captured, 1 }, { "A", t.audition, col::scripted, 2 }, { "L", t.locked, col::dim, 3 } };
        for (int k = 0; k < (t.hasLoops() ? 4 : 1); ++k)
        {
            auto& b = toggle ({ 110 + k * 22, y, 21, 22 }, bs[k].text, bs[k].on, bs[k].c, [this, id, k] (bool on)
            {
                auto* tr = ui.host.song.track (id);
                if (tr == nullptr) return;
                (k == 0 ? tr->power : k == 1 ? tr->solo : k == 2 ? tr->audition : tr->locked) = on;
                if (k == 3) ui.host.regionsChanged(); else ui.host.paramsChanged();
                if (! tr->hasLoops()) ui.refresh();     // the transport's Click button mirrors the click track
                ui.arrange.repaint();
            });
            const char* tips[] = { t.type == Track::Talkback ? "Talk" : "Power", "Solo", "Audition: hear the input when not recording", "Lock performances: Record does not overwrite this track" };
            b.setTooltip (tips[k]);
        }
    }
}

void Headers::paint (juce::Graphics& g)
{
    g.fillAll (col::panel);
    auto& song = ui.host.song;
    const int th = ui.arrange.trackH();
    for (int i = 0; i < (int) song.tracks.size(); ++i)
    {
        const juce::Rectangle<int> row (0, ArrangeView::rulerH + i * th + 1, getWidth(), th - 2);
        g.setColour (song.tracks[(size_t) i].id == ui.selTrack ? col::laneSel : col::lane);
        g.fillRect (row);
        g.setColour (trackColour (i));
        g.fillRect (row.withWidth (5));
        if (i < (int) levels.size() && ! song.tracks[(size_t) i].isAutomation() && song.tracks[(size_t) i].type != Track::Marker)
        {
            const auto m = row.withLeft (getWidth() - 8).reduced (1, 4).toFloat();
            g.setColour (col::bg);
            g.fillRect (m);
            const float db = juce::Decibels::gainToDecibels (levels[(size_t) i], -60.f);
            g.setColour (db > -1.f ? col::rec : col::play);
            g.fillRect (m.withTop (m.getBottom() - m.getHeight() * juce::jlimit (0.f, 1.f, (db + 60.f) / 60.f)));
        }
    }
}

void Headers::mouseDown (const juce::MouseEvent& e)
{
    ui.grabKeyboardFocus();
    const int i = (e.getEventRelativeTo (this).y - ArrangeView::rulerH) / ui.arrange.trackH();
    auto& tracks = ui.host.song.tracks;
    const bool onTrack = e.getEventRelativeTo (this).y >= ArrangeView::rulerH && i >= 0 && i < (int) tracks.size();
    if (onTrack) ui.select (tracks[(size_t) i].id, 0);
    if (! e.mods.isPopupMenu()) return;
    if (! onTrack) { ui.addTrackMenu (nullptr); return; }

    juce::PopupMenu m;      // right-click a track's name
    m.addItem (1, "Rename track...");
    m.addItem (2, "Add a track...");
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, id = tracks[(size_t) i].id] (int res)
    {
        if (res == 1) ui.renameTrack (id);
        else if (res == 2) ui.addTrackMenu (nullptr);
    });
}

// ---------------------------------------------------------------- track panel

void TrackPanel::rebuild()
{
    items.clear();
    auto& host = ui.host;
    auto* t = ui.track();
    const int w = getWidth() - 16;
    int y = 8;
    auto row = [&] (int h = 22, int gap = 6) { juce::Rectangle<int> r (8, y, w, h); y += h + gap; return r; };

    if (t == nullptr) { label (row(), "No track selected"); return; }
    const int id = t->id;
    const char* types[] = { "audio", "instrument", "command", "control", "click", "talkback", "markers" };
    {
        auto r = row (22);      // the name is editable right here
        label (r.removeFromRight (78), types[t->type]).setJustificationType (juce::Justification::centredRight);
        field (r, t->name, [this, id] (const juce::String& s)
        {
            if (auto* tr = ui.host.song.track (id)) { tr->name = s; ui.host.dirty = true; ui.refresh(); }
        });
    }
    if (host.deviceError.isNotEmpty())
    {
        auto& warn = label (row (76), host.deviceError, col::rec);
        warn.setJustificationType (juce::Justification::topLeft);
    }

    if (t->type == Track::Marker)
    {
        auto& hint = label (row (64), "Right-click the track to drop a marker. Double-click a marker to name it. The . key jumps to the next one.");
        hint.setJustificationType (juce::Justification::topLeft);
    }
    else if (t->isAutomation())
    {
        toggle (row (26), ui.learnTrack == id ? "Move a controller now..." : "Learn controller", ui.learnTrack == id, col::captured,
                [this, id] (bool on) { ui.learnTrack = on ? id : 0; ui.refresh(); });
        label (row (18, 2), t->sources.empty() ? "No controller learned yet" : "Listens to");
        for (size_t i = 0; i < t->sources.size(); ++i)
        {
            auto r = row (22, 3);
            const int s = t->sources[i];
            button (r.removeFromRight (26), "x", [this, id, i]
            {
                if (auto* tr = ui.host.song.track (id); tr != nullptr && i < tr->sources.size())
                {
                    tr->sources.erase (tr->sources.begin() + (int) i);
                    ui.host.dirty = true;
                    ui.refresh();
                }
            });
            label (r, s < 128 ? "CC " + juce::String (s) : "Note " + juce::String (s - 128), col::text);
        }
    }
    else
    {
        if (t->type == Track::Talkback)
        {
            auto& hint = label (row (48), "P on this track is the talk switch. A command track can put it on a pedal.");
            hint.setJustificationType (juce::Justification::topLeft);
        }
        if (t->type == Track::Audio || t->type == Track::Talkback)
        {
            label (row (16, 1), "Input");
            const auto names = host.channelNames (true);
            const int n = names.size();
            juce::StringArray opts;
            for (int i = 0; i < n; ++i) opts.add (juce::String (i + 1) + ": " + names[i]);
            for (int i = 0; i + 1 < n; ++i) opts.add (juce::String (i + 1) + "+" + juce::String (i + 2) + " stereo");
            combo (row(), opts, t->stereoIn ? n + t->input : t->input, [this, id, n] (int i)
            {
                if (auto* tr = ui.host.song.track (id)) { tr->stereoIn = i >= n; tr->input = i >= n ? i - n : i; ui.host.structureChanged(); }
            });
        }
        else if (t->type == Track::Instrument)
        {
            label (row (16, 1), "Instrument");
            auto r = row (24);
            const bool has = t->instrument.desc.name.isNotEmpty();
            auto& pick = button (r.removeFromRight (30), "...", {});
            pick.onClick = [this, &pick, id] { ui.pluginMenu (pick, id, 0, true); };
            auto& open = button (r.withTrimmedRight (4), has ? t->instrument.desc.name + (t->instrument.instance == nullptr ? " (missing)" : "") : juce::String ("Choose instrument"), {});
            open.onClick = [this, &open, id, has] { if (has) ui.host.showEditor (id, 0); else ui.pluginMenu (open, id, 0, true); };

            label (row (16, 1), "Played as");
            combo (row(), { "Keys", "Drum kit (pads, channel 10)" }, t->drums ? 1 : 0, [this, id] (int i)
            {
                if (auto* tr = ui.host.song.track (id)) { tr->drums = i == 1; ui.host.structureChanged(); }
            });

            label (row (16, 1), "MIDI in");
            juce::StringArray chans { "All channels" };
            for (int c = 1; c <= 16; ++c) chans.add ("Channel " + juce::String (c));
            combo (row(), chans, t->midiChannel, [this, id] (int i)
            {
                if (auto* tr = ui.host.song.track (id)) { tr->midiChannel = i; ui.host.structureChanged(); }
            });

            label (row (16, 1), "Record quantize");
            const double q[] = { 0, 1, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6 };
            int sel = 0;
            for (int i = 0; i < 7; ++i) if (std::abs (q[i] - t->quantize) < 1e-6) sel = i;
            combo (row(), { "Off", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T" }, sel, [this, id] (int i)
            {
                const double qv[] = { 0, 1, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6 };
                if (auto* tr = ui.host.song.track (id)) { tr->quantize = qv[juce::jlimit (0, 6, i)]; ui.host.paramsChanged(); }
            });
        }

        label (row (16, 1), "Effects");
        for (int i = 0; i < (int) t->fx.size(); ++i)
        {
            auto r = row (24, 3);
            auto& s = t->fx[(size_t) i];
            button (r.removeFromRight (24), "x", [this, id, i] { ui.host.removePlugin (id, i + 1); });
            toggle (r.removeFromRight (40).withTrimmedRight (3), "Byp", s.bypass, col::captured, [this, id, i] (bool on)
            {
                if (auto* tr = ui.host.song.track (id); tr != nullptr && i < (int) tr->fx.size()) { tr->fx[(size_t) i].bypass = on; ui.host.paramsChanged(); }
            });
            button (r.withTrimmedRight (3), s.desc.name + (s.instance == nullptr ? " (missing)" : ""), [this, id, i] { ui.host.showEditor (id, i + 1); });
        }
        auto& addFx = button (row (24, 8), "+ Add effect", {});
        addFx.onClick = [this, &addFx, id] { if (auto* tr = ui.host.song.track (id)) ui.pluginMenu (addFx, id, (int) tr->fx.size() + 1, false); };

        label (row (16, 1), "Output");
        const auto outs = host.channelNames (false);
        juce::StringArray pairs;
        for (int i = 0; i < outs.size(); i += 2) pairs.add (i + 1 < outs.size() ? juce::String (i + 1) + "+" + juce::String (i + 2) : juce::String (i + 1));
        combo (row(), pairs, t->output / 2, [this, id] (int i)
        {
            if (auto* tr = ui.host.song.track (id)) { tr->output = i * 2; ui.host.structureChanged(); }
        });

        label (row (16, 1), "Volume");
        slider (row(), -60, 12, t->gainDb, " dB", [this, id] (double v) { if (auto* tr = ui.host.song.track (id)) { tr->gainDb = (float) v; ui.host.paramsChanged(); } });
        label (row (16, 1), "Pan");
        slider (row (22, 12), -1, 1, t->pan, "", [this, id] (double v) { if (auto* tr = ui.host.song.track (id)) { tr->pan = (float) v; ui.host.paramsChanged(); } });
    }

    if (t->type != Track::Click) button (row (24), "Delete track", [this, id]
    {
        auto& h = ui.host;
        if (h.rolling()) h.stop();
        const int i = h.song.indexOf (id);
        if (i < 0) return;
        h.song.tracks.erase (h.song.tracks.begin() + i);
        ui.selTrack = h.song.tracks.empty() ? 0 : h.song.tracks[(size_t) juce::jmin (i, (int) h.song.tracks.size() - 1)].id;
        ui.selRegion = 0;
        ui.selection.clear();
        h.structureChanged();
    });
}

// ---------------------------------------------------------------- inspector

// The source record loop's waveform with the looped window on top. Drag the window or its edges.
struct SliceEditor : juce::Component
{
    SliceEditor (MainComponent& u, int regionId) : ui (u), id (regionId) {}

    bool fetch()
    {
        r = ui.host.song.region (id, &t);
        src = r != nullptr ? t->find (r->source) : nullptr;
        return src != nullptr;
    }
    double effLen() const { return juce::jmin (r->sliceLength > 0 ? r->sliceLength : 1e9, src->length - r->sliceStart); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        if (! fetch())
        {
            g.setColour (col::dim);
            g.drawText ("Pick a source record loop", getLocalBounds(), juce::Justification::centred);
            return;
        }
        auto& song = ui.host.song;
        const double ppb = getWidth() / src->length;
        for (double b = 0; b < src->length; b += 4.0 / song.den)
        {
            g.setColour (std::abs (std::remainder (b, song.beatsPerBar())) < 1e-6 ? col::gridBar : col::grid);
            g.fillRect ((float) (b * ppb), 0.f, 1.f, (float) getHeight());
        }
        const auto take = src->pending != nullptr ? src->pending : src->current;
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        if (take != nullptr) drawTake (g, getLocalBounds().toFloat().reduced (0, 4), *take, 0, (double) take->length, (double) take->length / getWidth());
        else g.drawText ("Nothing recorded in " + src->name + " yet. The slice still applies once it is.", getLocalBounds(), juce::Justification::centred);

        const float x0 = (float) (r->sliceStart * ppb), x1 = (float) ((r->sliceStart + effLen()) * ppb);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (0.f, 0.f, x0, (float) getHeight());
        g.fillRect (x1, 0.f, (float) getWidth() - x1, (float) getHeight());
        g.setColour (col::play);
        g.drawRect (juce::Rectangle<float> (x0, 0, x1 - x0, (float) getHeight()), 2.f);
        g.fillRect (x0, 0.f, 4.f, (float) getHeight());
        g.fillRect (x1 - 4.f, 0.f, 4.f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        mode = 0;
        if (! fetch()) return;
        ui.host.pushUndo();
        const double ppb = getWidth() / src->length, len = effLen();
        const float x0 = (float) (r->sliceStart * ppb), x1 = (float) ((r->sliceStart + len) * ppb);
        mode = std::abs (e.position.x - x1) < 7 ? 3 : std::abs (e.position.x - x0) < 7 ? 2 : 1;
        if (mode == 1 && (e.position.x < x0 || e.position.x > x1))     // click outside: bring the window here
        {
            r->sliceLength = len;
            r->sliceStart = juce::jlimit (0.0, src->length - len, snap (e.position.x / ppb - len / 2));
        }
        start0 = r->sliceStart;
        len0 = effLen();
        changed();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mode == 0 || ! fetch()) return;
        const double ppb = getWidth() / src->length, minLen = ui.snapBeats > 0 ? ui.snapBeats : 1.0 / 64;
        const double d = e.getDistanceFromDragStartX() / ppb, end0 = start0 + len0;
        if (mode == 1) { r->sliceStart = juce::jlimit (0.0, src->length - len0, snap (start0 + d)); r->sliceLength = len0; }
        else if (mode == 2) { r->sliceStart = juce::jlimit (0.0, end0 - minLen, snap (start0 + d)); r->sliceLength = end0 - r->sliceStart; }
        else r->sliceLength = juce::jlimit (minLen, src->length - start0, snap (end0 + d) - start0);
        changed();
    }

    void mouseUp (const juce::MouseEvent&) override { if (mode != 0) ui.refresh(); }

    double snap (double b) const { return ui.snapBeats > 0 ? std::round (b / ui.snapBeats) * ui.snapBeats : b; }
    void changed() { ui.host.regionsChanged(); ui.arrange.repaint(); repaint(); }

    MainComponent& ui;
    int id, mode = 0;
    double start0 = 0, len0 = 0;
    Track* t = nullptr;
    Region* r = nullptr;
    const Region* src = nullptr;
};

float curveAt (const Region& r, float x)
{
    if (r.curve.empty()) return 0.f;
    if (x <= r.curve.front().x) return r.curve.front().y;
    for (size_t i = 1; i < r.curve.size(); ++i)
        if (x <= r.curve[i].x)
        {
            const auto a = r.curve[i - 1], b = r.curve[i];
            return b.x - a.x < 1e-6f ? b.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
        }
    return r.curve.back().y;
}

// Click to add a point, drag to move it, alt-click to remove it.
struct CurveEditor : juce::Component
{
    CurveEditor (MainComponent& u, int regionId) : ui (u), id (regionId) {}

    juce::Point<float> toScreen (juce::Point<float> p) const { return { p.x * (float) getWidth(), 5.f + (1.f - p.y) * (float) (getHeight() - 10) }; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::grid);
        for (int i = 1; i < 4; ++i) g.fillRect (0.f, (float) getHeight() * (float) i / 4.f, (float) getWidth(), 1.f);
        auto* r = ui.host.song.region (id);
        if (r == nullptr) return;
        juce::Path p;
        for (size_t i = 0; i < r->curve.size(); ++i) { if (i == 0) p.startNewSubPath (toScreen (r->curve[i])); else p.lineTo (toScreen (r->curve[i])); }
        g.setColour (col::scripted);
        g.strokePath (p, juce::PathStrokeType (2.f));
        for (auto pt : r->curve) g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre (toScreen (pt)));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        index = -1;
        auto* r = ui.host.song.region (id);
        if (r == nullptr) return;
        ui.host.pushUndo();
        for (size_t i = 0; i < r->curve.size(); ++i) if (toScreen (r->curve[i]).getDistanceFrom (e.position) < 8) index = (int) i;
        if (index >= 0 && e.mods.isAltDown())
        {
            if (r->curve.size() > 2) r->curve.erase (r->curve.begin() + index);
            index = -1;
        }
        else if (index < 0)
        {
            const juce::Point<float> p (e.position.x / (float) getWidth(), juce::jlimit (0.f, 1.f, 1.f - (e.position.y - 5.f) / (float) (getHeight() - 10)));
            size_t at = 0;
            while (at < r->curve.size() && r->curve[at].x < p.x) ++at;
            r->curve.insert (r->curve.begin() + (int) at, p);
            index = (int) at;
        }
        ui.host.dirty = true;
        repaint();
        ui.arrange.repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* r = ui.host.song.region (id);
        if (r == nullptr || index < 0 || index >= (int) r->curve.size()) return;
        const float lo = index > 0 ? r->curve[(size_t) index - 1].x : 0.f;
        const float hi = index + 1 < (int) r->curve.size() ? r->curve[(size_t) index + 1].x : 1.f;
        r->curve[(size_t) index] = { juce::jlimit (lo, hi, e.position.x / (float) getWidth()), juce::jlimit (0.f, 1.f, 1.f - (e.position.y - 5.f) / (float) (getHeight() - 10)) };
        repaint();
        ui.arrange.repaint();
    }

    MainComponent& ui;
    int id, index = -1;
};

juce::String targetText (Song& song, const Target& tg)
{
    switch (tg.type)
    {
        case Target::None:     return "no target";
        case Target::Play:     return "Transport: Play";
        case Target::Record:   return "Transport: Record";
        case Target::Panic:    return "Transport: Panic";
        case Target::Position: return "Transport: Jump to bar";
        default: break;
    }
    auto* t = song.track (tg.trackId);
    if (t == nullptr) return "missing track";
    if (tg.type == Target::TrackPower) return t->name + ": Power";
    if (tg.type == Target::TrackVolume) return t->name + ": Volume";
    auto* s = t->slot (tg.slot);
    if (s == nullptr) return t->name + ": missing plugin";
    if (tg.type == Target::PluginBypass) return t->name + ": " + s->desc.name + " on/off";
    juce::String param = "param " + juce::String (tg.param + 1);
    if (s->instance != nullptr && tg.param < s->instance->getParameters().size()) param = s->instance->getParameters()[tg.param]->getName (40);
    return t->name + ": " + s->desc.name + " / " + param;
}

void Inspector::rebuild()
{
    items.clear();
    auto& host = ui.host;
    Track* t = nullptr;
    auto* r = host.song.region (ui.selRegion, &t);
    const int W = getWidth();
    if (r == nullptr) return;       // nothing selected: the inspector is hidden and the timeline gets the space
    const int id = r->id;
    auto edit = [this, id] (std::function<void (Region&)> fn)
    {
        return [this, id, fn]
        {
            if (auto* reg = ui.host.song.region (id)) { ui.host.pushUndo(); fn (*reg); ui.host.regionsChanged(); ui.refresh(); ui.arrange.repaint(); }
        };
    };
    const juce::String where = "bar " + barBeat (host.song, r->start) + ", " + juce::String (r->length, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " beats";

    if (r->kind == Region::Marker)
    {
        label ({ 12, 10, 60, 22 }, "Marker", col::text);
        field ({ 76, 10, 220, 22 }, r->name, [this, id] (const juce::String& s)
        {
            if (auto* reg = ui.host.song.region (id)) { reg->name = s; ui.host.dirty = true; ui.arrange.repaint(); }
        });
        label ({ 308, 10, 300, 22 }, where);
    }
    else if (r->kind == Region::Record)
    {
        label ({ 12, 10, 84, 22 }, "Record loop", col::rec);
        field ({ 100, 10, 170, 22 }, r->name, [this, id] (const juce::String& s)
        {
            if (auto* reg = ui.host.song.region (id)) { reg->name = s; ui.host.dirty = true; ui.arrange.repaint(); }
        });
        label ({ 282, 10, 300, 22 }, where);

        juce::StringArray names;
        int cur = -1;
        for (size_t i = 0; i < r->takes.size(); ++i)
        {
            names.add ("Take " + juce::String (i + 1) + (r->takes[i] == r->reference ? "  (reference)" : ""));
            if (r->takes[i] == r->current) cur = (int) i;
        }
        label ({ 12, 44, 84, 22 }, "Take");
        combo ({ 100, 44, 170, 22 }, names, cur, [this, id] (int i)
        {
            if (auto* reg = ui.host.song.region (id); reg != nullptr && i >= 0 && i < (int) reg->takes.size())
            {
                reg->current = reg->takes[(size_t) i];
                ui.host.regionsChanged();
                ui.arrange.repaint();
            }
        });
        button ({ 282, 44, 130, 22 }, "Set as reference", edit ([] (Region& reg) { reg.reference = reg.current; })).setEnabled (r->current != nullptr);
        button ({ 418, 44, 130, 22 }, "Use reference", edit ([] (Region& reg) { reg.current = reg.reference; })).setEnabled (r->reference != nullptr);
        button ({ 554, 44, 90, 22 }, "Empty", edit ([] (Region& reg) { reg.takes.clear(); reg.current = nullptr; }));
        label ({ 12, 78, W - 24, 20 }, r->reference != nullptr
                   ? juce::String ("Reference take is set. Panic (P or a pedal) swaps it in during a performance.")
                   : juce::String ("No reference take. Set one from a good take to make Panic work for this loop."));
        label ({ 12, 100, W - 24, 20 }, "The last 8 takes are kept. Lock the track (L) to rehearse without overwriting.");
    }
    else if (r->kind == Region::Play)
    {
        label ({ 12, 10, 70, 22 }, "Play loop", col::play);
        juce::StringArray names;
        std::vector<int> ids;
        for (auto& o : t->regions) if (o.kind == Region::Record) { names.add (o.name); ids.push_back (o.id); }
        const int sel = (int) (std::find (ids.begin(), ids.end(), r->source) - ids.begin());
        combo ({ 84, 10, 150, 22 }, names, sel < (int) ids.size() ? sel : -1, [this, id, ids] (int i)
        {
            if (auto* reg = ui.host.song.region (id); reg != nullptr && i >= 0 && i < (int) ids.size())
            {
                ui.host.pushUndo();
                reg->source = ids[(size_t) i];
                ui.host.regionsChanged();
                ui.refresh();
                ui.arrange.repaint();
            }
        });

        const auto* src = t->find (r->source);
        const double srcLen = src != nullptr ? src->length : 0.0;
        auto eff = [srcLen] (const Region& reg) { return juce::jmin (reg.sliceLength > 0 ? reg.sliceLength : 1e9, srcLen - reg.sliceStart); };
        int x = 246;
        auto next = [&] (int w) { juce::Rectangle<int> b (x, 10, w, 22); x += w + 4; return b; };
        button (next (56), "Whole", edit ([] (Region& reg) { reg.sliceStart = 0; reg.sliceLength = 0; }));
        button (next (48), "Half", edit ([eff] (Region& reg) { reg.sliceLength = juce::jmax (1.0 / 64, eff (reg) / 2); }));
        button (next (58), "Double", edit ([eff, srcLen] (Region& reg) { reg.sliceLength = juce::jmin (eff (reg) * 2, srcLen - reg.sliceStart); }));
        button (next (48), "Prev", edit ([eff] (Region& reg) { const double l = eff (reg); reg.sliceLength = l; reg.sliceStart = juce::jmax (0.0, reg.sliceStart - l); }));
        button (next (48), "Next", edit ([eff, srcLen] (Region& reg)
        {
            const double l = eff (reg);
            reg.sliceLength = l;
            reg.sliceStart = reg.sliceStart + l < srcLen - 1e-9 ? juce::jmin (reg.sliceStart + l, srcLen - l) : 0.0;
        }));
        if (src != nullptr)
            label ({ x + 8, 10, W - x - 20, 22 }, "Slice starts at " + barBeat (host.song, r->sliceStart) + " of " + src->name + ", "
                       + juce::String (eff (*r), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " beats long. Loop: " + where, col::text);
        add<SliceEditor> ({ 12, 40, W - 24, getHeight() - 48 }, ui, id);
    }
    else
    {
        const bool command = t->type == Track::Command;
        label ({ 12, 10, 110, 22 }, command ? "Command region" : "Control region", r->captured ? col::captured : col::scripted);
        combo ({ 124, 10, 210, 22 }, { "Scripted: runs by itself", "Captured: you perform it" }, r->captured ? 1 : 0, [this, id] (int i)
        {
            if (auto* reg = ui.host.song.region (id)) { reg->captured = i == 1; ui.host.dirty = true; ui.refresh(); ui.arrange.repaint(); }
        });
        label ({ 346, 10, 46, 22 }, "Target");
        auto& tb = button ({ 394, 10, 300, 22 }, targetText (host.song, r->target), {});
        tb.onClick = [this, &tb, id] { ui.targetMenu (tb, id); };
        toggle ({ 704, 10, 110, 22 }, "Revert on exit", r->revert, col::scripted, [this, id] (bool on)
        {
            if (auto* reg = ui.host.song.region (id)) { reg->revert = on; ui.host.dirty = true; }
        });
        label ({ 826, 10, W - 838, 22 }, where);

        if (command)
        {
            label ({ 12, 44, 50, 22 }, "Action");
            combo ({ 64, 44, 150, 22 }, { "On / set value", "Off", "Toggle" }, r->action, [this, id] (int i)
            {
                if (auto* reg = ui.host.song.region (id)) { reg->action = i; ui.host.dirty = true; }
            });
            label ({ 226, 44, 44, 22 }, "Value");
            field ({ 270, 44, 60, 22 }, juce::String (r->value), [this, id] (const juce::String& s)
            {
                if (auto* reg = ui.host.song.region (id)) { reg->value = s.getFloatValue(); ui.host.dirty = true; }
            });
            label ({ 338, 44, 330, 22 }, "0 to 1 for volume and parameters, the bar number for Jump");
            if (r->captured)
            {
                label ({ 12, 78, 50, 22 }, "Trigger");
                combo ({ 64, 78, 150, 22 }, { "Immediately", "Next beat", "Next bar", "End of region" }, r->trigger, [this, id] (int i)
                {
                    if (auto* reg = ui.host.song.region (id)) { reg->trigger = i; ui.host.dirty = true; }
                });
                label ({ 226, 78, W - 240, 22 }, "Fires when the learned pedal or button is pressed inside this region.");
            }
            else label ({ 12, 78, W - 24, 22 }, "Fires by itself when the playhead enters this region.");
        }
        else if (r->captured)
        {
            label ({ 12, 44, 40, 22 }, "Min");
            slider ({ 52, 44, 240, 22 }, 0, 1, r->lo, "", [this, id] (double v) { if (auto* reg = ui.host.song.region (id)) { reg->lo = (float) v; ui.host.dirty = true; } });
            label ({ 304, 44, 40, 22 }, "Max");
            slider ({ 344, 44, 240, 22 }, 0, 1, r->hi, "", [this, id] (double v) { if (auto* reg = ui.host.song.region (id)) { reg->hi = (float) v; ui.host.dirty = true; } });
            label ({ 12, 78, W - 24, 22 }, "The learned fader or knob drives the target inside this region, within this range. Swap Min and Max to invert it.");
        }
        else add<CurveEditor> ({ 12, 40, W - 24, getHeight() - 48 }, ui, id);
    }
}

// ---------------------------------------------------------------- drum pads

// The General MIDI kit on the computer keyboard, laid out like a kit: cymbals on the number row, hats on Q W E,
// toms on A S D, kick and snare on Z X. On a drum track these keys win over the Z and Q shortcuts (Cmd-Z still undoes).
const DrumKey drumMap[12] = {
    { '`', 49, "Crash L" }, { '1', 57, "Crash R" },   { '2', 51, "Ride" },     { '3', 52, "China" },
    { 'q', 42, "Hat" },     { 'w', 44, "Pedal hat" }, { 'e', 46, "Open hat" },
    { 'a', 50, "High tom" }, { 's', 47, "Mid tom" },  { 'd', 45, "Low tom" },
    { 'z', 36, "Kick" },    { 'x', 38, "Snare" },
};

struct DrumPads : Panel
{
    using Panel::Panel;
    void rebuild() override
    {
        items.clear();
        const int n = (int) std::size (drumMap), w = (getWidth() - 16) / n;
        for (int i = 0; i < n; ++i)
        {
            const auto& d = drumMap[i];
            auto& b = button ({ 8 + i * w, 6, w - 4, getHeight() - 12 }, juce::String::charToString ((juce::juce_wchar) d.key).toUpperCase() + "   " + d.name, {});
            b.onStateChange = [this, &b, note = d.note, was = false] () mutable
            {
                if (b.isDown() != was) { was = b.isDown(); ui.drum (note, was); }
            };
        }
    }
};

std::unique_ptr<Panel> makeDrumPads (MainComponent& ui) { return std::make_unique<DrumPads> (ui); }
