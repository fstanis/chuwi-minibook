{ source }:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.hardware.chuwi-minibook;
  packages = import ./packages.nix {
    inherit pkgs source;
    linuxPackages = config.boot.kernelPackages;
  };
  generatedVbt =
    if cfg.vbt.source == null || (cfg.vbt.refreshRate == null && cfg.vbt.rotation == null) then
      null
    else
      import ./vbt-firmware.nix {
        inherit pkgs;
        inherit (cfg.vbt) refreshRate rotation;
        sourceVbt = cfg.vbt.source;
        vbtPatch = packages.vbtPatch;
      };
  vbtPackage = if cfg.vbt.package == null then generatedVbt else cfg.vbt.package;

  mkEnabledOption =
    description:
    lib.mkOption {
      type = lib.types.bool;
      default = true;
      inherit description;
    };

  boolToModuleParameter = value: if value then "1" else "0";
in
{
  options.hardware.chuwi-minibook = {
    enable = mkEnabledOption "Enable the CHUWI MiniBook X support stack.";

    goodix = {
      enable = mkEnabledOption "Enable the patched Goodix touchscreen driver and firmware.";
    };

    minibookEc.enable = mkEnabledOption "Enable the MiniBook embedded-controller driver.";

    dptfEnabler = {
      enable = mkEnabledOption "Enable the DPTF device-enabling driver.";
      enableFans = lib.mkEnableOption "extra DPTF fan participants";
      enableSensors = lib.mkEnableOption "extra DPTF sensor participants";
    };

    i2cDesignwareSpklen.enable = mkEnabledOption "Enable I2C DesignWare spike suppression.";

    sensorProxy = {
      enable = mkEnabledOption "Enable the MiniBook dual-accelerometer SensorProxy.";
      setConvertibleChassis = mkEnabledOption "Identify the MiniBook X as a convertible through udev.";
      panelOrientation = lib.mkOption {
        type = lib.types.enum [
          "auto"
          "normal"
          "upside_down"
          "left_side_up"
          "right_side_up"
        ];
        default = "auto";
        description = "Static panel orientation reported to the MiniBook sensor driver.";
      };
      laptopOrientation = lib.mkOption {
        type = lib.types.enum [
          "normal"
          "left-up"
          "bottom-up"
          "right-up"
        ];
        default = "right-up";
        description = "Orientation reported while the device is in laptop mode.";
      };
      orientationSensor = lib.mkOption {
        type = lib.types.enum [
          "base"
          "display"
        ];
        default = "base";
        description = "Physical accelerometer used for screen orientation.";
      };
    };

    thermald.enable = mkEnabledOption "Enable the MiniBook-patched thermald.";

    kernelPanelOrientation = {
      enable = mkEnabledOption "Set the built-in panel orientation through the kernel command line.";
      connector = lib.mkOption {
        type = lib.types.str;
        default = "DSI-1";
        description = "DRM connector receiving the panel_orientation kernel parameter.";
      };
      orientation = lib.mkOption {
        type = lib.types.enum [
          "normal"
          "upside_down"
          "left_side_up"
          "right_side_up"
        ];
        default = "right_side_up";
        description = "Panel orientation passed to the kernel.";
      };
    };

    vbt = {
      enable = lib.mkEnableOption "generation and loading of a configured VBT";
      source = lib.mkOption {
        type = lib.types.nullOr lib.types.path;
        default = null;
        example = lib.literalExpression "./firmware/source-vbt.bin";
        description = "Original VBT captured from this machine.";
      };
      refreshRate = lib.mkOption {
        type = lib.types.nullOr lib.types.ints.positive;
        default = null;
        example = 90;
        description = "Optional refresh rate passed to vbt_patch.";
      };
      rotation = lib.mkOption {
        type = lib.types.nullOr (lib.types.ints.between 0 3);
        default = null;
        example = 1;
        description = "Optional panel rotation passed to vbt_patch.";
      };
      package = lib.mkOption {
        type = lib.types.nullOr lib.types.package;
        default = null;
        description = "Optional prebuilt firmware package used instead of generating a VBT.";
      };
      installPatcher = mkEnabledOption "Install the VBT inspection and patch utility.";
    };

    tools.enable = mkEnabledOption "Install the repository diagnostic tools.";

    disablePanelSelfRefresh = mkEnabledOption "Disable i915 panel self refresh to reduce DSI corruption.";

    mutter.enable = lib.mkEnableOption "the logical-normal built-in panel transform fix for Mutter";
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = !cfg.vbt.enable || vbtPackage != null;
        message = ''
          hardware.chuwi-minibook.vbt.enable requires either vbt.package, or
          vbt.source plus at least one of vbt.refreshRate and vbt.rotation.
        '';
      }
    ];

    nixpkgs.overlays = lib.optionals cfg.mutter.enable [
      (_final: prev: {
        mutter = prev.mutter.overrideAttrs (oldAttrs: {
          patches = (oldAttrs.patches or [ ]) ++ [
            (pkgs.fetchpatch {
              url = "https://gitlab.gnome.org/GNOME/mutter/-/commit/ebcac7d67b35fb3cf8a9dd86a5efe35a741b90cb.patch";
              hash = "sha256-NpEWqVNNIBHYY/UNzsLC35EK6ZlQgTxhDm4QYsyvd0k=";
            })
          ];
        });
      })
    ];

    boot.kernelParams =
      lib.optionals cfg.disablePanelSelfRefresh [ "i915.enable_psr=0" ]
      ++ lib.optionals cfg.kernelPanelOrientation.enable [
        "video=${cfg.kernelPanelOrientation.connector}:panel_orientation=${cfg.kernelPanelOrientation.orientation}"
      ]
      ++ lib.optionals cfg.vbt.enable [ "i915.vbt_firmware=vbt" ];

    boot.initrd.extraFirmwarePaths = lib.optionals cfg.vbt.enable [ "vbt" ];

    boot.extraModprobeConfig = lib.concatStringsSep "\n" (
      lib.optionals cfg.sensorProxy.enable [
        "options intel-hid enable_sw_tablet_mode=1"
      ]
      ++ lib.optionals cfg.dptfEnabler.enable [
        "options dptf_enabler enable_fans=${boolToModuleParameter cfg.dptfEnabler.enableFans} enable_sensors=${boolToModuleParameter cfg.dptfEnabler.enableSensors}"
      ]
    );

    boot.extraModulePackages =
      lib.optionals cfg.dptfEnabler.enable [ packages.dptfEnabler ]
      ++ lib.optionals cfg.minibookEc.enable [ packages.minibookEc ]
      ++ lib.optionals cfg.i2cDesignwareSpklen.enable [ packages.i2cDesignwareSpklen ]
      ++ lib.optionals cfg.goodix.enable [ packages.goodixTs ]
      ++ lib.optionals cfg.sensorProxy.enable [ config.boot.kernelPackages.acpi_call ];

    boot.kernelModules =
      lib.optionals cfg.dptfEnabler.enable [ "dptf_enabler" ]
      ++ lib.optionals cfg.minibookEc.enable [ "minibook_ec" ]
      ++ lib.optionals cfg.i2cDesignwareSpklen.enable [ "i2c_designware_spklen" ]
      ++ lib.optionals cfg.goodix.enable [ "goodix_ts" ]
      ++ lib.optionals cfg.sensorProxy.enable [
        "i2c-dev"
        "uinput"
        "acpi_call"
      ];

    hardware.firmware =
      lib.optionals cfg.goodix.enable [ packages.goodixFirmware ]
      ++ lib.optional (cfg.vbt.enable && vbtPackage != null) vbtPackage;

    hardware.sensor.iio = lib.mkIf cfg.sensorProxy.enable {
      enable = true;
      package = packages.iioSensorProxy;
    };

    systemd.services.iio-sensor-proxy.environment = lib.mkIf cfg.sensorProxy.enable {
      MINIBOOK_PANEL_ORIENTATION = cfg.sensorProxy.panelOrientation;
      MINIBOOK_LAPTOP_ORIENTATION = cfg.sensorProxy.laptopOrientation;
      MINIBOOK_ORIENTATION_SENSOR = cfg.sensorProxy.orientationSensor;
    };

    services.udev.extraHwdb = lib.mkIf cfg.sensorProxy.setConvertibleChassis (
      lib.mkAfter ''
        dmi:bvn*:bvr*:bd*:svnCHUWI*:pnMiniBookX*:*
          CHASSIS_TYPE=31
      ''
    );

    services.thermald = lib.mkIf cfg.thermald.enable {
      enable = true;
      package = packages.thermald;
      ignoreCpuidCheck = true;
    };

    environment.systemPackages =
      lib.optionals cfg.tools.enable [
        packages.captureVbt
        packages.minibookTools
      ]
      ++ lib.optionals cfg.vbt.installPatcher [ packages.vbtPatch ];
  };
}
