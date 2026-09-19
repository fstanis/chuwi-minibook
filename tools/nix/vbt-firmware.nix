{
  pkgs,
  refreshRate,
  rotation,
  sourceVbt,
  vbtPatch,
}:
let
  inherit (pkgs) lib;
  patchArguments =
    lib.optionals (refreshRate != null) [
      "--hz"
      (toString refreshRate)
    ]
    ++ lib.optionals (rotation != null) [
      "--rotation"
      (toString rotation)
    ];
  version = lib.concatStringsSep "-" (
    lib.optional (refreshRate != null) "${toString refreshRate}hz"
    ++ lib.optional (rotation != null) "rotation${toString rotation}"
  );
in
assert patchArguments != [ ];
pkgs.stdenvNoCC.mkDerivation {
  pname = "chuwi-minibook-vbt";
  inherit version;
  dontUnpack = true;
  nativeBuildInputs = [ vbtPatch ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib/firmware
    vbt_patch \
      ${sourceVbt} \
      ${lib.escapeShellArgs patchArguments} \
      $out/lib/firmware/vbt
    runHook postInstall
  '';

  meta = {
    description = "Configured CHUWI MiniBook VBT firmware";
    platforms = pkgs.lib.platforms.linux;
  };
}
