gst_vosk_sources = [
  'gstvosk.c',
  ]

vosk_libdir = meson.project_source_root() / 'vosk'
vosk_dep = declare_dependency(
	dependencies : cc.find_library('vosk', dirs : vosk_libdir),
	include_directories : include_directories('../vosk/'),
)

gstvosk = library('gstvosk',
  gst_vosk_sources,
  c_args: plugin_c_args,
  dependencies : [gst_dep, gio_dep, vosk_dep],
  install : true,
  install_dir : plugin_install_dir,
)
