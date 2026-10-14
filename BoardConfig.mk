#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from sm6125-common
include device/xiaomi/sm6125-common/BoardConfigCommon.mk

DEVICE_PATH := device/xiaomi/laurel_sprout

# A/B
AB_OTA_PARTITIONS += \
    boot \
    dtbo \
    system \
    vbmeta \
    vendor

BOARD_USES_RECOVERY_AS_BOOT := true
TARGET_NO_RECOVERY := true

# Bootloader
TARGET_BOOTLOADER_BOARD_NAME := laurel_sprout

# Display
TARGET_SCREEN_DENSITY := 320

# Fingerprint
TARGET_USES_FOD_ZPOS := true

# Kernel
TARGET_KERNEL_CONFIG += vendor/laurel_sprout.config

# Partitions
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3221225472
BOARD_VENDORIMAGE_PARTITION_SIZE := 1073741824

# EROFS
# Compression pcluster size, passed to mkfs.erofs as -C. The unprefixed name is
# deliberate. core/Makefile:2282 writes the bare "erofs_pcluster_size" key and
# build_image.py picks it up through common_props, applying it to every erofs
# partition. The per-partition spelling would be
# BOARD_SYSTEMIMAGE_EROFS_PCLUSTER_SIZE.
#
# erofs-utils defaults the pcluster to one 4096-byte block. 262144 is 64 blocks,
# inside the 1 MiB Z_EROFS_PCLUSTER_MAX_SIZE ceiling enforced by the kernel at
# decompressor.c:48. This 4.14 CAF tree carries the big-pcluster backport, so it
# mounts. The kernel prints "EXPERIMENTAL big pcluster feature in use" for any
# value of 2 blocks or more. It has done so since 65536, and it is an info line,
# not a mount failure.
#
# Measured on this device, not taken from an external benchmark:
#   pcluster 4096    Pico  693163 blocks   2.644 GiB
#   pcluster 65536   Pico  619719 blocks   2.364 GiB   -287 MiB
#   pcluster 65536   Full  765922 blocks   2.922 GiB   +571 MiB
#   pcluster 131072  Full  759148 blocks   2.896 GiB    -26.5 MiB
#   pcluster 262144  Full  755867 blocks   2.883 GiB    -12.8 MiB
#
# The erofs-utils published figure is -18.2%, drawn from a Debian rootfs corpus
# where every row also enables -Efragments. system.img is 44% .apk and 12% .so
# by bytes, already deflated, so LZ4HC finds far less to work with. Expect about
# half that headline number here.
#
# 65536 -> 131072 measured -26.5 MiB on build 305579. 131072 -> 262144 measured
# -12.8 MiB on build 305668, so the curve is flat now. Google recommends 262144
# for erofs generally, and Motorola and OnePlus ship it.
#
# The cost is read amplification. A 4 KiB random read now decompresses up to
# 256 KiB, 64x, up from 32x at 131072. Decompression throughput on the A55 cores
# falls monotonically with pcluster size. pcpubuf holds max_pclusterblks x
# nr_possible_cpus per mounted instance and never releases it.
#
# Scoped to this device rather than sm6125-common, because read latency is
# per-SoC and this is a size-constrained device.
# Revert this line to return to the 4096 default, 693163 blocks at Pico.
BOARD_EROFS_PCLUSTER_SIZE := 262144

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/etc/fstab.qcom

# Security patch level - V12.0.26.0.RFQMIXM
BOOT_SECURITY_PATCH := 2022-08-01
VENDOR_SECURITY_PATCH := $(BOOT_SECURITY_PATCH)

# Inherit from the proprietary version
include vendor/xiaomi/laurel_sprout/BoardConfigVendor.mk
