# minibook_ec kernel module

The `minibook_ec` module gives Linux access to hardware in the MiniBook X that
the stock kernel cannot reach. Once loaded, it provides:

- **Touchpad and keyboard toggles** -- enable or disable the touchpad and
  keyboard from the command line, useful for tablet mode scripts.
- **Two thermal sensors** -- a fast-reacting SoC temperature sensor and a
  charger temperature sensor, both visible as standard thermal zones. The
  patched [thermald](thermald.md) uses these to manage CPU power limits.
- **Fan speed monitoring** -- read current fan RPM and duty cycle. The fan is
  controlled by the EC firmware; this is read-only telemetry.
- **Keyboard backlight control** -- set brightness (0-255) and choose whether
  the backlight turns off automatically after inactivity. Desktop environments
  (GNOME, KDE) pick this up automatically.
- **EC version** -- read the embedded controller firmware version.
- **BIOS unlock** -- expose hidden BIOS menus on the next reboot into setup
  (one-shot, cleared after use).

Most of these integrate with standard Linux subsystems, so you rarely need to
access them directly: thermal sensors show up in `sensors` (`lm-sensors`), the
keyboard backlight works through your desktop environment's brightness controls
and fan RPM appears in any hwmon monitoring tool. The sysfs paths documented
below are there if you need them for scripts or debugging.

The module source lives in `modules/minibook_ec/`.

## Install

```
cd modules/minibook_ec
sudo make install
sudo make enable
```

This builds via DKMS against the running kernel, installs the module, and loads
it on boot. To remove:

```
sudo make disable
sudo make uninstall
```

## Verify

```
dmesg | grep minibook_ec
```

A successful load looks like:

```
minibook_ec minibook_ec: touchpad enabled, keyboard enabled, SoC registered, charger registered, fan registered, kbd_bl registered
```

Components that fail to initialize print a warning but do not prevent the rest
of the module from loading.

## Quick reference

### Touchpad and keyboard

```
# read current state
cat /sys/devices/platform/minibook_ec/touchpad_enabled
cat /sys/devices/platform/minibook_ec/keyboard_enabled

# disable touchpad (e.g., when using the device as a tablet)
echo 0 | sudo tee /sys/devices/platform/minibook_ec/touchpad_enabled

# re-enable
echo 1 | sudo tee /sys/devices/platform/minibook_ec/touchpad_enabled
```

`keyboard_enabled` works the same way. Setting it to `0` puts the device in
slate mode (keyboard off), `1` restores laptop mode.

### Keyboard backlight

```
# set brightness (0 = off, 255 = max)
echo 128 | sudo tee /sys/class/leds/minibook::kbd_backlight/brightness

# keep backlight on permanently (disable auto-off timer)
echo 0 | sudo tee /sys/class/leds/minibook::kbd_backlight/auto_off_timer

# re-enable auto-off (backlight turns off after ~30s of inactivity)
echo 1 | sudo tee /sys/class/leds/minibook::kbd_backlight/auto_off_timer
```

### Thermal sensors

```
# SoC temperature (millidegrees C, divide by 1000 for degrees)
cat /sys/class/thermal/thermal_zone*/temp   # find minibook_soc

# or more specifically:
grep -l minibook_soc /sys/class/thermal/thermal_zone*/type
```

### Fan speed

```
cat /sys/class/hwmon/hwmon*/fan1_input   # RPM (find minibook_fan)
cat /sys/class/hwmon/hwmon*/pwm1         # duty cycle, 0-255
```

### BIOS unlock

```
echo 1 | sudo tee /sys/devices/platform/minibook_ec/bios_unlock
```

Reboot into BIOS setup (press `DEL` during POST) to access hidden menus. The
flag is cleared after one use.

### EC version

```
cat /sys/devices/platform/minibook_ec/ec_version
```

______________________________________________________________________

## Technical details

### Thermal zones

Two thermal zones appear under `/sys/class/thermal/`:

| Zone               | Source                | What it measures                      |
| ------------------ | --------------------- | ------------------------------------- |
| `minibook_soc`     | EC register 0x70      | Temperature near the N150 SoC package |
| `minibook_charger` | IT5570E ADC channel 0 | Temperature near the charger IC       |

Both report temperature in millidegrees C via the standard `thermal_zone*/temp`
sysfs interface.

`minibook_soc` reads a single EC byte (register 0x70) that the firmware updates
continuously. It is not one of the documented DPTF sensor registers (TSR1-5) --
it is an undocumented register that appears to track a thermistor physically
adjacent to the SoC. It responds faster to load changes than the CPU die sensor
(TCPU/coretemp), making it useful as an early warning for thermal management.

`minibook_charger` reads the IT5570E's built-in ADC. The raw 10-bit ADC value is
converted to degrees C using a linear approximation calibrated against the NTC
thermistor on this board. It only warms significantly when the charger is active
under load.

Both zones require the I2EC interface to be available (charger) or the ACPI EC
to be responsive (SoC). If either fails its probe-time sanity check, that zone
is silently skipped.

The patched thermald uses these zones to install passive trip points. See
[thermald.md](thermald.md) for details.

### Fan monitoring (hwmon)

A read-only hwmon device (`minibook_fan`) appears under `/sys/class/hwmon/`:

| Attribute    | What it reads                                              |
| ------------ | ---------------------------------------------------------- |
| `fan1_input` | Fan RPM, derived from the IT5570E tachometer counter       |
| `pwm1`       | Fan PWM duty cycle (0-255), read from IT5570E DCR register |

The fan is controlled autonomously by the EC firmware -- there is no writable
speed control from the OS. The EC uses its own internal thermal curve. These
attributes are read-only telemetry.

RPM is calculated from the tachometer period: `RPM = 1875000 / tach_count`. A
tach count of zero or 0xFFFF means the fan is stopped or unreadable, reported as
0 RPM.

### Keyboard backlight (LED class)

A standard Linux LED device appears at
`/sys/class/leds/minibook::kbd_backlight/`:

| Attribute        | Type               | What it controls                                         |
| ---------------- | ------------------ | -------------------------------------------------------- |
| `brightness`     | read/write, 0-255  | PWM duty cycle on the keyboard backlight LED             |
| `max_brightness` | read-only          | Always 255                                               |
| `auto_off_timer` | read/write, 0 or 1 | EC auto-off timer (turns off backlight after inactivity) |

Setting `brightness` writes the PWM duty to the IT5570E hardware register and
updates the EC firmware's setpoint so the two stay in sync.

`auto_off_timer = 1` (default) lets the EC firmware turn off the backlight after
~30 seconds of keyboard inactivity. Keyboard activity re-arms the timer and
restores the last brightness level.

`auto_off_timer = 0` keeps the backlight on indefinitely regardless of keyboard
activity. This matches the BIOS setting "Keyboard Backlight Mode = Always on."

#### Tracking EC-initiated changes (opt-in)

Enable the `poll_hw_changed` module parameter to have GNOME/KDE/etc. track
brightness changes triggered by the dedicated backlight toggle key or the
auto-off timer:

```
echo 'options minibook_ec poll_hw_changed=1' \
  | sudo tee /etc/modprobe.d/minibook_ec.conf
```

This adds a `brightness_hw_changed` attribute under the LED node. UPower
watches it via `poll(2)`/`POLLPRI` and re-broadcasts changes over D-Bus, which
is what desktop environments listen to for "the keyboard backlight changed."
Off by default because it requires a 250 ms in-kernel poll of the EC.

### Sysfs attributes

Four attributes appear under the platform device
(`/sys/devices/platform/minibook_ec/`):

| Attribute          | Type       | What it does                                                                                                    |
| ------------------ | ---------- | --------------------------------------------------------------------------------------------------------------- |
| `touchpad_enabled` | read/write | `1` = touchpad on, `0` = touchpad off. Writes EC register 0x0C.                                                 |
| `keyboard_enabled` | read/write | `1` = laptop mode (keyboard on), `0` = slate mode (keyboard off). Calls ACPI method `\_SB.ACMK.LTSM`.           |
| `ec_version`       | read-only  | EC firmware version string (e.g., `1.7 kb=2 tp=3`).                                                             |
| `bios_unlock`      | write-only | Write `1` to set the BIOS unlock flag (EC register 0xF0 = 0xAA). Reboot into BIOS setup to access hidden menus. |

`touchpad_enabled` and `keyboard_enabled` take effect immediately. They are
useful for tablet mode scripts that need to disable input when the screen is
flipped.

`bios_unlock` is a one-shot flag. Writing `1` tells the EC to expose hidden BIOS
menus on the next boot into setup. The flag is cleared by the BIOS after use.

### I2EC interface

All features except `touchpad_enabled`, `keyboard_enabled`, `ec_version`,
`bios_unlock` and `minibook_soc` require the I2EC (Indirect EC) bus, which
provides host access to the IT5570E's full 16-bit internal address space via
PNPCFG Depth-2 I/O at port 0x4E/0x4F.

If another driver has claimed the 0x4E-0x4F I/O region, I2EC initialization
fails and the charger thermal zone, fan hwmon and keyboard backlight are
unavailable. The SoC thermal zone and sysfs attributes still work because they
use the standard ACPI EC interface (ports 0x62/0x66).
