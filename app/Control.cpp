// Commands from outside the app. Anything that can write a file (a Claude Code session turning a spoken
// sentence into an arrangement, a script) drops a json list of ops into
//   ~/Library/Application Support/Looper/control/in/
// The app applies them through the same calls the mouse uses and keeps control/state.json current.
// The format is in docs/CONTROL.md. Positions are in bars, counted from 1, fractions allowed.
#include "AudioHost.h"

namespace {

juce::File controlDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/Looper/control");
}

juce::var obj() { return juce::var (new juce::DynamicObject()); }
void put (juce::var& o, const char* k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }
bool isNumber (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

const char* typeName (const Track& t)
{
    const char* names[] = { "audio", "instrument", "command", "control", "click", "talkback", "marker" };
    return t.drums ? "drums" : names[t.type];
}

struct Applier
{
    AudioHost& h;
    juce::StringArray& errors;
    std::map<juce::String, int> refs;       // names given with "ref" earlier in the same file
    bool structural = false, changed = false;

    double bpb() const { return h.song.beatsPerBar(); }

    Track* track (const juce::var& v)
    {
        if (isNumber (v)) return h.song.track ((int) v);
        const auto name = v.toString();
        if (auto it = refs.find (name); it != refs.end()) if (auto* t = h.song.track (it->second)) return t;
        for (auto& t : h.song.tracks) if (t.name.equalsIgnoreCase (name)) return &t;
        return nullptr;
    }

    Region* region (Track* scope, const juce::var& v, Track** owner = nullptr)
    {
        if (isNumber (v)) return h.song.region ((int) v, owner);
        const auto name = v.toString();
        if (auto it = refs.find (name); it != refs.end()) if (auto* r = h.song.region (it->second, owner)) return r;
        for (auto& t : h.song.tracks)
            if (scope == nullptr || scope == &t)
                for (auto& r : t.regions) if (r.name.isNotEmpty() && r.name.equalsIgnoreCase (name)) { if (owner != nullptr) *owner = &t; return &r; }
        return nullptr;
    }

    // adds a region at bar/bars, growing the song if needed. Null (with an error) if it lands on another region.
    Region* add (const juce::var& op, Track& t, Region::Kind kind)
    {
        const double start = ((double) op.getProperty ("bar", 1.0) - 1.0) * bpb(), len = (double) op.getProperty ("bars", 1.0) * bpb();
        if (start < 0 || len <= 0) { errors.add (op["op"].toString() + ": bar must be 1 or more and bars above 0"); return nullptr; }
        h.song.bars = juce::jmax (h.song.bars, (int) std::ceil ((start + len) / bpb() - 1e-9));
        auto& r = h.song.addRegion (t, kind, start, len);
        if (! h.song.fits (t, r))
        {
            errors.add (op["op"].toString() + " on '" + t.name + "': bar " + op.getProperty ("bar", 1).toString() + " overlaps a region that is already there");
            t.regions.pop_back();
            return nullptr;
        }
        if (op.hasProperty ("name")) r.name = op["name"].toString();
        if (op.hasProperty ("ref")) refs[op["ref"].toString()] = r.id;
        changed = true;
        return &r;
    }

    void run (const juce::var& op)
    {
        const auto name = op["op"].toString();
        auto needTrack = [&] () -> Track*
        {
            auto* t = track (op["track"]);
            if (t == nullptr) errors.add (name + ": no track '" + op["track"].toString() + "'");
            return t;
        };

        if (name == "new_song")
        {
            bool content = false;
            for (auto& t : h.song.tracks) content |= ! t.regions.empty();
            if (h.dirty && content) h.autosave();       // the song being replaced stays on disk
            h.newSong();
            refs.clear();
        }
        else if (name == "set_song")
        {
            if (op.hasProperty ("bpm")) h.song.bpm = juce::jlimit (30.0, 300.0, (double) op["bpm"]);
            if (op.hasProperty ("bars")) h.song.bars = juce::jlimit (1, 2000, (int) op["bars"]);
            if (op.hasProperty ("time_signature"))
            {
                const auto sig = op["time_signature"].toString();
                const int num = sig.upToFirstOccurrenceOf ("/", false, false).getIntValue(), den = sig.fromFirstOccurrenceOf ("/", false, false).getIntValue();
                if (num >= 1 && num <= 32 && (den == 2 || den == 4 || den == 8 || den == 16)) { h.song.num = num; h.song.den = den; }
                else errors.add ("set_song: time_signature must look like 4/4");
            }
            changed = true;
        }
        else if (name == "add_track")
        {
            const auto type = op.getProperty ("type", "audio").toString();
            const juce::StringArray types { "audio", "instrument", "command", "control", "click", "talkback", "marker", "drums" };
            const int i = types.indexOf (type);
            if (i < 0 || i == 4) { errors.add ("add_track: type must be audio, instrument, drums, marker, command, control or talkback"); return; }
            if ((int) h.song.tracks.size() >= alk::Engine::kMaxTracks) { errors.add ("add_track: the song is full"); return; }
            auto& t = h.newTrack (i == 7 ? Track::Instrument : (Track::Type) i, i == 7);
            if (op.hasProperty ("name")) t.name = op["name"].toString();
            if (op.hasProperty ("ref")) refs[op["ref"].toString()] = t.id;
            structural = true;
        }
        else if (name == "add_record" || name == "add_play")
        {
            auto* t = needTrack();
            if (t == nullptr) return;
            if (! t->hasLoops()) { errors.add (name + ": '" + t->name + "' is not an audio, instrument or drum track"); return; }
            const int trackId = t->id;
            auto* r = add (op, *t, name == "add_record" ? Region::Record : Region::Play);
            if (r == nullptr || name == "add_record") return;
            if (op.hasProperty ("source"))
            {
                const int regionId = r->id;
                auto* src = region (h.song.track (trackId), op["source"]);
                r = h.song.region (regionId);
                if (src != nullptr && src->kind == Region::Record) r->source = src->id;
                else errors.add ("add_play: no recording '" + op["source"].toString() + "' on that track");
            }
            r->sliceStart = juce::jmax (0.0, ((double) op.getProperty ("slice_bar", 1.0) - 1.0) * bpb());
            r->sliceLength = juce::jmax (0.0, (double) op.getProperty ("slice_bars", 0.0) * bpb());
        }
        else if (name == "add_marker")
        {
            Track* t = op.hasProperty ("track") ? track (op["track"]) : nullptr;
            if (t == nullptr) for (auto& o : h.song.tracks) if (o.type == Track::Marker) { t = &o; break; }
            if (t == nullptr) { t = &h.newTrack (Track::Marker); structural = true; }
            if (t->type != Track::Marker) { errors.add ("add_marker: '" + t->name + "' is not a marker track"); return; }
            add (op, *t, Region::Marker);
        }
        else if (name == "delete_track")
        {
            auto* t = needTrack();
            if (t == nullptr) return;
            if (t->type == Track::Click) { errors.add ("delete_track: the click track stays"); return; }
            h.song.tracks.erase (h.song.tracks.begin() + h.song.indexOf (t->id));
            structural = true;
        }
        else if (name == "delete_region" || name == "rename_region" || name == "move_region")
        {
            Track* owner = nullptr;
            auto* r = region (op.hasProperty ("track") ? track (op["track"]) : nullptr, op["region"], &owner);
            if (r == nullptr) { errors.add (name + ": no region '" + op["region"].toString() + "'"); return; }
            if (name == "delete_region") owner->regions.erase (owner->regions.begin() + (r - owner->regions.data()));
            else if (name == "rename_region") r->name = op["name"].toString();
            else
            {
                const Region before = *r;
                if (op.hasProperty ("bar")) r->start = ((double) op["bar"] - 1.0) * bpb();
                if (op.hasProperty ("bars")) r->length = (double) op["bars"] * bpb();
                h.song.bars = juce::jmax (h.song.bars, (int) std::ceil ((r->start + r->length) / bpb() - 1e-9));
                if (! h.song.fits (*owner, *r)) { *r = before; errors.add ("move_region: that spot overlaps another region"); return; }
            }
            changed = true;
        }
        else if (name == "rename_track")
        {
            if (auto* t = needTrack()) { t->name = op["name"].toString(); changed = true; }
        }
        else if (name == "transport")
        {
            const auto action = op["action"].toString();
            if (action == "play") h.play();
            else if (action == "record") h.record();
            else if (action == "stop") h.stop();
            else if (action == "rewind") h.jump (0);
            else errors.add ("transport: action must be play, record, stop or rewind");
        }
        else errors.add ("unknown op '" + name + "'");
    }
};

} // namespace

void AudioHost::pollControl()
{
    const auto dir = controlDir(), in = dir.getChildFile ("in");
    if (! in.isDirectory()) { in.createDirectory(); stateStale = true; }

    auto files = in.findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    for (auto& f : files)
    {
        lastApplied = f.getFileName();
        lastErrors.clear();
        const auto doc = juce::JSON::parse (f);
        f.deleteFile();
        auto* ops = doc["ops"].getArray();
        if (ops == nullptr) { lastErrors.add ("the file needs an \"ops\" array"); stateStale = true; continue; }

        pushUndo();
        Applier a { *this, lastErrors, {} };
        for (auto& op : *ops)
        {
            // tracks added earlier in this file must exist for the audio side before transport ops run
            if (op["op"].toString() == "transport" && (a.structural || a.changed)) { structureChanged(); a.structural = a.changed = false; }
            a.run (op);
        }
        if (a.structural || a.changed) structureChanged();     // also refreshes the window
        stateStale = true;
    }

    if (! stateStale) return;
    stateStale = false;
    const double bpb = song.beatsPerBar();
    auto root = obj();
    put (root, "applied", lastApplied);
    put (root, "errors", juce::var (lastErrors));
    put (root, "bpm", song.bpm);
    put (root, "time_signature", juce::String (song.num) + "/" + juce::String (song.den));
    put (root, "bars", song.bars);
    put (root, "song", song.folder == juce::File() ? juce::String ("(unsaved)") : song.folder.getFullPathName());
    juce::Array<juce::var> tracks;
    for (auto& t : song.tracks)
    {
        auto to = obj();
        put (to, "id", t.id); put (to, "name", t.name); put (to, "type", typeName (t));
        juce::Array<juce::var> regions;
        for (auto& r : t.regions)
        {
            const char* kinds[] = { "record", "play", "automation", "marker" };
            auto ro = obj();
            put (ro, "id", r.id); put (ro, "kind", kinds[r.kind]);
            put (ro, "bar", r.start / bpb + 1.0); put (ro, "bars", r.length / bpb);
            if (r.kind == Region::Record || r.kind == Region::Marker) put (ro, "name", r.name);
            if (r.kind == Region::Record) put (ro, "has_take", r.current != nullptr);
            if (r.kind == Region::Play)
            {
                put (ro, "source", r.source);
                put (ro, "slice_bar", r.sliceStart / bpb + 1.0);
                put (ro, "slice_bars", r.sliceLength / bpb);
            }
            regions.add (ro);
        }
        put (to, "regions", regions);
        tracks.add (to);
    }
    put (root, "tracks", tracks);
    dir.getChildFile ("state.json").replaceWithText (juce::JSON::toString (root));
}
