#!/bin/bash

# find swift installation
# print the path to swift installed or return non zero rc


if ! which swiftc 2>/dev/null; then
  WILDCARDS="/opt/swift*/usr/bin/swiftc"
  if ls $WILDCARDS 2>/dev/null; then
    ls -1d $WILDCARDS | tail -n 1
  else  
    return $? 2>/dev/null || exit $?
  fi
fi
