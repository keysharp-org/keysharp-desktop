{
  description = "Desktop integration and per-application authorization broker for Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/e5bdc4a41d4c072fe1e3787eaa0320a384741d44";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = function: nixpkgs.lib.genAttrs systems (system: function system);
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        rec {
          keysharp-desktop = pkgs.callPackage ./nix/package.nix { };
          default = keysharp-desktop;
        }
      );

      checks = forAllSystems (
        system: {
          package = self.packages.${system}.default;
        }
      );

      nixosModules = rec {
        default = keysharp-desktop;
        keysharp-desktop =
          { config, lib, pkgs, ... }:
          import ./nix/module.nix {
            inherit config lib pkgs;
            defaultPackage = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
          };
      };
    };
}
