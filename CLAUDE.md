# Looper, notes for Claude

`PLAN.md` is the source of truth for scope, decisions and deliberate shortcuts. Read it before changing code.

## Building arrangements in the running app from plain language

When asked to build, change or describe an arrangement in Looper (for example "two bars of kick and snare, then layer hats, then bass"), do not edit code and do not touch song files. Drive the running app:

1. Read `docs/CONTROL.md` for the command format and how to turn a sentence into regions.
2. Read `~/Library/Application Support/Looper/control/state.json` to see the song as it is now.
3. Write one command file into `~/Library/Application Support/Looper/control/in/` (any name ending in `.json`).
4. Wait a second, read `state.json` again, and check `applied` matches your file and `errors` is empty. Report what was built in bars, and any error in plain words.

Only start with `new_song` when asked for a new song. Otherwise add to what is there.
If `state.json` does not change, the app is not running: `open build/Looper_artefacts/Release/Looper.app`.

## Code rules

- C++17 and JUCE 8 only. No new dependencies.
- `engine/` stays free of devices and UI. Nothing on the audio thread may allocate, lock or do file IO.
- One self-check (`build/selfcheck`). No test suites.
- Mark deliberate shortcuts with a `ponytail:` comment naming the limit and the upgrade path.
- Build: `cmake --build build --target Looper selfcheck`. Every rebuild makes macOS ask for the microphone again (ad hoc signing).
