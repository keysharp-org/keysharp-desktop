{
  description = "Desktop integration and per-application authorization broker for Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/e5bdc4a41d4c072fe1e3787eaa0320a384741d44";
  inputs.keysharp-permissions = {
    url = "github:keysharp-org/keysharp-permissions/b8f31942dd2c286608d390634a9916bffce55ddf";
    flake = false;
  };

  outputs =
    { self, nixpkgs, keysharp-permissions }:
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
          keysharp-desktop = pkgs.callPackage ./nix/package.nix {
            keysharpPermissions = keysharp-permissions;
          };
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
