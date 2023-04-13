#!/bin/sh

# install clang of the most recent version

set -e

sudo apt update
RECENT_VERSION=` \
  apt search -o APT::Cache::Search::Version=1 '^clang-[0-9]+$' \
    | tr - ' ' | awk '{print $2;}' | sort -n  | tail -n 1`

sudo DEBIAN_FRONTEND=noninteractive apt install -y \
  clang-$RECENT_VERSION libc++-$RECENT_VERSION-dev libc++abi-$RECENT_VERSION-dev 

[ -e /usr/local/bin/clang ] || sudo ln -s `ls -1 /usr/bin/clang-[0-9]* | tail -n 1` /usr/local/bin/clang
[ -e /usr/local/bin/clang++ ] || sudo ln -s `ls -1 /usr/bin/clang++-[0-9]* | tail -n 1` /usr/local/bin/clang++
