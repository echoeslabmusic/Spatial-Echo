// InteractiveWalkthrough: Live first-run coach marks over the plug-in editor.

#include "InteractiveWalkthrough.h"
#include "CRTLookAndFeel.h"
#include "../UIConstants.h"
#include <cmath>

namespace
{
void styleButton(juce::TextButton& button, const juce::String& componentId, const juce::String& title)
{
    button.setComponentID(componentId);
    button.setTitle(title);
    button.setWantsKeyboardFocus(true);
    button.setColour(juce::TextButton::buttonColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.72f));
    button.setColour(juce::TextButton::buttonOnColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.18f));
    button.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
    button.setColour(juce::TextButton::textColourOnId, CRTLookAndFeel::getTerminalGreen());
}
}  // namespace

juce::Font InteractiveWalkthrough::TertiaryButtonLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize);
}

void InteractiveWalkthrough::TertiaryButtonLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                                             const juce::Colour&, bool highlighted,
                                                                             bool down)
{
    if (!highlighted && !down)
        return;

    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(down ? 0.14f : 0.07f));
    g.fillRoundedRectangle(button.getLocalBounds().toFloat(), 2.0f);
}

void InteractiveWalkthrough::TertiaryButtonLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                                                       bool, bool)
{
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                          : juce::TextButton::textColourOffId));
    g.drawText(button.getButtonText().toUpperCase(), button.getLocalBounds(), juce::Justification::centredLeft);
}

const std::array<InteractiveWalkthrough::Step, 58>& InteractiveWalkthrough::steps()
{
    static const std::array<Step, 58> value{
        {{"MAKE YOUR FIRST ECHO",
          "Spatial Echo turns a sound into tape repeats, reverb, and movement around you in headphones. Play a loop "
          "and we will build the sound from left to right.",
          "", Target::none, ExpandedTarget::none, SpotlightDetail::none, CompletionSignal::none, "WELCOME",
          "FOLLOW THE SIGNAL PATH", 0},
         {"START WITH A PRESET",
          "The preset name at the top is the fastest way into Spatial Echo. Use the left and right arrows to audition "
          "sounds one by one. The preset name opens the full browser, where sounds are grouped by what you want them "
          "to do.",
          "", Target::presets, ExpandedTarget::none, SpotlightDetail::none, CompletionSignal::none, "PRESETS",
          "FIND A STARTING POINT", 1},
         {"CHOOSE BY RESULT",
          "Choose a result: Echo & Rhythm, Reverb & Atmosphere, Spatial & Motion, or Experimental. Each factory preset "
          "belongs to one group in the left column. All Presets shows the whole library. User shows your saved sounds.",
          "Choose a group to filter its sounds.", Target::presets, ExpandedTarget::presetBrowser,
          SpotlightDetail::presetCategories, CompletionSignal::none, "PRESETS", "FIND A STARTING POINT", 1},
         {"AUDITION THE OPTIONS",
          "The main list shows the sounds in the active group or shortcut. Each group offers distinct starting points "
          "for the same result. Click a preset or use Up/Down to hear each one in your track. The LOADED tag marks "
          "your current sound.",
          "Load another preset and compare.", Target::presets, ExpandedTarget::presetBrowser,
          SpotlightDetail::presetList, CompletionSignal::presetAudition, "PRESETS", "FIND A STARTING POINT", 1},
         {"READ THE SOUND DESCRIPTION",
          "The strip above the buttons describes the preset under your mouse, or your loaded sound when you are not "
          "hovering. Read it before loading, then compare it while you audition so you can choose by what a sound adds "
          "to the track.",
          "", Target::presets, ExpandedTarget::presetBrowser, SpotlightDetail::presetIntent, CompletionSignal::none,
          "PRESETS", "FIND A STARTING POINT", 1},
         {"SEARCH BY SOUND OR USE",
          "The filter searches names, descriptions, musical uses, and inspirations within your active group. Try "
          "guitar, drums, or forest. The count shows how many matches remain. Use All Presets to search the whole "
          "bank. Press Escape once to clear the search.",
          "Type a sound or instrument into the filter.", Target::presets, ExpandedTarget::presetBrowser,
          SpotlightDetail::presetFilter, CompletionSignal::presetFilter, "PRESETS", "FIND A STARTING POINT", 1},
         {"SAVE OR MOVE PRESETS",
          "Save As creates a new User preset. Open Library for Save, Import, Export, and Reveal: update your loaded "
          "User sound, move presets between setups, or open the preset folder. Init loads the starting state.",
          "", Target::presets, ExpandedTarget::presetBrowser, SpotlightDetail::presetActions, CompletionSignal::none,
          "PRESETS", "FIND A STARTING POINT", 1},
         {"MANAGE USER PRESETS",
          "Choose User to browse your saved sounds. Hover a User preset to reveal REN and DEL for rename and delete. "
          "User stays available when empty; Save As creates your first saved sound.",
          "", Target::presets, ExpandedTarget::presetBrowser, SpotlightDetail::presetUserPresets,
          CompletionSignal::none, "PRESETS", "FIND A STARTING POINT", 1},
         {"CHOOSE A TAPE FLAVOR",
          "Flavor sets the personality of every repeat. Clean stays clear. Warm rounds the edges. Hot adds stronger "
          "saturation. Cassette adds wobble and darkness. Worn sounds aged and unstable.",
          "Choose a Flavor and listen to the repeats.", Target::tape, ExpandedTarget::tapeMenu,
          SpotlightDetail::tapeFlavors, CompletionSignal::tapeFlavor, "TAPE", "COLOR AND CHARACTER", 2},
         {"TURN THE TAPE NOISE OFF",
          "Noise adds tape hiss to the repeats. Click Noise so its checkmark disappears. The Flavor still keeps its "
          "tone, wobble, and wear, which is useful when the track already has enough texture.",
          "Switch Noise off and listen to the cleaner space.", Target::tape, ExpandedTarget::tapeMenu,
          SpotlightDetail::tapeNoise, CompletionSignal::tapeNoise, "TAPE", "COLOR AND CHARACTER", 2},
         {"SET THE TAPE DRIVE",
          "Drive controls how hard the signal hits the tape. Lower settings stay cleaner and more open. Higher "
          "settings add saturation, compression, and a rougher edge to every repeat.",
          "Move Drive and listen for more grit and squeeze.", Target::tape, ExpandedTarget::none,
          SpotlightDetail::tapeDrive, CompletionSignal::tapeDrive, "TAPE", "COLOR AND CHARACTER", 2},
         {"CHOOSE THE ECHO PATTERN",
          "Mode decides which tape heads you hear. H1 is the first repeat; H2 and H3 arrive later. Single-head modes "
          "stay focused, while multi-head modes create patterns. E+REV adds reverb; REV ONLY removes the echoes.",
          "Compare a single head with a multi-head Mode.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delayMode, CompletionSignal::delayMode, "ECHO", "RHYTHM AND REPEATS", 3},
         {"SET A FREE ECHO TIME",
          "Synced presets such as Halo Orbit hand timing to the H1-H3 note values, so Rate is unavailable. Turn Sync "
          "off; the spotlight moves to Rate. Then set the repeat spacing by feel: left for more space, right for "
          "tighter echoes.",
          "Turn Sync off, then move Rate to find a groove.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delayRate, CompletionSignal::delayRate, "ECHO", "RHYTHM AND REPEATS", 3},
         {"LOCK THE ECHO TO THE BEAT",
          "Sync makes the echoes follow your DAW tempo. Rate hands timing to the H1-H3 note values, so tempo changes "
          "keep the pattern on the grid. Leave Sync off when you want timing by feel.",
          "Switch Sync and watch the head timing change.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delaySync, CompletionSignal::delaySync, "ECHO", "RHYTHM AND REPEATS", 3},
         {"BUILD THE HEAD RHYTHM",
          "With Sync on, each head can use a straight, triplet, or dotted note value. H1 sets one pulse; H2 and H3 can "
          "answer at different times. The open grid edits the highlighted head.",
          "Choose a division for the open head.", Target::delay, ExpandedTarget::delayDivisionMenu,
          SpotlightDetail::delayHeads, CompletionSignal::delayHeads, "ECHO", "RHYTHM AND REPEATS", 3},
         {"CONTROL HOW LONG IT REPEATS",
          "Feedback sends part of each echo around for another pass. Low settings give a short trail. Higher settings "
          "keep it going and push the tape character harder. Above 95% it can self-oscillate, so raise it while "
          "monitoring.",
          "Raise Feedback slowly and hear the trail grow.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delayFeedback, CompletionSignal::delayFeedback, "ECHO", "RHYTHM AND REPEATS", 3},
         {"SEND REPEATS LEFT AND RIGHT",
          "Ping-Pong alternates the feedback between left and right. Use it when a centered echo feels crowded or when "
          "you want obvious stereo movement. Switch it off to keep the repeats in their original stereo position.",
          "Switch Ping-Pong on and listen in stereo.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delayPingPong, CompletionSignal::delayPingPong, "ECHO", "RHYTHM AND REPEATS", 3},
         {"PLACE THE ECHO IN THE MIX",
          "Echo Level changes the echo branch without changing its timing, Feedback, reverb, or the main Mix. Set it "
          "after the rhythm feels right to place the repeats behind or in front of the source.",
          "Set Echo Level so the repeats sit in the track.", Target::delay, ExpandedTarget::none,
          SpotlightDetail::delayLevel, CompletionSignal::delayLevel, "ECHO", "RHYTHM AND REPEATS", 3},
         {"LET THE REPEATS RUN WITH DUB",
          "The DUB button under Echo Level stops new audio entering the delay while the repeats already on tape keep "
          "circulating. While Dub is on it turns amber and reads INPUT OFF. Use it to hold a word, chord, or texture, "
          "then click it again to catch the next phrase.",
          "Engage Dub after a phrase, then release it.", Target::delay, ExpandedTarget::none, SpotlightDetail::delayDub,
          CompletionSignal::delayDub, "ECHO", "RHYTHM AND REPEATS", 3},
         {"OPEN THE ECHO ENGINE",
          "Echo Engine holds the head spacing, a control you change less often. The tour opens it for you. PIN keeps "
          "the drawer visible while you work elsewhere in the plug-in.",
          "Use PIN when you want the drawer to stay open.", Target::echoEngine, ExpandedTarget::echoEngineDrawer,
          SpotlightDetail::echoDrawer, CompletionSignal::none, "ECHO", "RHYTHM AND REPEATS", 3},
         {"SPACE THE THREE HEADS",
          "Head Ratio places H2 and H3 around H1 when Sync is off. Classic feels like a tape machine. Idealized uses "
          "even spacing. Golden avoids an obvious cycle. Linear packs the heads closer. Sync uses the H1-H3 note "
          "values instead.",
          "Compare two Head Ratios with Sync off.", Target::echoEngine, ExpandedTarget::echoEngineDrawer,
          SpotlightDetail::echoHeadRatio, CompletionSignal::echoHeadRatio, "ECHO", "RHYTHM AND REPEATS", 3},
         {"READ THE ENGINE STATUS",
          "The Echo Engine label is a status light. Green is normal; amber and red show rising feedback heat. A red "
          "pulse means the host Power control is off. A flash means Auto-Panic cleared a runaway. Power is in your DAW "
          "parameter list.",
          "", Target::echoEngine, ExpandedTarget::none, SpotlightDetail::echoStatus, CompletionSignal::none, "ECHO",
          "RHYTHM AND REPEATS", 3},
         {"ARM THE RUNAWAY SAFETY",
          "Auto-Panic is an optional guard for a broken or extreme runaway echo, in the Spatial Echo menu. It clears "
          "the delay after a sustained blow-up while leaving musical self-oscillation alone, so Feedback remains a "
          "creative control.",
          "Turn Auto-Panic on if you want the safeguard armed.", Target::echoEngine, ExpandedTarget::wordmarkMenu,
          SpotlightDetail::echoAutoPanic, CompletionSignal::echoAutoPanic, "ECHO", "RHYTHM AND REPEATS", 3},
         {"OPEN THE REVERB CONTROLS",
          "REVERB opens the full shaping panel. Size, Decay, Tone, and Reverb Level stay on the main screen for quick "
          "moves. To hear a tail, choose an Echo Mode marked E+REV or REV ONLY.",
          "", Target::reverb, ExpandedTarget::none, SpotlightDetail::reverbOverview, CompletionSignal::none, "REVERB",
          "DEPTH AND SPACE", 4},
         {"CHOOSE SPRING OR CLOUD",
          "Spring gives the sound a bright, physical bounce like a real tank. Cloud makes a smoother, denser stereo "
          "space that can sit behind the source or bloom into a wash. Choose the basic character before shaping the "
          "tail.",
          "Switch engines and compare the same part of your loop.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbEngine, CompletionSignal::reverbEngine, "REVERB", "DEPTH AND SPACE", 4},
         {"PICK A STARTING SPACE",
          "The row below the engine loads a complete starting point for that engine. Use the arrows to compare tight, "
          "bright, dark, wide, or long spaces. After you edit one, the row reads Custom.",
          "Use the arrows to audition another starting space.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbPreset, CompletionSignal::reverbPreset, "REVERB", "DEPTH AND SPACE", 4},
         {"SET THE SIZE AND DECAY",
          "Size changes the apparent scale of the spring tank or room. Decay changes how long the tail continues. Set "
          "Size for the sense of place you want, then use Decay to leave clear space between notes.",
          "Set the space, then shorten or lengthen the tail.", Target::reverb, ExpandedTarget::none,
          SpotlightDetail::reverbSizeDecay, CompletionSignal::reverbSizeDecay, "REVERB", "DEPTH AND SPACE", 4},
         {"DARKEN OR BRIGHTEN THE TAIL",
          "Tone moves the tail from dark to bright. Darker settings help reverb sit behind a vocal, drum, or lead. "
          "Brighter settings bring the tail forward and reveal more high-frequency detail.",
          "Set Tone so the tail sits where you want it.", Target::reverb, ExpandedTarget::none,
          SpotlightDetail::reverbTone, CompletionSignal::reverbTone, "REVERB", "DEPTH AND SPACE", 4},
         {"SET THE REVERB LEVEL",
          "Reverb Level changes how loud the finished reverb is without changing its shape or length. Turn it down "
          "when the tail feels right but takes over the mix; turn it up when the space gets lost.",
          "Set the tail at the right level for the track.", Target::reverb, ExpandedTarget::none,
          SpotlightDetail::reverbLevel, CompletionSignal::reverbLevel, "REVERB", "DEPTH AND SPACE", 4},
         {"SEPARATE AND SPREAD THE TAIL",
          "Pre-Delay leaves a gap before the reverb arrives, keeping a hit or vocal upfront. Width moves the wet image "
          "from centered to wide. Add separation first, then spread the tail as far as the mix needs.",
          "Add separation, then set the stereo spread.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbPreDelayWidth, CompletionSignal::reverbPreDelayWidth, "REVERB", "DEPTH AND SPACE", 4},
         {"SHAPE THE REVERB CHARACTER",
          "Character changes how the chosen engine builds its tail. Spring moves from controlled to more lively and "
          "springy. Cloud moves from lighter and clearer to a denser, blooming wash.",
          "Move Character and hear the tail change shape.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbCharacter, CompletionSignal::reverbCharacter, "REVERB", "DEPTH AND SPACE", 4},
         {"ADD MOTION TO THE TAIL",
          "Mod adds gentle pitch movement and chorusing inside the reverb. Keep it low for a steady room; raise it for "
          "a softer, drifting wash. The Motion section later moves echoes around the listener.",
          "Raise Mod until the tail begins to drift.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbMod, CompletionSignal::reverbMod, "REVERB", "DEPTH AND SPACE", 4},
         {"MAKE ROOM FOR THE SOURCE",
          "Ducking lowers the audible reverb while a new note, drum hit, or vocal phrase is present, then lets the "
          "tail rise in the gaps. Raise it when the reverb sounds right alone but masks the source.",
          "Raise Ducking if the reverb masks new sounds.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbDucking, CompletionSignal::reverbDucking, "REVERB", "DEPTH AND SPACE", 4},
         {"KEEP THE PANEL OPEN",
          "PIN keeps these controls open while you work elsewhere in the plug-in. Use it when comparing settings "
          "across the echo, reverb, and spatial sections. Unpin it when you want the panel to close normally.",
          "Use PIN when you want this panel to stay open.", Target::reverb, ExpandedTarget::reverbDrawer,
          SpotlightDetail::reverbPin, CompletionSignal::none, "REVERB", "DEPTH AND SPACE", 4},
         {"RESET THE REVERB",
          "RESET DEFAULTS returns the reverb to Spring, its first starting space, Parallel routing, and no Ducking. "
          "Use it when you want to rebuild from a known starting point. It changes the current sound immediately.",
          "", Target::reverb, ExpandedTarget::reverbDrawer, SpotlightDetail::reverbReset, CompletionSignal::none,
          "REVERB", "DEPTH AND SPACE", 4},
         {"CHOOSE HOW THEY CONNECT",
          "Parallel keeps echo and reverb on separate paths. Echo > Reverb washes the repeats in a tail. Reverb > Echo "
          "sends the wash through the tape repeats. Choose the route after shaping both effects.",
          "Choose a route and listen to the tail.", Target::reverbRouting, ExpandedTarget::none, SpotlightDetail::none,
          CompletionSignal::reverbRouting, "REVERB", "DEPTH AND SPACE", 4},
         {"TURN ON SPATIAL",
          "Spatial turns the echo into a headphone-ready 3D sound field. Enable it before placing or moving sound. Judge "
          "front, back, height, and distance on headphones.",
          "Switch Enable on and play your loop.", Target::spatialPosition, ExpandedTarget::none,
          SpotlightDetail::spatialEnable, CompletionSignal::spatialEnable, "SPATIAL", "PLACE THE SOUND", 5},
         {"START WITH A SPATIAL PRESET",
          "Spatial presets are starting points for Width, Xover, Chain, and Motion. Use the arrows to audition, click "
          "the name for all 28, and use REVERT after editing a preset. Your Enable and head positions stay yours.",
          "Choose a spatial preset.", Target::spatialPosition, ExpandedTarget::spatialPresetMenu,
          SpotlightDetail::spatialPreset, CompletionSignal::spatialPreset, "SPATIAL", "PLACE THE SOUND", 5},
         {"CHOOSE LINKED OR PER-HEAD",
          "Linked gives every active echo head one shared position, which is the quickest way to make one clear 3D "
          "image. PER-HEAD gives H1, H2, and H3 their own positions.",
          "Switch between Linked and PER-HEAD.", Target::spatialPosition, ExpandedTarget::none,
          SpotlightDetail::spatialMode, CompletionSignal::spatialMode, "SPATIAL", "PLACE THE SOUND", 5},
         {"CHOOSE THE HEAD TO EDIT",
          "In PER-HEAD mode, H1, H2, and H3 select one echo. ALL moves every active head while keeping the formation "
          "together. Heads outside the current Echo Mode stay dim.",
          "Select H1, H2, H3, or ALL.", Target::spatialPosition, ExpandedTarget::none, SpotlightDetail::spatialHeads,
          CompletionSignal::none, "SPATIAL", "PLACE THE SOUND", 5},
         {"PLACE IT AROUND YOUR HEAD",
          "Drag a glowing marker around the head. Left and right circle the listener; up and down change height. "
          "Shift-drag holds one axis, and the marker snaps to Front, Left, Right, Behind, Above, or Below.",
          "Drag the marker to a new position.", Target::spatialPosition, ExpandedTarget::none,
          SpotlightDetail::spatialView, CompletionSignal::spatialPosition, "SPATIAL", "PLACE THE SOUND", 5},
         {"SET DISTANCE AND VIEW",
          "Use the mouse wheel over a marker to move that echo closer or farther. Drag empty space to orbit the "
          "view, Cmd/Ctrl-wheel to zoom, and double-click empty space to return to front. Double-click a marker "
          "resets its sound position.",
          "Scroll over the head view to change distance.", Target::spatialPosition, ExpandedTarget::none,
          SpotlightDetail::spatialDistanceView, CompletionSignal::spatialDistance, "SPATIAL", "PLACE THE SOUND", 5},
         {"TYPE OR LOCK A POSITION",
          "Click the position readout to type an exact left/right angle, height, and distance. Lock any axis you want "
          "to preserve. Arrow keys fine-tune; Shift+Up/Down changes distance.",
          "Type a value or lock one axis.", Target::spatialPosition, ExpandedTarget::spatialPositionEditor,
          SpotlightDetail::spatialReadout, CompletionSignal::spatialExactPosition, "SPATIAL", "PLACE THE SOUND", 5},
         {"SET THE SPATIAL AMOUNT",
          "Wet blends the placed version with the original signal. Use 100% for the clearest position; lower it when "
          "you want the 3D cue to sit more subtly in the track.",
          "Lower Wet, then bring it back up.", Target::spatialBlend, ExpandedTarget::spatialBlendDrawer,
          SpotlightDetail::spatialWet, CompletionSignal::spatialWet, "SPATIAL", "PLACE THE SOUND", 5},
         {"KEEP THE LOW END SOLID",
          "Xover leaves frequencies below the selected point in their original stereo image and spatializes the rest. "
          "Raise it to steady bass and kick; lower it for fuller-range placement.",
          "Raise Xover and listen to the low end.", Target::spatialBlend, ExpandedTarget::spatialBlendDrawer,
          SpotlightDetail::spatialCrossover, CompletionSignal::spatialCrossover, "SPATIAL", "PLACE THE SOUND", 5},
         {"FOCUS OR SPREAD THE SOURCE",
          "Width changes the size of the source at its chosen position. Focused feels point-like and precise; Natural "
          "is balanced; Wide creates a broader, more diffuse image.",
          "Move from Focused to Wide.", Target::spatialBlend, ExpandedTarget::spatialBlendDrawer,
          SpotlightDetail::spatialWidth, CompletionSignal::spatialWidth, "SPATIAL", "PLACE THE SOUND", 5},
         {"CHOOSE WHERE PLACEMENT HAPPENS",
          "Chain shapes Linked mode. After Effects places the finished plug-in output as one image. Before Effects "
          "places the sound first, so echoes and reverb grow from that location. PER-HEAD places the echo heads "
          "individually.",
          "Compare After Effects and Before Effects in Linked mode.", Target::spatialBlend,
          ExpandedTarget::spatialBlendDrawer, SpotlightDetail::spatialChain, CompletionSignal::spatialChain, "SPATIAL",
          "PLACE THE SOUND", 5},
         {"SET THE SPATIAL QUALITY",
          "Max gives the most detailed placement. Balanced uses less processing for busy sessions. Performance is the "
          "lightest option when you need more plug-in instances.",
          "Choose the quality that fits this session.", Target::spatialBlend, ExpandedTarget::spatialBlendDrawer,
          SpotlightDetail::spatialQuality, CompletionSignal::spatialQuality, "SPATIAL", "PLACE THE SOUND", 5},
         {"CHOOSE THE MOTION PATH",
          "Still holds position. Orbit circles it. Pendulum swings left and right. Figure-8 moves across and up/down. "
          "Breathe moves near/far. Wander drifts in every direction. Spatial must be on to hear movement.",
          "Pick a moving pattern.", Target::spatialMotion, ExpandedTarget::none, SpotlightDetail::motionPatterns,
          CompletionSignal::motionPattern, "MOTION", "ADD MOVEMENT", 6},
         {"SET A FREE SPEED",
          "With Sync off, Rate runs freely. Low values create slow evolution; high values create fast motion and "
          "texture. Turn Sync on when the full movement cycle should follow the song.",
          "Switch Sync off and adjust Rate.", Target::spatialMotion, ExpandedTarget::none, SpotlightDetail::motionRate,
          CompletionSignal::motionRate, "MOTION", "ADD MOVEMENT", 6},
         {"LOCK MOTION TO THE SONG",
          "With Sync on, Division sets one full movement cycle. Straight values follow regular time, T values use "
          "triplets, and D values are dotted. Long choices create slow movement across several bars.",
          "Turn Sync on and choose a division.", Target::spatialMotion, ExpandedTarget::motionDivisionMenu,
          SpotlightDetail::motionDivision, CompletionSignal::motionDivision, "MOTION", "ADD MOVEMENT", 6},
         {"SET HOW FAR IT MOVES",
          "Depth scales how far Pendulum, Figure-8, Breathe, and Wander travel. At 0% they stop. Orbit still circles "
          "above 0%; Depth changes its height and near/far sweep.",
          "Move Depth toward 0, then higher.", Target::spatialMotion, ExpandedTarget::none,
          SpotlightDetail::motionDepth, CompletionSignal::motionDepth, "MOTION", "ADD MOVEMENT", 6},
         {"CHOOSE THE GROUP MOVEMENT",
          "In PER-HEAD mode, Unison moves all active heads at the same point in the cycle. Constellation spaces them "
          "evenly around the path. Cascade makes them follow one another.",
          "Compare Unison and Cascade.", Target::spatialMotion, ExpandedTarget::motionFineTune,
          SpotlightDetail::motionChoreo, CompletionSignal::motionChoreo, "MOTION", "ADD MOVEMENT", 6},
         {"SET THE MOTION SPREAD",
          "Spread controls the gap between heads along a Constellation or Cascade cycle. At 0% they line up; higher "
          "values separate the motion. It becomes active in those two modes.",
          "Choose Constellation or Cascade, then raise Spread.", Target::spatialMotion, ExpandedTarget::motionFineTune,
          SpotlightDetail::motionSpread, CompletionSignal::motionSpread, "MOTION", "ADD MOVEMENT", 6},
         {"CHOOSE WHEN MOTION RESTARTS",
          "Free keeps the cycle running. Transport follows the song position. Random chooses a new starting phase each "
          "cycle. Note restarts on each MIDI note, so it works in the standalone app; in a DAW it is greyed out.",
          "Compare Free, Transport, and Random.", Target::spatialMotion, ExpandedTarget::motionFineTune,
          SpotlightDetail::motionRetrig, CompletionSignal::motionRetrig, "MOTION", "ADD MOVEMENT", 6},
         {"BALANCE YOUR MIX",
          "Echo Level and Reverb Level balance the two effects. Mix blends your dry track with the effects. Mix Lock "
          "holds the current Mix setting while the lock is on, which is useful when the plug-in is on a send. Out sets "
          "the final plug-in level.",
          "Adjust Mix or Out.", Target::mixOutput, ExpandedTarget::none, SpotlightDetail::none,
          CompletionSignal::mixOutput, "OUTPUT", "BALANCE THE RESULT", 7},
         {"FIND THE EXTRAS",
          "Click the Spatial Echo name for Undo, Redo, the Oscilloscope, Tooltips, Output Protection, Auto-Panic, this "
          "tour, keyboard shortcuts, and About. Output Protection catches sudden spikes; set your level with Out.",
          "", Target::utilities, ExpandedTarget::wordmarkMenu, SpotlightDetail::none, CompletionSignal::none,
          "UTILITIES", "HELP AND SAFETY", 8},
         {"YOU'RE READY",
          "Start with a preset. Shape the tape and echoes. Add reverb and movement. Then set Mix and Out for the "
          "track. You can replay this tour any time from the Spatial Echo menu.",
          "", Target::none, ExpandedTarget::none, SpotlightDetail::none, CompletionSignal::none, "COMPLETE",
          "YOU'RE READY", 0}}};
    return value;
}

InteractiveWalkthrough::InteractiveWalkthrough()
{
    setVisible(false);
    setAlwaysOnTop(true);
    setWantsKeyboardFocus(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle("Learn Spatial Echo");
    setDescription("Learn Spatial Echo one control at a time");

    styleButton(backButton, "walkthroughBack", "Go to the previous step");
    styleButton(nextButton, "walkthroughNext", "Go to the next step");
    styleButton(exitButton, "walkthroughExit", "Exit the tour");
    styleButton(suppressButton, "walkthroughSuppress", "Do not show this tour automatically again");
    suppressButton.setLookAndFeel(&tertiaryButtonLookAndFeel);
    exitButton.setExplicitFocusOrder(1);
    suppressButton.setExplicitFocusOrder(2);
    backButton.setExplicitFocusOrder(3);
    nextButton.setExplicitFocusOrder(4);

    backButton.onClick = [this] { goBack(); };
    nextButton.onClick = [this] { advance(); };
    exitButton.onClick = [this] { dismiss(Dismissal::notNow); };
    suppressButton.onClick = [this] { dismiss(Dismissal::dontShowAgain); };

    addAndMakeVisible(backButton);
    addAndMakeVisible(nextButton);
    addAndMakeVisible(exitButton);
    addAndMakeVisible(suppressButton);
    updateButtons();
}

InteractiveWalkthrough::~InteractiveWalkthrough()
{
    detachFromModalComponent();
    suppressButton.setLookAndFeel(nullptr);
    if (focusListenerRegistered)
        juce::Desktop::getInstance().removeFocusChangeListener(this);
}

void InteractiveWalkthrough::start()
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    previousFocus = focused != nullptr && focused->getTopLevelComponent() == getTopLevelComponent() ? focused : nullptr;
    currentStep = 0;
    actionComplete = false;
    completionPulse = 0.0f;
    attentionPhase = 0.0f;
    sectionTransition = 0.0f;
    setVisible(true);
    toFront(true);
    refreshTarget();
    updateButtons();
    updateAccessibilityText();
    if (isShowing())
    {
        if (!focusListenerRegistered)
        {
            juce::Desktop::getInstance().addFocusChangeListener(this);
            focusListenerRegistered = true;
        }
        nextButton.grabKeyboardFocus();
    }
    if (onActiveChanged)
        onActiveChanged(true);
}

void InteractiveWalkthrough::advance()
{
    if (isFinish())
    {
        dismiss(Dismissal::completed);
        return;
    }

    const juce::String previousSection(current().section);
    ++currentStep;
    actionComplete = false;
    completionPulse = 0.0f;
    attentionPhase = 0.0f;
    sectionTransition = previousSection != current().section ? 1.0f : 0.0f;
    if (onStepChanged)
        onStepChanged(current().target, current().expandedTarget, current().spotlightDetail);
    refreshTarget();
    updateButtons();
    updateAccessibilityText();
    repaint(cardBounds);
    if (isShowing())
    {
        if (spotlightFocusComponent != nullptr && spotlightFocusComponent->isShowing())
            spotlightFocusComponent->grabKeyboardFocus();
        else
            nextButton.grabKeyboardFocus();
    }
}

void InteractiveWalkthrough::goBack()
{
    if (currentStep <= 0)
        return;

    const juce::String previousSection(current().section);
    --currentStep;
    actionComplete = false;
    completionPulse = 0.0f;
    attentionPhase = 0.0f;
    sectionTransition = previousSection != current().section ? 1.0f : 0.0f;
    if (onStepChanged)
        onStepChanged(current().target, current().expandedTarget, current().spotlightDetail);
    refreshTarget();
    updateButtons();
    updateAccessibilityText();
    repaint(cardBounds);
    if (isShowing())
    {
        if (spotlightFocusComponent != nullptr && spotlightFocusComponent->isShowing())
            spotlightFocusComponent->grabKeyboardFocus();
        else
            nextButton.grabKeyboardFocus();
    }
}

void InteractiveWalkthrough::dismiss(Dismissal dismissal)
{
    detachFromModalComponent();
    if (focusListenerRegistered)
    {
        juce::Desktop::getInstance().removeFocusChangeListener(this);
        focusListenerRegistered = false;
    }
    setVisible(false);
    interactionBounds = {};
    spotlightBounds = {};
    if (onActiveChanged)
        onActiveChanged(false);
    if (onDismissed)
        onDismissed(dismissal);

    if (previousFocus != nullptr && previousFocus->isShowing())
        previousFocus->grabKeyboardFocus();
    previousFocus = nullptr;
}

void InteractiveWalkthrough::setExternalPopupActive(bool active)
{
    externalPopupActive = active;
    if (!active)
        globalFocusChanged(juce::Component::getCurrentlyFocusedComponent());
}

void InteractiveWalkthrough::attachToCurrentPopupMenu()
{
    auto* modal = juce::Component::getCurrentlyModalComponent();
    if (modal == nullptr || modal->getName() != "menu")
        return;

    attachToModalComponent(modal, false);
}

void InteractiveWalkthrough::attachToCurrentModalComponent()
{
    attachToModalComponent(juce::Component::getCurrentlyModalComponent(), true);
}

void InteractiveWalkthrough::attachToModalComponent(juce::Component* component, bool includeDescendants)
{
    detachFromModalComponent();
    if (component == nullptr)
        return;

    attachedModalComponent = component;
    const auto attach = [this](auto&& self, juce::Component& target) -> void
    {
        target.addKeyListener(this);
        attachedModalKeyComponents.emplace_back(&target);
        for (auto* child : target.getChildren())
            if (child != nullptr)
                self(self, *child);
    };
    if (includeDescendants)
        attach(attach, *component);
    else
    {
        component->addKeyListener(this);
        attachedModalKeyComponents.emplace_back(component);
    }
    popupCoachNavigationActive = !includeDescendants;

    // Keep a visible coach-card focus target even though JUCE redirects the
    // actual key event to its modal menu window.
    if (!includeDescendants && isShowing())
        nextButton.grabKeyboardFocus();
    else if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        if (focused == component || component->isParentOf(focused))
            modalFocusBeforeCoach = focused;
}

void InteractiveWalkthrough::detachFromModalComponent()
{
    for (auto& component : attachedModalKeyComponents)
        if (component != nullptr)
            component->removeKeyListener(this);
    attachedModalKeyComponents.clear();
    attachedModalComponent = nullptr;
    modalFocusBeforeCoach = nullptr;
    popupCoachNavigationActive = false;
}

void InteractiveWalkthrough::dismissAttachedModalComponent()
{
    const juce::Component::SafePointer<juce::Component> modal = attachedModalComponent;
    detachFromModalComponent();
    if (modal != nullptr && modal->isCurrentlyModal(false))
        modal->exitModalState(0);
}

void InteractiveWalkthrough::refreshTarget()
{
    const auto previousSpotlight = spotlightBounds;
    const auto previousCard = cardBounds;
    interactionBounds = {};
    spotlightBounds = {};
    coveringSurfaceBounds = {};
    if (targetResolver && current().target != Target::none)
    {
        if (auto target = targetResolver(current().target, current().expandedTarget, current().spotlightDetail))
        {
            interactionBounds = target->getIntersection(getLocalBounds());
            spotlightBounds = interactionBounds.expanded(kTargetPadding).getIntersection(getLocalBounds());
        }
    }
    if (coveringSurfaceResolver)
        if (auto surface = coveringSurfaceResolver(current().expandedTarget))
            coveringSurfaceBounds = surface->getIntersection(getLocalBounds());

    layoutCard();
    if (spotlightBounds != previousSpotlight || cardBounds != previousCard)
    {
        auto dirty =
            previousSpotlight.getUnion(spotlightBounds).getUnion(previousCard).getUnion(cardBounds).expanded(4);
        repaint(dirty);
    }
}

void InteractiveWalkthrough::refreshActionState()
{
    if (!isVisible())
        return;

    refreshTarget();
    attentionPhase += 1.0f / 48.0f;
    if (attentionPhase >= 1.0f)
        attentionPhase -= 1.0f;

    const float previousSectionTransition = sectionTransition;
    if (sectionTransition > 0.0f)
        sectionTransition = juce::jmax(0.0f, sectionTransition - 0.03f);

    const float previousPulse = completionPulse;
    const bool nowComplete = actionComplete || (current().completionSignal != CompletionSignal::none &&
                                                completionResolver && completionResolver(current().completionSignal));

    if (nowComplete && !actionComplete)
        completionPulse = 1.0f;
    else if (completionPulse > 0.0f)
        completionPulse = juce::jmax(0.0f, completionPulse - 0.07f);

    const bool completionChanged = nowComplete != actionComplete;
    if (completionChanged)
    {
        actionComplete = nowComplete;
        updateAccessibilityText();
    }

    if (!juce::exactlyEqual(sectionTransition, previousSectionTransition))
        repaint(cardBounds);
    else if (completionChanged || !juce::exactlyEqual(completionPulse, previousPulse))
        repaint(cardBounds.getUnion(spotlightBounds).expanded(4));
    else if (!spotlightBounds.isEmpty())
        repaint(spotlightBounds.expanded(6));
}

const InteractiveWalkthrough::Step& InteractiveWalkthrough::current() const
{
    return steps()[static_cast<size_t>(juce::jlimit(0, static_cast<int>(steps().size()) - 1, currentStep))];
}

void InteractiveWalkthrough::updateButtons()
{
    auto setAccessibleTitle = [this](juce::Button& button, const juce::String& title)
    {
        if (button.getTitle() == title)
            return;

        button.setTitle(title);
        if (isShowing())
            if (auto* handler = button.getAccessibilityHandler())
                handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
    };

    backButton.setVisible(!isWelcome());
    backButton.setEnabled(!isWelcome());
    nextButton.setButtonText(isWelcome() ? "START TOUR" : (isFinish() ? "FINISH" : "NEXT"));
    exitButton.setButtonText(isWelcome() ? "NOT NOW" : "EXIT");
    setAccessibleTitle(backButton, "Previous step");
    setAccessibleTitle(nextButton, isWelcome() ? "Start tour" : (isFinish() ? "Finish tour" : "Next step"));
    setAccessibleTitle(exitButton, isWelcome() ? "Not now" : "Exit tour");
    suppressButton.setVisible(isWelcome());
    suppressButton.setEnabled(isWelcome());

    const auto primaryBackground = CRTLookAndFeel::getVoidBlack().withAlpha(0.72f);
    const auto secondaryBackground = juce::Colours::transparentBlack;
    const auto green = CRTLookAndFeel::getTerminalGreen();
    exitButton.setColour(juce::TextButton::buttonColourId, isWelcome() ? secondaryBackground : primaryBackground);
    exitButton.setColour(juce::TextButton::buttonOnColourId,
                         isWelcome() ? secondaryBackground : green.withAlpha(0.18f));
    exitButton.setColour(juce::TextButton::textColourOffId, green.withAlpha(isWelcome() ? 0.68f : 1.0f));
    suppressButton.setColour(juce::TextButton::buttonColourId, secondaryBackground);
    suppressButton.setColour(juce::TextButton::buttonOnColourId, secondaryBackground);
    suppressButton.setColour(juce::TextButton::textColourOffId, green.withAlpha(0.68f));

    if (isWelcome())
    {
        nextButton.setExplicitFocusOrder(1);
        exitButton.setExplicitFocusOrder(2);
        suppressButton.setExplicitFocusOrder(3);
    }
    else
    {
        exitButton.setExplicitFocusOrder(1);
        backButton.setExplicitFocusOrder(2);
        nextButton.setExplicitFocusOrder(3);
    }
    resized();
}

void InteractiveWalkthrough::updateAccessibilityText()
{
    juce::String accessibleTitle(current().section);
    accessibleTitle << " // " << current().title;
    if (actionComplete)
        accessibleTitle << " — complete";
    setTitle(accessibleTitle);

    juce::String description(current().body);
    if (juce::String(current().action).isNotEmpty())
        description << " Try this: " << current().action;
    if (actionComplete)
        description << " Action complete.";
    setDescription(description);
    setHelpText(description);
    nextButton.setDescription(description);
    if (isShowing())
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
}

void InteractiveWalkthrough::moveCoachFocus(bool forwards)
{
    std::vector<juce::Button*> visible;
    const std::array<juce::Button*, 4> welcomeOrder{&nextButton, &exitButton, &suppressButton, &backButton};
    const std::array<juce::Button*, 4> lessonOrder{&exitButton, &backButton, &nextButton, &suppressButton};
    const auto& ordered = isWelcome() ? welcomeOrder : lessonOrder;
    for (auto* button : ordered)
        if (button->isVisible() && button->isEnabled())
            visible.push_back(button);

    if (visible.empty() || !isShowing())
        return;

    int currentIndex = -1;
    for (size_t i = 0; i < visible.size(); ++i)
        if (visible[i]->hasKeyboardFocus(false))
            currentIndex = static_cast<int>(i);

    const int count = static_cast<int>(visible.size());
    const int nextIndex =
        currentIndex < 0 ? (forwards ? 0 : count - 1) : (currentIndex + (forwards ? 1 : count - 1)) % count;
    visible[static_cast<size_t>(nextIndex)]->grabKeyboardFocus();
}

bool InteractiveWalkthrough::isFocusAllowed(const juce::Component* component) const
{
    if (component == nullptr)
        return true;
    if (component == this || isParentOf(component))
        return true;
    if (spotlightFocusComponent != nullptr &&
        (component == spotlightFocusComponent.getComponent() || spotlightFocusComponent->isParentOf(component)))
        return true;
    if (isWithinActiveExternalPopup(component))
        return true;
    if (getTopLevelComponent() != component->getTopLevelComponent())
        return true;

    const auto localBounds = getLocalArea(nullptr, component->getScreenBounds());
    return !interactionBounds.isEmpty() && interactionBounds.contains(localBounds.getCentre());
}

bool InteractiveWalkthrough::isWithinActiveExternalPopup(const juce::Component* component) const
{
    if (!externalPopupActive || component == nullptr)
        return false;

    const int modalCount = juce::Component::getNumCurrentlyModalComponents();
    for (int i = 0; i < modalCount; ++i)
    {
        auto* modal = juce::Component::getCurrentlyModalComponent(i);
        if (modal != nullptr && modal->getName() == "menu" && modal->getTopLevelComponent() == getTopLevelComponent() &&
            (component == modal || modal->isParentOf(component)))
            return true;
    }

    return false;
}

void InteractiveWalkthrough::globalFocusChanged(juce::Component* focusedComponent)
{
    if (!isVisible() || !isShowing() || restoringFocus || isFocusAllowed(focusedComponent))
        return;

    const juce::ScopedValueSetter<bool> guard(restoringFocus, true);
    nextButton.grabKeyboardFocus();
}

juce::Rectangle<int> InteractiveWalkthrough::chooseCardBounds() const
{
    const auto safe = getLocalBounds().reduced(kOuterMargin);
    const int width = juce::jmin(kCardWidth, safe.getWidth());
    const int height = juce::jmin(kCardHeight, safe.getHeight());

    // Keep clear of the spotlight and of any open drawer or menu drawn above
    // the tour (it would hide the card), while lining up with the spotlight.
    const auto keepClear = spotlightBounds.getUnion(coveringSurfaceBounds);
    if (keepClear.isEmpty())
        return {safe.getCentreX() - width / 2, safe.getCentreY() - height / 2, width, height};

    const auto anchor = spotlightBounds.isEmpty() ? keepClear : spotlightBounds;
    const int besideY = juce::jlimit(safe.getY(), safe.getBottom() - height, anchor.getCentreY() - height / 2);
    const int alignedX = juce::jlimit(safe.getX(), safe.getRight() - width, anchor.getCentreX() - width / 2);
    const std::array<juce::Rectangle<int>, 4> candidates{{
        {keepClear.getRight() + kOuterMargin, besideY, width, height},
        {keepClear.getX() - kOuterMargin - width, besideY, width, height},
        {alignedX, keepClear.getBottom() + kOuterMargin, width, height},
        {alignedX, keepClear.getY() - kOuterMargin - height, width, height},
    }};

    for (const auto& candidate : candidates)
        if (safe.contains(candidate) && !candidate.intersects(keepClear))
            return candidate;

    return {safe.getCentreX() - width / 2, safe.getBottom() - height, width, height};
}

void InteractiveWalkthrough::layoutCard()
{
    cardBounds = chooseCardBounds();
    if (isWelcome())
    {
        auto footer = cardBounds.reduced(18).removeFromBottom(70);
        auto primaryRow = footer.removeFromTop(34);
        nextButton.setBounds(primaryRow.withSizeKeepingCentre(180, primaryRow.getHeight()));

        footer.removeFromTop(8);
        exitButton.setBounds(footer.withSizeKeepingCentre(80, footer.getHeight()));
        suppressButton.setBounds(cardBounds.getX() + 12, cardBounds.getBottom() - 34, 132, 24);
        backButton.setBounds({});
        return;
    }

    auto buttons = cardBounds.reduced(18).removeFromBottom(34);
    exitButton.setBounds(buttons.removeFromLeft(64));
    suppressButton.setBounds({});

    auto nextArea = buttons.removeFromRight(78);
    nextButton.setBounds(nextArea);
    buttons.removeFromRight(8);
    backButton.setBounds(buttons.removeFromRight(72));
}

void InteractiveWalkthrough::resized()
{
    layoutCard();
}

bool InteractiveWalkthrough::hitTest(int x, int y)
{
    const juce::Point<int> point(x, y);
    if (cardBounds.contains(point))
        return true;
    if (!interactionBounds.isEmpty() && interactionBounds.contains(point))
        return false;
    return true;
}

bool InteractiveWalkthrough::keyPressed(const juce::KeyPress& key)
{
    // Preserve editor Undo/Redo and DAW shortcuts while a lesson is visible.
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
        return false;

    // Space is the host transport shortcut in DAWs. Leave it unhandled so
    // playback can start and stop while a lesson is visible.
    if (key == juce::KeyPress::spaceKey)
        return false;

    if (key.getKeyCode() == juce::KeyPress::tabKey)
    {
        if (attachedModalComponent != nullptr && popupCoachNavigationActive)
        {
            const bool backwards = key.getModifiers().isShiftDown();
            const bool atCoachBoundary =
                backwards ? exitButton.hasKeyboardFocus(false) : nextButton.hasKeyboardFocus(false);
            if (atCoachBoundary && modalFocusBeforeCoach != nullptr && modalFocusBeforeCoach->isShowing())
            {
                popupCoachNavigationActive = false;
                modalFocusBeforeCoach->grabKeyboardFocus();
                return true;
            }
        }
        moveCoachFocus(!key.getModifiers().isShiftDown());
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        dismiss(Dismissal::notNow);
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        advance();
        return true;
    }
    return true;
}

bool InteractiveWalkthrough::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (!isVisible())
        return false;

    if (key.getKeyCode() == juce::KeyPress::tabKey)
    {
        if (attachedModalComponent != nullptr)
        {
            if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
                if (focused == attachedModalComponent.getComponent() || attachedModalComponent->isParentOf(focused))
                    modalFocusBeforeCoach = focused;
        }
        popupCoachNavigationActive = true;
        if (attachedModalComponent != nullptr && !attachedModalComponent->canModalEventBeSentToComponent(&nextButton))
        {
            attachedModalComponent->exitModalState(0);
            attachedModalComponent->setVisible(false);
        }
        return keyPressed(key);
    }

    if (key == juce::KeyPress::escapeKey)
        return keyPressed(key);

    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey) ||
        key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey))
    {
        // Arrow navigation belongs to the highlighted menu. A following
        // Return then selects its highlighted item instead of advancing.
        popupCoachNavigationActive = false;
        return false;
    }

    if (popupCoachNavigationActive && key == juce::KeyPress::returnKey)
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (auto* button = dynamic_cast<juce::Button*>(focused))
        {
            if (button == &nextButton || button == &backButton || button == &exitButton || button == &suppressButton)
            {
                button->triggerClick();
                return true;
            }
        }

        advance();
        return true;
    }

    return false;
}

void InteractiveWalkthrough::paint(juce::Graphics& g)
{
    auto whole = getLocalBounds().toFloat();
    juce::Path dimmed;
    dimmed.setUsingNonZeroWinding(false);
    dimmed.addRectangle(whole);
    if (!spotlightBounds.isEmpty())
        dimmed.addRoundedRectangle(spotlightBounds.toFloat(), 8.0f);
    // Keep the full interface readable so the spotlight teaches location as
    // well as the active control. The input mask still blocks dimmed regions.
    g.setColour(CRTLookAndFeel::getVoidBlack().withAlpha(0.24f));
    g.fillPath(dimmed);

    if (!spotlightBounds.isEmpty())
    {
        const float attention = 0.5f + 0.5f * std::sin(juce::MathConstants<float>::twoPi * attentionPhase);
        const float pulse = juce::jmin(1.0f, 0.56f + attention * 0.22f + completionPulse * 0.22f);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.08f + attention * 0.10f));
        g.drawRoundedRectangle(spotlightBounds.toFloat().expanded(3.0f), 10.0f, 4.0f);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(pulse));
        g.drawRoundedRectangle(spotlightBounds.toFloat().reduced(0.5f), 8.0f, 2.0f);
    }

    auto card = cardBounds.toFloat();
    g.setColour(CRTLookAndFeel::getVoidBlack().withAlpha(0.98f));
    g.fillRoundedRectangle(card, 8.0f);
    CRTLookAndFeel::drawGradientOverlay(g, cardBounds);
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.78f));
    g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 1.5f);

    auto content = cardBounds.reduced(20);
    content.removeFromBottom(isWelcome() ? 70 : 48);

    if (sectionTransition > 0.0f)
        paintSectionTransition(g, content);
    else
        paintLessonContent(g, content);

    for (int y = cardBounds.getY(); y < cardBounds.getBottom(); y += 3)
    {
        g.setColour(juce::Colours::black.withAlpha(0.025f));
        g.drawHorizontalLine(y, static_cast<float>(cardBounds.getX()), static_cast<float>(cardBounds.getRight()));
    }
}

void InteractiveWalkthrough::paintLessonContent(juce::Graphics& g, juce::Rectangle<int> content) const
{
    juce::String progress;
    if (isWelcome())
        progress = "QUICK TOUR";
    else if (isFinish())
        progress = "TOUR COMPLETE";

    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.50f));
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    auto progressArea = content.removeFromTop(18);
    if (isWelcome() || isFinish())
    {
        g.drawText(progress, progressArea, juce::Justification::centredLeft);
    }
    else
    {
        auto stepArea = progressArea.removeFromRight(112);
        const auto section = "SECTION " + juce::String(current().sectionNumber) + " OF " + juce::String(kSectionCount) +
                             " // " + current().section;
        g.drawFittedText(section, progressArea, juce::Justification::centredLeft, 1, 0.88f);
        g.drawText("STEP " + juce::String(currentStep) + " OF " + juce::String(static_cast<int>(steps().size()) - 2),
                   stepArea, juce::Justification::centredRight);
    }

    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.24f));
    g.drawHorizontalLine(content.getY() + 1, static_cast<float>(content.getX()),
                         static_cast<float>(content.getRight()));

    content.removeFromTop(4);
    g.setColour(CRTLookAndFeel::getTerminalGreen());
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(17.0f));
    g.drawFittedText(current().title, content.removeFromTop(30), juce::Justification::centredLeft, 1);

    content.removeFromTop(8);
    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.76f));
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(11.5f));
    g.drawFittedText(current().body, content.removeFromTop(100), juce::Justification::topLeft, 6, 0.86f);

    if (juce::String(current().action).isNotEmpty())
    {
        content.removeFromTop(2);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(actionComplete ? 0.98f : 0.54f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
        const auto action =
            actionComplete ? juce::String("NICE // YOU GOT IT") : juce::String("TRY THIS // ") + current().action;
        g.drawFittedText(action, content.removeFromTop(34), juce::Justification::centredLeft, 2, 0.92f);
    }
}

void InteractiveWalkthrough::paintOverChildren(juce::Graphics& g)
{
    const std::array<juce::Button*, 4> buttons{&backButton, &nextButton, &exitButton, &suppressButton};

    for (auto* button : buttons)
    {
        if (button->isVisible() && button->hasKeyboardFocus(false))
            CRTLookAndFeel::drawCRTFocusRing(g, getLocalArea(button, button->getLocalBounds()).toFloat().reduced(2.0f));
    }
}

juce::String InteractiveWalkthrough::getSectionTransitionLead() const
{
    if (isWelcome())
        return "QUICK TOUR";
    if (isFinish())
        return "TOUR COMPLETE";
    return "NEW SECTION // " + juce::String(current().sectionNumber) + " OF " + juce::String(kSectionCount);
}

void InteractiveWalkthrough::paintSectionTransition(juce::Graphics& g, juce::Rectangle<int> content) const
{
    const auto green = CRTLookAndFeel::getTerminalGreen();
    g.setColour(green.withAlpha(0.42f));
    g.drawHorizontalLine(content.getY() + 1, static_cast<float>(content.getX()),
                         static_cast<float>(content.getRight()));

    auto textArea = content.reduced(8, 8);
    g.setColour(green.withAlpha(0.58f));
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    g.drawText(getSectionTransitionLead(), textArea.removeFromTop(24), juce::Justification::centred);

    textArea.removeFromTop(18);
    g.setColour(green);
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(32.0f));
    g.drawFittedText(current().section, textArea.removeFromTop(52), juce::Justification::centred, 1, 0.92f);

    g.setColour(green.withAlpha(0.72f));
    g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    g.drawFittedText(current().sectionTagline, textArea, juce::Justification::centredTop, 1, 0.92f);
}
