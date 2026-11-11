#!/bin/bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

readonly KERNEL_VERSION="$1"
readonly DESTINATION="$2"
readonly PATCH_FILE="$3"
readonly KERNEL_GIT=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain
readonly SOURCE_PATH=drivers/input/touchscreen
readonly SOURCES=(goodix.c goodix.h goodix_fwupload.c)

validate_version() {
  if [[ ! "${KERNEL_VERSION}" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
    echo "Error: KVER must be an upstream release, e.g. 6.12.10" >&2
    exit 1
  fi
}

download_sources() {
  local destination="$1" source
  for source in "${SOURCES[@]}"; do
    curl -fsSL -o "${destination}/${source}" \
      "${KERNEL_GIT}/${SOURCE_PATH}/${source}?h=v${KERNEL_VERSION}"
  done
}

main() {
  validate_version
  mkdir -p "$(dirname "${DESTINATION}")"
  local staging
  staging="$(mktemp -d "${DESTINATION}.XXXXXX")"
  trap 'rm -rf "${staging}"' EXIT
  download_sources "${staging}"
  patch --batch --forward -d "${staging}" -p1 < "${PATCH_FILE}"
  touch "${staging}/.patched.stamp"
  rm -rf "${DESTINATION}"
  mv "${staging}" "${DESTINATION}"
  trap - EXIT
}

main "$@"
