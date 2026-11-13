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

The bootstrap scripts install it (see [GUIDE.md](../GUIDE.md)); for manual
steps see [GUIDE-ADVANCED.md](../GUIDE-ADVANCED.md#4-iio-sensor-proxy).

## Desktop integration

The proxy exposes orientation on D-Bus (`net.hadess.SensorProxy`). How that
becomes a screen rotation depends on your desktop:

- **GNOME and KDE Plasma (Wayland)**: built-in. Enable auto-rotate in the
  quick-settings panel / System Settings. No extra daemon needed.
- **Niri**: install [`iio-niri`](https://github.com/Zhaith-Izaliel/iio-niri)
  and save as `~/.config/systemd/user/iio-niri.service`:
  ```
  [Unit]
  Description=Screen auto-rotation for niri (iio-niri)
  PartOf=graphical-session.target
  After=graphical-session.target
  Requisite=graphical-session.target

  [Service]
  ExecStart=/usr/bin/iio-niri listen --monitor DSI-1
  Restart=always
  RestartSec=2

  [Install]
  WantedBy=niri.service
  ```
  Then `systemctl --user enable --now iio-niri`. Optionally add
  `transform "270"` to niri's `output "DSI-1"` block so the screen starts in
  landscape before iio-niri connects.
- **Sway / wlroots compositors**: use
  [`iio-sway`](https://github.com/okeri/iio-sway) (works on Sway, river,
  Wayfire) or an equivalent bridge for your compositor.
- **Hyprland**: use
  [`iio-hyprland`](https://github.com/JeanSchoeller/iio-hyprland).

The patched proxy reports `right-up` whenever the device is in laptop mode, so
the compositor applies the 270° portrait correction dynamically and switches
to live accelerometer rotation in tablet mode.

**On Niri with iio-niri**, leave the Limine kernel-command-line rotation fix
(`video=DSI-1:panel_orientation=right_side_up`) disabled unless your iio-niri
includes the fix linked under [Known issues](#known-issues).

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

## Orientation sensor

The accelerometer supplying screen orientation is picked automatically: the
driver evaluates the ACPI `ACMK._CRS` method and uses the first I2C resource
listed -- the same "slot 1" the stock Windows driver feeds its orientation
output from. On this firmware that is the display accelerometer, so rotation
follows the screen whenever the two halves move independently (the hinge stays
free past the tablet threshold). The chosen sensor is logged at startup. If
`_CRS` cannot be evaluated or matched, the first sensor found is used and a
warning is logged. The `MINIBOOK_ORIENTATION_SENSOR` environment variable is
no longer honored. Selection does not change sensor discovery or the
calibrated readings used for hinge calculation. The selected raw reading has
its X axis reversed before entering the orientation filter.

## Lid gating

Polling stops while the lid is closed (regardless of `--lazy` or any claim) and
resumes when it opens; tablet mode is unaffected. The driver logs each
transition to the journal: `journalctl -u iio-sensor-proxy | grep 'polling'`.

## Runtime requirements

The [minibook_ec](minibook-ec.md) module provides everything the driver needs
from the firmware: GMTR calibration, the `_CRS` sensor order, and tablet-mode
keyboard toggling, all through its sysfs interface (`gmtr`, `crs`,
`keyboard_enabled`). Without the module the driver still rotates the screen,
using hardcoded calibration defaults and the first sensor for orientation;
each degradation logs a warning. The out-of-tree `acpi_call` module is no
longer used.

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

The driver polls both MXC6655 accelerometers at the `GMTR`-configured interval
(50 ms on this firmware) via raw I2C (bypassing the `mxc4005` kernel driver,
which it unbinds at startup). Each sample goes through:

1. **Calibration** -- a 3x3 rotation matrix per sensor, loaded from the ACPI
   `GMTR` method at startup. These correct for how each sensor is physically
   mounted relative to the chassis. If `GMTR` is unavailable, hardcoded defaults
   matching the MiniBook X layout are used. `GMTR` also supplies the debounce
   count and poll interval.

1. **Hinge angle computation** -- the calibrated readings from both
   accelerometers are projected onto the hinge axis using `atan2`, and the
   difference gives the angle between the display and base halves. The math
   follows the DSDT's `GMTR` routine (including its use of `180/3.14` instead
   of the true value of pi). The Z inputs pass a median-of-3 spike filter, and
   the angle is only computed while gravity stays off both sensors' hinge (Y)
   axes -- up to ~64 degrees of hinge tilt, the same abstain window the stock
   Windows driver uses.

1. **Tablet mode state machine** -- the hinge angle is compared against
   thresholds (default: 185 degrees for tablet, 175 degrees for laptop) with
   debouncing. Transitions emit `SW_TABLET_MODE` via a uinput device and call
   the ACPI `LTSM` method to toggle the keyboard/touchpad at the EC level.

1. **Orientation settle gate** -- the selected accelerometer's readings must
   be quiet and stable (a median filter plus variance check over a 20-sample
   window) before classification into one of four orientations (normal, left,
   right, inverted) is allowed; afterwards a debounce counter prevents rapid
   flickering, and a large Z jump suppresses classification for one sample.
   The final orientation is fed to iio-sensor-proxy's standard callback, which
   exposes it over D-Bus for desktop auto-rotation.

   Outside tablet mode the classification result is replaced with `right-up`.
   The MiniBook X panel is mounted in portrait, so a compositor consuming
   orientation events (e.g. via `iio-niri`) applies a 270° rotation in laptop
   mode and follows the accelerometer once the lid folds past the tablet
   threshold. Static panel rotation can also cover the boot splash and login
   screen; see [GUIDE-ADVANCED.md](../GUIDE-ADVANCED.md#display-rotation)
   and the iio-niri exception under [Known issues](#known-issues).

   Folding back to laptop mode emits the landscape orientation directly, instead
   of waiting for the accelerometer pipeline to settle. Compositors that only
   rotate when a reading arrives would stay in the last tablet rotation
   otherwise.

   If a static rotation *is* already applied, the driver reads the DRM
   `panel orientation` property of the DSI connector at startup and subtracts it
   from every reported orientation, so the dynamic and static rotations do not
   stack. The detected orientation is logged at startup (`grep 'panel
   orientation'` in the journal).

## Known issues

**Niri / iio-niri:** with kernel panel rotation enabled, Niri reports a
transform that combines the panel orientation with the applied transform.
iio-niri compares this value with the requested transform and can incorrectly
skip an update. Until your iio-niri includes the fix proposed in
[PR #14](https://github.com/Zhaith-Izaliel/iio-niri/pull/14), omit
`video=DSI-1:panel_orientation=right_side_up` from Limine's kernel command
line (or the equivalent setting in another bootloader). The proxy's panel
compensation does not need to be disabled.

Per https://github.com/fstanis/chuwi-minibook/issues/10#issuecomment-5690525031
there's [a known issue](https://gitlab.gnome.org/GNOME/mutter/-/issues/4650) in
[Mutter](https://mutter.gnome.org) which applies the wrong rotation when
returning from tablet mode back into laptop mode.
