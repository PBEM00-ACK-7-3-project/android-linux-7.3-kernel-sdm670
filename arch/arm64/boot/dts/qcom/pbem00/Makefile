# SPDX-License-Identifier: GPL-2.0-only
#
# PBEM00 (OPPO R17, SDM670 + PM660/PM660A) device tree sources.
#
# Verbatim port of OPPO android_kernel_oppo_sdm710 arch/arm64/boot/dts/18081
# (fc8c287470ca plus its fstab-system and timer interrupt-parent fixes). CLO
# dt-bindings headers are vendored under dtsinclude/ and put first on
# DTC_INCLUDE: 5 share filenames with mainline headers of different contents,
# 12 do not exist upstream, so shadowing must be strict.
#
# Artifacts:
#   sdm670-oppo-pbem00-soc.dtb       bare-SoC base; the boot blob. ABL matches
#                                    the partition stock dtbo (by prjversion)
#                                    and applies it onto this base at boot.
#   sdm670-oppo-pbem00-overlay.dtbo  board overlay (= stock dtbo entry 0
#                                    lineage); provenance/verification only,
#                                    the device's dtbo partition stays stock.
#
# No build-time merge on purpose: the base+overlay merge is ABL's job at
# boot. -@ is therefore pinned explicitly on both (the automatic base-dtb-y
# derivation only fires for composed dtbs): the base needs __symbols__ for
# ABL's overlay application, and this tree's dtc emits fixups but not
# symbols for /plugin/ unless asked.

dtb-$(CONFIG_ARCH_QCOM) += sdm670-oppo-pbem00-soc.dtb
always-y += sdm670-oppo-pbem00-overlay.dtbo

# CLO dt-bindings capsule wins over the mainline ones
DTC_INCLUDE := $(srctree)/$(src)/dtsinclude $(DTC_INCLUDE)

DTC_FLAGS += -Wno-graph_port
DTC_FLAGS_sdm670-oppo-pbem00-soc += -@
DTC_FLAGS_sdm670-oppo-pbem00-overlay += -@
