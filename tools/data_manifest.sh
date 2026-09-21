#!/bin/sh
# Prints the sizes and hashes recorded in data/MANIFEST.md (the output is
# results/manifest_20190130.log). Run it from the directory that holds the
# download and its decompression, e.g. from data/: sh ../tools/data_manifest.sh
set -e
for cmd in "gzip --version | head -1" \
           "sha256sum --version | head -1" \
           "stat -c '%s %n' 01302019.NASDAQ_ITCH50.gz 01302019.NASDAQ_ITCH50" \
           "sha256sum 01302019.NASDAQ_ITCH50.gz" \
           "sha256sum 01302019.NASDAQ_ITCH50" \
           "gzip -dc 01302019.NASDAQ_ITCH50.gz | sha256sum"; do
    echo "\$ $cmd"
    sh -c "$cmd"
done
