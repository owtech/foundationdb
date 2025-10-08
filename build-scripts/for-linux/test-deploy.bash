#!/bin/bash

# Test deploying foundationdb on rpm-and deb-based linux
# $1 - full Foundationdb version, ex. 7.1.29-0.ow.1
# $2 - distr dir. Default is bld/linux/packages relative to the current dir
# $3 - a rpm-based linux docker image. Default is oraclelinux:8
# $4 - a deb-based linux docker image. Default is debian:10

set -e

FULL_VERSION="$1"
DISTR_DIR="$(readlink -f ${2:-bld/linux/packages})"
RPM_IMAGE=${3:-oraclelinux:8}
DEB_IMAGE=${4:-debian:10}

CONTAINER_NAME="test_deploy"
CONTAINER_DISTR_DIR="/mnt/distr"
CONTAINER_MOUNTS="${DISTR_DIR}:${CONTAINER_DISTR_DIR}"

err() { 
  echo -e "\033[1;31m$*\033[0m" >&2; 
}

log() { 
  echo -e "\033[1;34m$*\033[0m"; 
}

print_usage() {
  err "Usage: $0 FullFdbVersion [FdbDistrDir] [RpmImage] [DebImage]"
}

# testing parameters
if [[ -z "$FULL_VERSION" ]]; then
  err "FullFdbVersion is not specified."
  print_usage
  return 1 2>/dev/null || exit 1
fi

if ! ls "$DISTR_DIR"/foundationdb-*$FULL_VERSION*.{rpm,deb}; then
  err "No $DISTR_DIR/foundationdb-*$FULL_VERSION*.{rpm,deb} files have been found."
  err "Possible FdbDistrDir is not correct."
  print_usage
  return 1 2>/dev/null || exit 1
fi

# foundationdb writes to files with 2600 mode
# but podman under kernel 6.1 or 6.2 does not respect this
# so test if the writing works and choose an appropriate container engine

test_container_engine() {
  local ENGINE="$1"
  local CONTAINER_TEST_IMAGE=$DEB_IMAGE
  local TEST_CMD='touch /tmp/file01.tst && chgrp users /tmp/file01.tst && chmod 2600 /tmp/file01.tst && echo test > /tmp/file01.tst'
  $ENGINE run --rm $CONTAINER_TEST_IMAGE bash -c "$TEST_CMD"
}

#choose an appropriate CONTAINER_ENGINE
if [[ -n "$CONTAINER_ENGINE" ]]; then
  if ! test_container_engine $CONTAINER_ENGINE; then
    err "Fatal: The specified CONTAINER_ENGINE ($CONTAINER_ENGINE) does not pass the test."
    return 1 2>/dev/null || exit 1
  fi
elif test_container_engine podman; then
  CONTAINER_ENGINE=podman
elif test_container_engine docker; then
  CONTAINER_ENGINE=docker
else
  err "Fatal: Neigther podman nor docker passes the test"
  return 1 2>/dev/null || exit 1
fi
log "Using $CONTAINER_ENGINE as a container image"

# check images
$CONTAINER_ENGINE pull "$RPM_IMAGE"
$CONTAINER_ENGINE pull "$DEB_IMAGE"

MY_ARCH_RPM=$(uname -m)
MY_ARCH_DEB=$(dpkg-architecture -q DEB_HOST_ARCH)

wait_for_systemd() {
  local CONTAINER_NAME="$1"
  local TIMEOUT_SEC=60
  log "Waiting for systemd to start in container $CONTAINER_NAME..."
  while true; do
    PID1=$($CONTAINER_ENGINE exec ${CONTAINER_NAME} ps -p 1 -o comm=)
    if [[ "$PID1" == "systemd" ]]; then
      printf "\nsystemd is running (PID 1)\n"
      return 0
    fi
    printf "\r\033[K[*] %s (%ds)" "$PID1" "$TIMEOUT_SEC"
    sleep 1
    ((TIMEOUT_SEC--))
    if [[ $TIMEOUT_SEC -le 0 ]]; then
      err "\nsystemd is not running in container (final state: $PID1)"
      return 1
    fi
  done
}

remove_container_if_exists() {
  if $CONTAINER_ENGINE ps -a --format "{{.Names}}" | grep -qx "${CONTAINER_NAME}"; then
    $CONTAINER_ENGINE rm -f "${CONTAINER_NAME}"
  fi
}

prepare_systemd_script() {
  case "$1" in
    *debian:10*)
      cat <<'EOF'
echo 'deb http://archive.debian.org/debian buster main' > /etc/apt/sources.list
echo 'deb http://archive.debian.org/debian buster-updates main' >> /etc/apt/sources.list
echo 'deb http://archive.debian.org/debian-security buster/updates main' >> /etc/apt/sources.list
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y systemd systemd-sysv dbus procps
exec /lib/systemd/systemd
EOF
      ;;
    *oraclelinux:8*)
      echo 'exec /lib/systemd/systemd'
      ;;
    *)
      return 1
      ;;
  esac
}

start_systemd_container() {
  local IMAGE="$1"

  local SCRIPT
  SCRIPT="$(prepare_systemd_script "$IMAGE")" || {
    err "systemd preparation script is not defined for $IMAGE"
    return 1
  }

  $CONTAINER_ENGINE run -d --privileged --systemd=always --cgroupns=host \
    --name "${CONTAINER_NAME}" \
    -e container=podman \
    --tmpfs /run --tmpfs /tmp \
    -v /sys/fs/cgroup:/sys/fs/cgroup:ro \
    -v "${CONTAINER_MOUNTS}:Z,ro" \
    "$IMAGE" \
    /bin/bash -c "$SCRIPT"
}

run_simple_container() {
  local IMAGE="$1"
  local INSTALL_CMD="$2"
  local INSTALL_DISTR="$3"
  local TEST_CLIENT="$4"
  $CONTAINER_ENGINE run --rm \
    -v "${CONTAINER_MOUNTS}:Z,ro" \
    "$IMAGE" \
    /bin/bash -ec "$INSTALL_CMD $INSTALL_DISTR && $TEST_CLIENT"
}

declare -A INSTALL_CMDS=(
  [deb]="apt-get install -y"
  [rpm]="dnf install -y"
)

get_pkg_type() {
  local FILE="$1"
  if [[ "$FILE" == *".deb"* ]]; then
    echo "deb"
  elif [[ "$FILE" == *".rpm"* ]]; then
    echo "rpm"
  else
    echo "unknown"
  fi
}

exec_in_container() {
  $CONTAINER_ENGINE exec -it "${CONTAINER_NAME}" /bin/bash -c "$1"
}

run_in_container() {
  local IMAGE="$1"
  local INSTALL_SYSTEMD=$2
  local INSTALL_DISTR="$3"
  local TEST_CLIENT="$4"
  local TEST_WRITE_READ="$5"
  
  local PKG_TYPE
  PKG_TYPE=$(get_pkg_type "$INSTALL_DISTR")
  local INSTALL_CMD
  INSTALL_CMD="${INSTALL_CMDS[$PKG_TYPE]}"

  remove_container_if_exists
  trap 'remove_container_if_exists' RETURN
  
  if [[ "$INSTALL_SYSTEMD" == "systemd" ]]; then
    if ! start_systemd_container "$IMAGE"; then
      err "Cannot start a container from $IMAGE"
      return 1
    fi
    if ! wait_for_systemd ${CONTAINER_NAME}; then
      return 1
    fi
    if ! exec_in_container "$INSTALL_CMD $INSTALL_DISTR"; then
      err "FDB installation or start failed"
      return 1
    fi
    if [[ -n "$TEST_CLIENT" ]] && ! exec_in_container "$TEST_CLIENT"; then
      err "FDB client test command failed ($TEST_CLIENT)"
      return 1
    fi
    if [[ -n "$TEST_WRITE_READ" ]] && ! exec_in_container "$TEST_WRITE_READ"; then
      err "FDB write/read test command failed: key is not 'testvalue'"
      return 1
    fi
  else 
    if ! run_simple_container "$IMAGE" "$INSTALL_CMD" "$INSTALL_DISTR" "$TEST_CLIENT"; then
      err "Cannot start a container from $IMAGE"
      return 1
    fi
  fi
}

run_install_test() {
  local IMAGE="$1"
  local INSTALL_SYSTEMD="$2"
  local INSTALL_DISTR="$3"
  local TEST_CLIENT="$4"
  local SHOULD_FAIL="${5:-0}"
  local ERRMSG="$6"
  local TEST_WRITE_READ="$7"

  if run_in_container "$IMAGE" "$INSTALL_SYSTEMD" "$INSTALL_DISTR" "$TEST_CLIENT" "$TEST_WRITE_READ"; then
    if [[ "$SHOULD_FAIL" -eq 1 ]]; then
      err "$ERRMSG"
      return 1
    fi
    log "<Test passed: $INSTALL_DISTR>"
  else
    if [[ "$SHOULD_FAIL" -eq 0 ]]; then
      err "$ERRMSG"
      return 1
    fi
    log "<Test failed as expected: $INSTALL_DISTR>"
  fi
}

test_deploy_pkgs() {
  local IMAGE=$1
  local SERVER_FILE=$2
  local CLIENT_FILE=$3
  local USER_AFTER_CLIENT=${4:-Y}
  
  local INSTALL_SYSTEMD=""
  local TEST_CLIENT_WITH=""
  local ERRMSG_CLIENT=""

  log SERVER_FILE="$SERVER_FILE"
  log CLIENT_FILE="$CLIENT_FILE"

  if [[ $USER_AFTER_CLIENT == Y ]]; then
    ERRMSG_CLIENT="the foundationdb user was not created"
  else
    TEST_CLIENT_WITH="!"
    ERRMSG_CLIENT="the foundationdb user created unexpectedly"
  fi

  local TEST_CLIENT="getent passwd foundationdb"
  TEST_CLIENT_WITH="$TEST_CLIENT_WITH $TEST_CLIENT"

  local TEST_WRITE_READ='fdbcli --exec "writemode on; set key testvalue; get key" | grep -E "key.*is.*testvalue"'

  declare -a TESTS=(
    # "desc|install_distr|should_fail|errmsg|errcode|start_with_systemd|fdb_user_check|fdb_write_read_check"
    "client_only|$CONTAINER_DISTR_DIR/$CLIENT_FILE|0|Installation of $CLIENT_FILE failed or $ERRMSG_CLIENT.|3|no|$TEST_CLIENT_WITH|:"
    "client_and_server|$CONTAINER_DISTR_DIR/$SERVER_FILE $CONTAINER_DISTR_DIR/$CLIENT_FILE|0|Installation $SERVER_FILE and $CLIENT_FILE failed or $ERRMSG_CLIENT.|2|systemd|$TEST_CLIENT|$TEST_WRITE_READ"
    "server_only|$CONTAINER_DISTR_DIR/$SERVER_FILE|1|Installation $SERVER_FILE without a client must fail.|1|no|:|:"
  )

  for TEST in "${TESTS[@]}"; do
    IFS="|" read -r DESC INSTALL_DISTR SHOULD_FAIL ERRMSG ERRCODE INSTALL_SYSTEMD TEST_CLIENT TEST_WRITE_READ <<< "$TEST"
    log "<Trying to install: $DESC...>\n"
    run_install_test "$IMAGE" "$INSTALL_SYSTEMD" "$INSTALL_DISTR" "$TEST_CLIENT" "$SHOULD_FAIL" "$ERRMSG" "$TEST_WRITE_READ" || return "$ERRCODE"
    log "<Test $DESC completed>\n"
  done
}

declare -a DEPLOY_SCENARIOS=(
  # "desc|container_image|server_package_file|client_package_file|check_user_after_client"
  "Testing DEBs deploy|$DEB_IMAGE|foundationdb-server_${FULL_VERSION}_$MY_ARCH_DEB.deb|foundationdb-clients_${FULL_VERSION}_$MY_ARCH_DEB.deb|Y"
  "Testing versioned DEBs deploy|$DEB_IMAGE|foundationdb-${FULL_VERSION}-server-versioned_${FULL_VERSION}_$MY_ARCH_DEB.deb|foundationdb-${FULL_VERSION}-clients-versioned_${FULL_VERSION}_$MY_ARCH_DEB.deb|N"
  "Testing RPMs deploy|$RPM_IMAGE|foundationdb-server-${FULL_VERSION}.$MY_ARCH_RPM.rpm|foundationdb-clients-${FULL_VERSION}.$MY_ARCH_RPM.rpm|Y"
  "Testing versioned RPMs deploy|$RPM_IMAGE|foundationdb-${FULL_VERSION}-server-versioned-${FULL_VERSION}.$MY_ARCH_RPM.rpm|foundationdb-${FULL_VERSION}-clients-versioned-${FULL_VERSION}.$MY_ARCH_RPM.rpm|N"
)

for SCENARIO in "${DEPLOY_SCENARIOS[@]}"; do
  IFS="|" read -r DESC IMAGE SERVER_FILE CLIENT_FILE USER_AFTER_CLIENT <<< "$SCENARIO"
  log "[!!!] $DESC... [!!!]\n"
  test_deploy_pkgs "$IMAGE" "$SERVER_FILE" "$CLIENT_FILE" "$USER_AFTER_CLIENT" || return $? 2>/dev/null || exit $?
done

log "\n[V] All deployment tests completed successfully [V]\n"