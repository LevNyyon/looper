// All UI declarations. ArrangeView.cpp: the timeline. Panels.cpp: everything around it. Main.cpp: the window.
#pragma once
#include "AudioHost.h"

struct MainComponent;

namespace col
{
    const juce::Colour bg { 0xff15171a }, panel { 0xff1c1f23 }, lane { 0xff202328 }, laneSel { 0xff282c33 },
                       grid { 0xff31363d }, gridBar { 0xff4a515b }, text { 0xffdde1e7 }, dim { 0xff8a919c },
                       rec { 0xffe5484d }, play { 0xff3fcf6f }, captured { 0xfff2c94c }, scripted { 0xff4cc9f0 },
                       loop { 0xff4a90e2 }, head { 0xffffffff };
}
juce::Colour trackColour (int index);
juce::String barBeat (const Song&, double beat);                 // "3.2"
juce::String targetText (Song&, const Target&);
struct DrumKey { char key; int note; const char* name; };
extern const DrumKey drumMap[12];
struct Panel;
std::unique_ptr<Panel> makeDrumPads (MainComponent&);
float curveAt (const Region&, float x);                          // scripted control curve, x and result in 0..1
void drawTake (juce::Graphics&, juce::Rectangle<float> area, const alk::Take&, double sliceStart, double sliceLen, double samplesPerPx);

// Runs command and control tracks. Ticked from the UI timer.
// ponytail: UI thread on a 10 ms timer, so triggers are as late as the slowest repaint in front of them.
// Move track power, plugin bypass and position jumps to the audio thread if a late pedal is ever audible.
struct Automation
{
    explicit Automation (AudioHost& h) : host (h) {}
    bool tick (const std::vector<AudioHost::Controller>& events);    // true if a track button state changed
    float get (const Target&);
    void set (const Target&, float value);

private:
    void fire (const Region&);
    struct State { int regionId = 0; float entry = 0, entryDb = 0; };
    struct Pending { double at; int trackId, regionId; };
    AudioHost& host;
    std::map<int, State> state;         // by track id
    std::vector<Pending> pending;
    bool changed = false;
    double lastPos = 0;
};

struct ArrangeView : juce::Component
{
    explicit ArrangeView (MainComponent&);
    static constexpr int rulerH = 24;
    double pxPerBeat = 22, scroll = 0;          // scroll in beats

    int trackH() const;
    float xOf (double beat) const { return (float) ((beat - scroll) * pxPerBeat); }
    double beatAt (float x) const { return x / pxPerBeat + scroll; }
    double snap (double beat) const;
    void playheadMoved();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

private:
    enum Drag { None, Move, ResizeL, ResizeR, Loop, Band };
    struct Item { Region orig; int trackIndex; };   // one region of a group move, as it was at mouse down
    void putBack (bool originals);
    void zoom (float factor, float aroundX);
    void contextMenu (Track&, Region* hit, double beat);
    void place (int trackId, Region::Kind, double beat, int source, bool captured);
    void rename (int regionId);
    juce::Rectangle<float> boundsOf (int trackIndex, const Region&) const;

    MainComponent& ui;
    Drag drag = None;
    int dragTrack = 0, dragId = 0;
    double dragBeat0 = 0;
    Region orig;
    bool moved = false, created = false, copying = false;
    std::vector<Item> items;
    std::vector<int> bandBase;                      // selection a shift-drag adds to
    juce::Point<float> bandStart, bandEnd;
    int dragTrackIndex0 = 0;
    float lastHeadX = -1;
};

// A panel rebuilds its child widgets from the song whenever something changes.
struct Panel : juce::Component
{
    explicit Panel (MainComponent& u) : ui (u) {}
    virtual void rebuild() = 0;
    void resized() override { rebuild(); }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }

protected:
    template <typename T, typename... A>
    T& add (juce::Rectangle<int> r, A&&... a)
    {
        auto* c = new T (std::forward<A> (a)...);
        items.add (c);
        addAndMakeVisible (c);
        c->setBounds (r);
        return *c;
    }
    juce::Label& label (juce::Rectangle<int>, const juce::String&, juce::Colour = col::dim);
    juce::Label& field (juce::Rectangle<int>, const juce::String&, std::function<void (const juce::String&)>);
    juce::TextButton& button (juce::Rectangle<int>, const juce::String&, std::function<void()>);
    juce::TextButton& toggle (juce::Rectangle<int>, const juce::String&, bool on, juce::Colour onColour, std::function<void (bool)>);
    juce::ComboBox& combo (juce::Rectangle<int>, const juce::StringArray&, int selected, std::function<void (int)>);
    juce::Slider& slider (juce::Rectangle<int>, double lo, double hi, double value, const juce::String& suffix, std::function<void (double)>);

    MainComponent& ui;
    juce::OwnedArray<juce::Component> items;
};

struct TransportBar : Panel
{
    using Panel::Panel;
    void rebuild() override;
    juce::Label* pos = nullptr;
    juce::Label* cpu = nullptr;
};

struct Headers : Panel      // one row per track, left of the timeline
{
    using Panel::Panel;
    void rebuild() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    std::vector<float> levels;
};

struct TrackPanel : Panel   // the selected track's input, plugins, output, mixer
{
    using Panel::Panel;
    void rebuild() override;
};

struct Inspector : Panel    // the selected region
{
    using Panel::Panel;
    void rebuild() override;
};

struct MainComponent : juce::Component, juce::Timer, juce::AsyncUpdater, juce::MidiKeyboardState::Listener
{
    explicit MainComponent (AudioHost&);
    ~MainComponent() override;

    AudioHost& host;
    Automation automation { host };
    int selTrack = 0, selRegion = 0;    // ids. selRegion is the one the inspector shows
    std::vector<int> selection;         // every selected region, selRegion included
    bool isSelected (int id) const { return std::find (selection.begin(), selection.end(), id) != selection.end(); }
    int snapIndex = 2;                  // into the snap menu
    double snapBeats = 1.0;             // 0 = off
    int learnTrack = 0;                 // automation track waiting for a controller

    Track* track() { return host.song.track (selTrack); }
    Region* region() { return host.song.region (selRegion); }
    void select (int trackId, int regionId);
    void refresh() { triggerAsyncUpdate(); }        // always async: widgets may be mid-callback
    void deleteSelected();
    void drum (int note, bool down);                // drum pads and their computer keys
    bool keyStateChanged (bool isKeyDown) override;
    void addTrackMenu (juce::Component* target);    // null: at the mouse, for right-clicks
    void pluginMenu (juce::Component& target, int trackId, int slot, bool instruments);
    void targetMenu (juce::Component& target, int regionId);
    void openSong();
    void saveSongAs();
    void showPrefs();

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override { host.injectMidi (juce::MidiMessage::noteOn (channel, note, velocity)); }
    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity) override { host.injectMidi (juce::MidiMessage::noteOff (channel, note, velocity)); }

    // shown for instrument tracks: click the keys, or just play on the computer keyboard
    juce::MidiKeyboardState keyState;
    juce::MidiKeyboardComponent keyboard { keyState, juce::MidiKeyboardComponent::horizontalKeyboard };
    std::map<int, std::pair<int, bool>> heldKeys;   // key code -> note, is it a drum
    std::unique_ptr<Panel> pads;                    // the drum map, shown instead of the piano on drum tracks
    TransportBar transport { *this };
    Headers headers { *this };
    ArrangeView arrange { *this };
    TrackPanel trackPanel { *this };
    Inspector inspector { *this };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this };
    std::vector<AudioHost::Controller> events;
    int ticks = 0;
};
