# Patched iio-sensor-proxy

The MiniBook X has two MXC6655 accelerometers -- one in the display half and one
in the base -- connected via I2C behind a single ACPI device (`MDA6655`).
Together they measure the hinge angle between the two halves, enabling automatic
screen rotation and tablet mode detection when the screen is flipped past ~185
degrees.

Stock iio-sensor-proxy does not support this hardware. The upstream `mxc4005`
kernel driver claims the I2C devices but only exposes a single accelerometer
through IIO, which is not enough to compute a hinge angle. This fork adds a
dedicated MXC6655 dual-accelerometer driver that reads both sensors directly via
raw I2C, computes the hinge angle and provides screen orientation and tablet
mode events.

Once installed, the following work automatically:

- **Screen auto-rotation** -- GNOME, KDE, and other desktops that use
  iio-sensor-proxy rotate the screen based on how you hold the device.
- **Tablet mode** -- flipping the screen past the hinge threshold disables the
  keyboard and touchpad (via ACPI and `SW_TABLET_MODE`). Folding it back
  re-enables them.

The fork source lives in `iio-sensor-proxy/`.

## Install

See [GUIDE.md](../GUIDE.md#7-iio-sensor-proxy).

## Lazy polling (`--lazy`)

By default the proxy polls both accelerometers continuously (every 50 ms) for
as long as it runs, so that tablet-mode transitions are detected even when no
desktop client is listening. This keeps the I2C bus and the sensors active the
whole time the daemon is up.

The `--lazy` flag makes the proxy poll a sensor only while a D-Bus client has
explicitly claimed it (via `ClaimAccelerometer` / `ClaimLight`), and stops
polling again once the last client releases it. This saves power when nothing
is consuming orientation events, at the cost of tablet-mode detection being
inactive until a client claims the accelerometer.

To enable it, add the flag to the service's `ExecStart`:

```
sudo systemctl edit iio-sensor-proxy
```

```
[Service]
ExecStart=
ExecStart=/usr/lib/iio-sensor-proxy --lazy
```

Then `sudo systemctl restart iio-sensor-proxy`. The path varies by distro
(`/usr/lib` on Arch, `/usr/libexec` elsewhere); `systemctl cat
iio-sensor-proxy` shows the current `ExecStart` to copy.

## Orientation overrides

The driver assumes the MiniBook X panel is mounted in portrait and that no
static rotation is applied, so in laptop mode it reports `right-up` and lets the
compositor rotate by 270 degrees. If a static rotation *is* applied it is
normally detected from DRM (see [How it works](#how-it-works)), but detection
does not cover every setup. Two environment variables override the defaults:

| Variable                      | Values                                                           | Default    |
| ----------------------------- | ---------------------------------------------------------------- | ---------- |
| `MINIBOOK_PANEL_ORIENTATION`  | `auto`, `normal`, `upside_down`, `left_side_up`, `right_side_up` | `auto`     |
| `MINIBOOK_LAPTOP_ORIENTATION` | `normal`, `left-up`, `bottom-up`, `right-up`                     | `right-up` |

`MINIBOOK_PANEL_ORIENTATION` takes the same values as the kernel's
`video=<connector>:panel_orientation=` -- copy whatever is on the kernel command
line. It replaces the DRM-detected panel orientation, and the rotation it
implies is subtracted from *every* reported orientation, so it shifts laptop
mode and tablet orientations alike. Use it when the panel is rotated but the DRM
property does not say so.

`MINIBOOK_LAPTOP_ORIENTATION` takes the orientation names `monitor-sensor`
prints, and changes only the fixed orientation reported outside tablet mode. It
is still compensated by the panel orientation, so it is the knob for a laptop
mode that is wrong while tablet rotations are right.

Invalid values are ignored with a warning in the journal.

Set them in `/etc/default/iio-sensor-proxy` (read by the service, no unit
editing needed):

```
MINIBOOK_PANEL_ORIENTATION=right_side_up
```

Then `sudo systemctl restart iio-sensor-proxy`. If the running unit predates
this fork and does not read that file (`systemctl cat iio-sensor-proxy` has no
`EnvironmentFile` line), set them with `sudo systemctl edit iio-sensor-proxy`
instead:

```
[Service]
Environment="MINIBOOK_PANEL_ORIENTATION=right_side_up"
```

The values in effect are logged at startup:

```
journalctl -u iio-sensor-proxy | grep MXC6655
```

```
MXC6655: panel orientation right_side_up
MXC6655: compensating sensor output by 270°, laptop mode reports normal
```

Check that line first: if the panel orientation it prints already matches the
kernel command line, detection is working and these overrides are not the fix.

## Lid gating

Polling stops while the lid is closed (regardless of `--lazy` or any claim) and
resumes when it opens; tablet mode is unaffected. The driver logs each
transition to the journal: `journalctl -u iio-sensor-proxy | grep 'polling'`.

## Runtime requirement

The `acpi_call` kernel module must be loaded for tablet mode transitions (the
driver calls ACPI method `\_SB.ACMK.LTSM` to toggle the keyboard). If
`acpi_call` is not available, screen rotation still works but tablet mode
toggling via ACPI is skipped.

## Verify

```
monitor-sensor
```

Tilt the device and watch the orientation change. Flip the screen past ~185
degrees and it should report tablet mode. The journal also shows debug output:

```
journalctl -u iio-sensor-proxy -f
```

## How it works

The driver polls both MXC6655 accelerometers at 50 ms intervals via raw I2C
(bypassing the `mxc4005` kernel driver, which it unbinds at startup). Each
sample goes through:

1. **Calibration** -- a 3x3 rotation matrix per sensor, loaded from the ACPI
   `GMTR` method at startup. These correct for how each sensor is physically
   mounted relative to the chassis. If `GMTR` is unavailable, hardcoded defaults
   matching the MiniBook X layout are used.

1. **Hinge angle computation** -- the calibrated readings from both
   accelerometers are projected onto the hinge axis using `atan2`, and the
   difference gives the angle between the display and base halves. The math
   follows the DSDT's `GMTR` routine (including its use of `180/3.14` instead of
   the true value of pi).

1. **Tablet mode state machine** -- the hinge angle is compared against
   thresholds (default: 185 degrees for tablet, 175 degrees for laptop) with
   debouncing. Transitions emit `SW_TABLET_MODE` via a uinput device and call
   the ACPI `LTSM` method to toggle the keyboard/touchpad at the EC level.

1. **Orientation filter** -- the display accelerometer's readings pass through a
   multi-stage pipeline (median filter, EMA smoothing, variance- based stability
   detection, gravity offset tracking) before being classified into one of four
   orientations (normal, left, right, inverted). A debounce counter prevents
   rapid flickering. The final orientation is fed to iio-sensor-proxy's standard
   callback, which exposes it over D-Bus for desktop auto-rotation.

   Outside tablet mode the classification result is replaced with `right-up`
   (configurable, see [Orientation overrides](#orientation-overrides)).
   The MiniBook X panel is mounted in portrait, so a compositor consuming
   orientation events (e.g. via `iio-niri`) applies a 270° rotation in laptop
   mode and follows the accelerometer once the lid folds past the tablet
   threshold. This removes the need for a separate static rotation fix (kernel
   cmdline, VBT patch, xrandr); see [GUIDE.md](../GUIDE.md#display-rotation).

   Folding back to laptop mode emits the landscape orientation directly, instead
   of waiting for the accelerometer pipeline to settle. Compositors that only
   rotate when a reading arrives would stay in the last tablet rotation
   otherwise.

   If a static rotation *is* already applied, the driver reads the DRM
   `panel orientation` property of the DSI connector at startup and subtracts it
   from every reported orientation, so the dynamic and static rotations do not
   stack. The detected orientation is logged at startup (`grep MXC6655` in the
   journal), and can be overridden when detection gets it wrong (see
   [Orientation overrides](#orientation-overrides)).
