project(
  'captureviewer',
  'c',
  version: '0.1.0',
  meson_version: '>= 0.60.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

cc = meson.get_compiler('c')
cc_args = []
foreach warning : ['-Wformat=2', '-Wshadow', '-Wstrict-prototypes']
  if cc.has_argument(warning)
    cc_args += [warning]
  endif
endforeach

gtk = dependency('gtk+-3.0', version: '>=3.22', required: true)
glib = dependency('glib-2.0', version: '>=2.74', required: true)

gst = dependency('gstreamer-1.0', required: true)
gst_video = dependency('gstreamer-video-1.0', required: true)
gst_audio = dependency('gstreamer-audio-1.0', required: true)
gst_app = dependency('gstreamer-app-1.0', required: true)
epoxy = dependency('epoxy', required: true)
math = cc.find_library('m', required: true)

uvc_libexecdir = get_option('libexecdir')
if not uvc_libexecdir.startswith('/')
  uvc_libexecdir = join_paths(get_option('prefix'), uvc_libexecdir)
endif
uvc_helper_dir = uvc_libexecdir / 'captureviewer' / 'uvc'

uvc_config = configuration_data()
uvc_config.set_quoted(
  'CAPTUREVIEWER_UVC_LIBEXECDIR',
  uvc_helper_dir,
)
configure_file(
  output: 'captureviewer-config.h',
  configuration: uvc_config,
)


viewer = executable(
  'captureviewer',
  [
    'src/main.c',
    'src/app.c',
    'src/capture.c',
    'src/renderer.c',
    'src/pipeline.c',
    'src/preferences.c',
    'src/uvc_setup.c',
    'src/audio.c',
    'src/ui.c',
  ],
  dependencies: [gtk, glib, gst, gst_video, gst_audio, gst_app, epoxy, math],
  c_args: cc_args,
  install: true,
)

renderer_layout_test = executable(
  'test-renderer-layout',
  ['tests/test_renderer_layout.c', 'src/renderer.c'],
  dependencies: [gtk, gst, gst_video, gst_app, epoxy, math],
  c_args: cc_args,
)
test('renderer-layout', renderer_layout_test)

capture_identity_test = executable(
  'test-capture-identity',
  ['tests/test_capture_identity.c', 'src/capture.c'],
  dependencies: [glib],
  c_args: cc_args,
)
test('capture-identity', capture_identity_test)

uvc_setup_test = executable(
  'test-uvc-setup',
  ['tests/test_uvc_setup.c', 'src/uvc_setup.c', 'src/capture.c'],
  dependencies: [glib],
  c_args: cc_args,
)
test('uvc-setup', uvc_setup_test)

install_data(
  'data/io.github.wully616.captureviewer.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data(
  'data/io.github.wully616.captureviewer.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)
install_data(
  'data/icons/io.github.wully616.captureviewer.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_data(
  'data/uvc/driver-helper.sh',
  'data/uvc/captureviewer-uvc.service',
  'data/uvc/LICENSE',
  install_dir: get_option('datadir') / 'captureviewer' / 'uvc-support',
  install_mode: 'rw-r--r--',
)

install_data(
  'data/uvc/setup.sh',
  'data/uvc/build.sh',
  'data/uvc/load.sh',
  'data/uvc/verify.py',
  'packaging/steam-frame/bootstrap-support.sh',
  install_dir: uvc_helper_dir,
  install_mode: 'rwxr-xr-x',
)
