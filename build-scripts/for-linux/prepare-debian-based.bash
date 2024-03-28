#!/bin/sh

set -e

BASE_DIR=`dirname $0`

sudo apt update

# wget is required to download cmake 3.24 if you are using Debian 12 to build.
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y wget

$BASE_DIR/prepare-debian-based-cmake.bash 3.24
$BASE_DIR/prepare-debian-based-clang.sh
$BASE_DIR/prepare-debian-based-jdk.bash

# make is necessary for building Jemalloc on Astra Linux

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  git \
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
