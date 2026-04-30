iio-sensor-proxy
================

IIO sensors to D-Bus proxy

See https://hadess.pages.freedesktop.org/iio-sensor-proxy/ for
developer information.

Matrix room: https://matrix.to/#/#iio-sensor-proxy:dylanvanassche.be

Installation
------------
```sh
$ meson _build -Dprefix=/usr
$ ninja -v -C _build install
```
It requires libgudev, systemd (>= 233 for the accelerometer quirks) and polkit-gobject.

Running the tests will require gobject-introspection support for Python,
[python-dbusmock](https://github.com/martinpitt/python-dbusmock/),
[umockdev](https://github.com/martinpitt/umockdev) and [psutil](https://github.com/giampaolo/psutil).

Usage
-----

With a GNOME 3.18 (or newer) based system, orientation changes will
automatically be applied when rotating the panel, ambient light will be used
to change the screen brightness, and GeoClue will be able to read the compass
data to show the direction in Maps.

Note that nothing in iio-sensor-proxy is GNOME specific, or relies on GNOME.
GNOME and GeoClue use the data provided by iio-sensor-proxy, other desktop
environments are more than welcome to use this as a basis for their own
integration.

Debugging
---------

Note that a number of kernel bugs will prevent it from working correctly on
some machines so please make sure to use the latest upstream kernel (kernel
crashes on the Surface Pro, sensor failing to work after suspend on the Yoga
Pro, etc.).

You can verify that sensors are detected by running `udevadm info --export-db`
and checking for an output resembling this one:
```
P: /devices/platform/80860F41:04/i2c-12/i2c-BMA250E:00/iio:device0
N: iio:device0
E: DEVNAME=/dev/iio:device0
E: DEVPATH=/devices/platform/80860F41:04/i2c-12/i2c-BMA250E:00/iio:device0
E: DEVTYPE=iio_device
E: MAJOR=249
E: MINOR=0
E: SUBSYSTEM=iio
E: SYSTEMD_WANTS=iio-sensor-proxy.service
E: TAGS=:systemd:
E: USEC_INITIALIZED=7750292
```

You can now check whether a sensor is detected by running:
```
gdbus introspect --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy
```

After that, use `monitor-sensor` to see changes in the ambient light,
accelerometer, compass or proximity sensors.
```sh
Usage:
  monitor-sensor [OPTION…]

Help Options:
  -h, --help       Show help options

Application Options:
  -a, --all        Monitor all the sensor changes
  --accel          Monitor accelerometer changes
  --proximity      Monitor proximity sensor changes
  --compass        Monitor compass changes
  --light          Monitor light changes changes
```

If that doesn't work, please file an issue, make sure iio-sensor-proxy is new
enough and attach the output of:
```sh
/usr/libexec/iio-sensor-proxy -v -r
```
running as `root`.

Note that, before filing any issues against iio-sensor-proxy, you'll want
to make doubly-sure that the IIO device actually works, using `iio_generic_buffer`
(usually packaged as part of the `kernel-tools` package). For example, to
test IIO sensor `0`:
```sh
sudo iio_generic_buffer -A -N 0
```

Accelerometer orientation
-------------------------

iio-sensor-proxy expects the accelerometer readings to match those defined
in the [Linux IIO](https://git.kernel.org/pub/scm/linux/kernel/git/jic23/iio.git/tree/Documentation/ABI/testing/sysfs-bus-iio#n1638)
documentation, the [Windows Integrating Motion and Orientation Sensors](https://docs.microsoft.com/en-us/windows-hardware/design/whitepapers/integrating-motion-and-orientation-sensors)
whitepaper, and the [Android sensor documentation](https://developer.android.com/reference/android/hardware/SensorEvent).

When the accelerometer is not mounted the same way as the screen, we need
to modify the readings from the accelerometer to make sure that the computed
orientation matches the screen one.

This is done using a “mount matrix”, a matrix that applied to the reading will
correct that reading to match the expected orientation, whether:
- selecting a value for an axis (with a $`1`$)
- negating that value (with a $`-1`$)
- ignoring that value (with a $`0`$)

```math
  \left[ {\begin{array}{ccc}
   accel~x & accel~y & accel~z\\
  \end{array} } \right]
*
  \left[ {\begin{array}{ccc}
   x_1 & y_1 & z_1\\
   x_2 & y_2 & z_2\\
   x_3 & y_3 & z_3\\
  \end{array} } \right]
 =
  \left[ {\begin{array}{ccc}
   corrected~x & corrected~y & corrected~z\\
  \end{array} } \right]
```

For each axis of the original accelerometer reading, a corresponding line in the
mount matrix will be used to compute which axis it should be assigned to in the
corrected readings. For example, the first line of the matrix will define whether
the $`accel~x`$ value will correspond to the corrected $`x`$, $`y`$ or $`z`$ axis
(with a $`1`$ for that axis, and $`0`$ for the others), or needs to be negated
(by setting it to $`-1`$).

A matrix of $` \left[ {\begin{array}{ccc}
   0 & 0 & 1\\
   0 & -1 & 0\\
   1 & 0 & 0\\
  \end{array} } \right]
`$ will swap the $`x`$ and $`z`$ values, and negate the $`y`$ reading.

A matrix of $` \left[ {\begin{array}{ccc}
   1 & 0 & 0\\
   0 & 1 & 0\\
   0 & 0 & 1\\
  \end{array} } \right]
`$ (the identity matrix) is the default, and will not change any of the read
values.

Each accelerometer axis reading can only be assigned (negated or not) to a
single corrected axis reading, and each corrected axis reading can only come
from a single accelerometer axis reading.

`iio-sensor-proxy` reads this information from the device's
`ACCEL_MOUNT_MATRIX` udev property. See [60-sensor.hwdb](https://github.com/systemd/systemd/blob/master/hwdb.d/60-sensor.hwdb)
for details.

For device-tree based devices, and some ACPI ones too, the `mount_matrix`,
`in_accel_mount_matrix` and `in_mount_matrix` sysfs files can also be used
to export that information directly from the kernel.

The mount matrix format used in systemd's hwdb and the IIO subsystem is a
one-line representation of the matrix above:
```math
x_1, y_1, z_1; x_2, y_2, z_2; x_3, y_3, z_3
```

Accelerometer testing (without an accelerometer)
------------------------------------------------

If your hardware lacks an accelerometer, you can run the `fake-input-accelerometer` binary as root.
This should create a new virtual device on the system, which can be interacted through `monitor-sensor`,
`evtest`, or raw D-Bus. Use the printed keyboard commands to change the calculated orientation
of that accelerometer.

Compass testing
---------------

Only the GeoClue daemon (as the geoclue user) is allowed to access the `net.hadess.SensorProxy.Compass`
interface, the results of which it will propagate to clients along with positional information.

If your device does not contain a compass, you can run tests with:
- If your device does not contain a real compass:
  - Add FAKE_COMPASS=1 to the environment of `iio-sensor-proxy.service` if your device does not contain a real one
  - Run the daemon manually with `systemctl start iio-sensor-proxy.service`
- Verify that iio-sensor-proxy is running with `systemctl` or `ps`
- As root, get a shell as the `geoclue` user with `su -s /bin/bash geoclue`
- Run, as the `geoclue` user, `monitor-sensor`

Proximity sensor near detection
-------------------------------

Whether an object is considered near to a proximity sensor depends on the
sensor configuration and scale but also on the device's physical properties
(e.g. the distance of the sensor from the covering glass) so there is
no good device-independent default.

`iio-sensor-proxy` reads this information from the device's
`PROXIMITY_NEAR_LEVEL` udev property. See [60-sensor.hwdb](https://github.com/systemd/systemd/blob/master/hwdb.d/60-sensor.hwdb)
for details.

For device-tree based devices, exporting the information through the kernel is done
via the [near-level property](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/devicetree/bindings/iio/common.yaml#n22).

Known problems
--------------

Every Linux kernel from 4.3 up to version 4.12 had a bug that made
made iio-sensor-proxy fail to see any events coming from sensors until the
sensor was power-cycled (unplugged and replugged, or suspended and resumed).

The bug was finally fixed in [this commit](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f1664eaacec31035450132c46ed2915fd2b2049a)
in the upstream kernel and backported to stable releases. If you experience
unresponsive sensors, ask your distributor to make sure this patch was
applied to the version you're using.

The IIO sensors regressed again not long afterwards, which got fixed in
[this commit](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=6f92253024d9d947a4f454654840ce479e251376).
Again, make sure that your kernel contains this fix.

References
----------

- [sensorfw](https://git.merproject.org/mer-core/sensorfw/tree/master)
- [android-iio-sensors-hal](https://github.com/01org/android-iio-sensors-hal)
- [Sensor orientation on MSDN](https://msdn.microsoft.com/en-us/windows/uwp/devices-sensors/sensor-orientation)
