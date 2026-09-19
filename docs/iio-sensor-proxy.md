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
  and add to one of your Niri config files (e.g.
  `~/.config/niri/cfg/autostart.kdl`):
  ```
  spawn-at-startup "iio-niri" "listen" "--monitor" "DSI-1"
  ```
- **Sway / wlroots compositors**: use
  [`iio-sway`](https://github.com/okeri/iio-sway) (works on Sway, river,
  Wayfire) or an equivalent bridge for your compositor.
- **Hyprland**: use
  [`iio-hyprland`](https://github.com/JeanSchoeller/iio-hyprland).

The patched proxy reports `right-up` whenever the device is in laptop mode, so
the compositor applies the 270° portrait correction dynamically and switches
to live accelerometer rotation in tablet mode. A static rotation (kernel
cmdline, VBT patch, xrandr) does not stack with this — the proxy detects and
subtracts it, see
[GUIDE-ADVANCED.md](../GUIDE-ADVANCED.md#display-rotation).

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

By default, the base accelerometer's raw reading supplies screen orientation.
To use the display accelerometer instead, add this to the same systemd override:

```
[Service]
Environment=MINIBOOK_ORIENTATION_SENSOR=display
```

Valid values are `base` and `display`. Restart `iio-sensor-proxy` after changing
the setting. Selection does not change sensor discovery or the calibrated
readings used for hinge calculation. The selected raw reading has its X axis
reversed before entering the existing orientation filter.

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

   Outside tablet mode the classification result is replaced with `right-up`.
   The MiniBook X panel is mounted in portrait, so a compositor consuming
   orientation events (e.g. via `iio-niri`) applies a 270° rotation in laptop
   mode and follows the accelerometer once the lid folds past the tablet
   threshold. This removes the need for a separate static rotation fix (kernel
   cmdline, VBT patch, xrandr); see
   [GUIDE-ADVANCED.md](../GUIDE-ADVANCED.md#display-rotation).

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

Per https://github.com/fstanis/chuwi-minibook/issues/10#issuecomment-5690525031
there's [a known issue](https://gitlab.gnome.org/GNOME/mutter/-/issues/4650) in
[Mutter](https://mutter.gnome.org) which applies the wrong rotation when
returning from tablet mode back into laptop mode.
