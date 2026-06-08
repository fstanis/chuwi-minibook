#!/bin/bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

readonly EXPECTED_MINIBOOK="minibook1"
readonly EXPECTED_VENDOR_PREFIX="CHUWI"
readonly EXPECTED_PRODUCT="MiniBook X"
readonly EXPECTED_CPU="Intel(R) N150"
readonly EXPECTED_DRI_PCI="0000:00:02.0"
readonly STOCK_VBT_CLOCK=1368870


has_cmd() { command -v "$1" &>/dev/null; }


module_loaded() {
  [[ -d "/sys/module/$1" ]]
}

print_field() {
  printf "%-24s%s\n" "$1" "$2"
}

WARNINGS=()

add_warning() {
  WARNINGS+=("$1")
}

print_warn() {
  printf "%-24s\033[33mWARN\033[0m  %s\n" "$1" "$2"
  add_warning "${1}: ${2}"
}

print_detail() {
  printf "%-24s%s\n" "" "$1"
}

# --- device ---

read_dmi() {
  cat "/sys/class/dmi/id/$1" 2>/dev/null || true
}

read_cpu_model() {
  local line
  line="$(grep -m1 'model name' /proc/cpuinfo || true)"
  echo "${line#*: }"
}

read_bios_version() {
  local ver date
  ver="$(cat /sys/devices/virtual/dmi/id/bios_version 2>/dev/null || true)"
  date="$(cat /sys/devices/virtual/dmi/id/bios_date 2>/dev/null || true)"
  echo "${ver}${date:+ ${date}}"
}

find_dsi_display() {
  local d
  for d in /sys/class/drm/card*-DSI-*/; do
    [[ -d "${d}" ]] || continue
    local name mode=""
    name="$(basename "${d}")"
    if [[ -f "${d}/modes" ]]; then
      read -r mode < "${d}/modes" || mode=""
    fi
    echo "${name}${mode:+ ${mode}}"
    return 0
  done
  return 1
}

check_vendor() {
  local vendor
  vendor="$(read_dmi sys_vendor)"
  if [[ "${vendor}" == "${EXPECTED_VENDOR_PREFIX}"* ]]; then
    print_field "dmi vendor" "${vendor}"
  else
    print_warn "dmi vendor" "${vendor} (expected ${EXPECTED_VENDOR_PREFIX}*)"
  fi
}

check_product() {
  local product
  product="$(read_dmi product_name)"
  if [[ "${product}" == "${EXPECTED_PRODUCT}" ]]; then
    print_field "dmi product" "${product}"
  else
    print_warn "dmi product" "${product} (expected ${EXPECTED_PRODUCT})"
  fi
}

check_cpu() {
  local cpu
  cpu="$(read_cpu_model)"
  if [[ "${cpu}" == *"${EXPECTED_CPU}"* ]]; then
    print_field "cpu" "${cpu}"
  else
    print_warn "cpu" "${cpu}"
    print_detail "only tested on ${EXPECTED_CPU}"
  fi
}

read_microcode_version() {
  local line
  line="$(grep -m1 'microcode' /proc/cpuinfo || true)"
  echo "${line#*: }"
}

check_microcode() {
  local ver
  ver="$(read_microcode_version)"
  if [[ -n "${ver}" ]]; then
    print_field "microcode" "${ver}"
  else
    print_warn "microcode" "not found"
  fi
}

check_bios() {
  print_field "bios" "$(read_bios_version)"
}

check_display() {
  local dsi
  if dsi="$(find_dsi_display)"; then
    print_field "display" "${dsi}"
  else
    print_warn "display" "no DSI panel found"
  fi
}

find_dri_device() {
  if [[ ! -d /sys/kernel/debug/dri ]]; then
    return 1
  fi
  local d
  for d in /sys/kernel/debug/dri/*/i915_vbt; do
    [[ -f "${d}" ]] || continue
    local pci_dir
    pci_dir="$(dirname "${d}")"
    basename "${pci_dir}"
    return 0
  done
  return 1
}

vbt_refresh_rate() {
  local clock=$1 htotal=$2 vtotal=$3
  local total=$(( htotal * vtotal ))
  if (( total == 0 )); then
    echo "0"
    return
  fi
  local rate_x10=$(( (clock * 1000 + total / 2) / total ))
  local whole=$(( rate_x10 / 10 ))
  local frac=$(( rate_x10 % 10 ))
  echo "${whole}.${frac}"
}

parse_vbt_block58() {
  local vbt_path="$1"
  local output
  if ! output="$(intel_vbt_decode --file="${vbt_path}" \
      --block=58 2>/dev/null)"; then
    return 1
  fi

  local clock htotal vtotal
  clock="$(grep -m1 'clock:' <<< "${output}" \
    | awk '{print $2}')"
  htotal="$(grep -m1 'htotal:' <<< "${output}" \
    | awk '{print $2}')"
  vtotal="$(grep -m1 'vtotal:' <<< "${output}" \
    | awk '{print $2}')"

  if [[ -z "${clock}" ]] || [[ -z "${htotal}" ]] \
      || [[ -z "${vtotal}" ]]; then
    return 1
  fi

  local hz
  hz="$(vbt_refresh_rate "${clock}" "${htotal}" "${vtotal}")"
  echo "${hz} Hz (clock=${clock} htotal=${htotal} vtotal=${vtotal})"
}

check_vbt() {
  echo ""
  echo "--- vbt ---"
  echo ""

  if ! has_cmd intel_vbt_decode; then
    echo "skipped (intel_vbt_decode not found)"
    return
  fi

  local dri_pci
  if ! dri_pci="$(find_dri_device)"; then
    if (( EUID != 0 )); then
      echo "skipped (run with sudo)"
    else
      print_warn "vbt" "no i915 DRI device found in debugfs"
    fi
    return
  fi

  if [[ "${dri_pci}" != "${EXPECTED_DRI_PCI}" ]]; then
    print_warn "vbt device" "${dri_pci} (expected ${EXPECTED_DRI_PCI})"
  else
    print_field "vbt device" "${dri_pci}"
  fi

  local vbt_path="/sys/kernel/debug/dri/${dri_pci}/i915_vbt"
  if [[ ! -r "${vbt_path}" ]]; then
    echo "skipped (cannot read ${vbt_path}, run with sudo)"
    return
  fi

  local info
  if ! info="$(parse_vbt_block58 "${vbt_path}")"; then
    print_warn "vbt refresh" "failed to decode block 58"
    return
  fi

  local clock
  clock="$(grep -oP 'clock=\K[0-9]+' <<< "${info}" || true)"
  if [[ -n "${clock}" ]] && (( clock == STOCK_VBT_CLOCK )); then
    print_warn "vbt refresh" "${info} (stock — not patched)"
    add_warning "vbt: not patched -- see GUIDE.md"
  else
    print_field "vbt refresh" "${info}"
  fi
}

read_active_sleep_mode() {
  local content
  content="$(cat /sys/power/mem_sleep 2>/dev/null || true)"
  if [[ "${content}" == *"[deep]"* ]]; then
    echo "deep"
  elif [[ "${content}" == *"[s2idle]"* ]]; then
    echo "s2idle"
  else
    echo ""
  fi
}

check_sleep_mode() {
  local mode
  mode="$(read_active_sleep_mode)"
  if [[ "${mode}" == "deep" ]]; then
    print_field "sleep mode" "S3 (hardware)"
  elif [[ "${mode}" == "s2idle" ]]; then
    print_field "sleep mode" "S0ix (software)"
  else
    print_warn "sleep mode" "unknown (cannot read /sys/power/mem_sleep)"
  fi
}

check_device() {
  echo "--- device ---"
  echo ""
  check_vendor
  check_product
  check_cpu
  check_microcode
  check_bios
  check_display
  check_sleep_mode
}

# --- kernel cmdline ---

read_cmdline() {
  cat /proc/cmdline 2>/dev/null || true
}

cmdline_has() {
  local cmdline="$1" param="$2"
  [[ " ${cmdline} " == *" ${param}"* ]]
}

cmdline_value() {
  local cmdline="$1" key="$2"
  local param
  for param in ${cmdline}; do
    if [[ "${param}" == "${key}="* ]]; then
      echo "${param#*=}"
      return
    fi
  done
}

check_vbt_firmware_param() {
  local cmdline="$1"
  local val
  val="$(cmdline_value "${cmdline}" "i915.vbt_firmware")"
  if [[ -n "${val}" ]]; then
    print_field "i915.vbt_firmware" "${val}"
  else
    print_warn "i915.vbt_firmware" "not set -- see GUIDE.md"
  fi
}

check_psr_param() {
  local cmdline="$1"
  local val
  val="$(cmdline_value "${cmdline}" "i915.enable_psr")"
  if [[ "${val}" == "0" ]]; then
    print_field "i915.enable_psr" "0 (disabled)"
  elif [[ -n "${val}" ]]; then
    print_warn "i915.enable_psr" "${val} (should be 0)"
  else
    print_warn "i915.enable_psr" "not set (should be 0 to fix display glitches)"
  fi
}

check_cmdline() {
  echo ""
  echo "--- kernel cmdline ---"
  echo ""

  local cmdline
  cmdline="$(read_cmdline)"

  check_vbt_firmware_param "${cmdline}"
  check_psr_param "${cmdline}"
}

# --- prerequisites ---

check_prereqs() {
  echo ""
  echo "--- prerequisites ---"
  echo ""

  local cmd
  for cmd in dkms clang curl patch meson ninja; do
    printf "%-24s" "${cmd}"
    if has_cmd "${cmd}"; then
      echo "ok"
    else
      echo "MISSING"
    fi
  done

  printf "%-24s" "kernel headers"
  if [[ -f "/lib/modules/$(uname -r)/build/Makefile" ]]; then
    echo "ok"
  else
    echo "MISSING"
  fi
}

# --- modules ---

check_module() {
  local dkms_name="$1"
  local expect_pattern="${2-}"

  printf "%-24s" "${dkms_name}"

  local dkms_ver
  dkms_ver="$(dkms status -m "${dkms_name}" 2>/dev/null \
    | grep 'installed' | head -1 \
    | awk -F'[,/]' '{print $2}' | tr -d ' ' || true)"

  if [[ -z "${dkms_ver}" ]]; then
    echo "NOT INSTALLED"
    return
  fi

  local status="${dkms_ver}"
  if [[ -n "${expect_pattern}" ]] \
      && [[ "${dkms_ver}" != *"${expect_pattern}"* ]]; then
    status="${status} (NOT PATCHED)"
    add_warning "${dkms_name}: installed but not the minibook-patched build"
  fi
  if module_loaded "${dkms_name}"; then
    status="${status}, loaded"
  fi
  if [[ -f "/etc/modules-load.d/${dkms_name}.conf" ]]; then
    status="${status}, boot"
  fi

  echo "${status}"
}

check_modules() {
  echo ""
  echo "--- modules ---"
  echo ""

  check_module goodix_ts "${EXPECTED_MINIBOOK}"

  printf "%-24s" "goodix firmware"
  if [[ -f /lib/firmware/goodix_9110_cfg.bin ]]; then
    echo "ok"
  else
    echo "MISSING"
  fi

  check_module minibook_ec
  check_module dptf_enabler
  check_module i2c_designware_spklen
}

# --- services ---

check_service() {
  local name="$1" ver="$2"
  printf "%-24s" "${name}"

  local status="${ver}"
  if ! echo "${ver}" | grep -q "${EXPECTED_MINIBOOK}"; then
    status="${status} (WRONG)"
  fi

  local enabled
  enabled="$(systemctl is-enabled "${name}" 2>/dev/null || echo "not found")"
  if [[ "${enabled}" == "enabled" || "${enabled}" == "static" ]]; then
    status="${status}, ${enabled}"
  else
    status="${status}, NOT ENABLED"
  fi

  if systemctl is-active --quiet "${name}" 2>/dev/null; then
    status="${status}, running"
  fi

  echo "${status}"
}

read_iio_version() {
  local path
  for path in /usr/libexec/iio-sensor-proxy /usr/lib/iio-sensor-proxy; do
    if [[ -x "${path}" ]]; then
      "${path}" -v 2>&1 | grep -oP 'version \K\S+' && return
    fi
  done
  echo "NOT FOUND"
}

read_iio_polling_line() {
  journalctl -u iio-sensor-proxy --no-pager 2>/dev/null \
    | grep -E 'MXC6655 dual-accel: polling (active|idle)' \
    | tail -1 || true
}

check_iio_polling() {
  local line
  line="$(read_iio_polling_line)"
  if [[ -z "${line}" ]]; then
    return
  fi
  if [[ "${line}" == *"polling active"* ]]; then
    print_detail "sensor polling active (client claimed)"
  else
    print_detail "sensor polling idle${line#*polling idle}"
  fi
}

check_services() {
  echo ""
  echo "--- services ---"
  echo ""

  local thermald_ver iio_ver
  thermald_ver="$(thermald --version 2>&1 | head -1 || echo "NOT FOUND")"
  check_service "thermald" "${thermald_ver}"

  iio_ver="$(read_iio_version)"
  check_service "iio-sensor-proxy" "${iio_ver}"
  check_iio_polling
}

print_warnings() {
  if (( ${#WARNINGS[@]} == 0 )); then
    return
  fi

  echo ""
  echo "--- warnings ---"
  echo ""
  local warning
  for warning in "${WARNINGS[@]}"; do
    echo "${warning}"
  done
}

main() {
  check_device
  check_cmdline
  check_vbt
  check_prereqs
  check_modules
  check_services
  print_warnings
}

main "$@"
