# SPDX-License-Identifier: GPL-2.0 OR Apache-2.0
# Copyright (C) 2025 The Android Open Source Project

"""Re-exports of symbols for external usage regarding to lists of modules.

DO NOT ADD MORE SYMBOLS. Additional symbols for external usage should be added
to bazel/modules.bzl, NOT modules.bzl (this file) or bazel/modules_private.bzl.
"""

load(
    ":bazel/modules.bzl",
    _get_gki_modules_list = "get_gki_modules_list",
    _get_kunit_modules_list = "get_kunit_modules_list",
)

visibility("public")

def get_gki_modules_list(*args, **kwargs):
    # buildifier: disable=print
    print("""
WARNING: {this_modules_bzl} is deprecated. Load get_gki_modules_list() from {new_modules_bzl} instead.""".format(
        this_modules_bzl = str(Label(":modules.bzl")),
        new_modules_bzl = str(Label(":bazel/modules.bzl")),
    ))
    return _get_gki_modules_list(*args, **kwargs)

def get_kunit_modules_list(*args, **kwargs):
    # buildifier: disable=print
    print("""
WARNING: {this_modules_bzl} is deprecated. Load get_kunit_modules_list() from {new_modules_bzl} instead.""".format(
        this_modules_bzl = str(Label(":modules.bzl")),
        new_modules_bzl = str(Label(":bazel/modules.bzl")),
    ))
    return _get_kunit_modules_list(*args, **kwargs)
