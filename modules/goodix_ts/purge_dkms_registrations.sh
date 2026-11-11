#!/bin/bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

readonly DRIVER_NAME_PATTERN='^goodix_(ts|touchscreen)'
readonly DKMS_TREE=/var/lib/dkms

registered_modules() {
  dkms status 2>/dev/null \
    | sed -E 's/[,:].*//' \
    | sort -u
}

is_this_driver() {
  local module_name="${1%%/*}"
  [[ "${module_name}" =~ ${DRIVER_NAME_PATTERN} ]]
}

remove_module() {
  local name="${1%%/*}"
  local version="${1#*/}"
  echo "Removing DKMS registration ${name}/${version}"
  dkms remove -m "${name}" -v "${version}" --all >/dev/null 2>&1 || true
  rm -rf "${DKMS_TREE}/${name}/${version}" "/usr/src/${name}-${version}"
}

main() {
  local modules
  mapfile -t modules < <(registered_modules)
  local module
  for module in "${modules[@]}"; do
    if is_this_driver "${module}"; then
      remove_module "${module}"
    fi
  done
}

main "$@"
