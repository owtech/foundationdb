#!/bin/bash

# $1 - the path to the correctness archive file, ex. correctness-7.3.49-2.ow.tar.gz
# $2 - number of tests

res=$(python3 -m joshua.joshua start --tarball $1 --max-runs $2 && python3 -m joshua.joshua tail --errors)

substr="TestUID"
echo $res
if [[ $res == *$substr* ]]; then
  echo >&2 "Test failed."
  exit 1
fi