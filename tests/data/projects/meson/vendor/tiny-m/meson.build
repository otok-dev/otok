project('tiny-m', 'c', version: '1.0', meson_version: '>= 1.2')

args = ['-DTINY_M_VALUE=' + get_option('value').to_string(), '-DTINY_M_BUILD']
if get_option('default_library') == 'static'
  args += ['-DTINY_M_STATIC']
endif
library('tiny_m', 'tiny_m.c', c_args: args, install: true)
install_headers('tiny_m.h')
