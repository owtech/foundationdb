#!/bin/sh

set -e

BASE_DIR=`dirname $0`

sudo apt update

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y wget

$BASE_DIR/prepare-debian-based-cmake.bash 3.24
$BASE_DIR/prepare-debian-based-clang.sh
$BASE_DIR/prepare-debian-based-jdk.bash

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
