{
  pkgs,
  source,
  linuxPackages ? pkgs.linuxPackages,
}:
let
  inherit (pkgs) lib stdenv stdenvNoCC;
  kernel = linuxPackages.kernel;

  mkKernelModule =
    {
      pname,
      src,
      moduleName ? pname,
      version ? "1.0",
    }:
    stdenv.mkDerivation {
      inherit pname src version;

      nativeBuildInputs = kernel.moduleBuildDependencies;
      hardeningDisable = [
        "pic"
        "format"
      ];
      dontConfigure = true;

      buildPhase = ''
        runHook preBuild
        make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build M=$PWD modules
        runHook postBuild
      '';

      installPhase = ''
        runHook preInstall
        install -D -m 0644 ${moduleName}.ko \
          $out/lib/modules/${kernel.modDirVersion}/extra/${moduleName}.ko
        runHook postInstall
      '';

      meta = {
        description = "${pname} kernel module for the CHUWI MiniBook X";
        license = lib.licenses.gpl2Plus;
        platforms = lib.platforms.linux;
      };
    };

  goodixSource = stdenvNoCC.mkDerivation {
    pname = "goodix-ts-source";
    version = "${kernel.version}-minibook1";
    dontUnpack = true;
    nativeBuildInputs = with pkgs; [
      gnutar
      patch
      xz
    ];

    installPhase = ''
      runHook preInstall
      mkdir -p $out
      tar -xf ${kernel.src} \
        --strip-components=4 \
        -C $out \
        linux-${kernel.version}/drivers/input/touchscreen/goodix.c \
        linux-${kernel.version}/drivers/input/touchscreen/goodix.h \
        linux-${kernel.version}/drivers/input/touchscreen/goodix_fwupload.c
      cp ${source}/modules/goodix_ts/Kbuild $out/
      cp ${source}/modules/goodix_ts/goodix_resume.patch $out/
      chmod u+w $out/goodix.c
      patch -d $out -p1 < $out/goodix_resume.patch
      runHook postInstall
    '';

    meta = {
      description = "Patched Goodix touchscreen source for kernel ${kernel.version}";
      license = lib.licenses.gpl2;
      platforms = lib.platforms.linux;
    };
  };

  goodixTs = stdenv.mkDerivation {
    pname = "goodix_ts";
    version = "${kernel.version}-minibook1";
    src = goodixSource;
    nativeBuildInputs = kernel.moduleBuildDependencies;
    hardeningDisable = [
      "pic"
      "format"
    ];
    dontConfigure = true;

    buildPhase = ''
      runHook preBuild
      make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build M=$PWD modules
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      install -D -m 0644 goodix_ts.ko \
        $out/lib/modules/${kernel.modDirVersion}/extra/goodix_ts.ko
      runHook postInstall
    '';

    meta = {
      description = "Patched Goodix touchscreen module for the CHUWI MiniBook X";
      license = lib.licenses.gpl2;
      platforms = lib.platforms.linux;
    };
  };

  goodixFirmware = stdenvNoCC.mkDerivation {
    pname = "goodix-9110-firmware";
    version = "minibook1";
    src = "${source}/modules/goodix_ts";
    dontBuild = true;

    installPhase = ''
      runHook preInstall
      install -D -m 0644 goodix_cfg.bin \
        $out/lib/firmware/goodix_9110_cfg.bin
      runHook postInstall
    '';

    meta = {
      description = "Goodix touchscreen firmware for the CHUWI MiniBook X";
      license = lib.licenses.unfreeRedistributableFirmware;
      platforms = lib.platforms.linux;
    };
  };

  vbtPatch = stdenv.mkDerivation {
    pname = "chuwi-vbt-patch";
    version = "1.0";
    src = "${source}/vbt_patch";
    dontConfigure = true;
    nativeBuildInputs = with pkgs; [
      clang
      gnutar
      xz
    ];

    buildPhase = ''
      runHook preBuild
      tar -xf ${kernel.src} \
        --strip-components=6 \
        linux-${kernel.version}/drivers/gpu/drm/i915/display/intel_vbt_defs.h \
        linux-${kernel.version}/drivers/gpu/drm/i915/display/intel_dsi_vbt_defs.h
      clang -Wall -Wextra -O2 -o vbt_patch vbt_patch.c
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      install -D -m 0755 vbt_patch $out/bin/vbt_patch
      runHook postInstall
    '';

    meta = {
      description = "CHUWI MiniBook VBT patch utility";
      license = lib.licenses.gpl2Plus;
      platforms = lib.platforms.linux;
    };
  };

  captureVbt = pkgs.writeShellApplication {
    name = "chuwi-minibook-capture-vbt";
    runtimeInputs = [ pkgs.coreutils ];
    text = ''
      if [[ $# -ne 1 ]]; then
        echo "Usage: chuwi-minibook-capture-vbt <output-file>" >&2
        exit 2
      fi

      output="$1"
      if [[ -e "$output" ]]; then
        echo "Refusing to overwrite existing VBT: $output" >&2
        exit 1
      fi

      for candidate in /sys/kernel/debug/dri/*/i915_vbt; do
        if [[ -r "$candidate" ]]; then
          install -D -m 0644 "$candidate" "$output"
          echo "Captured VBT from $candidate to $output"
          exit 0
        fi
      done

      echo "No readable i915_vbt found under /sys/kernel/debug/dri" >&2
      exit 1
    '';
    meta.description = "Capture the running machine's original Intel VBT";
  };

  minibookTools = stdenvNoCC.mkDerivation {
    pname = "chuwi-minibook-tools";
    version = "1.0";
    src = "${source}/tools";
    dontBuild = true;
    nativeBuildInputs = [ pkgs.gnused ];

    installPhase = ''
      runHook preInstall
      install -D -m 0755 check-status.sh $out/bin/chuwi-check-status
      install -D -m 0755 dptf-status.sh $out/bin/chuwi-dptf-status
      install -D -m 0755 gpu-status.sh $out/bin/chuwi-gpu-status
      install -D -m 0755 detect-hardware.sh $out/bin/chuwi-detect-hardware
      install -D -m 0755 update-vbt-clock.sh $out/bin/chuwi-update-vbt-clock
      substituteInPlace $out/bin/chuwi-update-vbt-clock \
        --replace-fail \
        'readonly VBT_TOOL="''${SCRIPT_DIR}/../vbt_patch/vbt_patch"' \
        'readonly VBT_TOOL="${vbtPatch}/bin/vbt_patch"'
      runHook postInstall
    '';

    meta = {
      description = "Diagnostic and maintenance tools for the CHUWI MiniBook X";
      license = lib.licenses.bsd0;
      platforms = lib.platforms.linux;
    };
  };
in
{
  dptfEnabler = mkKernelModule {
    pname = "dptf_enabler";
    src = "${source}/modules/dptf_enabler";
  };

  i2cDesignwareSpklen = mkKernelModule {
    pname = "i2c_designware_spklen";
    src = "${source}/modules/i2c_designware_spklen";
  };

  minibookEc = mkKernelModule {
    pname = "minibook_ec";
    src = "${source}/modules/minibook_ec";
  };

  inherit
    captureVbt
    goodixFirmware
    goodixTs
    minibookTools
    vbtPatch
    ;

  iioSensorProxy = pkgs.iio-sensor-proxy.overrideAttrs (oldAttrs: {
    version = "3.9.minibook1";
    src = "${source}/iio-sensor-proxy";
    buildInputs = (oldAttrs.buildInputs or [ ]) ++ [ pkgs.libdrm ];
  });

  thermald = pkgs.thermald.overrideAttrs (_oldAttrs: {
    version = "2.5.11.minibook1";
    src = "${source}/thermal_daemon";
    patches = [ ];
  });
}
