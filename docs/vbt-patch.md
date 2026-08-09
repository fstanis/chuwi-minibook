# VBT patcher

The MiniBook X's built-in display is a 1200x1920 DSI panel. The BIOS configures
it at 50 Hz by default, which makes scrolling and animations feel noticeably
sluggish. The VBT patcher changes the panel's refresh rate by modifying the
Video BIOS Table (VBT) -- the firmware data that tells the i915 graphics driver
how to drive the panel.

The patched VBT is embedded in the initramfs and loaded at early boot via a
kernel parameter, so the change takes effect before the display is initialized.
No permanent firmware modification is involved -- removing the kernel parameter
restores the original 50 Hz behavior.

The tool source lives in `vbt_patch/`. The `update-vbt-clock` script lives in
`tools/`.

## Quick start

Build and install the tool:

```
cd vbt_patch
make
sudo make install
```

Patch to 90 Hz (see "What rate to use" if this causes artifacts):

```
sudo update-vbt-clock 90
```

This extracts the current VBT from the running kernel, patches the pixel clock
for the requested rate, installs it into the initramfs, adds the
`i915.vbt_firmware=vbt` kernel parameter, rebuilds the initramfs, and tells you
to reboot.

To revert back to 50 Hz:

```
sudo update-vbt-clock --revert
```

## Verify

After rebooting, check the active mode:

```
cat /sys/kernel/debug/dri/0000:00:02.0/i915_display_info | grep DSI -A2
```

Look for the refresh rate in the mode string. At 90 Hz it reads something like:

```
"1200x1920": 90 246394 1200 1240 1252 1268 1920 2125 2127 2159 ...
```

You can also use `xrandr` or your desktop's display settings -- the panel should
show the new refresh rate.

## What rate to use

> **Treat the refresh-rate patch as an experiment, not a default.** Panels vary
> between units, and a rate can run cleanly for hours from a cold boot and still
> fail on the first suspend/resume, taking a revert and a reboot to clear and
> leaving temporary image retention behind. Suspend-test before relying on it,
> and see "What rate to use" below. This is why the patch is not part of
> `bootstrap-ubuntu.sh`.

The stock rate is 50 Hz. Try 90 Hz first -- it is a noticeable improvement in
smoothness and works on most MiniBook X units:

```
sudo update-vbt-clock 90
```

Not all units ship with the exact same panel, so 90 Hz may not work on yours. If
you see flickering, color banding, or horizontal lines after rebooting, **first
check the desktop is actually running at the patched rate**. The panel has a
single timing descriptor, so patching replaces the old rate rather than adding a
new one: a saved display configuration (e.g. GNOME's `~/.config/monitors.xml`)
that still pins the pre-patch rate now references a mode that no longer exists,
and the compositor silently falls back to a driver-synthesised mode. The panel
is then programmed for one rate and scanned out at another, which produces
exactly these artifacts. Immediately after patching, any saved config is
guaranteed stale, so this is the more likely cause. Select the new rate in
display settings and the artifacts should clear.

Only if the artifacts persist while running at the patched rate does the panel
not support it -- revert and try a lower value:

```
sudo update-vbt-clock --revert
```

**Test a suspend/resume cycle before considering a rate proven.** A cold boot is
not a sufficient test: the DSI link is brought up from scratch at boot but is
re-trained on resume, so a panel that is marginal at the requested clock can run
for hours from cold and then fail the moment the lid is reopened. The symptom is
distinctive and much more severe than the mismatch artifacts above: roughly half
the scanlines missing entirely, with horizontal light-trail smearing on the
rest.

Two things make that failure mode easy to misread. The driver logs nothing at
all -- no FIFO underruns, no link errors, no atomic commit failures -- so the
kernel believes it is driving the panel correctly. And because it is not the
stale-config problem above, the mode really is the patched one, so the first
check passes and points the wrong way. Forcing a fresh modeset does not clear
it; only reverting does.

Prolonged operation in that state can leave image retention on the panel,
visible as faint marks against black. On an LCD this is normally temporary and
fades over hours or days.

If the display does not come up at all after a reboot, the kernel falls back to
the BIOS VBT (50 Hz) automatically when the firmware file is missing or corrupt.
Revert from a TTY or recovery boot with `sudo update-vbt-clock --revert`.

______________________________________________________________________

## How it works

The i915 driver reads a VBT blob at initialization time to learn panel timings,
DSI lane configuration, power sequencing delays and rotation. Normally this
comes from the BIOS (opregion), but i915 supports loading an override from a
firmware file via the `i915.vbt_firmware` kernel parameter.

The `vbt_patch` tool parses the VBT binary, locates the Generic DTD (Block 58)
for the active panel, and modifies the pixel clock field. The refresh rate is
determined by:

```
refresh = pixel_clock / (htotal * vtotal)
```

For this panel, htotal = 1268 and vtotal = 2159 (fixed by the panel's blanking
intervals). Changing the pixel clock from ~137 MHz (50 Hz) to ~246 MHz (90 Hz)
changes the refresh rate without altering resolution or blanking.

The tool recalculates the VBT checksum after patching.

### vbt_patch usage

With no options, prints the current VBT state:

```
vbt_patch /sys/kernel/debug/dri/0000:00:02.0/i915_vbt
```

To patch and write a new VBT:

```
vbt_patch <input> [options] <output>
```

Options:

| Flag               | What it does                                            |
| ------------------ | ------------------------------------------------------- |
| `--hz <rate>`      | Set refresh rate (calculates pixel clock automatically) |
| `--clock <kHz>`    | Set pixel clock directly                                |
| `--rotation <0-3>` | MIPI panel rotation (0=0, 1=90, 2=180, 3=270)           |
| `--panel-on <ms>`  | Panel power-on delay                                    |
| `--panel-off <ms>` | Panel power-off delay                                   |
| `--bl-on <ms>`     | Backlight enable delay                                  |
| `--bl-off <ms>`    | Backlight disable delay                                 |
| `--cycle <ms>`     | Power cycle delay                                       |

The rotation and power sequence options modify MIPI config (Block 52). They are
not needed for a refresh rate change but are available if you need to adjust
panel timing parameters.

### update-vbt-clock

The `update-vbt-clock` script automates the full workflow:

1. Reads the current VBT from `/sys/kernel/debug/dri/0000:00:02.0/i915_vbt`
1. Runs `vbt_patch --hz <rate>` to produce a patched copy
1. Installs the patched VBT to `/lib/firmware/vbt`
1. Adds `/lib/firmware/vbt` to `mkinitcpio.conf` so it is included in the
   initramfs
1. Adds `i915.vbt_firmware=vbt` to the kernel command line
1. Rebuilds the initramfs

`--revert` removes the kernel parameter. The patched VBT file remains in
`/lib/firmware/vbt` but is ignored without the kernel parameter.

The script currently supports Limine (`/etc/default/limine`) as the bootloader
and mkinitcpio for initramfs generation.
