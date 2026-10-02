#include "Ui.h"

float Automation::get (const Target& tg)
{
    auto* t = host.song.track (tg.trackId);
    if (t == nullptr) return 0.f;
    auto* s = t->slot (tg.slot);
    switch (tg.type)
    {
        case Target::TrackPower:   return t->power ? 1.f : 0.f;
        case Target::TrackVolume:  return juce::jlimit (0.f, 1.f, (t->gainDb + 60.f) / 60.f);
        case Target::PluginBypass: return s != nullptr && ! s->bypass ? 1.f : 0.f;
        case Target::PluginParam:
            if (s != nullptr && s->instance != nullptr && tg.param < s->instance->getParameters().size())
                return s->instance->getParameters()[tg.param]->getValue();
            return 0.f;
        default: return 0.f;
    }
}

void Automation::set (const Target& tg, float v)
{
    auto* t = host.song.track (tg.trackId);
    if (t == nullptr) return;
    auto* s = t->slot (tg.slot);
    switch (tg.type)
    {
        case Target::TrackPower:   changed |= t->power != (v > 0.5f); t->power = v > 0.5f; host.paramsChanged(); break;
        case Target::TrackVolume:  t->gainDb = juce::jlimit (0.f, 1.f, v) * 60.f - 60.f; host.paramsChanged(); break;
        case Target::PluginBypass: if (s != nullptr) { changed |= s->bypass != (v <= 0.5f); s->bypass = v <= 0.5f; host.paramsChanged(); } break;
        case Target::PluginParam:
            if (s != nullptr && s->instance != nullptr && tg.param < s->instance->getParameters().size())
                s->instance->getParameters()[tg.param]->setValueNotifyingHost (juce::jlimit (0.f, 1.f, v));
            break;
        default: break;
    }
}

void Automation::fire (const Region& r)
{
    const auto& tg = r.target;
    switch (tg.type)
    {
        case Target::None:     break;
        case Target::Play:     if (host.rolling()) host.stop(); else host.play(); break;       // a pedal starts and stops
        case Target::Record:   if (host.recording()) host.stop(); else host.record(); break;
        case Target::Panic:    host.panic(); break;
        case Target::Position: host.jump ((juce::jmax (1.f, r.value) - 1.f) * host.song.beatsPerBar()); break;
        default:
        {
            const float on = tg.type == Target::TrackPower || tg.type == Target::PluginBypass ? 1.f : r.value;
            set (tg, r.action == 0 ? on : r.action == 1 ? 0.f : (get (tg) > 0.001f ? 0.f : on));
        }
    }
}

bool Automation::tick (const std::vector<AudioHost::Controller>& events)
{
    changed = false;
    auto& song = host.song;
    const double pos = host.posBeats();
    const bool rolling = host.rolling();

    // commands waiting for the next beat, bar or region end. A wrap or jump back counts as arriving.
    if (! rolling) pending.clear();
    const bool wentBack = pos < lastPos - 1e-6;
    for (size_t i = 0; i < pending.size();)
    {
        if (pos < pending[i].at && ! wentBack) { ++i; continue; }
        const auto p = pending[i];
        pending.erase (pending.begin() + (int) i);
        if (auto* t = song.track (p.trackId))
            if (auto* r = t->find (p.regionId)) fire (*r);
    }
    lastPos = pos;

    for (auto& t : song.tracks)
    {
        if (! t.isAutomation()) continue;

        // scripted regions only run while the transport rolls. Captured ones also listen when stopped,
        // so a pedal can start the song.
        const Region* active = nullptr;
        for (auto& r : t.regions)
            if (pos >= r.start && pos < r.start + r.length && (r.captured || rolling)) active = &r;

        auto& st = state[t.id];
        if ((active != nullptr ? active->id : 0) != st.regionId)
        {
            if (auto* old = t.find (st.regionId); old != nullptr && old->revert)
            {
                set (old->target, st.entry);
                if (auto* tt = song.track (old->target.trackId); tt != nullptr && old->target.type == Target::TrackVolume)
                {
                    tt->gainDb = st.entryDb;        // exact, the 0..1 scale tops out at 0 dB
                    host.paramsChanged();
                }
            }
            st.regionId = active != nullptr ? active->id : 0;
            if (active != nullptr)
            {
                st.entry = get (active->target);
                if (auto* tt = song.track (active->target.trackId)) st.entryDb = tt->gainDb;
                if (! active->captured && t.type == Track::Command) fire (*active);
            }
        }
        if (active == nullptr) continue;

        if (! active->captured && t.type == Track::Control)
            set (active->target, curveAt (*active, (float) ((pos - active->start) / active->length)));

        if (active->captured)
            for (auto& e : events)
            {
                if (std::find (t.sources.begin(), t.sources.end(), e.id) == t.sources.end()) continue;
                if (t.type == Track::Control) set (active->target, active->lo + e.value * (active->hi - active->lo));
                else if (e.value > 0.5f)
                {
                    const double bar = song.beatsPerBar(), beat = 4.0 / song.den;
                    const double at = active->trigger == 1 ? (std::floor (pos / beat) + 1) * beat
                                    : active->trigger == 2 ? (std::floor (pos / bar) + 1) * bar
                                    : active->start + active->length;
                    if (active->trigger == 0 || ! rolling) fire (*active);
                    else pending.push_back ({ at, t.id, active->id });
                }
            }
    }
    return changed;
}
