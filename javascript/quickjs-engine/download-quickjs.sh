#!/bin/sh

export QUICKJS_VERSION='2021-03-27'

set -e

rm -f quickjs.tar.xz
curl -L -o quickjs.tar.xz "https://bellard.org/quickjs/quickjs-${QUICKJS_VERSION}.tar.xz"

rm -rf quickjs
mkdir quickjs
tar Jxf quickjs.tar.xz --strip-components=1 -C quickjs
rm -f quickjs.tar.xz
