#!/bin/bash

# install python of a version not less than the minimal required
# $1 - minimal required version

set -e

REQUIRED_VERSION=${1:-3.7}

EXISTING_MAX_VERSION=`apt-cache search '^python3[.0-9]*$' | awk '{print $1}' | sort -V | tail -n 1 | tr -d python`

if dpkg --compare-versions "$EXISTING_MAX_VERSION" lt "$REQUIRED_MIN_VERSION"; then
  echo "Existing python $EXISTING_MAX_VERSION is absent or less than $REQUIRED_MIN_VERSION" 1>&2
  exit 1
fi

EXISTING_MAX_VERSION=python$EXISTING_MAX_VERSION

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  $EXISTING_MAX_VERSION \
  $EXISTING_MAX_VERSION-venv \
  $EXISTING_MAX_VERSION-distutils