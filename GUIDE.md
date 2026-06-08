# Installation Guide

## Check current status

Three diagnostic scripts in `tools/` cover different areas. Each produces a
warnings section at the end that collects everything that needs fixing.

### check-status.sh

General system and component status. Runs without root for most checks; root is
only needed for the VBT section (debugfs).

```
sudo tools/check-status.sh
```

| Section            | What it checks                                                                  |
| ------------------ | ------------------------------------------------------------------------------- |
| **device**         | DMI vendor/product, CPU model, microcode, BIOS version, DSI display, sleep mode |
| **kernel cmdline** | `i915.vbt_firmware` (custom VBT), `i915.enable_psr=0` (PSR fix)                 |
| **vbt**            | Panel refresh rate from VBT Block 58 (needs `intel_vbt_decode` and sudo)        |
| **prerequisites**  | Build tools: dkms, clang, curl, patch, meson, ninja, kernel headers             |
| **modules**        | DKMS install state, loaded state, and boot config for each kernel module        |
| **services**       | thermald and iio-sensor-proxy version, enabled/running state                    |

### dptf-status.sh

DPTF participant status and BIOS settings. Requires root (reads MSRs).

```
sudo tools/dptf-status.sh
```

| Section           | What it checks                                                                                                                                      |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| **modules**       | dptf_enabler loaded/params, int3400_thermal, int340x_thermal_zone, intel_rapl_common                                                                |
| **dptf manager**  | IETM presence via platform driver, data_vault, active policy UUID                                                                                   |
| **participants**  | Each DPTF device (TCPU, SEN1-5, DGPU, TFN1-3, CHRG, TPWR, TPCH, BAT1): ACPI status and platform driver binding                                      |
| **thermal zones** | Temps and trip points for zones thermald monitors (B0D4/TCPU, SEN3, minibook_soc, minibook_charger); shows `[thermald]` if under user_space control |
| **rapl**          | PL1 from MMIO and MSR powercap, PPCC range from processor thermal PCI device                                                                        |
| **bios settings** | CFG Lock (MSR 0xE2), RAPL PL1 writability, TCC Activation Offset (MSR 0x1A2)                                                                        |

### gpu-status.sh

GPU and media acceleration. Must be run as your normal user (not root).

```
tools/gpu-status.sh
```

See [GPU and Vulkan](#gpu-and-vulkan) below for what it checks and how to set up
GPU support.

______________________________________________________________________

## Install

Install components in this order to satisfy dependencies.

Examples below assume **Limine + mkinitcpio** (the CachyOS default). On
GRUB-based systems, edit `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`
instead of `/etc/default/limine`, and rebuild with
`sudo update-grub && sudo update-initramfs -u` (Debian/Ubuntu) or
`sudo grub2-mkconfig -o /boot/grub2/grub.cfg && sudo dracut -f` (Fedora) instead
of `sudo limine-mkinitcpio`.

### 1. dptf_enabler

Unhides BIOS-gated Intel DPTF devices. Required by thermald.

```
cd modules/dptf_enabler
sudo make install && sudo make enable
```

### 2. minibook_ec

EC platform driver for thermal sensors, fan monitoring, keyboard backlight and
input toggles. Required by thermald for its SoC and charger thermal zones.

```
cd modules/minibook_ec
sudo make install && sudo make enable
```

Verify: `dmesg | grep minibook_ec`. See [minibook-ec.md](docs/minibook-ec.md)
for sysfs interface documentation.

### 3. i2c_designware_spklen

I2C spike suppression fix. Prevents occasional touchscreen and sensor bus
errors.

```
cd modules/i2c_designware_spklen
sudo make install && sudo make enable
```

### 4. goodix_ts

Touchscreen resume fix and OEM config loading.

```
cd modules/goodix_ts
sudo make install && sudo make enable
```

### 5. BIOS tweaks

1. Unlock the hidden BIOS menus:
   `echo 1 | sudo tee /sys/devices/platform/minibook_ec/bios_unlock`
1. Reboot into BIOS setup (press DEL during POST).
1. Go to the `Advanced` tab.
1. Navigate to
   `Power & Performance -> CPU - Power Management Control -> CPU Lock Configuration`
1. Change `CFG Lock` to `Disabled`
1. Go back to the top level of the `Advanced` tab.
1. Navigate to `Thermal Configuration -> CPU Thermal Configuration`
1. Change `Tcc Activation Offset` to `10`

These are needed for thermald to control CPU power limits. See
[thermald.md](docs/thermald.md#bios-tweaks) for details.

### 6. thermald

Patched thermal daemon. Requires steps 1-2 and 5 above.

```
cd thermal_daemon
./configure && make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now thermald
```

On Arch and Manjaro, use `make install-arch` instead - it builds via `makepkg`
so pacman tracks the install. Add `IgnorePkg = thermald` to `/etc/pacman.conf`
so upgrades don't replace it with the unpatched repo build.

On Debian and Ubuntu, `sudo apt remove thermald` first so the distro package
doesn't shadow the fork.

On Fedora, `sudo dnf remove thermald` first for the same reason.

Verify: `journalctl -u thermald | grep minibook`. See
[thermald.md](docs/thermald.md) for patch details and tunable parameters.

### 7. iio-sensor-proxy

Screen rotation and tablet mode via dual accelerometers.

```
cd iio-sensor-proxy
make && sudo make install
sudo systemctl restart iio-sensor-proxy
```

On Arch and Manjaro, use `make install-arch` instead - it builds via `makepkg`
so pacman tracks the install. Add `IgnorePkg = iio-sensor-proxy` to
`/etc/pacman.conf` so upgrades don't replace it with the unpatched repo build.

On Debian and Ubuntu, `sudo apt remove iio-sensor-proxy` first so the distro
package doesn't shadow the fork.

On Fedora, `sudo dnf remove iio-sensor-proxy` first for the same reason.

The proxy exposes orientation on D-Bus (`net.hadess.SensorProxy`). How that
becomes a screen rotation depends on your desktop:

- **GNOME and KDE Plasma (Wayland)**: built-in. Enable auto-rotate in the
  quick-settings panel / System Settings. No extra daemon needed.
- **Niri**: install [`iio-niri`](https://github.com/Zhaith-Izaliel/iio-niri) and
  add to one of your Niri config files (e.g.
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
the compositor applies the 270° portrait correction dynamically and switches to
live accelerometer rotation in tablet mode. Do **not** combine this with a
static rotation (kernel cmdline, VBT patch, xrandr script) - they will stack.

Verify: `monitor-sensor` and tilt the device. See
[iio-sensor-proxy.md](docs/iio-sensor-proxy.md) for details.

### 8. VBT patcher (display refresh rate)

The stock DSI panel runs at 50 Hz. The VBT patcher changes the pixel clock to
increase the refresh rate. Build the tool first:

```
cd vbt_patch
make
```

Then use `update-vbt-clock.sh` to patch, install into the initramfs, and update
the kernel command line in one step:

```
sudo tools/update-vbt-clock.sh 90
```

This does the following:

1. Reads the current VBT from debugfs
1. Patches the pixel clock for the requested refresh rate
1. Installs the patched VBT to `/lib/firmware/vbt`
1. Adds the file to `mkinitcpio.conf` so it is included in the initramfs
1. Adds `i915.vbt_firmware=vbt` to the Limine kernel command line
1. Rebuilds the initramfs

Reboot to apply. If the display flickers or shows artifacts, your panel does not
support that rate - revert and try a lower value:

```
sudo tools/update-vbt-clock.sh --revert
```

See [vbt-patch.md](docs/vbt-patch.md) for the full tool reference and guidance
on choosing a refresh rate.

______________________________________________________________________

## GPU and Vulkan

The Intel N150 has UHD Graphics (Gen12.2, Alder Lake-N). Run
`tools/gpu-status.sh` (as your normal user, not root) to see what is working. It
checks Vulkan, VA-API, and OpenCL and lists which video codecs are available for
hardware decode and encode.

### Required packages

The exact package names vary by distro. On Arch/CachyOS:

| Package                 | What it provides                                |
| ----------------------- | ----------------------------------------------- |
| `mesa`                  | OpenGL and Vulkan (ANV) drivers for Intel       |
| `vulkan-intel`          | Intel ANV Vulkan ICD (may be bundled with mesa) |
| `intel-media-driver`    | VA-API hardware video acceleration (iHD driver) |
| `intel-compute-runtime` | OpenCL support (NEO runtime)                    |
| `vulkan-tools`          | `vulkaninfo` for `gpu-status.sh`                |
| `libva-utils`           | `vainfo` for `gpu-status.sh`                    |
| `clinfo`                | `clinfo` for `gpu-status.sh`                    |

On CachyOS most of these are installed by default. On other distros the package
names may differ (e.g. `mesa-vulkan-drivers` on Fedora/Ubuntu).

### Enable Vulkan video decode and encode

Intel's ANV driver supports hardware video decode and encode (H.264, H.265, AV1,
VP9) but these are behind a feature flag. Add to your environment (e.g.
`/etc/environment`):

```
ANV_DEBUG=video-decode,video-encode
```

Log out and back in for the change to take effect, then re-run
`tools/gpu-status.sh` to confirm the codecs appear in the vulkan section.

______________________________________________________________________

## DSI panel

The MiniBook X has a portrait-mode 1200x1920 MIPI DSI panel mounted in landscape
orientation. It needs rotation for normal use and has a known issue with DSI
link tearing.

### Display rotation

If your compositor consumes iio-sensor-proxy orientation events (see
[§7](#7-iio-sensor-proxy) for the per-desktop list), you do not need any of the
methods below. The patched proxy reports `right-up` in laptop mode so the
compositor applies the 270° rotation dynamically, and switches to live
accelerometer rotation in tablet mode. There is nothing to configure on the
kernel/firmware side.

Otherwise, pick one of the methods below for a fixed rotation.

#### Kernel command line

Add the `video=` parameter to the kernel command line in `/etc/default/limine`:

```
video=DSI-1:panel_orientation=right
```

This tells the i915 DRM driver to apply a hardware rotation, so the console
framebuffer and all desktop environments see the correct orientation from the
start -- including the boot splash, TTY consoles and login screen. After
editing, rebuild the initramfs with `sudo limine-mkinitcpio` and reboot.

#### Bootloader framebuffer

Limine can rotate its own framebuffer (boot menu, boot splash) independently of
the kernel. Add to `/boot/limine.conf`:

```
interface_rotation: 90
```

This only affects the Limine boot screen itself. You still need one of the other
methods for the kernel and desktop. After editing, rebuild with
`sudo limine-mkinitcpio`.

#### VBT patch

The `vbt_patch` tool can set the MIPI panel rotation in the Video BIOS Table.
This makes the i915 driver treat the panel as already rotated at the hardware
level:

```
cd vbt_patch
make
vbt_patch <input> --rotation 1 <output>
```

The rotation values are: 0 = 0 degrees, 1 = 90 degrees, 2 = 180 degrees, 3 = 270
degrees. This is a firmware-level change embedded in the initramfs (see
[VBT patcher](#8-vbt-patcher-display-refresh-rate) above). It can be combined
with a refresh rate patch in a single `vbt_patch` invocation.

#### Xrandr

For X11 sessions, `xrandr` can rotate the display at the compositor level:

```
xrandr --output DSI-1 --rotate right
```

This is a runtime-only change that does not persist across reboots unless added
to a startup script or xprofile. It does not affect the boot splash, TTY
consoles or login screen.

### DSI link tearing

Panel Self Refresh (PSR) can cause DSI link tearing on this panel - the screen
partially fills with green and horizontal lines. Disabling PSR with a kernel
parameter seems to fix it. Add to the kernel command line in
`/etc/default/limine`:

```
i915.enable_psr=0
```

Rebuild the initramfs with `sudo limine-mkinitcpio` and reboot.

______________________________________________________________________

## Sleep mode (S0ix vs S3)

The MiniBook X supports two suspend modes. Check which one is active with
`check-status.sh` or directly:

```
cat /sys/power/mem_sleep
```

The active mode is shown in brackets (e.g. `[s2idle]` or `[deep]`).

### S0ix (s2idle) - software sleep

Similar to how smartphones sleep: the CPU enters a low-power idle state but the
system does not fully power down. The hardware stays partially active, allowing
for faster wake times.

### S3 (deep) - hardware sleep

Traditional suspend-to-RAM. The system powers down everything except memory.

**Recommended.** On the MiniBook X, S0ix does not reach its low-power
residency states reliably and drains the battery noticeably overnight. S3
("deep") suspends to a true low-power state and, in testing on this device,
gives substantially better standby battery life. Switch to `deep` and make it
the default unless you have a specific reason to keep s2idle.

### Switching between modes

To switch to S3 (hardware sleep, recommended):

```
echo deep | sudo tee /sys/power/mem_sleep
```

To switch to S0ix (software sleep):

```
echo s2idle | sudo tee /sys/power/mem_sleep
```

These changes do not persist across reboots. To make `deep` the default
permanently, add `mem_sleep_default=deep` to the kernel command line in
`/etc/default/limine` and rebuild the initramfs with `sudo limine-mkinitcpio`
(use `mem_sleep_default=s2idle` if you ever need to go back).
