#!/bin/bash
# SPDX-License-Identifier: 0BSD
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR

readonly VBT_TOOL="${SCRIPT_DIR}/../vbt_patch/vbt_patch"
readonly FIRMWARE_VBT="/lib/firmware/vbt"
readonly CMDLINE_ARG="i915.vbt_firmware=vbt"

readonly MKINITCPIO_CONF="/etc/mkinitcpio.conf"
readonly DRACUT_CONF="/etc/dracut.conf.d/90-vbt.conf"
readonly LIMINE_CONF="/etc/default/limine"
readonly GRUB_CONF="/etc/default/grub"

SYS_VBT=""
BOOTLOADER=""
INITRAMFS=""

require_root() {
  if (( EUID != 0 )); then
    echo "This script must be run as root" >&2
    exit 1
  fi
}

find_sys_vbt() {
  local candidate
  for candidate in /sys/kernel/debug/dri/*/i915_vbt; do
    if [[ -f "${candidate}" ]]; then
      SYS_VBT="${candidate}"
      return
    fi
  done
  echo "No i915_vbt found under /sys/kernel/debug/dri — is i915 loaded?" >&2
  exit 1
}

detect_bootloader() {
  if [[ -f "${LIMINE_CONF}" ]]; then
    BOOTLOADER="limine"
  elif [[ -f "${GRUB_CONF}" ]]; then
    BOOTLOADER="grub"
  else
    echo "No supported bootloader config found (limine, grub)" >&2
    exit 1
  fi
}

detect_initramfs() {
  if command -v mkinitcpio &>/dev/null || command -v limine-mkinitcpio &>/dev/null; then
    INITRAMFS="mkinitcpio"
  elif command -v dracut &>/dev/null; then
    INITRAMFS="dracut"
  elif command -v update-initramfs &>/dev/null; then
    INITRAMFS="initramfs-tools"
  else
    echo "No supported initramfs generator found" >&2
    exit 1
  fi
}

build_vbt_tool() {
  if [[ -x "${VBT_TOOL}" ]]; then
    return
  fi
  echo "Building vbt_patch..."
  make -C "$(dirname "${VBT_TOOL}")"
}

patch_vbt() {
  local framerate="$1"

  if [[ -f "${FIRMWARE_VBT}" ]]; then
    echo "${FIRMWARE_VBT} already exists — remove it or run: $0 --revert" >&2
    exit 1
  fi

  local input_vbt output_vbt
  input_vbt="$(mktemp)"
  output_vbt="$(mktemp)"
  cp "${SYS_VBT}" "${input_vbt}"

  "${VBT_TOOL}" "${input_vbt}" --hz "${framerate}" "${output_vbt}"
  rm -f "${input_vbt}"

  install -Dm644 "${output_vbt}" "${FIRMWARE_VBT}"
  rm -f "${output_vbt}"
  echo "Installed patched VBT to ${FIRMWARE_VBT}"
}

update_initramfs_conf() {
  case "${INITRAMFS}" in
  mkinitcpio)
    if grep -q "${FIRMWARE_VBT}" "${MKINITCPIO_CONF}"; then
      return
    fi
    if grep -qE '^FILES=\(\)' "${MKINITCPIO_CONF}"; then
      sed -i "s|^FILES=()|FILES=(${FIRMWARE_VBT})|" "${MKINITCPIO_CONF}"
    elif grep -qE '^FILES=\(' "${MKINITCPIO_CONF}"; then
      sed -i "s|^FILES=(\(.*\))|FILES=(\1 ${FIRMWARE_VBT})|" "${MKINITCPIO_CONF}"
    else
      echo "FILES=(${FIRMWARE_VBT})" >>"${MKINITCPIO_CONF}"
    fi
    echo "Updated ${MKINITCPIO_CONF}"
    ;;
  dracut)
    printf 'install_items+=" %s "\n' "${FIRMWARE_VBT}" >"${DRACUT_CONF}"
    echo "Wrote ${DRACUT_CONF}"
    ;;
  initramfs-tools)
    # initramfs-tools pulls /lib/firmware in via the stock hooks; nothing to do.
    ;;
  esac
}

update_cmdline() {
  case "${BOOTLOADER}" in
  limine)
    if grep -q "${CMDLINE_ARG}" "${LIMINE_CONF}"; then
      return
    fi
    if ! grep -qE '^KERNEL_CMDLINE\[default\]' "${LIMINE_CONF}"; then
      echo "Could not find KERNEL_CMDLINE[default] in ${LIMINE_CONF}" >&2
      exit 1
    fi
    sed -i "/^KERNEL_CMDLINE\[default\]/s|\"$| ${CMDLINE_ARG}\"|" "${LIMINE_CONF}"
    ;;
  grub)
    if grep -q "${CMDLINE_ARG}" "${GRUB_CONF}"; then
      return
    fi
    if ! grep -qE '^GRUB_CMDLINE_LINUX_DEFAULT=' "${GRUB_CONF}"; then
      echo "Could not find GRUB_CMDLINE_LINUX_DEFAULT in ${GRUB_CONF}" >&2
      exit 1
    fi
    sed -i "/^GRUB_CMDLINE_LINUX_DEFAULT=/s|\"$| ${CMDLINE_ARG}\"|" "${GRUB_CONF}"
    ;;
  esac
  echo "Added ${CMDLINE_ARG} to kernel cmdline (${BOOTLOADER})"
}

rebuild() {
  case "${INITRAMFS}" in
  mkinitcpio)
    if command -v limine-mkinitcpio &>/dev/null; then
      limine-mkinitcpio
    else
      mkinitcpio -P
    fi
    ;;
  dracut | initramfs-tools)
    if command -v update-initramfs &>/dev/null; then
      update-initramfs -u -k all
    else
      dracut -f --regenerate-all
    fi
    ;;
  esac
  echo "Initramfs rebuilt"

  if [[ "${BOOTLOADER}" == "grub" ]]; then
    update-grub
  fi
}

revert() {
  rm -f "${FIRMWARE_VBT}" "${DRACUT_CONF}"

  if [[ -f "${LIMINE_CONF}" ]]; then
    sed -i "s| ${CMDLINE_ARG}||g" "${LIMINE_CONF}"
  fi
  if [[ -f "${GRUB_CONF}" ]]; then
    sed -i "s| ${CMDLINE_ARG}||g" "${GRUB_CONF}"
  fi
  if [[ -f "${MKINITCPIO_CONF}" ]]; then
    sed -i "s| \?${FIRMWARE_VBT}||g" "${MKINITCPIO_CONF}"
  fi

  detect_bootloader
  detect_initramfs
  rebuild
  echo "Revert complete — reboot to apply."
}

usage() {
  echo "Usage: $0 <framerate>"
  echo "       $0 --revert"
  exit 1
}

main() {
  require_root

  if [[ "${1:-}" == "--revert" ]]; then
    revert
    return
  fi

  (( $# == 1 )) || usage

  find_sys_vbt
  detect_bootloader
  detect_initramfs
  echo "Detected: bootloader=${BOOTLOADER}, initramfs=${INITRAMFS}, vbt=${SYS_VBT}"

  build_vbt_tool
  patch_vbt "$1"
  update_initramfs_conf
  update_cmdline
  rebuild

  echo "Done — reboot to apply. To revert: $0 --revert"
}

main "$@"
