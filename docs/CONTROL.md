# Driving Looper from outside

The running app can be told what to build. Anything that can write a file (a Claude Code session turning a spoken sentence into an arrangement, a script) drops a JSON file into an inbox folder. The app applies it within about a quarter of a second, through the same calls the mouse uses, and reports back.

```
~/Library/Application Support/Looper/control/in/      put command files here (*.json)
~/Library/Application Support/Looper/control/state.json   the app keeps this current
```

Files are applied in name order and deleted once applied. `state.json` is rewritten after every command file and after every change made in the app, so it always describes what is on screen.

## A command file

```json
{ "ops": [
  { "op": "new_song" },
  { "op": "set_song", "bpm": 120, "time_signature": "4/4", "bars": 12 },
  { "op": "add_track", "type": "drums", "name": "Kick and snare", "ref": "ks" },
  { "op": "add_record", "track": "ks", "name": "Kick snare", "bar": 1, "bars": 2 },
  { "op": "add_play", "track": "ks", "source": "Kick snare", "bar": 3, "bars": 8 }
] }
```

**Positions are in bars, counted from 1.** Fractions are fine: `"bar": 2.5` is halfway through bar 2, `"bars": 0.25` is one beat in 4/4. The song grows by itself if a region ends past its last bar.

**Naming things.** A `track` is an id from `state.json`, a `ref` given earlier in the same file, or a track name. A `region` or `source` is an id, a `ref`, or the name of a recording or marker.

## Ops

| op | fields | what it does |
|---|---|---|
| `new_song` | | Saves the current song if it has content, then starts an empty one |
| `set_song` | `bpm`, `time_signature`, `bars` (all optional) | Tempo, meter, length |
| `add_track` | `type`, `name`, `ref` | `type`: `audio`, `instrument`, `drums`, `marker`, `command`, `control`, `talkback` |
| `add_record` | `track`, `bar`, `bars`, `name`, `ref` | A record region: what is played here gets captured |
| `add_play` | `track`, `bar`, `bars`, `source`, `slice_bar`, `slice_bars` | A play region looping `source`. Without `source` it plays the recording just before it on that track. `slice_bar` and `slice_bars` pick the part of the recording that loops (default: all of it) |
| `add_marker` | `name`, `bar`, `bars`, `track` | A named section. A marker track is created if there is none |
| `move_region` | `region`, `bar`, `bars` | Moves or resizes |
| `rename_region` | `region`, `name` | |
| `delete_region` | `region` | |
| `rename_track` | `track`, `name` | |
| `delete_track` | `track` | The click track cannot be deleted |
| `transport` | `action`: `play`, `record`, `stop`, `rewind` | |

Two regions on one track cannot overlap. An op that would overlap is skipped and reported.

## What comes back

```json
{ "applied": "001.json", "errors": [], "bpm": 120.0, "time_signature": "4/4", "bars": 12,
  "tracks": [ { "id": 3, "name": "Kick and snare", "type": "drums",
    "regions": [ { "id": 4, "kind": "record", "name": "Kick snare", "bar": 1.0, "bars": 2.0, "has_take": false },
                 { "id": 5, "kind": "play", "source": 4, "bar": 3.0, "bars": 8.0, "slice_bar": 1.0, "slice_bars": 0.0 } ] } ] }
```

`errors` lists every op that could not be applied, in plain words. Always read it.

## From a sentence to an arrangement

"Start with a 2 bar kick and snare recording, then layer hi-hats on it, then add bass, and when all three are layered let them run for 4 bars together and stop at the same time."

How to read it:

- Each layer is its own track with one record region and one play region right after it.
- A new layer's record region starts where the previous layer's record region ended, so the earlier layers are already looping while the next one is played in.
- "Run together for 4 bars" starts when the last layer's record region ends. Every play region ends at that point plus 4 bars.

| Track | Record | Play |
|---|---|---|
| Kick and snare (drums) | bars 1 to 2 | bars 3 to 10 |
| Hi-hats (drums) | bars 3 to 4 | bars 5 to 10 |
| Bass (instrument) | bars 5 to 6 | bars 7 to 10 |

Drum parts go on `drums` tracks, pitched parts on `instrument` tracks, a mic or guitar on `audio` tracks.
