# Packaging findings and release checks

## Host investigation

The investigated device reports SteamOS 0.5.2, variant `vr`, on Linux kernel `6.18.0-gaf9d0a0c005b`. Flatpak is version 1.15.8. A Flathub remote is configured and Freedesktop Platform 25.08 is installed; `flatpak-builder` was unavailable in the investigated environment. Discover consumes Flatpak and AppStream metadata rather than using a unique package format. These are environment findings, not a claim that an application package has been built, installed, accepted by Discover, or published.

The application requires X11 `ximagesink`. The Flatpak manifest enables X11, PulseAudio protocol, and `--device=all` so that V4L2 video devices can be exposed. `--device=all` is a broad device permission and must be reviewed against the target sandbox policy. Neither these permissions nor an installed runtime prove that a real capture card, host audio output, display sink, or Steam Frame integration works in Flatpak. Validate on the real target hardware before release. The runtime's needed GStreamer elements/plugins must also be present and usable.

Host kernel driver setup remains separate. In the tested SteamOS environment, a headset reboot previously cleared a session-only `uvcvideo` compatibility-module load and left the Hagibis UHC07 interfaces unbound; they were available after a later load/bind. This is a platform-specific observation, not a universal Linux requirement. CaptureViewer must not install, load, or bundle kernel modules. The separate UVC module work is not part of this repository.

## Application ID and metadata

GApplication and Flatpak use application ID `io.github.wully616.captureviewer`; the desktop filename and AppStream component ID use the matching `io.github.wully616.captureviewer.desktop` identifier. The icon name is the application-ID stem. The AppStream metadata license `CC0-1.0` applies to metadata only; the application source license remains undecided. No credentials or distribution tokens are embedded in the manifest.

## Release verification still required

- Build the Flatpak manifest with the Freedesktop 25.08 runtime/SDK and validate it with the relevant Flatpak and AppStream tools.
- Inspect staged install paths and verify that desktop, icon, AppStream, and executable identities remain consistent.
- Test discovery/access to the actual Hagibis UVC device, video capture through `ximagesink`, audio capture and playback, configuration migration/persistence, hover controls, settings, and disconnect/reconnect on Steam Frame hardware.
- Confirm Flatpak permissions are acceptable and sufficiently narrow; investigate whether deployment supports narrower video-device access than `--device=all`.
- Obtain the project's source-license decision before distribution.
