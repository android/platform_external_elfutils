#!/bin/sh

# $1 Path to the new version.
# $2 Path to the old version.

set -x
set -e

# if called from the external_updater, change to the new dir first
if [ "$1" != "" ]; then
    cd $1
fi

autoreconf -i && ./configure --enable-maintainer-mode --disable-debuginfod --disable-libdebuginfod --without-lzma --without-bzlib --without-zstd

# if called from the external_updater, do not apply any patches as it will do
# that for us
if [ "$2" == "" ]; then
    git apply patches/*
fi
