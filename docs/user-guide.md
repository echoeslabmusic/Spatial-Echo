# Spatial Echo User Guide

Spatial Echo combines a three-head tape delay, Spring or Cloud reverb, and
spatial positioning. The interface is built around auditioning a complete
sound first, then adjusting Tape, Delay, Reverb, Spatial, Mix, and Output.

## Get to the first sound

1. Insert Spatial Echo on an audio track. Set the header **OUT** field near
   `0.0 dB`.
2. Click the preset name in the header to open the full preset browser.
3. Audition presets with a repeating phrase. Loading a factory preset first
   returns every control to its Init setting, then applies the preset's
   settings and its category's minimum Mix, so nothing from the previous sound
   carries over.
4. Set Mix for the track context, then adjust Tape Flavor and Drive before
   changing detailed delay, reverb, or spatial behaviour.
5. An asterisk on the preset name means at least one setting the preset
   stores has changed since it loaded. Settings that presets do not store,
   such as Auto-Panic and the tape Noise switch, are not compared. Save a user
   preset before switching if you want to keep the musical state.

The preset browser inside the plug-in lists the current factory names and
categories.

## First-run interactive tour

The first eligible editor in a host process opens a Welcome card. Choose
**Start Tour** to follow the interface from presets through tape, echo,
reverb, spatial placement and motion, Mix/Out, and the utility menu. A
highlighted control stays live, so you can audition or adjust it without
leaving the tour. Pressing **Next** automatically opens the browser, drawer, or
menu needed for the next explanation, so the full tour works with Back and Next
alone. The suggested action is optional: completing it produces a short
confirmation, and **Next** remains under your control.

The tour is divided into eight named sections: **Presets**, **Tape**, **Echo**,
**Reverb**, **Spatial**, **Motion**, **Output**, and **Utilities**. Each card
keeps the current section visible beside its step number. When an arrow enters
a new section, the coach card briefly announces **New Section**, the section
name, and its purpose before revealing the first lesson automatically.

The preset section first introduces the preset name and its audition arrows
while the browser is closed. Pressing **Next** opens the browser automatically.
Six short cards then move the highlight through the category column, preset
list, a description under a preset name, filter, library actions, and User
presets. The four browse groups are **Echo & Rhythm**, **Reverb & Atmosphere**,
**Spatial & Motion**, and **Experimental**. Choose **All Presets** to browse the
whole library. Each factory preset belongs to one group; **Spatial & Motion**
includes the focused demonstrations of movement and placement around your head.
Clicking a group filters the list without loading a sound; search narrows that
group further. Search by name, current or original category, description,
instrument, sound, or inspiration: try **guitar**, **drums**, or **forest**.
The final card selects User so you can see where saved presets are renamed or
deleted. Before your first saved preset exists, it highlights **Save As**
instead.

The Tape section uses three cards: **Choose a Tape Flavor** explains the five
repeat characters, **Turn the Tape Noise Off** points directly to the Noise row
and its checkmark, and **Set the Tape Drive** closes the menu and highlights
Drive by itself. You can try each control or continue using only Back and Next.

The Echo section follows the way you build a delay: choose which heads play,
set free time or Sync and head divisions, shape Feedback, choose Ping-Pong,
set Echo Level, and try the DUB button. It then opens **Echo Engine** for Head
Ratio and the heat/Power status light, and opens the Spatial Echo menu to arm
Auto-Panic. The Reverb section starts with Spring or Cloud and a starting
space, moves through Size, Decay, Tone, and Reverb Level, then opens the
detailed controls for Pre-Delay, Width, Character, Mod, Ducking, PIN, Reset,
and routing.

The Spatial section first enables headphone placement, then explains spatial
presets, Linked versus PER-HEAD editing, head selection, drag/scroll gestures,
and exact position entry with locks. **BLEND** opens automatically for Wet,
Xover, Width, Chain, and Quality. Motion then covers each path, free and synced
speed, Division, Depth, Choreo, Spread, and Retrig. The surrounding interface
stays readable under a light veil; the animated outline shows the exact control
for the current lesson.

Use **Back** and **Next** to move through the tour, **Exit** or **Not Now** to
dismiss it for the current host process, or **Don't Show Again** to suppress
future automatic launches. Finishing the tour also saves that preference.
Open the Spatial Echo wordmark menu and choose **Interactive Tour...** to replay
it at any time; manual replay always starts at Welcome even when automatic
launches are suppressed.

## Window size

Drag the window corner to make the interface larger. On a small screen, or a
Windows laptop with 150% display scaling, open the Spatial Echo wordmark menu
and choose **Window Size, 75%**. The whole interface scales down evenly, and
the choice is remembered for every new instance on that computer.

## Tape Flavor and Drive

Tape Flavor chooses the complete tape character:

- **CLEAN**: pristine, transparent tape path.
- **WARM**: the default studio character with balanced saturation.
- **HOT**: a driven warm-machine formulation for dub and feedback sounds.
- **CASSETTE**: consumer cassette with audible hiss and wobble.
- **WORN**: degraded tape with dropouts and heavy character.

**Drive** changes the selected flavor's resolved drive, saturation, and bias
values; it does not increase the plug-in's input gain. The tuned anchor is
`0.45`, slightly below the centre position. Moving upward applies that
flavor's slopes toward earlier breakup. The flavor bundle separately sets
noise, condition/wear, wow/flutter, scrape, and feedback darkening values that
are intentionally not separate controls.

Open the Tape Flavor selector to switch **Noise** off when you want a hard tape
character without its generated hiss, asperity, loop noise, or reverb-noise
injection. It is on by default and does not change Drive, saturation,
wow/flutter, wear, or the selected flavor.

Compare Drive settings at matched Output level. Louder does not mean a setting
sounds better.

## Delay

### Mode, rate, and feedback

**MODE**, listed as **Echo Mode** by the host, chooses which tape heads are
heard and whether the selected head signal also feeds reverb.

**Rate** changes tape speed and free-running delay time. Slower tape also
changes the modeled transport character. **Feedback** controls repeat
regeneration and can reach self-oscillation; Output Protection is a last safety
layer, not a substitute for monitoring level.

### Sync and Ping-Pong

Both toggles sit on the delay surface below **MODE**.

**SYNC** moves delay timing from free rate to host-tempo divisions. Click the
H1, H2, or H3 value to choose that head's note division. The free Rate control
is not the active timing authority while Sync is enabled.

**PING-PONG** alternates repeats between left and right. Check the result in
stereo and mono, especially with multiple heads active.

### Echo controls

Open **ECHO ENGINE** for head-ratio selection. The **DUB** button sits under
the Echo Level knob, and **Auto-Panic** is an item in the Spatial Echo wordmark
menu. Auto-Panic defaults off.

- **Head Ratio** selects the spacing relationship between H1, H2, and H3.
- **DUB** (small, dim green) is normal Echo operation: new input feeds the tape.
- **DUB: INPUT OFF** (amber, pulsing) is Dub mode: new input is muted while
  existing repeats continue and fade. Click it again to feed input. A session
  or automation that recalls Dub shows the same amber state.
- The host value text is **Normal** when DUB is green and **Dub** when it reads
  INPUT OFF.
- **Auto-Panic** watches for broken or extreme sustained runaway state. Normal
  musical self-oscillation should not trigger it.

There is no manual Panic control. The **Power** host parameter carries the
power state, and the ECHO ENGINE status light reports
a latched power-off or watchdog event. If host automation turns Power off,
restore it through the host parameter surface.

## Reverb

Choose **Spring** for the modeled spring tank or **Cloud** for a dense stereo
algorithmic reverb. Shared controls alter both engines through different
engine-specific laws:

- **Size**, **Decay**, **Tone**, **MOD**, and **Character** shape the engine;
  the host lists MOD as **Reverb Modulation**.
- **Pre-Delay** separates the onset from the source.
- **Width** changes the wet stereo image.
- **Ducking** lowers the audible reverb branch under dry input.
- **Reverb Level** trims the reverb branch.

Routing choices are:

- **Parallel**: delay and reverb receive parallel sends.
- **Echo to Reverb**: echoes feed the reverb.
- **Reverb to Echo**: reverb feeds the delay path.

Switching engines or routing during a live tail is supported from the UI, but
neither selector is automatable from the host. Engine changes use a 50 ms
equal-power transition and same-engine preset changes retain the tank.
Audition changes at your session's real buffer size.

## Spatial positioning and motion

Enable **Spatial Processing**, then use the head view to set azimuth, elevation,
and distance.

- **Linked** gives the three echo heads one shared position.
- **PER-HEAD** in the plug-in, called **Independent** by the host parameter,
  exposes H1, H2, H3, and All selection so each head can have a separate
  position.
- The position controls support dragging plus exact-value entry where exposed.
- **After Effects** spatializes the combined effect output.
- **Before Effects** spatializes before the delay/reverb graph so later tails
  inherit that coloration.

### Motion pattern cards

Motion is pattern-first. **STILL** is off; **ORBIT**, **PENDULUM**,
**FIGURE-8**, **BREATHE**, and **WANDER** move the selected position. Set free
**RATE** or enable motion **SYNC** and choose a division. The Straight column
extends through `2/1`, `4/1`, `8/1`, and `16/1`; the Triplet and Dotted columns
provide the matching `T` and `D` values. These twelve long choices affect
spatial motion only, not the delay heads. The selected division remains visible
when the editor is closed and reopened. Fast synced motion keeps its 16 Hz
safety ceiling. **DEPTH** scales every pattern's excursion except Orbit
azimuth: any nonzero Orbit depth still makes a full 360-degree azimuth cycle,
while elevation and distance scale with Depth. Zero Depth returns no motion
offsets.

Open **CHOREO** for retrigger behaviour. Choreography and Spread affect
per-head motion in Independent mode; they do not alter Linked motion. Retrigger
choices are Free, Transport, Note, and Random. Transport follows host position
while the transport is playing. Note restarts the motion on each incoming MIDI
note. Only the Standalone receives MIDI (enable a MIDI input in its audio
settings); plug-in hosts send Spatial Echo no MIDI, so Note is unavailable in
the AU and VST3 versions. Random applies a deterministic random phase offset
for each motion cycle.

In the Standalone, MIDI CC 21, 22, and 23 also set the linked source's
azimuth, elevation, and distance. See [known issues](known-issues.md).

Hosts list ten motion parameters: **Spatial Pattern**, **Spatial Rate**,
**Spatial Sync**, **Spatial Division**, **Spatial Long Division**, **Spatial
Long Variant Division**, **Spatial Depth**, **Spatial Restart**, **Spatial
Choreography**, and **Spatial Spread**. The plug-in combines the three division
parameters behind one division picker; its compact labels remain PATTERN, RATE,
SYNC, DIV, DEPTH, RETRIG, CHOREO, and SPREAD.

## Mix, Output, and send/return use

**Mix** crossfades preserved dry input with the combined wet branches.
**Echo Level** and **Reverb Level** shape the wet branch before Mix. There is
no separate branch-balance control: the tilt is pinned by the delay Mode at the
equal-power midpoint in dual mode, full echo in delay-only mode, and full
reverb in reverb-only mode. **OUT** in the header is the final Output level
control.

On an insert, set Mix by context. On an auxiliary send/return, start at fully
wet, set Echo Level, Reverb Level, and Output on the return, and leave the dry
source on its original channel. **Mix Lock** holds the effective Mix captured
when the lock engages; the stored Mix value can still change underneath it and
becomes audible when the lock is released. It does not freeze the rest of the
preset state. See the [preset system reference](preset-system.md).

Output Protection is enabled by default and is toggled from the wordmark menu,
alongside Auto-Panic. The plug-in reports a fixed four-sample latency at every
supported sample rate, including when protection is off and during host
bypass. Protection uses a minimum-phase oversampling filter, so its phase
response is frequency-dependent; host delay compensation does not make a
protected parallel path null against dry audio. Check the sound of parallel
blends.

## CPU and memory

Start with 44.1 or 48 kHz when choosing a new session's sample rate. Higher
rates, many instances, and moving Independent spatial voices need more
processing headroom. Both reverb engines reserve memory when an instance is
prepared, including when only Spring is selected, and the Cloud footprint grows
with sample rate.

Monitor the DAW's audio-thread load and memory use as instances are added.
Increase the audio buffer when practical, share a send/return where the routing
allows it, or freeze/bounce completed tracks. A larger buffer alone does not
guarantee that a heavy instance chain will run without dropouts. See
[known issues](known-issues.md).

## User presets and portability

The strip above the browser's buttons describes the preset under your mouse,
or your loaded preset when you are not hovering, so the loaded name stays
visible even when search or a group hides its row. Click a result or use
Up/Down to audition it while the live spatial head stays visible. Use the close
button, Escape, or a click outside the panel to close it; Escape clears a
non-empty search first. **Init**, beside Save As, restores the initial sound.

Open the preset browser actions to:

- **Save As** a new user preset, or use **Library, Save** to update a loaded
  user preset;
- use **Library, Import** or **Export** for a `.spatialEchopreset` file;
- rename, delete, or reveal the user-preset folder.

User presets store every plug-in setting plus the preset's name. The spatial
renderer always uses the bundled HUTUBS head-related transfer functions, so a
preset needs no extra files when you move it to another computer.

Presets saved by this or an earlier released version load normally. A preset
file from a newer, incompatible version, or a damaged file, is rejected before
it changes the sound or is added to your library. Presets saved by pre-release
test builds are not supported. Keep exported copies of important presets
before trying a test build.

## Keyboard and accessibility

The preset browser exposes a native accessible list, filter, checked browse
controls, scrollbar, close button, and preset-management actions. Up/Down
audition visible results; Left/Right change the browse filter. Return loads
the focused row. Escape clears a non-empty filter, then closes the browser.
Motion cards expose selected state and support arrows and Return when focused.
Space always goes to the host, so it starts and stops playback whichever
control has focus. Command/Control shortcuts can continue to the editor or
host.

The plug-in editor does not take keyboard focus, so host focus routing and
screen-reader task completion have not been accepted in the supported DAWs. The
Echo/Reverb popups also need host confirmation. See
[known issues](known-issues.md).

The interactive tour provides named, focusable card controls, a visible focus
ring, Return activation for the focused coach or highlighted control, and
Escape-to-exit. Those improvements do not resolve the wider plug-in
accessibility gaps above.

For the remaining host and accessibility limits:

- verify every automation or preset change visually and audibly;
- do not assume Tab reaches every surface;
- confirm DUB from the chip under Echo Level (amber **DUB: INPUT OFF** means
  Dub is on) or the host parameter, and confirm Power/watchdog state from the
  ECHO ENGINE status light plus audio behaviour;
- when reporting a problem, include host, format, screen-reader state, focus
  path, and the control that became unreachable.

## Troubleshooting and About

- **No echo:** confirm Mix, Echo Level, and an active delay Mode, then check
  that the DUB chip under Echo Level is green rather than amber
  **DUB: INPUT OFF**. If the ECHO ENGINE status light reports power-off,
  restore the host **Power** parameter; there is no manual Panic control.
- **No synced repeats:** enable host transport/tempo, enable Sync, and inspect
  each active head division.
- **No reverb:** check Reverb Level, routing, engine, and the selected delay
  Mode's send behaviour.
- **Unexpected spatial sound:** compare Linked versus Independent, After versus
  Before Effects, Spatial Mix, Spatial Crossover, and mono fold-down.
- **Preset sounds different on another computer:** check that both computers
  run the same Spatial Echo version (see About), the same sample rate, and the
  same plug-in format.
- **Host loads the wrong build (macOS):** run
  `./scripts/verify_macos_plugin_installs.sh --format all` (it derives each
  bundle's executable from its own `Info.plist`) and force a host rescan.

Open **About** from the wordmark menu to check the product version. The dialog
does not show a build date.
