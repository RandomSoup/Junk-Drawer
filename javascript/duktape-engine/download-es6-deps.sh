#!/bin/sh

# Last Working Babel Version
export BABEL_VERSION='7.7.4'

set -e

rm -rf data
mkdir data

cd polyfill

npm install

BROWSERIFY_CMD="node_modules/browserify/bin/cmd.js"
UGLIFY_CMD="node_modules/uglify-js/bin/uglifyjs"

node "${BROWSERIFY_CMD}" src/index.js \
    --insert-global-vars 'global' \
    --plugin bundle-collapser/plugin \
    --plugin derequire/plugin \
| node "${UGLIFY_CMD}" \
    --compress keep_fnames,keep_fargs \
    --mangle keep_fnames \
    > ../data/polyfill.min.js

rm -rf node_modules
rm -f package-lock.json

cd ../

curl -L -o data/babel.min.js "https://unpkg.com/@babel/standalone@${BABEL_VERSION}/babel.min.js"
curl -L "https://unpkg.com/@babel/preset-env-standalone@${BABEL_VERSION}/babel-preset-env.min.js" >> data/babel.min.js
