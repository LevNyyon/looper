#include "Ui.h"

juce::Colour trackColour (int index)
{
    return juce::Colour::fromHSV (std::fmod (0.58f + (float) index * 0.137f, 1.f), 0.55f, 0.85f, 1.f);
}

juce::String barBeat (const Song& s, double beat)
{
    const double bpb = s.beatsPerBar();
    const int bar = (int) std::floor (beat / bpb + 1e-9);
    const double inBar = (beat - bar * bpb) * s.den / 4.0;
    return juce::String (bar + 1) + "." + juce::String (std::round ((inBar + 1) * 100.0) / 100.0);
}

// Column x shows the take from sliceStart + (offset mod sliceLen), so a looped slice draws as it sounds.
// ponytail: scans samples on every paint, limited to the dirty area. Cache peaks if painting gets slow.
void drawTake (juce::Graphics& g, juce::Rectangle<float> area, const alk::Take& t, double sliceStart, double sliceLen, double spp)
{
    const auto clip = g.getClipBounds().toFloat().getIntersection (area);
    if (clip.isEmpty() || sliceLen < 1 || spp <= 0) return;

    if (! t.midi.empty())
    {
        const int n = t.midiCount;
        const double total = area.getWidth() * spp;
        if (sliceLen * 2000 < total) return;
        for (double base = 0; base < total; base += sliceLen)
            for (int i = 0; i < n; ++i)
            {
                auto& e = t.midi[(size_t) i];
                if ((e.data[0] & 0xF0) != 0x90 || e.data[2] == 0 || e.time < sliceStart || e.time >= sliceStart + sliceLen) continue;
                double end = sliceStart + sliceLen;
                for (int j = i + 1; j < n; ++j)
                {
                    auto& o = t.midi[(size_t) j];
                    if (o.data[1] == e.data[1] && ((o.data[0] & 0xF0) == 0x80 || o.data[2] == 0)) { end = juce::jmin (end, (double) o.time); break; }
                }
                const float x0 = area.getX() + (float) ((base + (double) e.time - sliceStart) / spp);
                const float x1 = juce::jmin (area.getRight(), area.getX() + (float) ((base + end - sliceStart) / spp));
                const float y = area.getBottom() - juce::jlimit (0.f, 1.f, (float) (e.data[1] - 24) / 84.f) * area.getHeight();
                if (x0 < area.getRight()) g.fillRect (x0, y - 1.5f, juce::jmax (2.f, x1 - x0), 3.f);
            }
        return;
    }

    const float mid = area.getCentreY(), half = area.getHeight() * 0.5f;
    for (float x = std::floor (clip.getX()); x < clip.getRight(); x += 1.f)
    {
        const double o = std::fmod (juce::jmax (0.0, (double) (x - area.getX())) * spp, sliceLen);
        const auto a = juce::jlimit<juce::int64> (0, t.length, (juce::int64) (sliceStart + o));
        const auto b = juce::jlimit<juce::int64> (a, t.length, (juce::int64) juce::jmin (sliceStart + sliceLen, sliceStart + o + spp) + 1);
        const auto step = juce::jmax<juce::int64> (1, (b - a) / 48);
        float lo = 0, hi = 0;
        for (auto i = a; i < b; i += step) { const float v = t.L[(size_t) i]; lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
        lo = juce::jmax (-1.f, lo);
        hi = juce::jmin (1.f, hi);
        g.fillRect (x, mid - hi * half, 1.f, juce::jmax (1.f, (hi - lo) * half));
    }
}

ArrangeView::ArrangeView (MainComponent& u) : ui (u) {}

int ArrangeView::trackH() const
{
    const int n = juce::jmax (1, (int) ui.host.song.tracks.size());
    return juce::jlimit (36, 84, (getHeight() - rulerH) / n);      // ponytail: no vertical scrolling yet, rows shrink to 36 px
}

double ArrangeView::snap (double beat) const
{
    return ui.snapBeats > 0 ? std::round (beat / ui.snapBeats) * ui.snapBeats : beat;
}

juce::Rectangle<float> ArrangeView::boundsOf (int ti, const Region& r) const
{
    return { xOf (r.start), (float) (rulerH + ti * trackH() + 2), (float) (r.length * pxPerBeat), (float) (trackH() - 4) };
}

void ArrangeView::playheadMoved()
{
    const float x = xOf (ui.host.posBeats());
    if (ui.host.rolling() && (x > (float) getWidth() - 40 || x < 0))    // follow the playhead
    {
        scroll = juce::jmax (0.0, ui.host.posBeats() - 2);
        repaint();
    }
    else    // only the strip the playhead crossed: that is also where a take being recorded grew
    {
        const float a = juce::jmin (lastHeadX, x) - 3, b = juce::jmax (lastHeadX, x) + 3;
        if (b - a < 200) repaint (juce::Rectangle<float> (a, 0, b - a, (float) getHeight()).toNearestInt());
        else repaint();
    }
    lastHeadX = x;
}

void ArrangeView::paint (juce::Graphics& g)
{
    auto& host = ui.host;
    auto& song = host.song;
    const auto clip = g.getClipBounds().toFloat();
    const int th = trackH();
    const double bpb = song.beatsPerBar();
    g.fillAll (col::bg);

    for (int ti = 0; ti < (int) song.tracks.size(); ++ti)
    {
        g.setColour (song.tracks[(size_t) ti].id == ui.selTrack ? col::laneSel : col::lane);
        g.fillRect (0, rulerH + ti * th + 1, getWidth(), th - 2);
    }

    // grid and ruler
    const float endX = xOf (song.lengthBeats());
    const double sub = ui.snapBeats > 0 && ui.snapBeats * pxPerBeat >= 9 ? ui.snapBeats : (pxPerBeat >= 9 ? 1.0 : bpb);
    g.setFont (11.f);
    for (double b = std::floor (beatAt (clip.getX()) / sub) * sub; b <= song.lengthBeats() + 1e-9 && xOf (b) <= clip.getRight(); b += sub)
    {
        if (b < 0) continue;
        const float x = xOf (b);
        const bool isBar = std::abs (std::remainder (b, bpb)) < 1e-6;
        const bool isBeat = std::abs (std::remainder (b, 4.0 / song.den)) < 1e-6;
        g.setColour (isBar ? col::gridBar : isBeat ? col::grid : col::grid.withAlpha (0.45f));
        g.fillRect (x, (float) (isBar ? 0 : rulerH), 1.f, (float) getHeight());
        const int bar = (int) std::round (b / bpb);
        if (isBar && (pxPerBeat * bpb >= 34 || bar % 4 == 0))
        {
            g.setColour (col::dim);
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 4, 0, 40, (float) rulerH), juce::Justification::centredLeft);
        }
    }
    g.setColour (col::bg.withAlpha (0.6f));
    g.fillRect (endX, 0.f, (float) getWidth() - endX, (float) getHeight());     // past the song end

    if (host.loopB > host.loopA)
    {
        const juce::Rectangle<float> r (xOf (host.loopA), 0, (float) ((host.loopB - host.loopA) * pxPerBeat), (float) getHeight());
        g.setColour (col::loop.withAlpha (0.10f));
        g.fillRect (r);
        g.setColour (col::loop.withAlpha (0.8f));
        g.fillRect (r.withHeight (4));
    }

    const double spb = host.spb(), spp = spb / pxPerBeat;
    for (int ti = 0; ti < (int) song.tracks.size(); ++ti)
    {
        auto& t = song.tracks[(size_t) ti];
        const auto base = trackColour (ti);
        for (auto& r : t.regions)
        {
            const auto rect = boundsOf (ti, r);
            if (! rect.intersects (clip)) continue;
            const bool sel = ui.isSelected (r.id);
            const auto border = r.kind == Region::Record ? (t.locked ? base : col::rec)
                              : r.kind == Region::Play ? col::play
                              : r.captured ? col::captured : col::scripted;
            g.setColour (base.withMultipliedBrightness (sel ? 0.62f : 0.42f).withMultipliedSaturation (0.8f));
            g.fillRoundedRectangle (rect, 4.f);

            auto inner = rect.reduced (3.f, 3.f);
            inner.removeFromTop (12.f);
            juce::String text;
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            if (r.kind == Region::Record)
            {
                const auto take = r.pending != nullptr ? r.pending : r.current;
                if (take != nullptr) drawTake (g, inner, *take, 0, (double) take->length, (double) take->length / juce::jmax (1.f, inner.getWidth()));
                text = r.name + (r.pending != nullptr ? "  rec" : take == nullptr ? "  (empty)" : "")
                     + (r.reference != nullptr && r.current == r.reference ? "  ref" : "");
            }
            else if (r.kind == Region::Play)
            {
                const auto* src = t.find (r.source);
                text = src != nullptr ? src->name : juce::String ("no source");
                if (r.sliceStart > 0 || r.sliceLength > 0)
                    text << "  [" << barBeat (song, r.sliceStart) << (r.sliceLength > 0 ? " +" + juce::String (r.sliceLength, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") : juce::String()) << "]";
                const auto take = src == nullptr ? nullptr : src->pending != nullptr ? src->pending : src->current;
                if (take != nullptr)
                {
                    const double s = r.sliceStart * spb;
                    const double len = juce::jmin (r.sliceLength > 0 ? r.sliceLength * spb : 1e18, (double) take->length - s);
                    drawTake (g, inner, *take, s, len, spp);
                    g.setColour (col::play.withAlpha (0.5f));       // where the slice repeats
                    if (len / spp >= 5)
                        for (double x = len / spp; x < inner.getWidth(); x += len / spp)
                            g.fillRect (inner.getX() + (float) x, rect.getY() + 2, 1.f, rect.getHeight() - 4);
                }
            }
            else
            {
                text = targetText (song, r.target);
                if (t.type == Track::Command) text << (r.captured ? "  (pedal)" : "  (auto)");
                else if (! r.captured && r.curve.size() > 1)
                {
                    juce::Path p;
                    for (size_t i = 0; i < r.curve.size(); ++i)
                    {
                        const juce::Point<float> pt (inner.getX() + r.curve[i].x * inner.getWidth(), inner.getBottom() - r.curve[i].y * inner.getHeight());
                        if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
                    }
                    g.strokePath (p, juce::PathStrokeType (1.5f));
                }
            }

            g.setColour (border);
            g.drawRoundedRectangle (rect.reduced (0.75f), 4.f, sel ? 2.5f : 1.5f);
            g.setColour (col::text);
            g.drawText (text, rect.reduced (5.f, 2.f).withHeight (13.f), juce::Justification::centredLeft, false);
        }
    }

    if (drag == Band)
    {
        const juce::Rectangle<float> band (bandStart, bandEnd);
        g.setColour (col::loop.withAlpha (0.15f));
        g.fillRect (band);
        g.setColour (col::loop);
        g.drawRect (band, 1.f);
    }

    const float hx = xOf (host.posBeats());
    g.setColour (host.recording() ? col::rec : col::head);
    g.fillRect (hx - 0.5f, 0.f, 1.5f, (float) getHeight());
}

// ---------------------------------------------------------------- mouse

void ArrangeView::mouseDown (const juce::MouseEvent& e)
{
    ui.grabKeyboardFocus();
    auto& song = ui.host.song;
    const double beat = beatAt (e.position.x);
    drag = None;
    moved = created = copying = false;
    items.clear();

    if (e.position.y < rulerH) { drag = Loop; dragBeat0 = snap (beat); return; }

    auto startBand = [&]    // a plain drag on empty space selects everything it touches
    {
        drag = Band;
        bandStart = bandEnd = e.position;
        bandBase = e.mods.isShiftDown() ? ui.selection : std::vector<int>();
        ui.selection = bandBase;
        ui.selRegion = bandBase.empty() ? 0 : bandBase.back();
        ui.refresh();
    };

    const int ti = (int) (e.position.y - rulerH) / trackH();
    if (ti < 0 || ti >= (int) song.tracks.size())       // below the last track
    {
        if (e.mods.isPopupMenu()) ui.addTrackMenu (nullptr); else startBand();
        return;
    }
    auto& t = song.tracks[(size_t) ti];

    Region* hit = nullptr;
    for (auto& r : t.regions) if (beat >= r.start && beat < r.start + r.length) hit = &r;
    if (e.mods.isPopupMenu()) { contextMenu (t, hit, beat); return; }

    const double minLen = ui.snapBeats > 0 ? ui.snapBeats : 0.25;
    if (hit == nullptr)
    {
        const bool wantRec = e.mods.isCommandDown(), wantPlay = e.mods.isAltDown();
        if ((! wantRec && ! wantPlay) || ! (t.hasLoops() || t.isAutomation()))
        {
            ui.selTrack = t.id;
            startBand();
            return;
        }

        ui.host.pushUndo();
        const double start = ui.snapBeats > 0 ? std::floor (beat / ui.snapBeats) * ui.snapBeats : beat;
        auto& r = song.addRegion (t, t.isAutomation() ? Region::Auto : wantRec ? Region::Record : Region::Play, start, minLen);
        r.captured = wantRec;           // automation tracks: record pencil = performed live, play pencil = scripted
        orig = r;
        created = true;
        drag = ResizeR;
        dragTrack = t.id;
        dragId = r.id;
        dragBeat0 = beat;
        ui.select (t.id, r.id);
        return;
    }

    if (e.mods.isShiftDown())       // shift-click adds or removes one region
    {
        if (ui.isSelected (hit->id)) ui.selection.erase (std::remove (ui.selection.begin(), ui.selection.end(), hit->id), ui.selection.end());
        else ui.selection.push_back (hit->id);
        ui.selRegion = ui.selection.empty() ? 0 : ui.selection.back();
        ui.selTrack = t.id;
        ui.refresh();
        return;
    }

    if (! ui.isSelected (hit->id)) ui.select (t.id, hit->id);       // a click on an unselected region selects just it
    else { ui.selRegion = hit->id; ui.selTrack = t.id; ui.refresh(); }     // a click inside a group keeps the group

    orig = *hit;
    dragTrack = t.id;
    dragId = hit->id;
    dragBeat0 = beat;
    dragTrackIndex0 = ti;
    const float x0 = xOf (hit->start), x1 = xOf (hit->start + hit->length);
    drag = e.position.x > x1 - 6 ? ResizeR : e.position.x < x0 + 6 ? ResizeL : Move;
    copying = drag == Move && e.mods.isAltDown();
    if (drag == Move)
        for (int i = 0; i < (int) song.tracks.size(); ++i)
            for (auto& r : song.tracks[(size_t) i].regions)
                if (ui.isSelected (r.id)) items.push_back ({ r, i });
}

// Takes the dragged regions out of whatever track they are in now, and optionally back where they started.
void ArrangeView::putBack (bool originals)
{
    auto& tracks = ui.host.song.tracks;
    for (auto& it : items)
        for (auto& t : tracks)
            t.regions.erase (std::remove_if (t.regions.begin(), t.regions.end(), [&] (const Region& r) { return r.id == it.orig.id; }), t.regions.end());
    if (originals) for (auto& it : items) tracks[(size_t) it.trackIndex].regions.push_back (it.orig);
}

void ArrangeView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == None) return;
    auto& host = ui.host;
    auto& song = host.song;
    const double beat = beatAt (e.position.x);

    if (drag == Loop)
    {
        const double a = juce::jmax (0.0, juce::jmin (dragBeat0, snap (beat))), b = juce::jmax (dragBeat0, snap (beat));
        if (b > a) { host.setLoop (a, b); moved = true; repaint(); }
        return;
    }

    if (drag == Band)
    {
        bandEnd = e.position;
        const juce::Rectangle<float> band (bandStart, bandEnd);
        ui.selection = bandBase;
        for (int i = 0; i < (int) song.tracks.size(); ++i)
            for (auto& r : song.tracks[(size_t) i].regions)
                if (boundsOf (i, r).intersects (band) && ! ui.isSelected (r.id)) ui.selection.push_back (r.id);
        ui.selRegion = ui.selection.empty() ? 0 : ui.selection.back();
        repaint();
        return;
    }

    if (drag == Move)       // the whole selection moves together, in time and across tracks of the same kind
    {
        if (! moved)
        {
            if (e.getDistanceFromDragStart() < 4) return;
            host.pushUndo();
            if (copying)    // alt-drag leaves the originals and moves copies. A copied recording starts empty.
            {
                std::map<int, int> newId;
                for (auto& it : items) newId[it.orig.id] = song.nextId++;
                for (auto& it : items)
                {
                    auto& c = it.orig;
                    if (c.id == orig.id) orig.id = newId[c.id];
                    c.id = newId[c.id];
                    c.takes.clear();
                    c.current = c.reference = c.pending = nullptr;
                    if (c.kind == Region::Record) c.name += " copy";
                    if (newId.count (c.source) != 0) c.source = newId[c.source];    // a copied play region follows its copied recording
                }
                ui.selection.clear();
                for (auto& it : items) ui.selection.push_back (it.orig.id);
                ui.selRegion = orig.id;
            }
            moved = true;
        }

        double d = snap (orig.start + beat - dragBeat0) - orig.start;
        for (auto& it : items) d = juce::jmax (d, -it.orig.start);
        int dt = juce::jlimit (0, (int) song.tracks.size() - 1, (int) (e.position.y - rulerH) / trackH()) - dragTrackIndex0;
        for (auto& it : items)
        {
            const int target = it.trackIndex + dt;
            if (target < 0 || target >= (int) song.tracks.size() || song.tracks[(size_t) target].type != song.tracks[(size_t) it.trackIndex].type) dt = 0;
        }

        putBack (false);
        for (auto& it : items)
        {
            Region r = it.orig;
            r.start += d;
            song.tracks[(size_t) (it.trackIndex + dt)].regions.push_back (r);
        }
        ui.selTrack = song.tracks[(size_t) (dragTrackIndex0 + dt)].id;
        repaint();
        return;
    }

    auto* t = song.track (dragTrack);
    auto* r = t != nullptr ? t->find (dragId) : nullptr;
    if (r == nullptr) return;
    if (! moved)
    {
        if (! created && std::abs (e.getDistanceFromDragStartX()) < 3) return;
        if (! created) host.pushUndo();
        moved = true;
    }

    const double minLen = ui.snapBeats > 0 ? ui.snapBeats : 0.25;
    const double d = beat - dragBeat0, origEnd = orig.start + orig.length;
    if (drag == ResizeR) r->length = juce::jmax (minLen, snap (created ? beat : origEnd + d) - r->start);
    else
    {
        r->start = juce::jlimit (0.0, origEnd - minLen, snap (orig.start + d));
        r->length = origEnd - r->start;
    }
    repaint();
}

void ArrangeView::mouseUp (const juce::MouseEvent& e)
{
    auto& host = ui.host;
    auto& song = host.song;
    if (drag == Loop && ! moved) host.jump (snap (beatAt (e.position.x)));
    else if (drag == Band) ui.refresh();
    else if (drag == Move && moved)
    {
        bool ok = true;     // every moved region must land in free space, or the whole group goes back
        for (auto& it : items)
        {
            Track* t = nullptr;
            auto* r = song.region (it.orig.id, &t);
            ok &= r != nullptr && song.fits (*t, *r);
        }
        if (! ok)
        {
            putBack (! copying);
            if (copying) { ui.selection.clear(); ui.selRegion = 0; }
        }
        host.regionsChanged();
        ui.refresh();
    }
    else if ((drag == ResizeL || drag == ResizeR) && (moved || created))
    {
        auto* t = song.track (dragTrack);
        auto* r = t != nullptr ? t->find (dragId) : nullptr;
        if (r != nullptr && ! song.fits (*t, *r))       // landed on a neighbour: put it back
        {
            if (created)
            {
                t->regions.erase (t->regions.begin() + (r - t->regions.data()));
                ui.select (t->id, 0);
            }
            else *r = orig;
        }
        host.regionsChanged();
        ui.refresh();
    }
    drag = None;
    items.clear();
    repaint();
}

void ArrangeView::mouseMove (const juce::MouseEvent& e)
{
    bool edge = false;
    const int ti = (int) (e.position.y - rulerH) / trackH();
    if (e.position.y >= rulerH && ti >= 0 && ti < (int) ui.host.song.tracks.size())
        for (auto& r : ui.host.song.tracks[(size_t) ti].regions)
        {
            const float x0 = xOf (r.start), x1 = xOf (r.start + r.length);
            edge |= e.position.x >= x0 && e.position.x <= x1 && (e.position.x < x0 + 6 || e.position.x > x1 - 6);
        }
    setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void ArrangeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.position.y < rulerH) { ui.host.setLoop (0, 0); repaint(); return; }
    if (auto* r = ui.region(); r != nullptr && r->kind == Region::Record) rename (r->id);   // the first click selected it
}

// Right-click: on empty track space, choose what to put there. On a region, what to do with it.
void ArrangeView::contextMenu (Track& t, Region* hit, double beat)
{
    const int trackId = t.id, hitId = hit != nullptr ? hit->id : 0;
    auto sources = std::make_shared<std::vector<int>>();
    juce::PopupMenu m, from;
    for (auto& o : t.regions)
        if (o.kind == Region::Record) { sources->push_back (o.id); from.addItem (100 + (int) sources->size(), o.name, true, hit != nullptr && hit->source == o.id); }

    if (hit != nullptr)
    {
        ui.select (trackId, hitId);
        if (hit->kind == Region::Record) m.addItem (1, "Rename...");
        if (hit->kind == Region::Play) m.addSubMenu ("Plays", from, ! sources->empty());
        m.addItem (2, "Delete");
    }
    else if (t.hasLoops())
    {
        m.addItem (3, "Record region here");
        m.addItem (4, "Play region here", ! sources->empty());
        if (sources->size() > 1) m.addSubMenu ("Play region of", from);
    }
    else if (t.isAutomation())
    {
        m.addItem (5, "Captured region here (you perform it)");
        m.addItem (6, "Scripted region here (runs by itself)");
    }
    else return;

    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, trackId, hitId, beat, sources] (int res)
    {
        auto& host = ui.host;
        if (res == 1) rename (hitId);
        else if (res == 2) { ui.select (trackId, hitId); ui.deleteSelected(); }
        else if (res == 3) place (trackId, Region::Record, beat, 0, false);
        else if (res == 4) place (trackId, Region::Play, beat, 0, false);
        else if (res == 5 || res == 6) place (trackId, Region::Auto, beat, 0, res == 5);
        else if (res > 100 && res - 101 < (int) sources->size())
        {
            const int source = (*sources)[(size_t) res - 101];
            if (auto* r = host.song.region (hitId))     // an existing play region switches to that recording
            {
                host.pushUndo();
                r->source = source;
                host.regionsChanged();
                ui.refresh();
            }
            else place (trackId, Region::Play, beat, source, false);
        }
        repaint();
    });
}

// One bar at the grid line under the click, shortened to fit before the next region.
void ArrangeView::place (int trackId, Region::Kind kind, double beat, int source, bool captured)
{
    auto& host = ui.host;
    auto* t = host.song.track (trackId);
    if (t == nullptr) return;
    const double grid = ui.snapBeats > 0 ? ui.snapBeats : 1.0;
    double start = std::floor (beat / grid) * grid, len = host.song.beatsPerBar();
    for (auto& o : t->regions) if (o.start <= beat) start = juce::jmax (start, o.start + o.length);     // not on top of the one before
    for (auto& o : t->regions) if (o.start > beat) len = juce::jmin (len, o.start - start);
    len = juce::jmin (len, host.song.lengthBeats() - start);
    if (len < 1e-6) return;

    host.pushUndo();
    auto& r = host.song.addRegion (*t, kind, start, len);
    if (source != 0) r.source = source;
    r.captured = captured;
    const int id = r.id;
    host.regionsChanged();
    ui.select (trackId, id);
}

void ArrangeView::rename (int regionId)
{
    auto* r = ui.host.song.region (regionId);
    if (r == nullptr) return;
    auto* w = new juce::AlertWindow ("Name this recording", "Play regions show the name of the recording they play.", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", r->name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, regionId] (int res)
    {
        if (auto* reg = ui.host.song.region (regionId); reg != nullptr && res == 1)
        {
            reg->name = w->getTextEditorContents ("name");
            ui.host.dirty = true;
            ui.refresh();
        }
    }), true);
}

// Zooms around the pointer: the beat under the fingers stays put.
void ArrangeView::zoom (float factor, float aroundX)
{
    const double len = ui.host.song.lengthBeats(), beat = beatAt (aroundX);
    const double fit = juce::jmax (1.0, getWidth() - 20.0) / len;                  // fully zoomed out = the whole song on screen
    pxPerBeat = juce::jlimit (juce::jmin (fit, 3.0), 600.0, pxPerBeat * (double) juce::jlimit (0.5f, 2.f, factor));
    scroll = juce::jlimit (0.0, juce::jmax (0.0, len - (getWidth() - 20.0) / pxPerBeat), beat - aroundX / pxPerBeat);
    repaint();
}

// Trackpad: two fingers sideways scrolls, pinch zooms. Cmd or Alt with a two-finger swipe (or a mouse wheel) zooms too.
void ArrangeView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isAltDown()) { zoom (std::exp (w.deltaY * 1.5f), e.position.x); return; }
    const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
    const double len = ui.host.song.lengthBeats();
    scroll = juce::jlimit (0.0, juce::jmax (0.0, len - (getWidth() - 20.0) / pxPerBeat), scroll - d * 400.0 / pxPerBeat);
    repaint();
}

void ArrangeView::mouseMagnify (const juce::MouseEvent& e, float scale) { zoom (scale, e.position.x); }
