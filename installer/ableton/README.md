# Spatial Echo starting controls in Ableton Live

These native Ableton configurations give new Spatial Echo AU and VST3 instances
a panel with ten controls. The files select which controls Live displays
initially and store no sound parameter values. The installers do not copy them;
follow the setup steps below.

The starting controls are Mix, Feedback, Echo Level, Reverb Level, Decay, Tone,
Source Width, Spatial Pattern, Spatial Rate, and Spatial Depth. They cover the
wet/dry balance, echo regeneration, echo/reverb balance, reverb shaping, source
width, and spatial motion. The AU and VST3 files select the same controls;
their native panel order differs.

Per-head coordinates and mode-dependent setup controls add complexity to the
starting panel. They stay automatable and can be added when needed through
Live's Configure workflow. Spatial Rate controls free motion; when Spatial Sync
is enabled in the plug-in, use its motion division control instead.

## Setup

1. Locate your User Library using Live's **Settings → Library**. The default
   macOS location is `~/Music/Ableton/User Library`; use your configured location
   if it differs.
2. Quit Live before copying the configuration files.
3. Check the two destination paths below for an existing `Default.appc`. If you
   already have a custom default, keep it or move it to a backup location before
   replacing it.
4. Copy each supplied `Default.appc` to its matching location inside your User
   Library, creating the folders if necessary.
5. Reopen Live and insert a new Spatial Echo instance from the Plug-Ins browser.
6. Unfold the device's parameter panel to display the ten controls.

Paths relative to your User Library:

| Format | Configuration path |
| --- | --- |
| AU | `Defaults/Plug-In Configurations/Audio Units/Echoes Lab/Spatial Echo/Default.appc` |
| VST3 | `Defaults/Plug-In Configurations/VST3s/Echoes Lab/Spatial Echo/Default.appc` |

The supplied `Defaults` folder mirrors these paths. Copy only the two
configuration files; preserve your other plug-in defaults.

New defaults apply to newly inserted devices. Existing devices retain their
saved panel configuration. To change an existing panel, use Configure to remove
the unwanted displayed controls, then choose **Save as Default Configuration**
from the device's context menu if you want that layout for future instances.

## Add a control

1. Click **Configure** on Spatial Echo's Live device panel.
2. Move the desired control in the plug-in's own window to add it to Live's panel.
3. Click **Configure** again to finish editing the panel.

Save the Live Set to retain the expanded panel for that instance. Saving a new
default configuration separately changes the panel for future instances.
