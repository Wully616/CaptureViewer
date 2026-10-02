# CaptureViewer

CaptureViewer is a lightweight, low-latency Linux video-capture viewer built with native C, GTK 3, and GStreamer. It is designed for VR viewing, with the Steam Frame as the primary development and test platform. This is a hobby/development project, not official Steam Frame support.

V4L2 provides capture-device access and mode enumeration; GStreamer handles video and HDMI audio. The fullscreen/windowed viewer has a pointer- and VR-pointer-friendly auto-hiding control panel, dynamic mode selection, audio monitoring controls, capture statistics, settings/diagnostics, and device reconnect handling. Bounded queues prioritize low latency.

## Hardware and current status

The tested hardware profile is a Hagibis UHC07 / MACROSILICON capture card (USB VID `345f`, PID `2130`) with an Amazon Fire TV source on Steam Frame. The current device discovery and audio pairing are explicitly matched to that USB identity. Other V4L2/UVC capture cards are potential targets, but have not been tested or verified by this project.

The application enumerates the tested device's advertised capture modes at runtime and remembers the selected mode. HDMI audio is captured separately and routed to the host's default output. Switch and Steam Deck inputs are potential uses only; they have not been verified.

Historical measurements from prior hardware runs (not measurements from the current environment):

- MJPEG 1280×720 at 60 fps: about 59 fps live and 58 fps average, zero reported drops, video queue depth 0–1 (a prior run recorded 1), approximately 76.7% CPU.
- MJPEG 1920×1080 at 50 fps: about 49.7 fps with zero reported drops.

These figures describe specific earlier runs, not a performance guarantee. GStreamer pipeline-reported latency is not HDMI-to-eye end-to-end latency and is deliberately not presented as such.

### Steam Frame UVC driver caveat

In the SteamOS test environment, a headset reboot previously cleared a session-only `uvcvideo` compatibility-module load and left the tested USB video interfaces unbound; they were available again after a later driver load/bind. Capture therefore depends on a compatible host UVC driver being available and bound on that system. This is a platform-specific observed caveat, not a claim that custom compatibility modules are required on Linux generally. CaptureViewer never loads or installs kernel modules, changes boot configuration, or modifies the kernel. Separate UVC module work is not part of this repository.

### Platform limitations

The current video pipeline requires X11 `ximagesink`; Wayland-only environments may not work. Device access, audio routing, and sink availability depend on the host. Flatpak permissions alone do not prove that real hardware capture or audio routing works in the sandbox. Steam Frame is the primary development/test platform; there is no claim of SteamOS/Discover certification or a published distribution.

## Controls and behavior

The application starts fullscreen. Use **S** to show or hide the auto-hiding control panel, **F11** to toggle fullscreen, and **Q** or the panel's **Quit** button to exit. **Escape** closes an open settings dialog or submenu first; otherwise it exits fullscreen or hides the panel. Escape never quits the application. The panel provides dynamic capture-mode selection, HDMI audio enable/volume, fullscreen, settings, stats, and pin controls. Settings includes diagnostics and panel timing controls. Statistics label GStreamer latency as pipeline-reported, not end-to-end.

HDMI audio is captured separately from video and routed through GStreamer's PulseAudio-compatible `pulsesink` to the host's default output. The intended Steam Frame route is its speakers. The quick-panel toggle and HDMI volume slider affect captured HDMI audio only; actual availability and routing depend on the host audio service and sandbox permissions.

## Configuration and logs

Settings are stored at `$XDG_CONFIG_HOME/captureviewer/config.ini` (normally `~/.config/captureviewer/config.ini`). On first launch after the rename, if the new config file does not exist and the prior `$XDG_CONFIG_HOME/hagibis-viewer/config.ini` exists and is readable, CaptureViewer copies its parsed settings to the new location. The old file is left untouched. Existing diagnostic logs under `$XDG_DATA_HOME/hagibis-viewer/` are also left untouched; new logs go to `$XDG_DATA_HOME/captureviewer/captureviewer.log` (normally `~/.local/share/captureviewer/captureviewer.log`).

## Screenshot

![CaptureViewer controls and UVC status on SteamOS](docs/screenshots/captureviewer-no-uvc.png)

*Development UI screenshot from the Steam Frame desktop with the tested Hagibis USB device detected but no UVC video interface bound. The black video area is expected in this driver-unavailable state; no capture stream was running.*

## Build dependencies

A C11 compiler, Meson (0.60 or newer), Ninja, and pkg-config are needed, along with development packages discoverable via pkg-config for:

- GTK 3 (`gtk+-3.0`)
- GStreamer core (`gstreamer-1.0`)
- GStreamer video (`gstreamer-video-1.0`)
- GStreamer audio (`gstreamer-audio-1.0`)

At runtime, install the applicable GStreamer plugins/elements for V4L2 capture, MJPEG decoding/conversion, `fpsdisplaysink`, `ximagesink`, and the chosen audio source/sink. Exact plugin packages vary by distribution. No dependencies are vendored; the source implementation contains no intentionally copied third-party code. The listed libraries and runtime plugins are external dependencies.

### Build and run

From the project root, configure and compile a debug build:

```sh
meson setup build --buildtype=debug
meson compile -C build
./build/captureviewer
```

Or configure a release build in its own directory:

```sh
meson setup build-release --buildtype=release
meson compile -C build-release
./build-release/captureviewer
```

List supported modes for the currently recognized capture card with `./build/captureviewer --list-modes`. Meson build directories are independent; install to a chosen prefix with `meson install -C build --destdir "$PWD/stage"`. The desktop entry, AppStream metadata, and scalable icon install under the standard data directories.

## Packaging and identity

GApplication and Flatpak use application ID `io.github.wully616.captureviewer`; the matching desktop filename and AppStream component ID use `io.github.wully616.captureviewer.desktop`. The icon uses the application-ID stem. The ID is derived from the supplied GitHub owner `Wully616`. Flatpak sandbox/device permissions and actual capture/audio operation still require target-device validation.

See [packaging findings and release checks](docs/packaging.md) for the sandbox and host-driver constraints. Host driver setup remains separate from the application.

## Source licensing

The project license is undecided; no project license is asserted here. A source audit found no intentionally copied third-party implementation code or vendored dependencies. GTK 3, GLib, and GStreamer upstream code are LGPL-2.1-or-later; individual GStreamer plugins and system packages can have additional or differing terms, so verify the exact runtime components for distribution. AppStream metadata uses `CC0-1.0` for metadata only; it does not license the application source.
