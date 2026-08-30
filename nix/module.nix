{
  config,
  lib,
  pkgs,
  defaultPackage,
  ...
}:

let
  cfg = config.services.keysharp-desktop;
in
{
  options.services.keysharp-desktop = {
    enable = lib.mkEnableOption "Keysharp desktop integration broker";

    package = lib.mkOption {
      type = lib.types.package;
      default = defaultPackage;
      defaultText = lib.literalExpression
        "inputs.keysharp-desktop.packages.${pkgs.stdenv.hostPlatform.system}.default";
      description = "keysharp-desktop package to install and supervise.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
    security.polkit.enable = true;

    systemd.tmpfiles.rules = [
      "d /var/lib/keysharp-permissions 0700 root root - -"
      "d /var/lib/keysharp-permissions/v1 0700 root root - -"
      "d /run/keysharp-permissions 0755 root root - -"
    ];

    systemd.sockets.keysharp-desktop-authority = {
      description = "Keysharp desktop authorization socket";
      wantedBy = [ "sockets.target" ];
      socketConfig = {
        ListenStream = "/run/keysharp-desktop/authority.sock";
        SocketMode = "0666";
        DirectoryMode = "0755";
        RemoveOnStop = true;
      };
    };

    systemd.services.keysharp-desktop-authority = {
      description = "Keysharp desktop authorization authority";
      requires = [ "keysharp-desktop-authority.socket" ];
      serviceConfig = {
        Type = "simple";
        ExecStart = "${cfg.package}/bin/keysharp-desktop authority";
        User = "root";
        Group = "root";
        UMask = "0077";
        NoNewPrivileges = true;
        PrivateTmp = true;
        ProtectSystem = "strict";
        ProtectHome = "read-only";
        ReadWritePaths = [
          "/var/lib/keysharp-permissions"
          "/run/keysharp-desktop"
          "/run/keysharp-permissions"
        ];
        RestrictAddressFamilies = [
          "AF_UNIX"
          "AF_NETLINK"
          "AF_ALG"
        ];
        LockPersonality = true;
      };
    };

    systemd.user.sockets.keysharp-desktop = {
      description = "Keysharp desktop broker socket";
      wantedBy = [ "sockets.target" ];
      socketConfig = {
        ListenStream = "%t/keysharp-desktop/keysharp-desktop.sock";
        SocketMode = "0600";
        DirectoryMode = "0700";
        RemoveOnStop = true;
      };
    };

    systemd.user.services.keysharp-desktop = {
      description = "Keysharp desktop broker";
      requires = [ "keysharp-desktop.socket" ];
      after = [ "graphical-session.target" ];
      serviceConfig = {
        Type = "simple";
        ExecStart = "${cfg.package}/bin/keysharp-desktop serve";
        StandardInput = "null";
        StandardOutput = "journal";
        StandardError = "journal";
        NoNewPrivileges = true;
        PrivateTmp = true;
        ProtectSystem = "strict";
        ProtectHome = "read-only";
        RestrictAddressFamilies = [ "AF_UNIX" ];
        LockPersonality = true;
      };
    };
  };
}
