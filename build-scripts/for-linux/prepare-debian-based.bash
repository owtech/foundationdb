#!/bin/sh

set -e

BASE_DIR=`dirname $0`

sudo apt update

CURRENT_LINUX_VER=`grep VERSION_ID /etc/os-release | grep -o "[0-9]*"`
DEBIAN10=10
DEBIAN12=12

case $CURRENT_LINUX_VER in
    $DEBIAN12) sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
                 cmake \
                 clang \
                 libc++-dev \
                 libc++abi-dev \
                 default-jdk
               ;;
    $DEBIAN10) $BASE_DIR/prepare-debian-based-cmake.bash 3.24
               $BASE_DIR/prepare-debian-based-clang.sh
               $BASE_DIR/prepare-debian-based-jdk.bash
               ;;
    *)         echo "Current version linux [$CURRENT_LINUX_VER] not supported"
               exit 1;;
esac

# make is necessary for building Jemalloc on Astra Linux

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  git \
  libncurses5 \
  lld \
  mono-mcs \
  make \
  ninja-build \
  pigz \
  python3 \
  python3-pip \
  python3-sphinx \
  python3-venv \
  rpm

$BASE_DIR/prepare-debian-based-swift.bash

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  libjemalloc-dev \
  liblz4-dev \
  libssl-dev

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  libmono-system-data-datasetextensions4.0-cil \
  libmono-system-runtime-serialization4.0-cil \
  libmono-system-xml-linq4.0-cil

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  python3-sphinx-bootstrap-theme
