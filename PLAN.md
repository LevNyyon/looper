# ALK rebuild: operational plan

Working title: `alk-rebuild`. Pick a real name before anything is distributed, "ALK" belongs to the old company.

Written 2026-10-01. Status per milestone is tracked in section 8.

## 1. What we are building

An arranged live looper for macOS. You script a performance on a timeline, press Record, and the app records and loops your playing hands-free according to the script.

The spec is the original ALK2 manual as archived on the Wayback Machine: `web.archive.org/web/2019*/zenaud.io/manual/*`. A text copy is kept locally in `docs/alk2-manual/` and is not in the repository, because it is zenAud.io's copyright. No ALK2 code was ever available or used.

One deliberate improvement over ALK2, per your request:

**Slice playback.** A play loop can play any sub-range of a record loop, in any subdivision, and loop that sub-range.

- ALK2 had only a start offset. An 8 bar play loop with a 1 bar offset on a 4 bar recording played bars 2, 3, 4, 1, 2, 3, 4, 1.
- Ours has offset and slice length. The same play loop with slice "bars 2 to 3" plays 2, 3, 2, 3, 2, 3, 2, 3.
- Record 4 bars once, then place bars 1 to 2 here, 2 to 3 there, 3 to 4 somewhere else, or a single 16th note stuttered for a bar.

## 2. Technical approach

**Stack: C++ and JUCE, nothing else.** JUCE alone covers audio devices, MIDI, VST3/AU hosting, GUI and file IO.

I am dropping Tracktion Engine from my earlier suggestion. Its model is a tape reel (clips pointing at audio files). ALK's model is a script driving loopers, and playing back audio that was recorded one bar ago is exactly where a tape-reel engine fights you. This is my judgment from how the two models differ, not something I tested.

**The core is small because everything is known in advance.** Every record loop has a fixed length at a fixed tempo, so its buffer is allocated before the performance starts. The audio thread then does only this, per track, per sample block:

- Playhead inside a record loop: write input into that loop's buffer, pass input through to the output.
- Playhead inside a play loop: read from the linked buffer at
  `sliceStart + ((playhead - playLoopStart) mod sliceLength)`.
- Otherwise: silence.

That is Record mode. In Play mode nothing is recorded and a record loop plays its current take. Input monitoring: in Record mode you hear yourself only inside record loops. In Play and Stop mode each track has a manual Audition button.

**Data model**

```
Song        tempo, time signature, length, tracks[]
Track       type, name, input, output, fx chain, gain, pan, power, solo, regions[]
RecordLoop  id, name, start, length, takes[up to 8], referenceTake, currentTake
PlayLoop    start, length, sourceRecordLoopId, sliceStart, sliceLength
```

All positions are in beats. Regions on one track never overlap. Songs save as JSON plus WAV takes.

**Details that decide whether it feels right**

| Problem | Handling |
|---|---|
| Slicing mid-waveform clicks | Short crossfade (a few ms) at every slice wrap and region edge |
| Recorded audio lands late by the round-trip latency | Shift the write position by the reported latency plus a manual offset in Preferences |
| Reported latency is often wrong | Loopback calibration tool in M3, manual knob from day one |
| Glitches from allocating or locking on the audio thread | Buffers preallocated; arrangement edits handed over as an immutable snapshot swap |
| Plugin crash kills the app mid-song | Autosave from M2; scanner blacklist in M4; out-of-process hosting only if it actually bites |

**Repo layout**

```
engine/   pure C++, no devices, no UI. Model, transport, loop buffers, slicing.
app/      JUCE app: device IO, plugin hosting, arranger UI.
docs/     the recovered manual, this plan.
```

Keeping `engine/` free of devices lets me verify it without ears: feed synthetic input through an offline render and assert the output is sample-exact. One self-check program, no test framework.

## 3. Milestones

Each milestone ends with a short session where you play through it with an instrument. I can verify compiling, the offline engine check and screenshots. I cannot verify sound, latency or feel.

### M0. Setup and audio passthrough (about 1 day)
- Install cmake and ninja via Homebrew. Try building with the Command Line Tools already on this Mac.
- JUCE app that passes input to output, with microphone permission and a device picker.
- **Done when:** you hear your instrument through the app and it shows buffer size and reported latency.

### M1. Engine core, headless (2 to 3 days)
- Song model, sample-accurate transport, record buffers, play loops with slice and crossfade, latency offset.
- **Done when:** the offline check proves "record 4 bars, play bars 1 to 2, 2 to 3, 3 to 4 and a 16th-note stutter" sample-exactly outside the declick fades, and that the fades are there.

### M2. Arranger UI, first playable (3 to 5 days)
- Timeline with bar and beat grid, zoom, snap.
- Draw record loops (red) and play loops (green). Move, resize, copy, delete, undo.
- Play loop links to a record loop by name. Slice handles for offset and length, with the source waveform drawn inside the play loop.
- Transport: Play, Record, Rewind, tempo, time signature, metronome.
- Save, load, autosave.
- **Done when:** you build and perform a real song sketch on one audio track using slices. **This is the go or no-go point for the rest.**

### M3. Live-ready audio (3 to 5 days)
- Multiple audio tracks with per-track input, output, gain, pan, power, solo.
- Monitoring rules as in the manual: during Record, input is heard only inside record loops.
- Takes (last 8), Lock Performances, reference takes, Panic.
- Global Loop to extend a section on the fly.
- Latency calibration by loopback.
- **Done when:** you can rehearse a full multi-track song and recover from a fluffed take.

### M4. Plugins and MIDI (about 1 week)
- Scan and host VST3 and AU. FX chain per track, plugin windows, state saved in the song.
- Instrument tracks: record MIDI into record loops, play loops replay it through the instrument, slicing included. Held notes ring across the repeat of a whole loop, slices cut them. Record quantize. MIDI channel filter per track. On-screen piano that also plays from the computer keyboard. Apple's built-in synth is the default instrument.
- **Done when:** a song with your real plugins survives save, quit, reload and a full run.

### M5. Foot control and automation (about 1 week)
- MIDI learn.
- Command tracks: captured regions (immediate, quantized, end of region; On, Off, Toggle or a fixed value) and scripted regions. Targets: Play, Record, Panic, position jump, track power, track volume, plugin bypass, plugin parameter.
- Control tracks: scripted curves and captured faders (with min and max range) to track volume or any plugin parameter.
- Latch or revert on region exit.
- **Done when:** you play a whole song without touching the laptop.

### Later, only if you miss them
Pitch shift per play loop, play loops linking across tracks, track-to-track routing ("cables"), MIDI tracks to external gear, audio import and export, MIDI clock, Windows.

**Total:** M0 to M2 is about 1.5 to 2 weeks of build time. M3 to M5 is another 3 to 4 weeks. Calendar time depends on how fast we can get your test sessions in.

## 4. Who does what

| Me | You |
|---|---|
| All code, builds, the offline engine check | A 30 minute play-through at the end of each milestone |
| UI checks by screenshot | Judging latency, clicks, feel |
| Reading the manual as spec | Telling me where the manual and your memory of the app disagree |
| | Any installs that need your password (full Xcode, if the Command Line Tools turn out not to be enough) |

## 5. Decisions (Lev, 2026-10-01)

1. **Rig.** A USB audio interface with multiple inputs and outputs comes later. Per-track input and output selection is required.
2. **Audio and MIDI.** Both. Instrument tracks are in scope.
3. **Loop tap point.** Record dry, play back through the track effects.
4. **Go.** Run M1 to M5. One agent audits for drift at each milestone. No excessive testing before an operational bit is alive. Ponytail rules for architecture.

Open source under AGPLv3, decided 2026-10-01 with the public v0.1. That is the license JUCE is free under.

## 5b. Build decisions made under ponytail

- **Plugin routing is a straight chain per track** (instrument, then effects, then gain and pan). No routing graph until track-to-track cables are asked for.
- **Automation runs on the UI thread** with a fast timer, about 10 ms jitter. Move track power and bypass to the audio thread only if that is audible.
- **Undo covers loop edits only** (draw, move, resize, delete, slice, curve points), as a stack of copies of every track's regions. Adding or removing a track or plugin, or changing I/O, clears it. Automation region settings (target, action, value, trigger) are not undoable.
- **One engine self-check program.** No renderer CLI, no test framework.
- **UI verification by the app rendering its own window to a PNG** (`--snapshot`), no screen control needed.
- **Held MIDI notes:** when a play loop replays the whole record loop, a note held past the loop end rings across the repeat, as in ALK (its note-off is wrapped into the loop). A play loop that replays a slice cuts its notes at every repeat. Every play loop releases notes and the sustain pedal when it ends.
- **Only 3-byte MIDI messages are looped** (notes, controllers, pitch bend). Program change and channel pressure are dropped.
- **MIDI timing compensation is an estimate** (output latency plus two buffers). There is no measurement for it yet.
- **Takes are not time-stretched.** Changing the tempo or the sample rate after recording leaves old takes at their original speed.
- **Plugin chain was smoke-run with Apple's built-in delay and synth only**, inside the `--snapshot` demo pass. That shows they load, run in the callback without crashing, and save and restore state. It does not prove what they sound like.
- **MIDI input is per channel, not per device.** Every instrument track hears all MIDI devices, filtered by channel. Learned pedals and faders are kept away from instruments.
- **Panic works as in the manual:** it acts on record loops under the playhead. Pressed after the loop has ended, it does nothing.
- **The app is ad hoc signed.** macOS treats every rebuild as a new app and asks for the microphone again. A stable signing certificate fixes that; creating one is Lev's call.
- **Device log** at `~/Library/Logs/Looper/Looper.log`: microphone decision, device name, channel counts.
- **Dropped from scope until asked for:** MIDI tracks to external gear, pitch shift, cross-track play loops, hover auditioning, name-based track colors, tempo changes during a song.

## 6. Risks

| Risk | Likelihood | What we do |
|---|---|---|
| Building without full Xcode fails | Medium | Install Xcode (large download, needs you) |
| Latency compensation feels off | High at first | Manual offset from M0, loopback calibration in M3 |
| Timeline UI takes longer than the engine | High | M2 ships ugly but complete. Polish only after M5 |
| A plugin misbehaves in our host | Medium | Test with your actual plugins in M4, not a generic set |
| Scope creep toward a full DAW | High | No audio editor, no mixer page, no MIDI editor. The original had none either |

## 7b. Added after M5 (Lev's requests, 2026-10-01)

- **Click track.** The metronome is a track (always present, cannot be deleted) with its own output pair, volume and pan. Its P button and the transport's Click button are the same switch.
- **Talkback track.** An input passed straight to any output pair, never looped. P is the talk switch and starts off. A command track can put it on a pedal (target: track Power).
- Click and talkback ignore solo on other tracks.
- **Drum track.** An instrument track "played as" a drum kit: every note is sent on MIDI channel 10 (the General MIDI kit in Apple's synth). 12 pads, each with a computer key: ` crash left, 1 crash right, 2 ride, 3 china, Q closed hat, W pedal hat, E open hat, A high tom, S mid tom, D low tom, Z kick, X snare. While a drum track is selected these keys win over the Z (undo) and Q (loop) shortcuts. Cmd-Z still undoes.
- **The loop range is hidden by default.** Prefs has a checkbox, "Hide the loop range". While hidden, the ruler only moves the playhead, Q does nothing and the song always runs straight through. The engine's loop is untouched, so it comes back by unticking the box.
- **Marker track.** A track type holding named marker regions (sections). Right-click it to drop one, double-click to name it, drag to move or resize. A marker's start shows as a line across all tracks. The . key jumps to the next marker, the , key goes back to the beginning.
- **Track names are editable** at the top of the track panel, by double-clicking the name in the header, or from the header's right-click menu.
- **No help text.** The inspector strip only appears while a region is selected, so the timeline gets the space. The shortcuts live in the README.
- **Control inbox.** The app polls `~/Library/Application Support/Looper/control/in/` about four times a second, applies JSON command files (add track, add record or play region, markers, transport) through the same calls the UI uses, and keeps `control/state.json` current. This is how a Claude Code session builds an arrangement from a sentence (`CLAUDE.md`, `docs/CONTROL.md`). Checked live: the kick, hats and bass example was sent as one file and appeared on the timeline with no errors.
- **No tool buttons.** Select, Rec loop and Play loop (and their 1 2 3 keys) are gone: regions are placed from the right-click menu. Cmd-drag and Alt-drag still draw a record or play region directly.
- **Computer keyboard plays the selected instrument track** with no click on the piano first: A S D F G H J K and W E T Y U, one octave from middle C. On-screen keys and pads reach only the selected track. MIDI devices reach every instrument track, filtered by channel.
- **Recording follows the script, not the mouse.** While recording, the computer keys and pads reach every instrument track and each track's record regions decide who sounds and records. The selection, with its piano or pads and key map, moves by itself to the instrument whose record region is coming up (half a beat ahead).
- The loop shortcut moved from G to Q, because G is a note.
- **Right-click on a track** places a record or play region there (one bar, shortened to fit). Right-click a region to rename, delete, or choose which recording a play region plays. Double-click a recording to name it.
- A MIDI record pass where nothing was played no longer replaces the take.

- **Multi-select.** Drag on empty timeline space selects every region the box touches, shift-click adds or removes one. Dragging a selected region moves the whole group in time and to other tracks of the same kind, alt-drag copies it, Backspace deletes it. A group that would land on another region goes back.
- **Songs autosave.** An opened song is written back within about 15 seconds of any change while stopped, and takes that rotate out are removed from disk. Use Save As (Cmd-Shift-S) to branch a copy before experimenting.
- On-screen keys and pads never trigger command tracks. Only MIDI devices do.

Checked in the live app: a selection box picked up four regions on two tracks, the group moved one bar together and back.

Not verified by me, because the user was working in the app and I did not drive it: the drum kit's sound and the drum keys, talkback with a live mic, the click on any output, the right-click menus (the user used them), group copy and moves across tracks, marker tracks and the , and . keys, renaming a track, the selection following record regions, held notes ringing across a repeat by ear (the self-check only proves the note-off is stored), renaming.

## 8. Status

Updated 2026-10-01. "Checked" means what was actually exercised, nothing more.

| Milestone | Code | Checked by me | Still needs you |
|---|---|---|---|
| M0 | written, builds with the Command Line Tools (no Xcode) | app builds and launches | hearing your instrument through it |
| M1 | written | `build/selfcheck` passes: slices sample-exact, fades present, latency shift, MIDI slice replay | nothing |
| M2 | written | window rendered to PNG. In the live app: drew a record and a play loop by mouse, dragged the slice window, ran a Record pass on the real device, take committed and autosaved (silent: the built-in mic is cut while the lid is closed) | performing a sketch you can hear |
| M3 | written | compiles. Record, commit, save, load exercised by the snapshot run | multi-track rehearsal, takes, Panic, Global Loop, loopback calibration, by ear |
| M4 | written | In the live app: added an instrument track, clicked six notes on the on-screen piano during a Record pass, they landed in the record loop, the play loop drew them repeated, and the synth's output meter moved on playback. Self-check covers slice replay and the wrapped note-off | hearing it, a real MIDI keyboard, typing on the computer keyboard, your own plugins and their windows |
| M5 | written | compiles. No MIDI controller has been connected | learn, pedals, faders, curves |

How to build and run:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Looper selfcheck
build/selfcheck
open build/Looper_artefacts/Release/Looper.app
```

`Looper --snapshot out.png [n]` (run from `build/`) renders the window with a demo song to a PNG and quits. It is how the UI was checked without screen control.
