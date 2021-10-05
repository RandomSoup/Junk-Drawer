#!/bin/sh

export DUKTAPE_VERSION='2.6.0'

set -e

rm -f duktape.tar.xz
curl -L -o duktape.tar.xz "https://duktape.org/duktape-${DUKTAPE_VERSION}.tar.xz"

rm -rf duktape-src
mkdir duktape-src
tar Jxf duktape.tar.xz --strip-components=1 -C duktape-src
rm -f duktape.tar.xz

rm -rf duktape
mkdir duktape

chmod +x ./duktape-src/tools/configure.py
./duktape-src/tools/configure.py \
    --output-directory duktape \
    --line-directives \
    --compiler=clang \
    -DDUK_USE_GLOBAL_BINDING \
    -UDUK_USE_DUKTAPE_BUILTIN

rm -rf ./duktape-src
