{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  glib,
  polkit,
}:

stdenv.mkDerivation {
  pname = "keysharp-desktop";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];
  buildInputs = [ glib ];

  cmakeFlags = [
    "-DBUILD_TESTING=ON"
    "-DKSD_PKCHECK_PATH=${polkit}/bin/pkcheck"
    "-DKEYSHARP_DESKTOP_SYSTEMD_SYSTEM_DIR=lib/systemd/system"
    "-DKEYSHARP_DESKTOP_SYSTEMD_USER_DIR=lib/systemd/user"
  ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';

  meta = {
    description = "Desktop integration and per-application authorization broker for Linux";
    homepage = "https://github.com/keysharp-org/keysharp-desktop";
    license = lib.licenses.mit;
    mainProgram = "keysharp-desktop";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
