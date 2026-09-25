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

    autoEnableExtension = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Enable the GNOME or Cinnamon extension once per user and desktop, preserving later user changes.";
    };

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
        ListenStream = "/run/keysharp-desktop/keysharp-desktop.sock";
        FileDescriptorName = "public";
        SocketMode = "0666";
        DirectoryMode = "0755";
        RemoveOnStop = true;
      };
    };

    systemd.services.keysharp-desktop-authority = {
      description = "Keysharp desktop authorization authority";
      requires = [ "keysharp-desktop-authority.socket" ];
      preStart = ''
        ${pkgs.coreutils}/bin/install -m 0700 \
          ${cfg.package}/libexec/keysharp-desktop-capture-worker \
          /run/keysharp-desktop/keysharp-desktop-capture-worker
      '';
      serviceConfig = {
        Type = "simple";
        ExecStart = "${cfg.package}/bin/keysharp-desktop authority-daemon";
        User = "root";
        Group = "root";
        UMask = "0077";
        NoNewPrivileges = true;
        AmbientCapabilities = [ "CAP_SETUID" ];
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

    environment.etc."xdg/autostart/keysharp-desktop-enable-extension.desktop" = lib.mkIf cfg.autoEnableExtension {
      text = ''
        [Desktop Entry]
        Type=Application
        Name=Keysharp desktop extension setup
        NoDisplay=true
        Exec=${pkgs.runtimeShell} ${./enable-extension.sh} ${cfg.package}/bin/keysharp-desktop ${pkgs.coreutils}/bin
      '';
    };

    systemd.user.services.keysharp-desktop = {
      description = "Keysharp desktop broker";
      after = [ "graphical-session.target" ];
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      serviceConfig = {
        Type = "simple";
        ExecStart = "${cfg.package}/bin/keysharp-desktop daemon";
        StandardInput = "null";
        StandardOutput = "journal";
        StandardError = "journal";
        NoNewPrivileges = true;
        # User-manager filesystem namespaces hide the root ownership that the
        # daemon authenticates on the system authority socket.
        PrivateTmp = false;
        ProtectSystem = false;
        ProtectHome = false;
        RestrictAddressFamilies = [ "AF_UNIX" ];
        LockPersonality = true;
        Restart = "on-failure";
        RestartSec = "2s";
      };
    };
  };
}
