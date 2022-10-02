project('gst-vosk', 'c',
        version: '0.3.0',
        meson_version: '>= 0.56.0',
        license:'GPL2+',
        default_options: [ 'warning_level=2',
                         ],
)

plugin_c_args = ['-DHAVE_CONFIG_H']

plugin_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_dep = dependency('gstreamer-1.0',
                     version : '>=1.20',
                     required : true,
                     fallback : ['gstreamer', 'gst_dep'])

gio_dep = dependency('gio-2.0', required : true)

i18n = import('i18n')

config_h = configuration_data()
config_h.set_quoted('PACKAGE', meson.project_name())
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('PACKAGE_LICENSE', meson.project_license())
config_h.set_quoted('GETTEXT_PACKAGE', 'gst-vosk')
config_h.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))

uselocale_test = '''
  #include <locale.h>
  int main(void) {locale_t locale_test; locale_test = uselocale (NULL); uselocale (locale_test); return 0; }'''

if cc.links(uselocale_test)
  config_h.set('HAVE_USELOCALE', 1)
endif

configure_file(
  output: 'gst-vosk-config.h',
  configuration: config_h,
)

subdir('src')
subdir('vosk')
subdir('po')
