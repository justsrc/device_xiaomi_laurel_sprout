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
# deliberate: core/Makefile:2282 writes the bare "erofs_pcluster_size" key, which
# build_image.py picks up via common_props and applies to every erofs partition.
# The per-partition spelling would be BOARD_SYSTEMIMAGE_EROFS_PCLUSTER_SIZE.
#
# erofs-utils defaults the pcluster to one 4096-byte block. 131072 is 32 blocks,
# inside the 1 MiB Z_EROFS_PCLUSTER_MAX_SIZE ceiling enforced by the kernel at
# decompressor.c; on this 4.14 CAF tree the big-pcluster backport is present
# (fs/erofs/pcpubuf.c, erofs_pcpubuf_growsize honours max_pclusterblks), and the
# kernel logs "EXPERIMENTAL big pcluster feature in use" for any value >= 2 blocks
# - it already did at 65536, and it is informational, not a mount failure.
#
# Measured on this device, not from an external benchmark: pcluster 4096 -> 65536
# took system.img from 693163 to 619719 blocks (-73444, -10.6%) on identical
# content. The erofs-utils published figure is -18.2%, but that comes from a
# Debian-rootfs corpus and every row there also enables -Efragments; system.img is
# 44% .apk and 12% .so by bytes, already deflated, so LZ4HC has far less to find.
#
# Going 65536 -> 131072 buys roughly 0.8 further points (~27 MiB at the 765922-block
# Full GApps size), which is why it is worth doing: 305467 fits with only ~56.7 MiB
# of true headroom after the AVB hashtree. Extrapolated, not yet built.
#
# Cost: a 4 KiB random read now decompresses up to 32 KiB x 32 = 128 KiB, versus
# 16x at 65536. Decompression throughput on the A55 cores degrades monotonically
# with pcluster size. pcpubuf holds max_pclusterblks x nr_possible_cpus per mounted
# instance, never released.
#
# Scoped to this device rather than sm6125-common: read latency is per-SoC and this
# is a size-constrained device, not a general tuning decision.
# Revert this line to return to the 4096 default (693163 blocks at Pico / 2.644 GiB).
BOARD_EROFS_PCLUSTER_SIZE := 131072

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/etc/fstab.qcom

# Security patch level - V12.0.26.0.RFQMIXM
BOOT_SECURITY_PATCH := 2022-08-01
VENDOR_SECURITY_PATCH := $(BOOT_SECURITY_PATCH)

# Inherit from the proprietary version
include vendor/xiaomi/laurel_sprout/BoardConfigVendor.mk
