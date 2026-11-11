# Installation Guide

Three steps: run the bootstrap script, flip two BIOS settings, install
thermald. Reboot when done.

This page covers the standard setup. For manual per-component installation,
GPU setup, display rotation internals and sleep modes, see
[GUIDE-ADVANCED.md](GUIDE-ADVANCED.md).

## 1. Run the bootstrap script

On Ubuntu or Debian:

```
sudo tools/bootstrap-ubuntu.sh
```

On Arch, CachyOS or Manjaro (with `sudo` from your normal user account, not
from a root shell — it builds a package, which refuses to run as root):

```
sudo tools/bootstrap-arch.sh
```

The script installs build dependencies, the four DKMS kernel modules
(touchscreen fix, EC driver, DPTF enabler, I2C fix), the patched
iio-sensor-proxy for auto-rotation and tablet mode, and the kernel command
line arguments for deep sleep and the PSR screen-tearing fix. Add
`--panel-rotation` to also rotate the boot splash and LUKS prompt (see
[GUIDE-ADVANCED.md](GUIDE-ADVANCED.md#display-rotation)).

It is idempotent — safe to re-run at any time. If it stops with an error
saying the running kernel is no longer installed, a package upgrade pulled in
a new kernel: reboot and run it again.

## 2. BIOS tweaks

thermald needs two hidden BIOS settings changed to control CPU power limits.

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

See [thermald.md](docs/thermald.md#bios-settings) for what these do.

## 3. thermald

Patched thermal daemon. Requires steps 1 and 2.

```
cd thermal_daemon
./configure && make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now thermald
```

On Arch and Manjaro, use `make install-arch` instead — it builds via `makepkg`
so pacman tracks the install. Add `IgnorePkg = thermald` to `/etc/pacman.conf`
so upgrades don't replace it with the unpatched repo build.

On Debian and Ubuntu, `sudo apt remove thermald` first; on Fedora,
`sudo dnf remove thermald` — so the distro package doesn't shadow the fork.

Verify: `journalctl -u thermald | grep minibook`. See
[thermald.md](docs/thermald.md) for details.

## 4. Reboot and verify

Reboot, then run:

```
sudo tools/check-status.sh
```

The warnings section at the end lists anything still missing. Two more status
scripts dig deeper — see
[GUIDE-ADVANCED.md](GUIDE-ADVANCED.md#status-scripts).

## Optional: higher refresh rate

The panel runs at 50 Hz stock. To try 90 Hz:

```
cd vbt_patch && make && cd ..
sudo tools/update-vbt-clock.sh 90
```

**Treat this as an experiment, not a default.** Panels vary between units, and
a rate can survive a cold boot yet fail on the first suspend/resume. Test a
suspend cycle before relying on it, and revert if the display misbehaves:

```
sudo tools/update-vbt-clock.sh --revert
```

See [vbt-patch.md](docs/vbt-patch.md) for choosing a rate and telling the two
failure modes apart.

## Optional: screen auto-rotation on other desktops

GNOME and KDE Plasma pick up rotation and tablet mode automatically. On Niri,
Sway, Hyprland and other wlroots compositors you need a small bridge daemon —
see [iio-sensor-proxy.md](docs/iio-sensor-proxy.md#desktop-integration).
