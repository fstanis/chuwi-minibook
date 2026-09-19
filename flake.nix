{
  description = "NixOS support for the CHUWI MiniBook X";

  outputs = { self }: {
    nixosModules.default = import ./tools/nix/module.nix { source = self; };
  };
}
