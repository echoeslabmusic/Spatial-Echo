// FactoryPresetBrowseGroups: UI-only grouping by primary audible purpose. Authored factory categories, values, and
//   search metadata remain in FactoryPresetLibrary and FactoryPresetDescriptions.
// See docs/preset-system.md.

#pragma once

#include "FactoryPresetLibrary.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace FactoryPresetBrowseGroups
{
enum class Group
{
    echoAndRhythm,
    reverbAndAtmosphere,
    spatialAndMotion,
    experimental
};

inline constexpr std::array<const char*, 4> kGroupLabels{{
    "Echo & Rhythm",
    "Reverb & Atmosphere",
    "Spatial & Motion",
    "Experimental",
}};

struct Entry
{
    const char* name;
    Group group;
};

// These are deliberate preset-by-preset assignments, not a mapping of the
// ten authoring categories. Listening experiments about rear/height cues stay
// with the other focused spatial presets and retain their description caveats.
inline constexpr std::array<Entry, FactoryPresetLibrary::kNumFactoryPresets> kAssignments{{
    {"Halo Orbit", Group::spatialAndMotion},
    {"SubSafe Orbit", Group::spatialAndMotion},
    {"Triplet Air Anchor", Group::spatialAndMotion},
    {"Vocal Halo Doubler", Group::echoAndRhythm},
    {"Slap Tighten", Group::echoAndRhythm},
    {"Dotted 8 Grid", Group::echoAndRhythm},
    {"Clean PingPong Throw", Group::echoAndRhythm},
    {"Dotted 8 Lead Throw", Group::echoAndRhythm},
    {"Wide Filter Throw", Group::echoAndRhythm},
    {"Dust Carousel", Group::experimental},
    {"Wander Morph", Group::spatialAndMotion},
    {"PingPong Tunnel", Group::spatialAndMotion},
    {"Tape Drift Atmos", Group::reverbAndAtmosphere},
    {"Dark Spring Room", Group::reverbAndAtmosphere},
    {"Whole-Note Ambient", Group::reverbAndAtmosphere},
    {"Dub Spiral Feedback", Group::echoAndRhythm},
    {"Echo Into Spring", Group::echoAndRhythm},
    {"Infinite Pad Ride", Group::reverbAndAtmosphere},
    {"Golden Gallop Drive", Group::echoAndRhythm},
    {"Bright Glitch Echo", Group::experimental},
    {"Broken Loop Dropout", Group::experimental},
    {"Helix Surge", Group::spatialAndMotion},
    {"Hard Lock Drop Hits", Group::echoAndRhythm},
    {"Rush 16 Fill", Group::echoAndRhythm},
    {"Externalizer Wash", Group::spatialAndMotion},
    {"Crossover Clarity", Group::spatialAndMotion},
    {"Elevation Cue", Group::spatialAndMotion},
    {"Envelopment Hall", Group::reverbAndAtmosphere},
    {"Motion Tracker Clean", Group::spatialAndMotion},
    {"Ping-Pong Ricochet", Group::echoAndRhythm},
    {"Cloud Packets", Group::reverbAndAtmosphere},
    {"Forest Afterglow", Group::reverbAndAtmosphere},
    {"Crosscurrent", Group::echoAndRhythm},
    {"Offbeat Weave", Group::echoAndRhythm},
    {"Velvet Answer", Group::echoAndRhythm},
    {"Pond Round", Group::spatialAndMotion},
    {"Distant Lantern", Group::spatialAndMotion},
    {"Spring Aftershock", Group::reverbAndAtmosphere},
    {"Bass Reply", Group::echoAndRhythm},
    {"Hat Ghost Triplets", Group::echoAndRhythm},
    {"Quarter Adlib", Group::echoAndRhythm},
    {"Halfstep Shadow", Group::echoAndRhythm},
    {"Riddim Pocket Room", Group::reverbAndAtmosphere},
    {"Lead Gap Cloud", Group::reverbAndAtmosphere},
    {"Canon Windows", Group::echoAndRhythm},
    {"Bell Counterphrase", Group::echoAndRhythm},
    {"Concrete Chatter", Group::experimental},
    {"Hidden Rhythm", Group::experimental},
    {"Static Sweep", Group::experimental},
    {"Spatial Pull", Group::experimental},
    {"Point Orbit", Group::spatialAndMotion},
    {"Beat Orbit", Group::spatialAndMotion},
    {"Front Swing", Group::spatialAndMotion},
    {"Near Ear Pass", Group::spatialAndMotion},
    {"Side Relay", Group::spatialAndMotion},
    {"Orbit Replies", Group::spatialAndMotion},
    {"Approach Retreat", Group::spatialAndMotion},
    {"Near Far Relay", Group::spatialAndMotion},
    {"Rear Window", Group::spatialAndMotion},
    {"Front Back Relay", Group::spatialAndMotion},
    {"Height Relay", Group::spatialAndMotion},
    {"Sky Figure Eight", Group::spatialAndMotion},
}};

// Too many entries fail to compile; too few would leave null names.
inline constexpr bool everyAssignmentNamed()
{
    for (const auto& entry : kAssignments)
        if (entry.name == nullptr)
            return false;

    return true;
}
static_assert(everyAssignmentNamed(), "Every factory preset needs a browse group.");

inline constexpr const Entry* find(const char* presetName)
{
    if (presetName == nullptr)
        return nullptr;

    for (const auto& entry : kAssignments)
        if (std::string_view(entry.name) == presetName)
            return &entry;

    return nullptr;
}

// A missing name is not silently placed into a group; coverage tests bind this
// UI catalog to the generated factory roster.
inline constexpr int groupIndexFor(const char* presetName)
{
    if (const auto* entry = find(presetName))
        return static_cast<int>(entry->group);

    return -1;
}

}  // namespace FactoryPresetBrowseGroups
