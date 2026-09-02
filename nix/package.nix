{
  lib,
  stdenv,
  cmake,
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
    "-DKEYSHARP_DESKTOP_SETUP_ON_INSTALL=OFF"
    # The cmake hook points each install directory at an absolute $out path, which makes
    # CMake bake that prefix into the exported targets instead of deriving it from where
    # the package config is found. The layout is unchanged, but the export stays
    # relocatable, which is what a consumer reading a staged install tree needs.
    "-DCMAKE_INSTALL_BINDIR=bin"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_INSTALL_INCLUDEDIR=include"
    "-DCMAKE_INSTALL_LIBEXECDIR=libexec"
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
