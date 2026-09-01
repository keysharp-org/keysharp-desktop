{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  glib,
  polkit,
  keysharpPermissions,
}:

stdenv.mkDerivation {
  pname = "keysharp-desktop";
  version = "0.2.0";

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
    "-DKEYSHARP_PERMISSIONS_SOURCE_DIR=${keysharpPermissions}"
    "-DKEYSHARP_DESKTOP_SYSTEMD_SYSTEM_DIR=lib/systemd/system"
    "-DKEYSHARP_DESKTOP_SYSTEMD_USER_DIR=lib/systemd/user"
    "-DKEYSHARP_DESKTOP_CAPTURE_WORKER_PATH=/run/keysharp-desktop/keysharp-desktop-capture-worker"
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
