#!/bin/bash
# SPDX-License-Identifier: 0BSD
#
# Bootstrap a CHUWI MiniBook X on Arch/CachyOS. Idempotent: safe to re-run.
# Run it with sudo from your normal user: makepkg refuses to run as root.
#
#   sudo tools/bootstrap-arch.sh            # all but the refresh rate
#   sudo tools/bootstrap-arch.sh modules    # DKMS modules only
#   sudo tools/bootstrap-arch.sh sensor     # iio-sensor-proxy only
#   sudo tools/bootstrap-arch.sh cmdline    # kernel cmdline only
#
# Panel rotation defaults on unless Niri is detected; flags override this.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly REPO_DIR="${SCRIPT_DIR}/.."
readonly LIMINE_CONF="/etc/default/limine"
readonly GRUB_CONF="/etc/default/grub"
readonly PACMAN_CONF="/etc/pacman.conf"
readonly SENSOR_PKG_DIR="${REPO_DIR}/iio-sensor-proxy/packaging/arch"

readonly GOODIX_FIRMWARE="/lib/firmware/goodix_9110_cfg.bin"
readonly MKINITCPIO_CONF="/etc/mkinitcpio.conf"
readonly DRACUT_GOODIX_CONF="/etc/dracut.conf.d/91-goodix-fw.conf"

readonly MODULES=(i2c_designware_spklen goodix_ts minibook_ec dptf_enabler)

readonly BASE_CMDLINE_ARGS=(
  "mem_sleep_default=deep"
  "i915.enable_psr=0"
)

readonly PANEL_ROTATION_ARG="video=DSI-1:panel_orientation=right_side_up"

# Console rotation does not compose additively with the panel orientation, so
# try all four values rather than deriving one. See GUIDE-ADVANCED.md.
readonly FBCON_ROTATE=1
readonly FBCON_TMPFILES="/etc/tmpfiles.d/fbcon-rotate.conf"

readonly PACMAN_PACKAGES=(
  base-devel meson ninja pkgconf clang llvm lld git curl patch
  dkms
  glib2-devel libgudev libdrm polkit libssc
  gtk-doc docbook-xsl
)

BOOTLOADER=""
CMDLINE_CONF=""
CMDLINE_KEY=""
COMMAND="all"
PANEL_ROTATION="auto"
CMDLINE_ARGS=("${BASE_CMDLINE_ARGS[@]}")

require_root() {
  if (( EUID != 0 )); then
    echo "This script must be run as root" >&2
    exit 1
  fi
}

# makepkg refuses to run as root, so the package build drops back to the user
# who invoked sudo.
require_build_user() {
  if [[ -z "${SUDO_USER:-}" || "${SUDO_USER}" == "root" ]]; then
    echo "Run this script with sudo from your normal user account:" \
      "makepkg refuses to run as root" >&2
    exit 1
  fi
}

# Arch kernel packages record their pkgbase next to the modules, which is the
# only reliable way to name the matching headers package (linux-cachyos, ...).
kernel_headers_package() {
  local pkgbase_file
  pkgbase_file="/usr/lib/modules/$(uname -r)/pkgbase"

  if [[ -r "${pkgbase_file}" ]]; then
    echo "$(<"${pkgbase_file}")-headers"
  else
    echo "linux-headers"
  fi
}

# A plain -Sy would leave a partial upgrade behind, so this upgrades the system.
upgrade_system() {
  pacman -Syu --needed --noconfirm "${PACMAN_PACKAGES[@]}" \
    "$(kernel_headers_package)"
}

# Bracketed by the kernel check: a stale kernel picks the wrong headers package,
# and the upgrade itself can replace the one we booted.
install_packages() {
  echo "==> Installing packages"
  require_running_kernel_current
  upgrade_system
  require_running_kernel_current
}


installed_kernels() {
  local dir

  for dir in /usr/lib/modules/*/; do
    if [[ -f "${dir}pkgbase" ]]; then
      dir="${dir%/}"
      echo "${dir##*/}"
    fi
  done
}

# pacman replaces the modules directory on a kernel upgrade, so a running kernel
# with neither a pkgbase nor a headers link is one that is no longer installed.
running_kernel_is_installed() {
  local dir
  dir="/usr/lib/modules/$(uname -r)"

  [[ -f "${dir}/pkgbase" || -e "${dir}/build" ]]
}

report_kernel_mismatch() {
  local installed=()
  mapfile -t installed < <(installed_kernels)

  echo "Error: the running kernel ($(uname -r)) is no longer installed" >&2
  if (( ${#installed[@]} > 0 )); then
    echo "       installed instead: ${installed[*]}" >&2
  fi
  echo "       Reboot into the new kernel and re-run this script: DKMS" \
    "builds against the running kernel" >&2
}

# DKMS builds against the running kernel, so an upgrade that replaced it leaves
# nothing to build or load modules for until the reboot.
require_running_kernel_current() {
  if running_kernel_is_installed; then
    return
  fi
  report_kernel_mismatch
  exit 1
}

require_running_kernel_headers() {
  local release
  release="$(uname -r)"

  if [[ -d "/usr/lib/modules/${release}/build" ]]; then
    return
  fi
  echo "No headers for the running kernel (${release}) — install the" \
    "matching headers package and re-run this script" >&2
  exit 1
}

install_modules() {
  local m

  for m in "${MODULES[@]}"; do
    echo "==> Installing ${m}"
    make -C "${REPO_DIR}/modules/${m}" install
    make -C "${REPO_DIR}/modules/${m}" enable
  done

  bundle_goodix_firmware
}

add_mkinitcpio_file() {
  local file="$1"

  if grep -qF "${file}" "${MKINITCPIO_CONF}"; then
    return
  fi
  if grep -qE '^FILES=\(\)' "${MKINITCPIO_CONF}"; then
    sed -i "s|^FILES=()|FILES=(${file})|" "${MKINITCPIO_CONF}"
  elif grep -qE '^FILES=\(' "${MKINITCPIO_CONF}"; then
    sed -i "s|^FILES=(\(.*\))|FILES=(\1 ${file})|" "${MKINITCPIO_CONF}"
  else
    echo "FILES=(${file})" >>"${MKINITCPIO_CONF}"
  fi
}

write_dracut_goodix_conf() {
  echo "install_items+=\" ${GOODIX_FIRMWARE} \"" >"${DRACUT_GOODIX_CONF}"
}

rebuild_initramfs() {
  if command -v limine-mkinitcpio &>/dev/null; then
    limine-mkinitcpio
  elif command -v mkinitcpio &>/dev/null; then
    mkinitcpio -P
  else
    dracut -f --regenerate-all
  fi
}

# goodix_ts probes inside the initramfs, long before the real root exists, so
# its OEM config has to travel with it or the controller silently falls back to
# the built-in one.
bundle_goodix_firmware() {
  echo "==> Bundling goodix config into the initramfs"

  if [[ -f "${MKINITCPIO_CONF}" ]]; then
    add_mkinitcpio_file "${GOODIX_FIRMWARE}"
  elif [[ -d /etc/dracut.conf.d ]]; then
    write_dracut_goodix_conf
  else
    echo "    neither mkinitcpio nor dracut found, skipping" >&2
    return
  fi

  rebuild_initramfs
}

backup_conf() {
  local conf="$1"
  cp -a "${conf}" "${conf}.bak-$(date +%F)"
}

ignore_distro_sensor_proxy() {
  if grep -qE '^IgnorePkg.*iio-sensor-proxy' "${PACMAN_CONF}"; then
    return
  fi

  backup_conf "${PACMAN_CONF}"
  if grep -qE '^IgnorePkg' "${PACMAN_CONF}"; then
    sed -i '/^IgnorePkg/s|$| iio-sensor-proxy|' "${PACMAN_CONF}"
  else
    sed -i '/^\[options\]/a IgnorePkg = iio-sensor-proxy' "${PACMAN_CONF}"
  fi
  echo "    pinned iio-sensor-proxy in ${PACMAN_CONF}"
}

# -C wipes the build dir: a meson or toolchain upgrade since the last run makes
# a reused one fail to reconfigure.
build_sensor_proxy_package() {
  ( cd "${SENSOR_PKG_DIR}" && sudo -u "${SUDO_USER}" -H makepkg -fC )
}

sensor_proxy_package_path() {
  ( cd "${SENSOR_PKG_DIR}" && sudo -u "${SUDO_USER}" -H makepkg --packagelist )
}

restart_sensor_proxy() {
  systemctl daemon-reload
  udevadm control --reload-rules && udevadm trigger
  systemctl restart iio-sensor-proxy
}

install_sensor_proxy() {
  echo "==> Installing patched iio-sensor-proxy"
  require_build_user
  build_sensor_proxy_package

  local pkg
  pkg="$(sensor_proxy_package_path)"

  # Same pkgname as the repo build, so this replaces it instead of shadowing it.
  pacman -U --noconfirm "${pkg}"

  ignore_distro_sensor_proxy
  restart_sensor_proxy
}

detect_bootloader() {
  if [[ -f "${LIMINE_CONF}" ]]; then
    BOOTLOADER="limine"
    CMDLINE_CONF="${LIMINE_CONF}"
    CMDLINE_KEY='KERNEL_CMDLINE\[default\]'
  elif [[ -f "${GRUB_CONF}" ]]; then
    BOOTLOADER="grub"
    CMDLINE_CONF="${GRUB_CONF}"
    CMDLINE_KEY='GRUB_CMDLINE_LINUX_DEFAULT='
  else
    echo "No supported bootloader config found (limine, grub)" >&2
    exit 1
  fi
}

append_cmdline_arg() {
  local arg="$1"

  if ! grep -qE "^${CMDLINE_KEY}" "${CMDLINE_CONF}"; then
    echo "Could not find ${CMDLINE_KEY} in ${CMDLINE_CONF}" >&2
    exit 1
  fi
  sed -i "/^${CMDLINE_KEY}/s|\"$| ${arg}\"|" "${CMDLINE_CONF}"
}

apply_bootloader_config() {
  if [[ "${BOOTLOADER}" == "limine" ]]; then
    limine-mkinitcpio
  elif command -v update-grub &>/dev/null; then
    update-grub
  else
    grub-mkconfig -o /boot/grub/grub.cfg
  fi
}

update_cmdline() {
  configure_panel_rotation
  detect_bootloader
  echo "==> Updating kernel cmdline (${BOOTLOADER})"
  local arg changed=0

  backup_conf "${CMDLINE_CONF}"

  for arg in "${CMDLINE_ARGS[@]}"; do
    if grep -qF -- "${arg}" "${CMDLINE_CONF}"; then
      continue
    fi
    append_cmdline_arg "${arg}"
    echo "    added ${arg}"
    changed=1
  done

  if (( changed )); then
    apply_bootloader_config
  else
    echo "    already up to date"
  fi
}

rotate_consoles() {
  echo "==> Rotating text consoles"
  echo "w /sys/class/graphics/fbcon/rotate_all - - - - ${FBCON_ROTATE}" \
    >"${FBCON_TMPFILES}"
  systemd-tmpfiles --create "${FBCON_TMPFILES}" || true
}

print_next_steps() {
  echo
  echo "Done — reboot to apply the kernel cmdline."
  echo "thermald needs BIOS changes first, see GUIDE.md."
  echo
  echo "The VBT refresh rate is deliberately NOT applied here: a raised pixel"
  echo "clock can pass a cold boot and still fail on the first suspend."
  echo "Read docs/vbt-patch.md before running update-vbt-clock."
}

niri_detected() {
  local desktop="${XDG_CURRENT_DESKTOP:-}"

  case "${desktop}" in
    *[Nn][Ii][Rr][Ii]*) return 0 ;;
  esac
  command -v niri &>/dev/null || pgrep -x niri &>/dev/null
}

configure_panel_rotation() {
  if [[ "${PANEL_ROTATION}" == "off" ]]; then
    return
  fi
  if [[ "${PANEL_ROTATION}" == "auto" ]] && niri_detected; then
    echo "    Niri detected: skipping kernel panel rotation (iio-niri #14)."
    echo "    Use --panel-rotation if your iio-niri includes the fix."
    return
  fi
  CMDLINE_ARGS+=("${PANEL_ROTATION_ARG}")
}

parse_args() {
  local arg

  for arg in "$@"; do
    case "${arg}" in
      --panel-rotation) PANEL_ROTATION="on" ;;
      --no-panel-rotation) PANEL_ROTATION="off" ;;
      *) COMMAND="${arg}" ;;
    esac
  done
}

main() {
  require_root
  parse_args "$@"

  case "${COMMAND}" in
    all)
      install_packages
      require_running_kernel_headers
      install_modules
      install_sensor_proxy
      update_cmdline
      rotate_consoles
      ;;
    modules)
      install_packages
      require_running_kernel_headers
      install_modules
      ;;
    sensor)
      install_sensor_proxy
      ;;
    cmdline)
      update_cmdline
      rotate_consoles
      ;;
    *)
      echo "Usage: $0 [all|modules|sensor|cmdline]" \
        "[--panel-rotation|--no-panel-rotation]" >&2
      exit 1
      ;;
  esac

  print_next_steps
}

main "$@"
