# Known Issues and Limitations

Behaviour that users and contributors should know about before reporting a
bug. Each item says what happens and, where one exists, the workaround.

## Platforms and distribution

- **Unsigned installers.** The macOS package is not Developer ID signed or
  notarized, so Gatekeeper requires the per-item "Open Anyway" path. The
  Windows installer is unsigned, so SmartScreen shows "More info, Run anyway".
  Signing is an optional build step for anyone with a Developer ID
  (`scripts/create_installer_mac.sh` without `--skip-sign`).
- **Windows x64 only.** There is no native Windows on ARM build. AAX is not
  built on either platform.
- **MIDI works only in the Standalone.** The AU and VST3 plug-ins declare no
  MIDI input, so hosts never send them MIDI. The Standalone receives MIDI from
  the inputs enabled in its audio settings, so the spatial CC mapping (CC 21,
  22, and 23 for azimuth, elevation, and distance) and the Note retrigger
  choice work only there. In a host, use the Free, Transport, or Random
  retrigger modes.

## Memory and CPU

- **Reverb memory per instance.** Both reverb engines are prepared when an
  instance is prepared, even when only Spring is selected. Cloud's delay
  network is allocated for the prepared sample rate (`CloudReprepareMemory`
  checks that a re-prepare never retains excess capacity), so each instance
  carries the Cloud footprint and it grows with sample rate. Workaround: share
  one instance on a send/return, start sessions at 44.1 or 48 kHz, freeze or
  bounce finished tracks.
- **Oversized callbacks clear Cloud output.** If a host delivers a callback
  larger than the prepared maximum block size, Cloud clears its output for
  that block rather than allocating on the audio thread.
- **Independent spatial voices cost more.** Per-Head placement runs three
  binaural voices instead of one. Higher sample rates, many instances, and
  moving Independent voices need more headroom; a larger audio buffer alone
  does not guarantee a heavy chain runs without dropouts.

## Latency and phase

- **Output Protection phase.** The plug-in reports a fixed four-sample latency
  at every supported sample rate, with and without protection and during host
  bypass, and the dry and bypass paths are sample-aligned. Protection itself
  uses a minimum-phase oversampling filter, so its phase response is
  frequency-dependent: a protected parallel path will not null against dry
  audio even with host delay compensation. Workaround: switch Output Protection
  off (wordmark menu) when a parallel blend must null, or check blends by ear.

## Delay and sync

- **Tempo-sync delay-time changes carry a transition signature.** A synced
  delay change larger than 10 ms uses a 15 ms dual-read crossfade; smaller
  changes glide. Measured on a sine, the crossfade produces +21.9 dB band
  excess at the 10 ms threshold (a 2 ms threshold measured +11.0 dB), so a
  threshold change alone cannot make it inaudible. It is only heard on abrupt
  division changes while repeats are sounding.
- **Tape noise at high Feedback.** Loop gain and noise intensity are separate
  signals (the public Feedback value feeds the noise model in its 0..1 domain),
  but the noise amount near self-oscillation has not been tuned with
  level-matched comparisons. Use the Noise toggle in the Tape Flavor menu if
  the hiss is unwanted.

## Spatial

- **Orbit ignores Depth for azimuth.** Any nonzero Depth makes Orbit sweep a
  full 360 degrees of azimuth; Depth scales only elevation and distance. Zero
  Depth returns no motion.
- **Host automation of several position parameters is not atomic.** Position
  edits made by the plug-in itself (presets, all-head edits, numeric entry,
  undo) publish one complete head formation per audio read. Parameters
  automated independently by the host are sampled independently, so the audio
  thread can observe a mixed formation within one block when several
  coordinates change at once.
- **Engine and routing selectors are not automatable.** Reverb Engine, Reverb
  Routing, and Spatial Routing can be changed from the UI or a preset during a
  live tail, but they are not exposed as automatable host parameters.

## Presets and state

- **The modified asterisk compares the factory schema only.** The header
  asterisk and MODIFIED tag compare the 64 values the factory schema stores.
  They do not compare Debug-only values or the four Release parameters outside
  the schema (`AutoPanic`, `TapeNoiseEnabled`, `SpatialLongDivision`,
  `SpatialLongVariantDivision`).
- **One state schema.** Host state and user presets accept schema `1` and
  unversioned state with the official root type; explicit unknown, future,
  empty, or invalid versions are rejected before any live state changes. There
  is no general migration dispatcher for later schemas, and factory arrays are
  positional (parallel ID and value arrays), so reordering them is a
  compatibility break. Root types written by pre-release builds are not
  supported.
- **Unknown factory preset names reset the browse identity.** A session that
  names a factory preset absent from the current bank restores its audio
  values, but its browse identity resets to Init. No name migration is
  performed.
- **Mix Lock holds the effective Mix only.** While locked, the stored Mix value
  can still change underneath and becomes audible on release. Mix Lock does not
  exclude any other parameter from preset recall.
- **Spring and Cloud differ in tail level.** The calibration keeps the two
  engines within 1.44 dB over the early window at matched macros, but the
  compact Spring tail can sit as much as 14.87 dB below Cloud late in the
  report window. Decay and Size still change total tail energy by design.

## Accessibility

- **Editor keyboard focus inside hosts.** The wrapper disables editor keyboard
  focus, so host focus routing and screen-reader task completion have not been
  accepted in the supported DAWs. Tab does not necessarily reach every surface.
  The preset browser, motion cards, and the interactive tour expose native
  accessible controls and work with arrows, Return, Space, and Escape in the
  Standalone. The Echo Engine and Reverb popups also need host confirmation.
- **`ReleaseAccessibility` can time out on slow machines.** Its menu-callback
  wait can expire under Rosetta, so the test is not part of the
  release-contract set that runs in the release workflow.

## Contributor notes

- **Realtime audit is lexical.** `scripts/audit_rt_safety.sh` scans the hot
  paths, including `plugin/Source/DSP/dependencies/`, for allocation, locks,
  I/O, and logging. It catches obvious hazards and
  is not proof of every call path; targeted tests still own the realtime
  contract.
- **Diffuse-field EQ and source width are measured, not perceptually
  validated.** The diffuse-field correction flattens the bundled dataset's
  direction-independent response to within +/-0.45 dB from 100 Hz to 12 kHz,
  and source width decorrelates the focused HRIR per ear. Localization claims
  are stated as objective cue metrics (ITD, ILD, spectral deltas) against the
  dataset, not as listening-test results.
