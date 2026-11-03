Spatial Echo: macOS unsigned-install guide

IMPORTANT
Spatial Echo is free to download and use. Donations are optional.
This installer has no Apple Developer ID signature and is not notarized by
Apple. Obtain it from Echoes Lab's official download page. Do not disable
Gatekeeper globally.

WHAT IS IN THIS FOLDER
- The .pkg installer (Spatial-Echo-v<version>-mac.pkg)
- The matching .pkg.sha256 checksum file
- README.txt: this guide

OPTIONAL: VERIFY THE DOWNLOAD
1. Open Terminal and change into this folder.
2. Run:
   shasum -a 256 Spatial-Echo-*-mac.pkg
3. Compare the printed hash with the one in the .pkg.sha256 file in this
   folder. They must match exactly.

INSTALL
1. Quit Ableton Live and any other audio applications.
2. Open the .pkg installer in this folder in Finder.
3. If macOS blocks it, go to System Settings > Privacy & Security,
   scroll to the security message for Spatial Echo, choose Open Anyway, then
   confirm Open. Authenticate if prompted. On older macOS: System Preferences
   > Security & Privacy > General > Open Anyway. Control-click does not bypass
   this warning on macOS Sequoia or later.
4. Run the installer. It will request a Mac administrator password because it
   installs into shared plug-in and Applications folders.
5. Leave the needed formats selected, then complete installation.
6. Open your DAW and rescan plug-ins.

FIRST OPEN OF THE STANDALONE APP
If you install the standalone app, open /Applications/Spatial-Echo.app.
If macOS blocks it, use the
same Privacy & Security > Open Anyway path above.

WHERE IT INSTALLS
- VST3: /Library/Audio/Plug-Ins/VST3/Spatial-Echo.vst3
- Audio Unit: /Library/Audio/Plug-Ins/Components/Spatial-Echo.component
- Standalone app: /Applications/Spatial-Echo.app

SYSTEM REQUIREMENTS
- macOS 11 Big Sur or later
- Apple Silicon or Intel Mac
- A DAW that supports VST3 or Audio Units, if using the plug-in formats

PERFORMANCE
Spatial Echo is CPU-heavy. At 88.2/96 kHz or with many instances, watch your
DAW's CPU meter; if audio crackles, raise the buffer size or use fewer
instances.

TROUBLESHOOTING
If a DAW does not list the plug-in after a rescan, quit the DAW completely and
open it again. This package does not remove older copies under your home
Library; duplicate user/system copies can affect which version a DAW loads.
Do not run Terminal commands that disable Gatekeeper or remove macOS security
protections.

HELP
Email hello@echoeslabmusic.com with the exact message, your macOS version and
your DAW.
