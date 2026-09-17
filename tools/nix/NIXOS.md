# NixOS

This flake provides a NixOS module for the CHUWI MiniBook X.

## Install

Add the input and module to your `flake.nix`:

```nix
{
  inputs.chuwi-minibook.url = "github:fstanis/chuwi-minibook";

  outputs =
    {
      nixpkgs,
      chuwi-minibook,
      ...
    }:
    {
      nixosConfigurations.minibook = nixpkgs.lib.nixosSystem {
        system = "x86_64-linux";
        modules = [
          chuwi-minibook.nixosModules.default
          ./configuration.nix
        ];
      };
    };
}
```

Replace `minibook` with your configuration name, then rebuild and reboot:

```sh
sudo nixos-rebuild boot --flake .#minibook
sudo reboot
```

Importing the module enables:

- touchscreen fixes
- MiniBook EC and DPTF support
- I2C error handling
- tablet mode and automatic screen rotation
- MiniBook thermald
- the Panel Self Refresh workaround
- MiniBook diagnostic tools

Kernel panel rotation, VBT changes and the Mutter patch are opt-in.

## Display rotation

The proxy reports laptop orientation automatically and follows the display
accelerometer in tablet mode. Leave static rotation disabled for rotation
bridges such as iio-niri; see [desktop integration](../../docs/iio-sensor-proxy.md#desktop-integration).

To opt into kernel panel rotation for the boot splash or encrypted-root prompt:

```nix
hardware.chuwi-minibook.kernelPanelOrientation = {
  enable = true;
  connector = "DSI-1";
  orientation = "right_side_up";
};
```

This rotates the built-in display through the kernel.

## Higher refresh rate

The stock display rate is 50 Hz. Higher rates are experimental. Test the chosen
rate after a cold boot and after suspend and resume.

First boot with `vbt.enable = false`. Capture the stock VBT:

```sh
sudo chuwi-minibook-capture-vbt \
  /path/to/your/nixos-config/firmware/source-vbt.bin
```

Add it to your Git flake:

```sh
git add firmware/source-vbt.bin
```

To use 60 Hz:

```nix
hardware.chuwi-minibook = {
  vbt = {
    enable = true;
    source = ./firmware/source-vbt.bin;
    refreshRate = 60;
  };
};
```

Change `refreshRate` to test another rate. Rebuild, reboot, and select the new
rate in your desktop display settings.

To return to the stock VBT:

```nix
hardware.chuwi-minibook.vbt.enable = false;
```

## VBT rotation

Leave VBT rotation unset when using dynamic rotation. To opt into static
VBT rotation instead of kernel panel rotation:

```nix
hardware.chuwi-minibook = {
  kernelPanelOrientation.enable = false;

  vbt = {
    enable = true;
    source = ./firmware/source-vbt.bin;
    refreshRate = 90;
    rotation = 1;
  };
};
```

`rotation = 1` is the correct MiniBook X value for `right_side_up`.

| VBT value | Panel orientation |
| --------- | ----------------- |
| `0`       | `normal`          |
| `1`       | `right_side_up`   |
| `2`       | `upside_down`     |
| `3`       | `left_side_up`    |

The values describe how the panel is mounted.

## GNOME and Mutter

To enable the optional Mutter tablet-exit rotation patch:

```nix
hardware.chuwi-minibook = {
  mutter.enable = true;
};
```

The patch addresses the display direction when leaving tablet mode.

The first build takes longer because it rebuilds Mutter.

## SensorProxy

The defaults are:

```nix
hardware.chuwi-minibook.sensorProxy = {
  enable = true;
  setConvertibleChassis = true;
};
```

`setConvertibleChassis` identifies the MiniBook X as a convertible.

SensorProxy requires `minibookEc.enable = true` for firmware calibration,
orientation sensor selection and keyboard toggling. The driver detects panel
orientation through DRM and selects the accelerometer through ACPI `_CRS`.
Laptop orientation is fixed to `right-up` before panel compensation.

## Components

Set a switch to `false` to disable that part. Disable SensorProxy as well
when disabling `minibookEc`.

| Setting | Default | Purpose |
| --- | --- | --- |
| `enable` | `true` | Complete MiniBook configuration |
| `goodix.enable` | `true` | Touchscreen driver and firmware |
| `minibookEc.enable` | `true` | Fan, thermal sensor, keyboard and touchpad EC support |
| `dptfEnabler.enable` | `true` | Intel DPTF devices used by thermald |
| `i2cDesignwareSpklen.enable` | `true` | I2C error workaround |
| `sensorProxy.enable` | `true` | Tablet mode and screen rotation |
| `sensorProxy.setConvertibleChassis` | `true` | Convertible chassis classification |
| `thermald.enable` | `true` | MiniBook thermald service |
| `kernelPanelOrientation.enable` | `false` | Kernel display rotation |
| `vbt.enable` | `false` | Custom VBT |
| `disablePanelSelfRefresh` | `true` | Display corruption workaround |
| `mutter.enable` | `false` | Mutter tablet-exit rotation fix |
| `tools.enable` | `true` | Status and VBT capture commands |

Extra DPTF participants:

```nix
hardware.chuwi-minibook.dptfEnabler = {
  enableFans = false;
  enableSensors = false;
};
```

## Check the result

After rebooting:

```sh
sudo chuwi-check-status
```

Check the display direction, touch input, tablet mode, automatic rotation,
thermald, and suspend/resume.
