rules = files(
  '80-iio-sensor-proxy.rules'
)

install_data(
  '80-iio-sensor-proxy.rules',
  install_dir: udev_rules_dir
)

data_conf = configuration_data()
data_conf.set('libexecdir', libexecdir)
data_conf.set('geoclue_user', get_option('geoclue-user'))

configure_file(
  input: 'iio-sensor-proxy.service.in',
  output: 'iio-sensor-proxy.service',
  configuration: data_conf,
  install_dir: systemd_system_unit_dir,
)

configure_file(
  input: 'net.hadess.SensorProxy.conf.in',
  output: 'net.hadess.SensorProxy.conf',
  configuration: data_conf,
  install_dir: dbusconfdir
)

polkit_policy = 'net.hadess.SensorProxy.policy'
if xmllint.found()
  test(polkit_policy,
       xmllint,
       args: [
           '--noout',
           meson.source_root() / 'data' / polkit_policy,
       ])
endif

install_data(
  polkit_policy,
  install_dir: polkit_policy_directory,
)
