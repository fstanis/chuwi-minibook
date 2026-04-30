project('iio-sensor-proxy', [ 'c' ],
        version: '3.9',
        license: 'GPLv3+',
        default_options: [
          'buildtype=debugoptimized',
          'warning_level=1',
          'c_std=c99',
        ],
        meson_version: '>= 0.54.0')

cc = meson.get_compiler('c')

common_cflags = cc.get_supported_arguments([
    '-fgnu89-inline',
    '-fvisibility=hidden',
    '-std=gnu99',
    '-Wall',
    '-Wundef',
    '-Wunused',
    '-Wstrict-prototypes',
    '-Werror-implicit-function-declaration',
    '-Wno-pointer-sign',
    '-Wshadow'
])

prefix = get_option('prefix')
libexecdir = prefix / get_option('libexecdir')
bindir = get_option('bindir')
dbusconfdir = get_option('datadir') / 'dbus-1' / 'system.d'

mathlib_dep = cc.find_library('m', required: false)
udev_rules_dir = get_option('udevrulesdir')
if udev_rules_dir == 'auto'
    udev_dep = dependency('udev')
    udev_rules_dir = udev_dep.get_pkgconfig_variable('udevdir') + '/rules.d'
endif
systemd_system_unit_dir = get_option('systemdsystemunitdir')
if systemd_system_unit_dir == 'auto'
    systemd_dep = dependency('systemd')
    systemd_system_unit_dir = systemd_dep.get_pkgconfig_variable('systemdsystemunitdir')
endif
if get_option('gtk-tests')
    gtk_dep = dependency('gtk+-3.0', required: false)
endif
gio_dep = dependency('gio-2.0', version: '>= 2.76')
gudev_dep = dependency('gudev-1.0', version: '>= 237')
polkit_gobject_dep = dependency('polkit-gobject-1', version: '>= 0.91')
polkit_policy_directory = polkit_gobject_dep.get_pkgconfig_variable('policydir')
libssc_dep = dependency('libssc', version: '>=0.2.1', required: get_option('ssc-support'))

xmllint = find_program('xmllint', required: false)

gnome = import('gnome')

add_global_arguments('-D_GNU_SOURCE=1', language: 'c')

subdir('src')
subdir('data')

if get_option('gtk_doc')
  # Make like license available in the build root for docs
  configure_file(
    input: 'COPYING',
    output: 'COPYING',
    copy: true,
  )
  subdir('docs')
endif

if get_option('tests')
  # Python 3 required modules
  python3_required_modules = ['psutil', 'dbusmock', 'gi']

  python = import('python')
  python3 = python.find_installation('python3')
  foreach p : python3_required_modules
    # Source: https://docs.python.org/3/library/importlib.html#checking-if-a-module-can-be-imported
    script = 'import importlib.util; import sys; exit(1) if importlib.util.find_spec(\''+ p +'\') is None else exit(0)'
    if run_command(python3, '-c', script, check: false).returncode() != 0
      error('Python3 module \'' + p + '\' required for running tests but not found')
    endif
  endforeach

  subdir('tests')
endif
