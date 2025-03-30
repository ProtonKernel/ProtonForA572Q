#!/bin/bash

regen_defconfig() {
    echo "Generating defconfig for a52q"
    make ARCH=arm64 O=out vendor/pixelos-a52q_defconfig
    cp out/.config "arch/arm64/configs/vendor/pixelos-a52q_defconfig"
    echo "defconfig regenerated for a52q"
}

regen_defconfig
