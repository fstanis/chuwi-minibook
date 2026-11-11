# Advanced Guide

Everything the bootstrap scripts do, done by hand, plus GPU setup, display
rotation internals and sleep modes. Start with [GUIDE.md](GUIDE.md) unless you
know you need this.

## Status scripts

Three diagnostic scripts in `tools/`. Each ends with a warnings section that
collects everything that needs fixing.

**`sudo tools/check-status.sh`** — general system and component status: DMI and
BIOS info, kernel cmdline, VBT refresh rate, build prerequisites, DKMS module
state, thermald and iio-sensor-proxy services.

**`sudo tools/dptf-status.sh`** — DPTF and thermals: dptf_enabler and the
int340x driver stack, DPTF participants and thermal zones, RAPL power limits,
and the BIOS settings thermald depends on (CFG Lock, TCC offset).

**`tools/gpu-status.sh`** (as your normal user, not root) — Vulkan, VA-API and
OpenCL state and which video codecs have hardware decode/encode. See
[GPU and Vulkan](#gpu-and-vulkan).

______________________________________________________________________

## Manual install

What follows replicates the bootstrap scripts step by step, in dependency
order.

Examples assume **Limine + mkinitcpio** (the CachyOS default). On GRUB-based
systems, edit `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub` instead of
`/etc/default/limine`, and rebuild with
`sudo update-grub && sudo update-initramfs -u` (Debian/Ubuntu) or
`sudo grub2-mkconfig -o /boot/grub2/grub.cfg && sudo dracut -f` (Fedora)
instead of `sudo limine-mkinitcpio`.

Both bootstrap scripts take an optional stage argument (`modules`, `sensor`,
`cmdline`) to run just one part.

### 1. Kernel modules

Four DKMS modules, in this order:

| Module                   | What it does                                                  |
| ------------------------ | ------------------------------------------------------------- |
| `dptf_enabler`           | Unhides BIOS-gated Intel DPTF devices; required by thermald   |
| `minibook_ec`            | EC driver: thermal sensors, fan, keyboard backlight, toggles  |
| `i2c_designware_spklen`  | I2C spike suppression; prevents touchscreen/sensor bus errors |
| `goodix_ts`              | Touchscreen resume fix and OEM config loading                 |

Each installs the same way:

```
cd modules/<name>
sudo make install && sudo make enable
```

Verify with `dmesg | grep <name>`. See [minibook-ec.md](docs/minibook-ec.md)
for the EC driver's sysfs interface.

### 2. BIOS tweaks

See [GUIDE.md](GUIDE.md#2-bios-tweaks). Required before thermald is useful.

### 3. thermald

See [GUIDE.md](GUIDE.md#3-thermald) for install and
[thermald.md](docs/thermald.md) for patch details and tunables.

### 4. iio-sensor-proxy

Screen rotation and tablet mode via the dual accelerometers.

```
cd iio-sensor-proxy
make && sudo make install
sudo systemctl restart iio-sensor-proxy
```

On Arch and Manjaro, use `make install-arch` instead — it builds via `makepkg`
so pacman tracks the install. Add `IgnorePkg = iio-sensor-proxy` to
`/etc/pacman.conf` so upgrades don't replace it with the unpatched repo build.

On Debian and Ubuntu, `sudo apt remove iio-sensor-proxy` first; on Fedora,
`sudo dnf remove iio-sensor-proxy` — so the distro package doesn't shadow the
fork.

Verify: run `monitor-sensor` and tilt the device. See
[iio-sensor-proxy.md](docs/iio-sensor-proxy.md) for how it works and which
desktops need a bridge daemon.

### 5. Kernel command line

Add to the kernel command line in `/etc/default/limine`:

```
mem_sleep_default=deep i915.enable_psr=0
```

Then rebuild with `sudo limine-mkinitcpio`. Add the optional
`video=DSI-1:panel_orientation=right_side_up` only if you want a static
rotation (see [Display rotation](#display-rotation)). What each does:

| Argument                                      | Purpose                                     |
| --------------------------------------------- | ------------------------------------------- |
| `video=DSI-1:panel_orientation=right_side_up` | Optional: rotate the panel; see [Display rotation](#display-rotation) |
| `mem_sleep_default=deep`                      | Default to S3 sleep; see [Sleep mode](#sleep-mode-s0ix-vs-s3) |
| `i915.enable_psr=0`                           | Fix DSI link tearing; see [DSI link tearing](#dsi-link-tearing) |

### 6. Console rotation

Text consoles do not follow `panel_orientation`. Rotate them at boot with a
tmpfiles entry:

```
echo 'w /sys/class/graphics/fbcon/rotate_all - - - - 1' \
  | sudo tee /etc/tmpfiles.d/fbcon-rotate.conf
```

Console rotation does not compose additively with the panel orientation, so if
1 comes out wrong, try the other values (0-3) rather than deriving one.

### 7. VBT patcher (display refresh rate)

Deliberately not part of the bootstrap scripts: a raised pixel clock can pass
a cold boot and still fail on the first suspend. See
[GUIDE.md](GUIDE.md#optional-higher-refresh-rate) for the quick start and
[vbt-patch.md](docs/vbt-patch.md) for the full tool reference.

______________________________________________________________________

## GPU and Vulkan

The Intel N150 has UHD Graphics (Gen12.2, Alder Lake-N). Run
`tools/gpu-status.sh` to see what is working.

### Required packages

On Arch/CachyOS (names differ on other distros, e.g. `mesa-vulkan-drivers` on
Fedora/Ubuntu):

| Package                 | What it provides                                |
| ----------------------- | ----------------------------------------------- |
| `mesa`                  | OpenGL and Vulkan (ANV) drivers for Intel       |
| `vulkan-intel`          | Intel ANV Vulkan ICD (may be bundled with mesa) |
| `intel-media-driver`    | VA-API hardware video acceleration (iHD driver) |
| `intel-compute-runtime` | OpenCL support (NEO runtime)                    |
| `vulkan-tools`          | `vulkaninfo` for `gpu-status.sh`                |
| `libva-utils`           | `vainfo` for `gpu-status.sh`                    |
| `clinfo`                | `clinfo` for `gpu-status.sh`                    |

### Enable Vulkan video decode and encode

ANV supports hardware video decode and encode (H.264, H.265, AV1, VP9) behind
a feature flag. Add to `/etc/environment`:

```
ANV_DEBUG=video-decode,video-encode
```

Log out and back in, then re-run `tools/gpu-status.sh` to confirm the codecs
appear in the vulkan section.

______________________________________________________________________

## Display rotation

The MiniBook X has a portrait-mode 1200x1920 DSI panel mounted in landscape
orientation, so everything needs a 270° rotation.

If your compositor consumes iio-sensor-proxy orientation events (see
[iio-sensor-proxy.md](docs/iio-sensor-proxy.md#desktop-integration)), the
desktop needs no configuration: the patched proxy reports `right-up` in laptop
mode and live accelerometer rotation in tablet mode. The static methods below
cover what a compositor cannot reach — the boot splash, TTYs and the LUKS
passphrase prompt — or replace the proxy entirely.

Static and dynamic rotation do not stack: the proxy reads the DRM
`panel orientation` property at startup and subtracts any static rotation from
what it reports. `tools/check-status.sh` shows the applied static rotation on
its `panel rotation` line, and the proxy logs its decision at startup
(`journalctl -u iio-sensor-proxy | grep 'panel orientation'`).

Bridges that read the screen's transform back (iio-niri) still skip some
rotations, so prefer no static rotation at all.

### Kernel command line

```
video=DSI-1:panel_orientation=right_side_up
```

The i915 driver applies a hardware rotation, so the boot splash, TTYs, LUKS
prompt and login screen all come up upright. The bootstrap scripts add it
only with `--panel-rotation`. Rebuild the initramfs after editing.

The parser accepts `normal`, `upside_down`, `left_side_up` and
`right_side_up`. It compares only as many characters as you supply (a bare
`right` also works — don't rely on it), and an unrecognised value silently
discards the **entire** `video=` option. Confirm with
`dmesg | grep panel_orientation`: success logs
`cmdline forces connector DSI-1 panel_orientation to 3`.

**On an encrypted root, use this method even if your compositor handles
rotation** — the passphrase prompt is drawn from the initramfs, long before
any compositor exists.

### Text consoles

`panel_orientation` does not rotate TTYs. Use the tmpfiles entry from
[§6](#6-console-rotation) above, or `fbcon=rotate:N` on the kernel command
line (which takes precedence over anything the console would inherit). Both
need `CONFIG_FRAMEBUFFER_CONSOLE_ROTATION=y`; without it the writes are
accepted and ignored, so check that and `/proc/cmdline` if nothing happens.

### Bootloader menu

Limine can rotate its own boot menu: add `interface_rotation: 90` to
`/boot/limine.conf`. Stock GRUB has no equivalent (checked against GRUB 2.14
— no rotation module or option exists), so on GRUB the menu stays sideways;
`GRUB_TIMEOUT=0` hides it on a normal boot anyway.

### VBT patch

`vbt_patch --rotation 3` sets the MIPI panel rotation in the Video BIOS Table,
making i915 treat the panel as already rotated. Firmware-level, embedded in
the initramfs, combinable with a refresh rate patch in one invocation. See
[vbt-patch.md](docs/vbt-patch.md).

### Xrandr

For X11 sessions: `xrandr --output DSI-1 --rotate right`. Runtime-only, does
not persist and does not affect boot splash, TTYs or the login screen.

## DSI link tearing

Panel Self Refresh (PSR) can cause DSI link tearing — the screen partially
fills with green and horizontal lines. `i915.enable_psr=0` on the kernel
command line fixes it (the bootstrap scripts add it).

______________________________________________________________________

## Sleep mode (S0ix vs S3)

The MiniBook X supports both s2idle (S0ix, software sleep) and deep (S3,
suspend-to-RAM). **Use deep**: S0ix does not reach its low-power residency
states reliably on this machine and drains the battery noticeably overnight.

Check the active mode (shown in brackets):

```
cat /sys/power/mem_sleep
```

Switch at runtime with `echo deep | sudo tee /sys/power/mem_sleep`. To make it
permanent, add `mem_sleep_default=deep` to the kernel command line (the
bootstrap scripts do).
