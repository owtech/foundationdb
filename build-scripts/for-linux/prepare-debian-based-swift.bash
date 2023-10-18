#!/bin/bash

# install swift from the home side

set -e

BASE_DIR=`dirname $0`

if ! $BASE_DIR/find-swift.bash; then
  SWIFT_ARCH=`mktemp --suffix=.tar.gz`
  wget https://download.swift.org/swift-5.8.1-release/ubuntu1804/swift-5.8.1-RELEASE/swift-5.8.1-RELEASE-ubuntu18.04.tar.gz -O $SWIFT_ARCH
  sudo tar -xvf $SWIFT_ARCH -C /opt
  EXISTING_SWIFT_DIR=`ls -1d /opt/swift* | tail -n 1`
  rm $SWIFT_ARCH
fi
