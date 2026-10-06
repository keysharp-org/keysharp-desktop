#!/usr/bin/env bash
# Usage: rehearse.sh DSC SERIES [OUT_DIR]
#
# Builds a source package the way a Launchpad builder does - in a clean ubuntu:SERIES container with
# the build dependencies from the archive, without network, as an unprivileged user and with no
# usable HOME - then asks apt whether the result would install. Launchpad accepts each version once,
# so this is where a broken upload has to be caught. The binary packages land in OUT_DIR.
set -euo pipefail

dsc="$(realpath "${1:?source package .dsc}")"
series="${2:?Ubuntu series}"
out="$(mkdir -p "${3:-rehearsal-${series}}" && cd "${3:-rehearsal-${series}}" && pwd)"
source_dir="$(dirname "${dsc}")"
image="ppa-rehearsal-${series}-$$"
build="$(mktemp -d)"
container=""
cleanup() {
  if [[ -n "${container}" ]]; then
    docker rm -f "${container}" >/dev/null 2>&1 || true
  fi
  docker image rm -f "${image}" >/dev/null 2>&1 || true
  rm -rf -- "${build}"
}
trap cleanup EXIT

# Separate accounts reproduce sbuild's foreign-owned, group-writable /build.
container="$(docker create -v "${source_dir}:/src:ro" "ubuntu:${series}" bash -euc '
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -qq -y --no-install-recommends dpkg-dev >/dev/null
  DEBIAN_FRONTEND=noninteractive apt-get build-dep -qq -y "/src/$1" >/dev/null
  getent group 2002 >/dev/null || groupadd --gid 2002 ppa-sbuild
  getent passwd 2002 >/dev/null || useradd --uid 2002 --gid 2002 --no-create-home --home-dir /nonexistent ppa-sbuild
  getent passwd 2001 >/dev/null || useradd --uid 2001 --gid 2002 --no-create-home --home-dir /nonexistent ppa-builder
  install -d -o 2002 -g 2002 -m 2770 /build' \
  rehearse "$(basename "${dsc}")")"
docker start -a "${container}"
docker commit "${container}" "${image}" >/dev/null
docker rm "${container}" >/dev/null
container=""

# Only the disposable artifact directory is shared with the host.
chmod 0777 "${build}"
docker run --rm --network none --user 2001:2002 -e HOME=/nonexistent -e LC_ALL=C.UTF-8 \
  -e DEB_BUILD_OPTIONS="parallel=$(nproc)" -v "${source_dir}:/src:ro" -v "${build}:/out" -w /build \
  "${image}" bash -euc '
    umask 0002
    mkdir package
    dpkg-source -x "/src/$1" package/source
    cd package/source
    dpkg-buildpackage -b -us -uc
    install -m 0644 ../*.deb /out/' \
  rehearse "$(basename "${dsc}")"

cp "${build}"/*.deb "${out}/"
for deb in "${out}"/*.deb; do
  dpkg-deb --info "${deb}"
done
docker run --rm --network none -v "${out}:/debs:ro" "${image}" \
  bash -euc 'apt-get install --simulate /debs/*.deb >/dev/null && echo "apt can install the packages on '"${series}"'."'
