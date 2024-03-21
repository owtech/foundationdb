#!/bin/sh

# install clang of the most recent version

set -e

sudo apt update

# clang no more than 14 version is needed for boost-5.3 building
max_clang_version=14

RECENT_VERSION=` \
  apt-cache search -o APT::Cache::Search::Version=1 '^clang-[0-9]+$' | \
  tr - ' ' | \
  awk -v ver="$max_clang_version" '{if($2<=ver) print $2;}' | \
  sort -n  | \
  tail -n 1`

sudo DEBIAN_FRONTEND=noninteractive apt install -y \
  clang-$RECENT_VERSION libc++-$RECENT_VERSION-dev libc++abi-$RECENT_VERSION-dev 

[ -e /usr/local/bin/clang ] || sudo ln -s `ls -1 /usr/bin/clang-[0-9]* | tail -n 1` /usr/local/bin/clang
[ -e /usr/local/bin/clang++ ] || sudo ln -s `ls -1 /usr/bin/clang++-[0-9]* | tail -n 1` /usr/local/bin/clang++
