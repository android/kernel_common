#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2020, Google LLC. All rights reserved.
# Author: Saravana Kannan <saravanak@google.com>

# Find top of git repo
while [ `pwd` != '/' -a ! -d .git ]
do
	cd ..
done

# Exit if you can't find it
if [ ! -d .git ]
then
	exit 1
fi

if [ ! -f ./scripts/get_maintainer.pl ]
then
	exit 2
fi

opt='--no-rolestats --no-git-fallback --multiline --n'
if [ "$1" = '-cc' ]
then
	shift
	opt="$opt --no-m --no-r --l"
	echo kernel-team@android.com
else
	opt="$opt --m --r --no-l"
fi

# Special case for git send-email that only passes one patch at a time
if [ $# -eq 1 ]
then
	prefix="$1"
	# Intentionally stripping out only the last 2 digits in the patch
	# number
	prefix=$(echo $1 | sed -e "s/[0-9][0-9]-.*\.patch/*.patch/")
	set "$prefix"
fi

./scripts/get_maintainer.pl $opt $* 2>/dev/null
