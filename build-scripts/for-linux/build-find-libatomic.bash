#!/bin/bash

# should be called with "source" command
# try to find libatomic library and set -DATOMIC_LIBRARY_FILE to the APP_PRMS

set -e

# try to find the static library
LIBRARY_CANDIDATE=$(ls -t1 /usr/lib/gcc/x86_64*/*/libatomic.a | head -n 1) || true

if [[ -n "$LIBRARY_CANDIDATE" ]]; then
  # test if the library is linked relocatable
  PROBE_O=glfree.o
  TMP_DIR=`mktemp -d`
  pushd $TMP_DIR
  ar x $LIBRARY_CANDIDATE $PROBE_O
  REL_TYPE=`objdump -r glfree.o | awk '/.rodata/{ print $2;exit;}'`
  popd
  rm -rf $TMP_DIR
  # R_X86_64_32S is prohibited for linking shared libraries
  [[ "$REL_TYPE" == "R_X86_64_32S" ]] && LIBRARY_CANDIDATE=""
fi

# try to find a dynamic library
[[ -z "$LIBRARY_CANDIDATE" ]] \
  && LIBRARY_CANDIDATE=$(ls -1 /usr/lib64/libatomic.so* /usr/lib/gcc/x86_64-*/*/libatomic.so* | sort | head -n 1) \
  || true

# if found then put in to APP_PRMS
[[ -n "$LIBRARY_CANDIDATE" ]] && APP_PRMS="$APP_PRMS -DATOMIC_LIBRARY_FILE=$LIBRARY_CANDIDATE"
