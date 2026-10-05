# CaptureViewer

CaptureViewer is a low-latency Linux viewer for USB video capture devices. It uses V4L2 for capture, GStreamer for video and audio, and GTK 3 for the interface. It provides fullscreen and windowed viewing, capture-mode selection, scaling controls, audio selection, and capture statistics.

## Tested hardware

Tested on Steam Frame with the Hagibis UHC07 HDMI capture card (USB VID `345f`, PID `2130`). Other V4L2 capture devices may work, but have not been verified. The tested display setup uses X11/Xwayland; Wayland-only environments are unverified.

## UVC support on Steam Frame

If SteamOS's native `uvcvideo` driver does not bind to the capture card, V4L2 cannot provide its capture modes. Open CaptureViewer's **Advanced Capture Diagnostics** panel and select **Manage Capture Support**, then choose **Install** when support is needed. CaptureViewer installs a compatibility module for the running kernel and configures it to load on startup.
CaptureViewer checks support status in the background, so a slow host-module lookup does not stall the viewer; an already loaded `uvcvideo` module is recognized without a connected capture card.

The module is installed outside the app so it remains available across app restarts. Persistence across SteamOS updates has not been verified.

## Performance

On Steam Frame with the Hagibis UHC07, the highest validated capture mode is MJPEG 1280×720 at 60 fps: about 60 fps live, 57.6 fps startup average, and zero pipeline-reported dropped frames. MJPEG 1920×1080 at 50 fps reached about 49.9 fps with zero reported drops. These are observed results, not performance guarantees; pipeline statistics do not measure end-to-end display latency.

## Build and install on SteamOS

Build requirements: a C compiler, Meson 0.60 or newer, Ninja, pkg-config, GTK 3, GLib 2.74 or newer, GStreamer core/video/audio/app development libraries, libepoxy, GNU tar, and zstd. Run these commands from the repository root to build a user-installable archive and install it without system-wide package installation:

```sh
tmp=$(mktemp -d /tmp/captureviewer-uvc-test.XXXXXX)

meson setup "$tmp/build" \
  --prefix=/captureviewer/app \
  --bindir=bin \
  --libexecdir=libexec \
  --datadir=share

meson compile -C "$tmp/build"
DESTDIR="$tmp/stage" meson install -C "$tmp/build"

mkdir -p "$tmp/payload"
mv "$tmp/stage/captureviewer/app" "$tmp/payload/app"
tar --zstd -cf "$tmp/captureviewer-steamos-aarch64.tar.zst" \
  -C "$tmp/payload" app

bash packaging/steam-frame/install-user.sh \
  "$tmp/captureviewer-steamos-aarch64.tar.zst"
```
