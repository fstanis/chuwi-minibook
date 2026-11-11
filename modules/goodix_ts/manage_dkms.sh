#!/bin/bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

readonly MODULE=goodix_ts
readonly SOURCE_ROOT=/usr/src
readonly KERNEL_ROOT=/lib/modules
readonly FIRMWARE=/lib/firmware/goodix_9110_cfg.bin
readonly SOURCE_FILES=(goodix.c goodix.h goodix_fwupload.c)
readonly VERSION_PATTERN='^([0-9]+\.[0-9]+(\.[0-9]+)?[.-])?minibook1(\.[0-9a-f]{12})?$'

fail() {
  echo "Error: $*" >&2
  exit 1
}

package_owned() {
  if command -v pacman >/dev/null && pacman -Qo "$1" >/dev/null 2>&1; then
    return 0
  fi
  if command -v dpkg-query >/dev/null \
    && dpkg-query -S "$1" >/dev/null 2>&1; then
    return 0
  fi
  if command -v rpm >/dev/null && rpm -qf "$1" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

verify_source() {
  local directory="$1"
  local version="${1#"${SOURCE_ROOT}/${MODULE}-"}"
  if package_owned "${directory}/dkms.conf"; then
    fail "${directory} belongs to a package; use the package manager"
  fi
  grep -qx 'PACKAGE_NAME="goodix_ts"' "${directory}/dkms.conf" \
    || fail "Unrecognized source: ${directory}"
  grep -qx "PACKAGE_VERSION=\"${version}\"" "${directory}/dkms.conf" \
    || fail "Version mismatch: ${directory}"
  grep -q 'goodix_i2c_read_quiet' "${directory}/goodix.c" \
    || fail "Unrecognized Goodix patch: ${directory}"
  grep -q 'loaded firmware %s (%zu bytes)' "${directory}/goodix.c" \
    || fail "Unrecognized Goodix firmware: ${directory}"
}

registered_versions() {
  local status line version
  status="$(dkms status -m "${MODULE}")" || return 1
  while IFS= read -r line; do
    [[ "${line}" == "${MODULE}/"* ]] || continue
    version="${line#*/}"
    version="${version%%,*}"
    version="${version%%:*}"
    [[ "${version}" =~ ${VERSION_PATTERN} ]] \
      || fail "Unrecognized ${MODULE} version: ${version}"
    echo "${version}"
  done <<< "${status}"
}

kernel_is_installed() {
  local directory="$1" kernel="${1##*/}"
  [[ -f "${directory}/pkgbase" || -f "${directory}/vmlinuz" \
    || -f "/boot/vmlinuz-${kernel}" ]]
}

installed_kernels() {
  local directory kernel
  for directory in "${KERNEL_ROOT}"/*; do
    [[ -d "${directory}" ]] || continue
    kernel_is_installed "${directory}" || continue
    kernel="${directory##*/}"
    [[ -f "${directory}/build/Makefile" ]] \
      || fail "Install headers for ${kernel} before migrating Goodix"
    echo "${kernel}"
  done
}

verify_versions() {
  local version
  for version in "$@"; do
    verify_source "${SOURCE_ROOT}/${MODULE}-${version}"
  done
}

remove_version() {
  local version="$1"
  dkms remove -m "${MODULE}" -v "${version}" --all
  rm -rf "${SOURCE_ROOT}/${MODULE}-${version}"
}

stage_source() {
  local version="$1" source="$2" staging="$3"
  local file
  for file in "${SOURCE_FILES[@]}"; do
    cp "${source}/${file}" "${staging}/"
  done
  cp Kbuild "${staging}/"
  sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${version}\"/" \
    dkms.conf > "${staging}/dkms.conf"
}

publish_source() {
  local staging="$1" destination="$2" file
  if [[ -e "${destination}" ]]; then
    verify_source "${destination}"
    for file in "${SOURCE_FILES[@]}" Kbuild dkms.conf; do
      cmp -s "${staging}/${file}" "${destination}/${file}" \
        || fail "Source changed without a version change: ${destination}"
    done
    return
  fi
  chmod 755 "${staging}"
  mv "${staging}" "${destination}"
}

register_source() {
  local version="$1" source="$2" staging status
  staging="$(mktemp -d "${SOURCE_ROOT}/.${MODULE}.XXXXXX")"
  trap 'rm -rf "${staging}"' EXIT
  stage_source "${version}" "${source}" "${staging}"
  publish_source "${staging}" "${SOURCE_ROOT}/${MODULE}-${version}"
  rm -rf "${staging}"
  trap - EXIT
  status="$(dkms status -m "${MODULE}" -v "${version}")"
  if [[ -z "${status}" ]]; then
    dkms add -m "${MODULE}" -v "${version}"
  fi
}

build_kernels() {
  local version="$1" kernel
  shift
  for kernel in "$@"; do
    dkms build -m "${MODULE}" -v "${version}" -k "${kernel}"
  done
}

install_kernels() {
  local version="$1" kernel
  shift
  for kernel in "$@"; do
    dkms install --force -m "${MODULE}" -v "${version}" -k "${kernel}"
  done
}

remove_old_versions() {
  local current="$1" version
  shift
  for version in "$@"; do
    [[ "${version}" == "${current}" ]] && continue
    remove_version "${version}"
  done
}

install_driver() {
  local version="$1" source="$2" kernel_list
  shift 2
  [[ "${version}" =~ ${VERSION_PATTERN} ]] || fail "Invalid version"
  kernel_list="$(installed_kernels)"
  [[ -n "${kernel_list}" ]] || fail "No installed kernels found"
  local kernels=()
  mapfile -t kernels <<< "${kernel_list}"
  register_source "${version}" "${source}"
  build_kernels "${version}" "${kernels[@]}"
  install -Dm644 goodix_cfg.bin "${FIRMWARE}"
  remove_old_versions "${version}" "$@"
  install_kernels "${version}" "${kernels[@]}"
}

uninstall_driver() {
  local version
  for version in "$@"; do
    remove_version "${version}"
  done
  rm -f "${FIRMWARE}"
}

main() {
  (( EUID == 0 )) || fail "Run with sudo"
  if package_owned "${FIRMWARE}"; then
    fail "${FIRMWARE} belongs to a package; use the package manager"
  fi
  local versions_text
  versions_text="$(registered_versions)"
  local versions=()
  if [[ -n "${versions_text}" ]]; then
    mapfile -t versions < <(sort -u <<< "${versions_text}")
  fi
  verify_versions "${versions[@]}"
  case "$1" in
    install) install_driver "$2" "$3" "${versions[@]}" ;;
    uninstall) uninstall_driver "${versions[@]}" ;;
    *) fail "Usage: $0 install VERSION SOURCE | uninstall" ;;
  esac
}

main "$@"
