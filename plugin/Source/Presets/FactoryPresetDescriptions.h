// FactoryPresetDescriptions: Hand-written one-line intent per factory preset, shown in each preset browser row. Copy
//   follows the preset roster.
// See docs/preset-system.md. Keyed by the SHIPPED preset names in FactoryPresetLibrary.h (the generated roster is the
//   naming source of truth; this file is hand-maintained copy). Keep every description <= kMaxDescriptionLength for
//   concise browsing. Rows measure and wrap the complete copy without truncation.

#pragma once

#include <cstddef>
#include <cstring>

namespace FactoryPresetDescriptions
{
inline constexpr size_t kMaxDescriptionLength = 56;

struct Entry
{
    const char* name;
    const char* description;
    const char* searchTerms;  // musical uses, sound qualities, and inspirations
};

inline constexpr const char* kInitDescription = "all parameters at their defaults";

inline constexpr Entry kDescriptions[] = {
    // Spatial Anchors
    {"Halo Orbit", "three echoes circle at distinct heights around you", "synth keys guitar moving spacious surround"},
    {"SubSafe Orbit", "orbiting echoes leave the low end dry and centered", "bass drums synth low end centered moving"},
    {"Triplet Air Anchor", "airy triplets hold a high, wide rhythmic anchor",
     "guitar keys drums percussion rhythm triplets elevated"},
    // Mix Utilities
    {"Vocal Halo Doubler", "short offset echoes wrap vocals in a soft stereo halo",
     "voice vocals guitar doubling short wide"},
    {"Slap Tighten", "one tight slap adds presence without clouding the source",
     "voice vocals guitar drums slapback tight short"},
    {"Dotted 8 Grid", "clean dotted-eighth repeats lock into a steady grid",
     "guitar keys synth rhythm dotted eighth clean"},
    // Throws
    {"Clean PingPong Throw", "clear dotted repeats alternate hard left and right",
     "voice vocals lead guitar phrase stereo clean"},
    {"Dotted 8 Lead Throw", "dotted-eighth echoes lift lead phrases above the mix",
     "voice vocals lead guitar synth phrase rhythm"},
    {"Wide Filter Throw", "wide filtered repeats recede into a dark reverb tail",
     "voice vocals lead transition dark diffuse distant"},
    // Orbits
    {"Dust Carousel", "worn tape repeats circle in a dusty, uneven carousel",
     "keys synth guitar texture lo fi lofi tape worn"},
    {"Wander Morph", "each repeat wanders across a slow, irregular field", "keys synth pad ambient irregular moving"},
    {"PingPong Tunnel", "alternating echoes travel away through a narrow tunnel",
     "guitar synth percussion distance receding stereo"},
    // Reverb Beds
    {"Tape Drift Atmos", "soft tape echoes drift behind a broad ambient bed",
     "keys guitar pad ambient tape warm diffuse"},
    {"Dark Spring Room", "dark spring reflections place the source in a small room",
     "guitar keys drums room dark intimate spring"},
    {"Whole-Note Ambient", "long whole-note echoes dissolve into a diffuse cloud",
     "keys synth pad ambient long sustained diffuse"},
    // Dub Feedback
    {"Dub Spiral Feedback", "warm dub repeats spiral outward with long feedback",
     "drums guitar dub reggae feedback warm moving"},
    {"Echo Into Spring", "tape echoes feed a dark spring for classic dub depth",
     "drums guitar dub reggae warm echo to reverb"},
    {"Infinite Pad Ride", "near-endless repeats bloom into a wide, stable pad",
     "keys synth ambient drone sustained feedback"},
    // Glitch Patterns
    {"Golden Gallop Drive", "driven asymmetric repeats create a lurching gallop",
     "drums percussion guitar rhythm golden ratio asymmetric"},
    {"Bright Glitch Echo", "fast bright repeats scatter in a sharp stereo pattern",
     "drums percussion synth rhythm fast bright glitch"},
    {"Broken Loop Dropout", "worn echoes wobble, thin, and drop like damaged tape",
     "drums keys texture lo fi lofi damaged tape dropout"},
    // Transitions & Impacts
    {"Helix Surge", "dense bright echoes twist upward through a helix", "drums impacts transition riser bright moving"},
    {"Hard Lock Drop Hits", "short locked repeats turn hits into hard spatial stabs",
     "drums impacts percussion transient tight short"},
    {"Rush 16 Fill", "rapid sixteenth repeats make an immediate stereo rush",
     "drums percussion transition fill sixteenth fast"},
    // Perception Lab
    {"Externalizer Wash", "diffuse spatial reflections push the wet field outward",
     "keys voice vocals ambient diffuse spatial hearing"},
    {"Crossover Clarity", "bass stays centered while high echoes spread around it",
     "bass drums synth centered low end spatial hearing"},
    {"Elevation Cue", "bright elevated repeats sound clearly above the source",
     "keys synth percussion height spatial hearing"},
    // Motion Lab
    {"Envelopment Hall", "early reflections and a broad tail surround the source",
     "keys synth pad ambient spacious hall envelopment"},
    {"Motion Tracker Clean", "clean slow-moving repeats remain easy to locate",
     "guitar keys percussion motion localization clean"},
    {"Ping-Pong Ricochet", "tight echoes ricochet left and right on the beat",
     "drums percussion guitar rhythm stereo bouncing"},
    // Additions: preserve the original entries and their description copy.
    {"Cloud Packets", "soft cloud bursts for transitions and percussion",
     "drums impacts ambient texture envelope sparse bloom reverb to echo"},
    {"Forest Afterglow", "soft lingering echoes for acoustic guitar and keys",
     "nature forest woodland acoustic distance softened diffuse"},
    {"Crosscurrent", "straight and triplet echoes interlock on drums",
     "percussion guitar rhythm polyrhythm polyrhythmic crossrhythm pulse"},
    {"Offbeat Weave", "offbeat echoes add space around a drum or guitar groove",
     "drums percussion keys rhythm syncopation syncopated groove psychology"},
    {"Velvet Answer", "warm soft replies leave room for vocals and guitar",
     "voice vocal phrase warm dark answer foreground"},
    {"Pond Round", "three separate replies trade places around the source",
     "nature frog frogs animal calls turn taking percussion plucks conversation"},
    {"Distant Lantern", "distant melodic echoes behind a clear foreground",
     "keys guitar synth distance depth spatial hearing psychoacoustics"},
    {"Spring Aftershock", "returning spring echoes extend drum and impact hits",
     "drums percussion transitions dispersive metallic resonant reverb to echo"},
    // Round two: musical uses and exploratory listening comparisons.
    {"Bass Reply", "one clean echo answers upper-bass stabs",
     "dubstep riddim upper bass stab answer clean short fixed INFEKT"},
    {"Hat Ghost Triplets", "two quiet triplet ticks extend hat accents",
     "rap trap electronic trap hats hihat percussion triplet accent short quiet clean Lex Luger Crosstik"},
    {"Quarter Adlib", "a steady quarter-note echo answers vocal ad-libs",
     "rap trap voice vocal adlib ad lib phrase quarter answer warm fixed Seth Firkins"},
    {"Halfstep Shadow", "quiet quarter-note replies add detail to sparse drums",
     "deep dubstep halfstep percussion rim clack sparse warm static Mala"},
    {"Riddim Pocket Room", "a short cloud room softens upper-bass stabs",
     "dubstep riddim upper bass room compact static cloud ducked atmosphere INFEKT"},
    {"Lead Gap Cloud", "wide cloud tails become clearer between lead notes",
     "electronic trap festival trap lead horns chord wide ducked reverb cloud Flosstradamus"},
    {"Canon Windows", "clean melodic echoes return two and four beats later",
     "melody canon counterpoint guitar mallets marimba imitation clean late fixed Dowling"},
    {"Bell Counterphrase", "a dark bell reply arrives three beats later",
     "rap trap bells plucks piano late dark counterphrase warm distant Lex Luger"},
    {"Concrete Chatter", "experiment: short echoes make percussion chatter",
     "experiment architecture flutter echo hallway concrete knock percussion metallic clean unsynced DAFx"},
    {"Hidden Rhythm", "experiment: a side echo tests rhythmic grouping",
     "experiment spatial hearing grouping rhythm woodblock patterned percussion psychoacoustics Middlebrooks Onsan"},
    {"Static Sweep", "experiment: two fixed echoes test apparent travel",
     "experiment apparent motion short fixed positions noisy stab clipped plucks stationary wet only Strybel Neale"},
    {"Spatial Pull", "experiment: three fixed replies test spatial pull",
     "experiment auditory rabbit saltation spatial hearing clicks fixed left left right wet only residual cloud Getzmann"},
    // Spatial showcase: focused trajectories and separated directional replies.
    {"Point Orbit", "a focused sound follows a slow full-circle path",
     "headphones spatial showcase focused binaural mono sustained synth guitar noise point circular horizontal localization continuous"},
    {"Beat Orbit", "one focused orbit per whole note on sustained sounds",
     "headphones spatial showcase focused binaural mono sustained synth rhythm beat synced circular horizontal tempo localization"},
    {"Front Swing", "a focused sound swings across the front between sides",
     "headphones spatial showcase focused binaural mono sustained guitar synth speech frontal lateral pendulum tracking"},
    {"Near Ear Pass", "a close focused sound passes between the two sides",
     "headphones spatial showcase focused binaural mono sustained textured synth noise nearfield close intimate lateral pendulum"},
    {"Side Relay", "two clean replies answer from clearly separated sides",
     "headphones spatial showcase focused binaural mono sparse plucks percussion guitar side lateral fixed turn taking separated replies"},
    {"Orbit Replies", "two separated replies sample different orbit positions",
     "headphones spatial showcase focused binaural mono sparse plucks bells guitar rotating cascade independent heads conversation"},
    {"Approach Retreat", "a focused side sound cycles through near and far cues",
     "headphones spatial showcase focused binaural mono sustained noise synth textured lateral distance looming approach recede depth breathe"},
    {"Near Far Relay", "two side replies contrast close and distant cue settings",
     "headphones spatial showcase focused binaural mono sparse textured plucks noise side distance near far depth comparison experiment"},
    {"Rear Window", "experiment: a focused sound moves across rear angles",
     "headphones spatial showcase focused binaural mono sustained noise synth rear behind back pendulum front back confusion experiment"},
    {"Front Back Relay", "experiment: separate front and rear directional replies",
     "headphones spatial showcase focused binaural mono sparse noise textured plucks front rear back directional spectral comparison experiment"},
    {"Height Relay", "experiment: two replies contrast low and high angles",
     "headphones spatial showcase focused binaural mono sparse broadband textured noise plucks elevation vertical up down height spectral experiment"},
    {"Sky Figure Eight", "experiment: one sound traces a tilted figure-eight path",
     "headphones spatial showcase focused binaural mono sustained broadband noise synth lissajous elevation vertical figure eight trajectory experiment"},
};

inline constexpr size_t kDescriptionCount = sizeof(kDescriptions) / sizeof(kDescriptions[0]);

// Empty string when unknown (the browser paints its own fallback).
inline const char* find(const char* presetName)
{
    if (presetName == nullptr)
        return "";
    for (const auto& entry : kDescriptions)
        if (std::strcmp(entry.name, presetName) == 0)
            return entry.description;
    return "";
}

inline const char* findSearchTerms(const char* presetName)
{
    if (presetName == nullptr)
        return "";
    for (const auto& entry : kDescriptions)
        if (std::strcmp(entry.name, presetName) == 0)
            return entry.searchTerms;
    return "";
}
}  // namespace FactoryPresetDescriptions
