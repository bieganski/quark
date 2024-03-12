#!/bin/bash

set -eux

CC_URL='https://developer.arm.com/-/media/Files/downloads/gnu-a/8.2-2018.08/gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabi.tar.xz?revision=421a06dd-24c4-4ffb-bf4c-b73411b28680&ln=en&hash=A2642B2DC2083AB57E25DBE88B8A18FEEF5397F1'

CC_TAR_XZ=compiler.tar.xz
CC_TAR=compiler.tar
CC_DIR=compiler

if ! [ -f "$CC_TAR_XZ" ]; then
    wget $CC_URL -O $CC_TAR_XZ
else
    echo skipping download
fi

if ! [ -f "$CC_TAR" ]; then
    7z e $CC_TAR_XZ -so > $CC_TAR
else
    echo skipping extract
fi

if ! [ -d "$CC_DIR" ]; then
    mkdir -p $CC_DIR ; tar xf $CC_TAR -C $CC_DIR --strip-components=1
else
    echo skipping extract2
fi
