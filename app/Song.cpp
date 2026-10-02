#include "Song.h"

Region* Song::region (int id, Track** owner)
{
    for (auto& t : tracks)
        if (auto* r = t.find (id)) { if (owner != nullptr) *owner = &t; return r; }
    return nullptr;
}

bool Song::fits (const Track& t, const Region& r) const
{
    if (r.length <= 0 || r.start < 0 || r.start + r.length > lengthBeats() + 1e-9) return false;
    for (auto& o : t.regions)
        if (o.id != r.id && r.start < o.start + o.length - 1e-9 && o.start < r.start + r.length - 1e-9) return false;
    return true;
}

Track& Song::addTrack (Track::Type type)
{
    Track t;
    t.id = nextId++;
    t.type = type;
    const char* names[] = { "Audio", "Instrument", "Command", "Control", "Click", "Talkback" };
    int n = 1;
    for (auto& o : tracks) n += o.type == type;
    t.name = juce::String (names[type]) + " " + juce::String (n);
    tracks.push_back (std::move (t));
    return tracks.back();
}

Region& Song::addRegion (Track& t, Region::Kind kind, double start, double length)
{
    Region r;
    r.id = nextId++;
    r.kind = kind;
    r.start = start;
    r.length = length;
    if (kind == Region::Record)
    {
        int n = 1;
        for (auto& o : t.regions) n += o.kind == Region::Record;
        r.name = "Loop " + juce::String (n);
    }
    else if (kind == Region::Play)      // link to the record loop just before it
    {
        const Region* best = nullptr;       // else the first one after it
        for (auto& o : t.regions)
        {
            if (o.kind != Region::Record) continue;
            const bool oBefore = o.start <= start, bestBefore = best != nullptr && best->start <= start;
            if (best == nullptr || (oBefore && (! bestBefore || o.start > best->start))
                || (! oBefore && ! bestBefore && o.start < best->start))
                best = &o;
        }
        r.source = best != nullptr ? best->id : 0;
    }
    else if (t.type == Track::Control) r.curve = { { 0.f, 0.f }, { 1.f, 1.f } };
    t.regions.push_back (std::move (r));
    return t.regions.back();
}

std::unique_ptr<alk::Arrangement> Song::compile (double sr, bool recording) const
{
    // ponytail: slice lengths round to whole samples, so a tiny slice looped for minutes at an odd
    // tempo drifts a few ms off the grid. Compute the read position from beats if that shows up.
    const double spb = sr * 60.0 / bpm;
    auto S = [spb] (double beats) { return (int64_t) std::llround (beats * spb); };

    auto a = std::make_unique<alk::Arrangement>();
    a->length = S (lengthBeats());
    a->tracks.resize (tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        auto& t = tracks[i];
        if (t.isAutomation()) continue;
        for (auto& r : t.regions)
        {
            alk::Region e;
            e.start = S (r.start);
            e.end = S (r.start + r.length);
            if (r.kind == Region::Record)
            {
                // outside a record pass (or locked, or panicked) a record loop just plays its take
                if (recording && r.pending != nullptr) { e.record = true; e.take = r.pending; }
                else e.take = r.current;
            }
            else if (auto* src = t.find (r.source))
            {
                e.take = recording && src->pending != nullptr ? src->pending : src->current;
                e.sliceStart = S (r.sliceStart);
                e.sliceLen = S (r.sliceLength);
            }
            a->tracks[i].regions.push_back (std::move (e));
        }
        std::sort (a->tracks[i].regions.begin(), a->tracks[i].regions.end(),
                   [] (const alk::Region& x, const alk::Region& y) { return x.start < y.start; });
    }
    return a;
}

void Song::arm (double sr, double fromBeat)
{
    const double spb = sr * 60.0 / bpm;
    auto S = [spb] (double beats) { return (int64_t) std::llround (beats * spb); };
    for (auto& t : tracks)
        if (! t.isAutomation() && ! t.locked)
            for (auto& r : t.regions)
                if (r.kind == Region::Record && r.start + r.length > fromBeat)
                {
                    r.pending = std::make_shared<AppTake> (S (r.start + r.length) - S (r.start), t.type == Track::Instrument);
                    r.pending->id = nextId++;
                    r.pending->sampleRate = sr;
                }
}

void Song::commit()
{
    for (auto& t : tracks)
        for (auto& r : t.regions)
            if (r.pending != nullptr)
            {
                // a midi pass where nothing was played keeps the take you had
                const bool played = r.pending->midi.empty() || r.pending->midiCount > 0;
                if (r.pending->touched && played)
                {
                    r.takes.push_back (r.pending);
                    if (r.takes.size() > 8) r.takes.erase (r.takes.begin());
                    r.current = r.pending;
                }
                r.pending = nullptr;
            }
}

bool Song::panic (double atBeat)
{
    bool any = false;
    for (auto& t : tracks)
        for (auto& r : t.regions)
            if (r.kind == Region::Record && r.reference != nullptr && atBeat >= r.start && atBeat < r.start + r.length)
            {
                r.pending = nullptr;        // stop recording into it, later play loops use the reference
                r.current = r.reference;
                any = true;
            }
    return any;
}

// ---------------------------------------------------------------- file format
// Name.looper/song.json + takes/<id>.wav (audio takes). Midi takes live in the json.

static juce::var obj() { return juce::var (new juce::DynamicObject()); }
static void set (juce::var& o, const char* k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }

static juce::var slotToVar (const PluginSlot& s)
{
    auto o = obj();
    juce::MemoryBlock state = s.state;
    if (s.instance != nullptr) s.instance->getStateInformation (state);
    if (auto xml = s.desc.createXml()) set (o, "desc", xml->toString());
    set (o, "state", state.toBase64Encoding());
    set (o, "bypass", s.bypass);
    return o;
}

static PluginSlot slotFromVar (const juce::var& o)
{
    PluginSlot s;
    if (auto xml = juce::parseXML (o["desc"].toString())) s.desc.loadFromXml (*xml);
    s.state.fromBase64Encoding (o["state"].toString());
    s.bypass = o["bypass"];
    return s;
}

void Song::save (const juce::File& dir)
{
    // take ids restart in every song, so a wav already sitting in someone else's folder is not ours
    const bool sameFolder = dir == folder;
    folder = dir;
    const auto takesDir = dir.getChildFile ("takes");
    takesDir.createDirectory();
    juce::StringArray keep;

    auto takeToVar = [&] (const TakePtr& t) -> juce::var
    {
        if (t == nullptr) return {};
        auto o = obj();
        set (o, "id", t->id);
        set (o, "length", (juce::int64) t->length);
        set (o, "sampleRate", t->sampleRate);
        if (! t->midi.empty())
        {
            juce::Array<juce::var> ev;
            for (int i = 0; i < t->midiCount; ++i)
            {
                auto& m = t->midi[(size_t) i];
                ev.add ((juce::int64) m.time); ev.add (m.data[0]); ev.add (m.data[1]); ev.add (m.data[2]);
            }
            set (o, "midi", ev);
        }
        else
        {
            const auto f = takesDir.getChildFile (juce::String (t->id) + ".wav");
            keep.add (f.getFileName());
            if (! sameFolder) f.deleteFile();
            if (! f.existsAsFile())     // takes never change once recorded
            {
                std::unique_ptr<juce::OutputStream> out = f.createOutputStream();
                const auto options = juce::AudioFormatWriterOptions().withSampleRate (t->sampleRate).withNumChannels (2).withBitsPerSample (24);
                if (auto writer = out != nullptr ? juce::WavAudioFormat().createWriterFor (out, options) : nullptr)
                {
                    const float* ch[] = { t->L.data(), t->R.data() };
                    writer->writeFromFloatArrays (ch, 2, (int) t->length);
                }
            }
        }
        return o;
    };

    auto root = obj();
    set (root, "bpm", bpm); set (root, "num", num); set (root, "den", den); set (root, "bars", bars); set (root, "nextId", nextId);
    juce::Array<juce::var> ts;
    for (auto& t : tracks)
    {
        auto o = obj();
        set (o, "id", t.id); set (o, "type", (int) t.type); set (o, "name", t.name);
        set (o, "power", t.power); set (o, "solo", t.solo); set (o, "audition", t.audition); set (o, "locked", t.locked);
        set (o, "gainDb", t.gainDb); set (o, "pan", t.pan);
        set (o, "input", t.input); set (o, "output", t.output); set (o, "stereoIn", t.stereoIn); set (o, "quantize", t.quantize); set (o, "midiChannel", t.midiChannel); set (o, "drums", t.drums);
        set (o, "instrument", slotToVar (t.instrument));
        juce::Array<juce::var> fx, sources, rs;
        for (auto& s : t.fx) fx.add (slotToVar (s));
        for (int s : t.sources) sources.add (s);
        for (auto& r : t.regions)
        {
            auto ro = obj();
            set (ro, "id", r.id); set (ro, "kind", (int) r.kind); set (ro, "start", r.start); set (ro, "length", r.length);
            if (r.kind == Region::Record)
            {
                set (ro, "name", r.name);
                juce::Array<juce::var> takes;
                int cur = -1;
                for (size_t i = 0; i < r.takes.size(); ++i) { takes.add (takeToVar (r.takes[i])); if (r.takes[i] == r.current) cur = (int) i; }
                set (ro, "takes", takes);
                set (ro, "current", cur);
                set (ro, "reference", takeToVar (r.reference));
                set (ro, "currentIsReference", r.current != nullptr && r.current == r.reference);
            }
            else if (r.kind == Region::Play)
            {
                set (ro, "source", r.source); set (ro, "sliceStart", r.sliceStart); set (ro, "sliceLength", r.sliceLength);
            }
            else
            {
                set (ro, "captured", r.captured); set (ro, "value", r.value); set (ro, "action", r.action);
                set (ro, "trigger", r.trigger); set (ro, "lo", r.lo); set (ro, "hi", r.hi); set (ro, "revert", r.revert);
                set (ro, "tType", r.target.type); set (ro, "tTrack", r.target.trackId);
                set (ro, "tSlot", r.target.slot); set (ro, "tParam", r.target.param);
                juce::Array<juce::var> curve;
                for (auto p : r.curve) { curve.add (p.x); curve.add (p.y); }
                set (ro, "curve", curve);
            }
            rs.add (ro);
        }
        set (o, "fx", fx); set (o, "sources", sources); set (o, "regions", rs);
        ts.add (o);
    }
    set (root, "tracks", ts);
    dir.getChildFile ("song.json").replaceWithText (juce::JSON::toString (root));

    for (auto& f : takesDir.findChildFiles (juce::File::findFiles, false, "*.wav"))   // takes that rotated out
        if (! keep.contains (f.getFileName())) f.deleteFile();
}

bool Song::load (const juce::File& dir)
{
    const auto root = juce::JSON::parse (dir.getChildFile ("song.json"));
    if (! root.isObject()) return false;

    juce::AudioFormatManager afm;
    afm.registerBasicFormats();
    std::map<int, TakePtr> seen;        // the reference is usually also one of the takes
    auto takeFromVar = [&] (const juce::var& o) -> TakePtr
    {
        if (! o.isObject()) return nullptr;
        const int id = o["id"];
        if (auto it = seen.find (id); it != seen.end()) return it->second;
        const bool isMidi = o.hasProperty ("midi");
        auto t = std::make_shared<AppTake> ((juce::int64) o["length"], isMidi);
        t->id = id;
        t->sampleRate = o["sampleRate"];
        t->touched = true;
        if (isMidi)
        {
            auto* ev = o["midi"].getArray();
            const int n = juce::jmin (ev->size() / 4, (int) alk::Take::kMaxMidi);
            for (int i = 0; i < n; ++i)
            {
                auto& m = t->midi[(size_t) i];
                m.time = (juce::int64) (*ev)[i * 4];
                for (int b = 0; b < 3; ++b) m.data[b] = (uint8_t) (int) (*ev)[i * 4 + 1 + b];
            }
            t->midiCount = n;
        }
        else if (std::unique_ptr<juce::AudioFormatReader> reader { afm.createReaderFor (dir.getChildFile ("takes").getChildFile (juce::String (id) + ".wav")) })
        {
            float* ch[] = { t->L.data(), t->R.data() };
            reader->read (ch, 2, 0, (int) juce::jmin ((juce::int64) t->length, reader->lengthInSamples));
        }
        seen[id] = t;
        return t;
    };

    Song s;
    s.folder = dir;
    s.bpm = root["bpm"]; s.num = root["num"]; s.den = root["den"]; s.bars = root["bars"]; s.nextId = root["nextId"];
    if (auto* ts = root["tracks"].getArray())
        for (auto& o : *ts)
        {
            Track t;
            t.id = o["id"]; t.type = (Track::Type) (int) o["type"]; t.name = o["name"].toString();
            t.power = o["power"]; t.solo = o["solo"]; t.audition = o["audition"]; t.locked = o["locked"];
            t.gainDb = o["gainDb"]; t.pan = o["pan"];
            t.input = o["input"]; t.output = o["output"]; t.stereoIn = o["stereoIn"]; t.quantize = o["quantize"]; t.midiChannel = o["midiChannel"]; t.drums = o["drums"];
            t.instrument = slotFromVar (o["instrument"]);
            if (auto* fx = o["fx"].getArray()) for (auto& f : *fx) t.fx.push_back (slotFromVar (f));
            if (auto* src = o["sources"].getArray()) for (auto& v : *src) t.sources.push_back (v);
            if (auto* rs = o["regions"].getArray())
                for (auto& ro : *rs)
                {
                    Region r;
                    r.id = ro["id"]; r.kind = (Region::Kind) (int) ro["kind"]; r.start = ro["start"]; r.length = ro["length"];
                    if (r.kind == Region::Record)
                    {
                        r.name = ro["name"].toString();
                        if (auto* takes = ro["takes"].getArray()) for (auto& tv : *takes) if (auto tk = takeFromVar (tv)) r.takes.push_back (tk);
                        r.reference = takeFromVar (ro["reference"]);
                        const int cur = ro["current"];
                        if (ro["currentIsReference"]) r.current = r.reference;
                        else if (cur >= 0 && cur < (int) r.takes.size()) r.current = r.takes[(size_t) cur];
                    }
                    else if (r.kind == Region::Play)
                    {
                        r.source = ro["source"]; r.sliceStart = ro["sliceStart"]; r.sliceLength = ro["sliceLength"];
                    }
                    else
                    {
                        r.captured = ro["captured"]; r.value = ro["value"]; r.action = ro["action"];
                        r.trigger = ro["trigger"]; r.lo = ro["lo"]; r.hi = ro["hi"]; r.revert = ro["revert"];
                        r.target.type = ro["tType"]; r.target.trackId = ro["tTrack"];
                        r.target.slot = ro["tSlot"]; r.target.param = ro["tParam"];
                        if (auto* c = ro["curve"].getArray())
                            for (int i = 0; i + 1 < c->size(); i += 2) r.curve.push_back ({ (float) (*c)[i], (float) (*c)[i + 1] });
                    }
                    t.regions.push_back (std::move (r));
                }
            s.tracks.push_back (std::move (t));
        }
    *this = std::move (s);
    return true;
}
