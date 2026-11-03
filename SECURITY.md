# Security Policy

Spatial Echo is an audio plug-in and standalone application. It has no server
component, makes no network connections of its own, and stores no credentials.
The OPEN LAB button opens a web page in your default browser; nothing else
leaves your machine.

## Supported versions

Only the latest 1.x release receives fixes.

## What counts as a security issue

In scope:

- Memory-safety or crash bugs reachable through untrusted input: a
  `.spatialEchopreset` file or host session state.
- Installer behaviour that writes outside the documented locations or needs
  more privilege than the installer asks for.

Out of scope: CPU overload, dropouts, crashes caused by the host, and
sound-quality problems. Report those as ordinary bugs.

## Reporting

Email hello@echoeslabmusic.com with "Spatial Echo security" in the subject
line. You can also use GitHub's private vulnerability reporting for this
repository (Security tab, "Report a vulnerability") when it is available.
Please do not open a public issue for a vulnerability. Include the plug-in
version, your OS, and a way to reproduce the issue.

We aim to acknowledge reports within a week. Fixes ship in the next release and
are noted in the changelog. There is no bug bounty.

## Verifying downloads

Every installer is published with a `.sha256` checksum file. Compare the hash
before installing.
