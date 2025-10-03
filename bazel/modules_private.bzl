# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022 The Android Open Source Project

"""
This module contains a full list of kernel modules
 compiled by GKI.
"""

visibility("private")

_COMMON_GKI_MODULES_LIST = [
    # keep sorted
    "drivers/block/virtio_blk.ko",
    "drivers/block/zram/zram.ko",
    "drivers/bluetooth/btbcm.ko",
    "drivers/bluetooth/btqca.ko",
    "drivers/bluetooth/btsdio.ko",
    "drivers/bluetooth/hci_uart.ko",
    "drivers/char/virtio_console.ko",
    "drivers/gnss/gnss.ko",
    "drivers/misc/vcpu_stall_detector.ko",
    "drivers/net/can/dev/can-dev.ko",
    "drivers/net/can/slcan/slcan.ko",
    "drivers/net/can/vcan.ko",
    "drivers/net/macsec.ko",
    "drivers/net/mii.ko",
    "drivers/net/ppp/bsd_comp.ko",
    "drivers/net/ppp/ppp_deflate.ko",
    "drivers/net/ppp/ppp_generic.ko",
    "drivers/net/ppp/ppp_mppe.ko",
    "drivers/net/ppp/pppox.ko",
    "drivers/net/ppp/pptp.ko",
    "drivers/net/slip/slhc.ko",
    "drivers/net/usb/aqc111.ko",
    "drivers/net/usb/asix.ko",
    "drivers/net/usb/ax88179_178a.ko",
    "drivers/net/usb/cdc_eem.ko",
    "drivers/net/usb/cdc_ether.ko",
    "drivers/net/usb/cdc_ncm.ko",
    "drivers/net/usb/r8152.ko",
    "drivers/net/usb/r8153_ecm.ko",
    "drivers/net/usb/rtl8150.ko",
    "drivers/net/usb/usbnet.ko",
    "drivers/net/wwan/wwan.ko",
    "drivers/power/sequencing/pwrseq-core.ko",
    "drivers/pps/pps_core.ko",
    "drivers/ptp/ptp.ko",
    "drivers/thunderbolt/thunderbolt.ko",
    "drivers/usb/class/cdc-acm.ko",
    "drivers/usb/mon/usbmon.ko",
    "drivers/usb/serial/ftdi_sio.ko",
    "drivers/usb/serial/usbserial.ko",
    "drivers/virtio/virtio_balloon.ko",
    "drivers/virtio/virtio_pci.ko",
    "drivers/virtio/virtio_pci_legacy_dev.ko",
    "drivers/virtio/virtio_pci_modern_dev.ko",
    "fs/netfs/netfs.ko",
    "kernel/kheaders.ko",
    "lib/crc/crc-ccitt.ko",
    "lib/crypto/libarc4.ko",
    "mm/zsmalloc.ko",
    "net/6lowpan/6lowpan.ko",
    "net/6lowpan/nhc_dest.ko",
    "net/6lowpan/nhc_fragment.ko",
    "net/6lowpan/nhc_hop.ko",
    "net/6lowpan/nhc_ipv6.ko",
    "net/6lowpan/nhc_mobility.ko",
    "net/6lowpan/nhc_routing.ko",
    "net/6lowpan/nhc_udp.ko",
    "net/8021q/8021q.ko",
    "net/9p/9pnet.ko",
    "net/9p/9pnet_fd.ko",
    "net/bluetooth/bluetooth.ko",
    "net/bluetooth/hidp/hidp.ko",
    "net/bluetooth/rfcomm/rfcomm.ko",
    "net/can/can.ko",
    "net/can/can-bcm.ko",
    "net/can/can-gw.ko",
    "net/can/can-raw.ko",
    "net/ieee802154/6lowpan/ieee802154_6lowpan.ko",
    "net/ieee802154/ieee802154.ko",
    "net/ieee802154/ieee802154_socket.ko",
    "net/l2tp/l2tp_core.ko",
    "net/l2tp/l2tp_ppp.ko",
    "net/mac802154/mac802154.ko",
    "net/nfc/nfc.ko",
    "net/rfkill/rfkill.ko",
    "net/tipc/tipc.ko",
    "net/tipc/tipc_diag.ko",
    "net/tls/tls.ko",
    "net/vmw_vsock/vmw_vsock_virtio_transport.ko",
]

# Deprecated - Use `get_gki_modules_list` function instead.
COMMON_GKI_MODULES_LIST = _COMMON_GKI_MODULES_LIST

_ARM_GKI_MODULES_LIST = [
    # keep sorted
    "drivers/ptp/ptp_kvm.ko",
]

_ARM64_GKI_MODULES_LIST = [
    # keep sorted
    "drivers/char/hw_random/cctrng.ko",
    "drivers/misc/open-dice.ko",
    "drivers/ptp/ptp_kvm.ko",
]

_X86_GKI_MODULES_LIST = [
    # keep sorted
    "drivers/ptp/ptp_kvm.ko",
]

_X86_64_GKI_MODULES_LIST = [
    # keep sorted
    "drivers/ptp/ptp_kvm.ko",
]

def _apply(map_each, lst):
    if not map_each:
        return lst
    ret = []
    for elem in lst:
        mapped = map_each(elem)
        if mapped:
            ret.append(mapped)
    return ret

def _get_gki_modules_list_minus_select(arch, map_each):
    """ Provides the list of GKI modules, minus those in select() branches.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and returns
            the mapped value. If the module should be filtered out, the function
            should return None.

    Returns:
        The list of GKI modules for the given |arch|.
    """
    if not arch in ("arm64", "x86_64", "arm", "i386"):
        fail("{}: arch {} not supported. Use one of [arm, arm64, i386, x86_64]".format(
            str(native.package_relative_label(":x")).removesuffix(":x"),
            arch,
        ))

    if arch == "arm":
        return _apply(map_each, _COMMON_GKI_MODULES_LIST + _ARM_GKI_MODULES_LIST)

    if arch == "i386":
        return _apply(map_each, _COMMON_GKI_MODULES_LIST + _X86_GKI_MODULES_LIST)

    gki_modules_list = _apply(map_each, [] + _COMMON_GKI_MODULES_LIST)
    if arch == "arm64":
        gki_modules_list += _apply(map_each, _ARM64_GKI_MODULES_LIST)
    elif arch == "x86_64":
        gki_modules_list += _apply(map_each, _X86_64_GKI_MODULES_LIST)

    return gki_modules_list

# buildifier: disable=unnamed-macro
def get_gki_modules_list(arch = None, map_each = None):
    """Provides the list of GKI modules.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and
            returns the mapped value. If the module should be filtered out, the
            function should return None.

    Returns:
        An opaque expression that represents the list of GKI modules for the
        given |arch|. Do not treat the returned value as a list (e.g. use
        list comprehension); instead, use the |map_each| argument.
    """

    return select({
        "//conditions:default": _get_gki_modules_list_minus_select(arch, map_each),
    })

# buildifier: disable=unnamed-macro
def get_gki_modules_superset(arch = None, map_each = None):
    """Provides the list of superset of GKI modules.

    This includes all modules on each branch of the conditionals. For example,
    Rust modules may always be included regardless of the value of
    --kasan_sw_tags.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and
            returns the mapped value. If the module should be filtered out, the
            function should return None.

    Returns:
        A list that contains the superset of GKI modules for the given |arch|.
    """
    return _get_gki_modules_list_minus_select(arch, map_each)

_KUNIT_FRAMEWORK_MODULES = [
    "lib/kunit/kunit.ko",
]

# Modules defined by tools/testing/kunit/configs/android/kunit_defconfig
_KUNIT_COMMON_MODULES_LIST = [
    # keep sorted
    "drivers/android/tests/binder_alloc_kunit.ko",
    "drivers/base/regmap/regmap-kunit.ko",
    "drivers/base/regmap/regmap-ram.ko",
    "drivers/base/regmap/regmap-raw-ram.ko",
    "drivers/hid/hid-uclogic-test.ko",
    "drivers/iio/test/iio-test-format.ko",
    "drivers/input/tests/input_test.ko",
    "drivers/of/of_kunit_helpers.ko",
    "drivers/rtc/test_rtc_lib.ko",
    "fs/ext4/ext4-inode-test.ko",
    "fs/fat/fat_test.ko",
    "kernel/time/time_test.ko",
    "lib/kunit/kunit-example-test.ko",
    "lib/kunit/kunit-test.ko",
    "lib/kunit/platform-test.ko",
    # "mm/kfence/kfence_test.ko",
    "net/core/dev_addr_lists_test.ko",
    "sound/soc/soc-topology-test.ko",
    "sound/soc/soc-utils-test.ko",
]

# Modules defined by tools/testing/kunit/configs/android/kunit_clk_defconfig
_KUNIT_CLK_MODULES_LIST = [
    "drivers/clk/clk-gate_test.ko",
    "drivers/clk/clk-test.ko",
    "drivers/clk/clk_kunit_helpers.ko",
]

def _get_kunit_modules_list_minus_select(arch, map_each):
    """ Provides the list of KUnit modules, minus those in select() branches.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and returns
            the mapped value. If the module should be filtered out, the function
            should return None.
    Returns:
        The list of KUnit modules for the given |arch|.
    """
    if not arch in ("arm64", "x86_64", "arm", "i386"):
        fail("{}: arch {} not supported. Use one of [arm, arm64, i386, x86_64]".format(
            str(native.package_relative_label(":x")).removesuffix(":x"),
            arch,
        ))

    kunit_modules_list = _KUNIT_FRAMEWORK_MODULES + _KUNIT_COMMON_MODULES_LIST
    if arch == "arm":
        kunit_modules_list += _KUNIT_CLK_MODULES_LIST
    elif arch == "arm64":
        kunit_modules_list += _KUNIT_CLK_MODULES_LIST
    elif arch == "i386":
        kunit_modules_list.append("drivers/clk/clk_kunit_helpers.ko")
    elif arch == "x86_64":
        kunit_modules_list.append("drivers/clk/clk_kunit_helpers.ko")

    return _apply(map_each, kunit_modules_list)

# buildifier: disable=unnamed-macro
def get_kunit_modules_list(arch = None, map_each = None):
    """ Provides the list of KUnit modules.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and returns
            the mapped value. If the module should be filtered out, the function
            should return None.

    Returns:
        An opaque expression that represents the list of Kunit modules for the
        given |arch|. Do not treat the returned value as a list (e.g. use
        list comprehension); instead, use the |map_each| argument.
    """

    return select({
        "//conditions:default": _get_kunit_modules_list_minus_select(arch, map_each),
    })

# buildifier: disable=unnamed-macro
def get_kunit_modules_superset(arch = None, map_each = None):
    """Provides the list of superset of KUnit modules.

    This includes all modules on each branch of the conditionals.

    Args:
        arch: One of [arm, arm64, i386, x86_64].
        map_each: A function that takes the module name as parameter, and
            returns the mapped value. If the module should be filtered out, the
            function should return None.

    Returns:
        A list of superset of KUnit modules for the given |arch|.
    """
    return _get_kunit_modules_list_minus_select(arch, map_each)

_COMMON_UNPROTECTED_MODULES_LIST = []

# buildifier: disable=unused-variable
def get_gki_unprotected_modules_list(arch = None):
    return select({
        "//conditions:default": _COMMON_UNPROTECTED_MODULES_LIST,
    })

# buildifier: disable=unnamed-macro
def get_gki_kunit_modules(arch, page_size = None):
    """Returns the list of labels pointing to the GKI modules for KUnit.

    Args:
        arch: one of arm64, x86_64
        page_size: if arch is arm64, the page_size ("4k" or "16k")

    Returns:
        The list of labels pointing to the GKI modules for KUnit.
    """
    if arch == "arm64":
        if page_size == "16k":
            return get_kunit_modules_list(arch, map_each = lambda e: ":kernel_aarch64_16k/" + e)
        if page_size == "4k":
            return get_kunit_modules_list(arch, map_each = lambda e: ":kernel_aarch64/" + e)
    if arch == "x86_64":
        return get_kunit_modules_list(arch, map_each = lambda e: ":kernel_x86_64/" + e)

    fail("{}: arch {} (page_size {}) not supported. Use one of [arm64, x86_64]".format(
        str(native.package_relative_label(":x")).removesuffix(":x"),
        arch,
        page_size,
    ))
