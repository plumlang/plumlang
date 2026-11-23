#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Print Plumlang's version from include/plum/version.h, and check that
# CMakeLists.txt agrees. With a tag (v1.2.3), also check the tag matches.
#
#   sh tools/release/version.sh [vX.Y.Z]
set -eu
cd "$(dirname "$0")/../.."
part() { sed -n "s/^#define PLUM_VERSION_$1 \([0-9][0-9]*\)$/\1/p" include/plum/version.h; }
version="$(part MAJOR).$(part MINOR).$(part PATCH)"
cmake="$(sed -n 's/^project(plumlang VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)"
if [ "$cmake" != "$version" ]; then
    echo "version.h says $version but CMakeLists.txt says $cmake" >&2
    exit 1
fi
if [ $# -gt 0 ] && [ "$1" != "v$version" ]; then
    echo "tag $1 does not match the version in the source, v$version" >&2
    exit 1
fi
echo "$version"
