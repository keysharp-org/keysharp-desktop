#!/bin/sh
set -eu

build_dir=$1
source_dir=$2
install_prefix=$3
install_libdir=$4
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT HUP INT TERM

DESTDIR=$stage cmake --install "$build_dir"
installed_prefix=$stage$install_prefix
installed_libdir=$installed_prefix/$install_libdir

PKG_CONFIG_PATH=$installed_libdir/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=$stage \
cmake -S "$source_dir/tests/install_tree_consumer" \
    -B "$stage/consumer" \
    -DCMAKE_PREFIX_PATH="$installed_prefix"
cmake --build "$stage/consumer"

LD_LIBRARY_PATH=$installed_libdir "$stage/consumer/cmake-consumer"
LD_LIBRARY_PATH=$installed_libdir "$stage/consumer/pkg-config-consumer"
