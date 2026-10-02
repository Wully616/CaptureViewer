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

gtk = dependency('gtk+-3.0', required: true)
gst = dependency('gstreamer-1.0', required: true)
gst_video = dependency('gstreamer-video-1.0', required: true)
gst_audio = dependency('gstreamer-audio-1.0', required: true)

viewer = executable(
  'captureviewer',
  ['src/main.c', 'src/capture.c'],
  dependencies: [gtk, gst, gst_video, gst_audio],
  c_args: cc_args,
  install: true,
)

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
