# Preset and State System

This document covers stable preset and state behaviour and the compatibility
rules behind it. The current factory catalog is `tools/presets/roster-values.json`
and the header generated from it, `plugin/Source/Presets/FactoryPresetLibrary.h`;
parameter IDs, ranges and choice labels are declared in the processor sources.

## Preset layers

Spatial Echo has four preset/state layers:

1. **Factory presets**: complete curated sounds compiled into
   `FactoryPresetLibrary.h`.
2. **User presets**: APVTS state saved to `.spatialEchopreset` XML wrappers in
   the on-disk user library.
3. **Module catalogs**: Spring, Cloud, and spatial preset definitions that
   write a smaller parameter subset.
4. **Host state**: the complete APVTS ValueTree plus custom root properties
   serialized by `getStateInformation()`.

These layers are not interchangeable. A module preset does not establish a
complete sound. A factory definition does not explicitly carry every Release
parameter, but loading it first resets every known Release parameter to its
Init default, so values absent from the factory array still receive a defined
whole-plug-in state.

The spatial module catalog contains 28 spatial presets displayed as two
uncategorized columns of 14. Their names describe the stored width, routing,
motion pattern, timing, or depth behaviour. These presets do not set the base
azimuth, elevation, distance, head position mode, or 3D enable state.

## Factory browser groups

`FactoryPresetBrowseGroups.h` assigns every factory name to one of four UI
groups by primary audible purpose: Echo & Rhythm, Reverb & Atmosphere,
Spatial & Motion, or Experimental. This catalog is separate from authoring
categories, Mix floors, parameter values, and serialized state. The authoring
category labels remain searchable, including genre, use, and inspiration
metadata from `FactoryPresetDescriptions.h`.

Each factory preset belongs to one group. The 12 focused spatial demonstration
presets sit under Spatial & Motion, without a separate overlapping shortcut.
Choosing a group filters the list without loading or modifying a preset. All
Presets includes factory and user entries. User is always present, with an
empty-state message before the first save. Init is a separate action rather
than a row. Descriptions wrap in each row, while loaded identity and dirty state
remain visible in the description strip even when the active group or query
excludes that row. `FactoryPresetDescriptionCoverage` verifies assignment
uniqueness, complete roster coverage, valid groups, and the focused spatial
presets' group and search metadata.

## Factory schema and ordering

Every factory definition contains a fixed-order float array whose positions are
interpreted through `FactoryPresetLibrary::kParameterIds`. Factory loading:

1. resets every known Release parameter to its Init default through
   host-notifying writes while leaving non-parameter root properties in place;
2. applies the fixed array by matching each position to its ID;
3. applies selector parameters after dependent values;
4. enforces the category's minimum Mix for immediate audition;
5. restores the detached Debug test-tone values, which are outside the Release
   parameter set.

The parallel ID and value arrays make ordering a compatibility boundary. A
reordered ID or value can silently assign valid numbers to the wrong parameter.
The generator verifies the current declaration, but runtime factory data is not
self-describing by ID.

The factory arrays are generated. `tools/presets/roster-values.json` is the
source of truth and `python3 tools/presets/gen_factory.py` emits
`FactoryPresetLibrary.h`, refusing to write when domain validation
(`validate_values.py`), category floors, or name uniqueness fail;
`gen_factory.py --check` verifies the committed header without writing, and
`check_story.py` confirms that literal claims in preset names (such as `Orbit`
or `Whole-Note`) are true immediately after load. Preset values are never
hand-edited in the header.

The bank currently holds 62 presets in ten categories. The APVTS exposes 68
Release parameters; the factory schema stores 64 in runtime order and
intentionally omits `AutoPanic`, `TapeNoiseEnabled`, `SpatialLongDivision`,
and `SpatialLongVariantDivision`. All four reset to their Init defaults before
the row is applied.

A session that names a factory preset absent from the current bank restores
its audio values, but its browse identity resets to Init. No name migration is
performed.

When changing the factory schema, update the ID array, the roster, generator
fixtures, preset tests, and migration policy in one change. Never insert a
value without regenerating and rendering the full bank.

## Auto-load of the first factory preset

Wrapper-hosted instances (AU, VST3, Standalone) construct onto the first
factory preset instead of the Init state: the constructor loads the preset,
stamps the persisted preset identity (`activePresetName`,
`activePresetType` = Factory), and excludes the startup load from undo history
by flushing the APVTS and clearing the UndoManager. Direct construction
(`wrapperType_Undefined`: headless tests and offline render tools) keeps Init
defaults so measurement baselines stay Init-anchored. Host session restore
always wins because `setStateInformation` replaces the whole state after
construction. The `DefaultPresetAutoLoad` headless test covers all three
behaviours.

## User preset storage

User presets use the `.spatialEchopreset` extension and a `SpatialEchoPreset`
XML wrapper containing the APVTS state tree. They are stored under the user's
application-data `Spatial Echo/Presets` directory. The on-disk library is the
only user-preset store; host state carries no session-local library copy.

Save, export, and host serialization build a detached snapshot. Pending
audio-published parameter values and selector side effects are applied only
to that snapshot, using the parameter's normal value snapping. Saving neither
consumes queued host updates nor changes live DSP caches or sends host
notifications. A non-audio lock serializes snapshots with deferred UI flushes;
the audio callback does not acquire it. This is not a guarantee of one atomic
generation across independently automated parameters.

Nested `Presets` children are stripped and `stateSchemaVersion=1` is written
to the root. File writes use a same-directory temporary file and require a
successful write and flush before replacing the destination. A failed Save or
Save As keeps the editor's current identity and dirty state. Rename removes the
original only after the new file is successfully written.

Loading a user preset resets known parameters to defaults and applies the saved
known IDs through host-notifying writes. This gives missing known parameters
their current default and preserves undo history.

Import accepts the official wrapper with the expected state child; bare-state
files are rejected. It commits the validated state to the on-disk library
before replacing live state, so a failed write preserves the current sound.
The current scope visibility is preserved when applying the imported state.

## Custom APVTS properties

Not every persisted value is a host parameter. Current root properties include
UI/preset identity state, scope visibility, and tape schema metadata. They
travel with host state.

No HRTF selection is stored: the binaural renderer always uses the bundled
HUTUBS dataset, so presets and host state need no external files.

## Mix Lock

Mix Lock is an APVTS state parameter. When it becomes engaged, the DSP captures
the current effective Mix and continues using that held value while the lock
stays on. The underlying APVTS Mix value can still be changed or recalled, and
both Mix and Mix Lock are serialized.

This matters for preset audition and send/return use: check that the lock is
still engaged after the operation, and inspect the value that becomes active
when it is released. Mix Lock is not a general "exclude this parameter from
presets" mechanism.

## Host state and schema boundary

Host state is the APVTS ValueTree serialized as XML in JUCE binary state. The
save path stamps the first official schema, root `stateSchemaVersion=1`.
Host restore and user-preset load/import accept schema `1` and unversioned
state using the current official root type. Explicit unknown, future, empty,
or invalid versions are rejected before replacing live state or saving an
import (`StateSchemaBoundary` test). Pre-release root types are intentionally
unsupported. There is no general migration dispatcher for future schemas. The
base parameter set uses ParameterID version `1`; the additive long-division
extensions use versions `2` and `3`, and `TapeNoiseEnabled` uses the
append-only version `4`, with targeted missing-value materialization to each
parameter's Init default.

Remaining compatibility work:

- ordered migrations between shipped versions;
- explicit handling for renamed, removed, unknown, and range-changed IDs;
- compatibility fixtures for older AU/VST3/Standalone sessions;
- factory data keyed by stable ID rather than parallel position.

Until that exists, parameter, routing, preset-array, and custom-property
changes must not be called backward compatible without fixtures.

Code-owned spatial edits publish one complete position formation per audio
read. Preset/state application, all-head edits, numeric head edits, and
Undo/Redo are bracketed by a non-audio writer scope. A callback overlapping a
batch keeps the previous complete formation; it never locks or retries.
Individually host-automated parameters remain independent. This is not a
transaction across all APVTS parameters.

## Verification checklist

For any preset/state change, verify:

- factory header consistency (`tools/presets/gen_factory.py --check`);
- every factory preset and category Mix floor;
- selector side effects after stopped and running transport;
- user save/load/rename/delete/import/export;
- missing and unknown parameter IDs;
- host project recall in AU, VST3, and Standalone;
- Mix Lock held/effective value behaviour;
- Debug values remaining detached from Release/user presets;
- undo after factory and user preset loads;
- fixtures from every schema that has shipped.
