#!/bin/bash

# find swift installation
# print the path to swift installed or return non zero rc

which swiftc || ls -1d /opt/swift*/usr/bin/swiftc 2>/dev/null | tail -n 1
