# Packaging findings and release checks

## Host investigation

The investigated device reports SteamOS 0.5.2, variant `vr`, on Linux kernel `6.18.0-gaf9d0a0c005b`. Flatpak is version 1.15.8. A Flathub remote is configured and Freedesktop Platform 25.08 is installed; `flatpak-builder` was unavailable in the investigated environment. Discover consumes Flatpak and AppStream metadata rather than using a unique package format. These are environment findings, not a claim that an application package has been built, installed, accepted by Discover, or published.

The renderer uses GTK's OpenGL area with a Cairo fallback, rather than GStreamer `ximagesink`. The Flatpak manifest currently enables X11, PulseAudio protocol, and `--device=all` so that the tested desktop and V4L2 video devices can be exposed; Wayland is not enabled or validated here. `--device=all` is a broad device permission and must be reviewed against the target sandbox policy. Neither these permissions nor an installed runtime prove that a real capture card, host audio output, GTK rendering, or Steam Frame integration works in Flatpak. Validate on the real target hardware before release. The runtime's needed GStreamer elements/plugins must also be present and usable.

Host kernel driver setup remains separate. In the tested SteamOS environment, a headset reboot previously cleared a session-only `uvcvideo` compatibility-module load and left the Hagibis UHC07 interfaces unbound; they were available after a later load/bind. This is a platform-specific observation, not a universal Linux requirement. CaptureViewer must not install, load, or bundle kernel modules. The separate UVC module work is not part of this repository.

## Application ID and metadata

GApplication and Flatpak use application ID `io.github.wully616.captureviewer`; the desktop filename and AppStream component ID use the matching `io.github.wully616.captureviewer.desktop` identifier. The icon name is the application-ID stem. The AppStream metadata license `CC0-1.0` applies to metadata only; the application source license remains undecided. No credentials or distribution tokens are embedded in the manifest.

## Release verification still required

- Build the Flatpak manifest with the Freedesktop 25.08 runtime/SDK and validate it with the relevant Flatpak and AppStream tools.
- Inspect staged install paths and verify that desktop, icon, AppStream, and executable identities remain consistent.
- On Steam Frame hardware, verify the exact format/resolution/frame-rate selectors, per-device preference persistence, interface selection where applicable, USB audio auto-matching and explicit **None/device** choices, playback, diagnostics from a real error, hover controls, and unplug/reconnect behavior. With the capture card connected but HDMI absent, confirm the app stays alive and reports waiting for frames without claiming signal state; test frame delivery only with an active HDMI source. An advertised mode or successful enumeration is not proof of stream negotiation or frame delivery.
- Confirm Flatpak permissions are acceptable and sufficiently narrow; investigate whether deployment supports narrower video-device access than `--device=all`.
- Obtain the project's source-license decision before distribution.
