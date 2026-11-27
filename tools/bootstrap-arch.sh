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
#   sudo tools/bootstrap-arch.sh greeter    # greetd + noctalia-greeter login screen
#
# greeter is opt-in, not part of the default run: switching the system login
# manager is a deliberate choice, not something a bootstrap script should do
# on the side.
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

# Left behind by a pre-pacman manual `meson install`; block the package build
# the same way a raw goodix_ts install blocks its firmware file below.
readonly SENSOR_PROXY_UNPACKAGED=(
  /usr/bin/monitor-sensor
  /usr/lib/systemd/system/iio-sensor-proxy.service
  /usr/lib/udev/rules.d/80-iio-sensor-proxy.rules
  /usr/share/dbus-1/system.d/net.hadess.SensorProxy.conf
  /usr/share/polkit-1/actions/net.hadess.SensorProxy.policy
)

readonly GOODIX_FIRMWARE="/lib/firmware/goodix_9110_cfg.bin"
readonly MKINITCPIO_CONF="/etc/mkinitcpio.conf"
readonly DRACUT_GOODIX_CONF="/etc/dracut.conf.d/91-goodix-fw.conf"

readonly MODULES=(i2c_designware_spklen goodix_ts minibook_ec dptf_enabler)

readonly BASE_CMDLINE_ARGS=(
  "mem_sleep_default=deep"
  "i915.enable_psr=0"
)

# Opt-in: iio-niri needs 2.3.3+ and an offset mapping (see desktop integration).
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

readonly LOGIN_STACK_PACKAGES=(seatd greetd noctalia-greeter)
readonly GREETD_CONFIG="/etc/greetd/config.toml"
readonly GREETER_HOME="/var/lib/greeter"
readonly NIRI_SESSION="/usr/bin/niri-session"
readonly NIRI_SESSION_BACKUP="${NIRI_SESSION}.orig-pre-import-env-fix"
readonly NIRI_SESSION_PATCH_MARKER="HACK: The following line has been automatically patched by a pacman hook"
readonly NIRI_HOOK="/etc/pacman.d/hooks/niri-import-environment-patch.hook"
readonly OTHER_DISPLAY_MANAGERS=(sddm gdm lightdm lxdm)

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
  modprobe i2c-dev
  systemctl daemon-reload
  udevadm control --reload-rules && udevadm trigger
  systemctl restart iio-sensor-proxy
}

install_sensor_proxy() {
  echo "==> Installing patched iio-sensor-proxy"
  require_build_user

  local f
  for f in "${SENSOR_PROXY_UNPACKAGED[@]}"; do
    clean_unpackaged_file "${f}"
  done

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

  if [[ "${PANEL_ROTATION}" == "off" ]] \
    && grep -qF "${PANEL_ROTATION_ARG}" "${CMDLINE_CONF}"; then
    sed -i "/^${CMDLINE_KEY}/s| ${PANEL_ROTATION_ARG}||g" "${CMDLINE_CONF}"
    changed=1
  fi

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

# seatd is the seat backend Niri prefers. Without it, switching VTs between
# the greeter and an already-running Niri session can wedge Niri's DRM output
# permanently (Page flip commit failed ... Permission denied, needs a hard
# reset) -- libseat falling back to logind does not hand DRM master back and
# forth reliably on this hardware. Any second compositor grabbing a VT hits
# this, not just a Niri-vs-Niri conflict.
add_seat_group() {
  local user="$1"

  if ! id "${user}" &>/dev/null; then
    return
  fi
  if id -nG "${user}" | grep -qw seat; then
    return
  fi
  usermod -aG seat "${user}"
  echo "    added ${user} to the seat group"
}

# greetd's greeter system user defaults to / as its home; GTK/Qt-based
# greeters abort (SIGABRT) trying to create cache/config directories there.
fix_greeter_home() {
  if [[ "$(getent passwd greeter | cut -d: -f6)" == "${GREETER_HOME}" ]]; then
    return
  fi
  mkdir -p "${GREETER_HOME}"
  chown greeter:greeter "${GREETER_HOME}"
  usermod -d "${GREETER_HOME}" greeter
  echo "    set greeter home to ${GREETER_HOME}"
}

write_greetd_config() {
  if [[ -f "${GREETD_CONFIG}" ]]; then
    echo "    ${GREETD_CONFIG} already exists, leaving it alone"
    return
  fi
  mkdir -p "$(dirname "${GREETD_CONFIG}")"
  cat >"${GREETD_CONFIG}" <<'EOF'
[terminal]
vt = 1

[default_session]
command = "/usr/bin/noctalia-greeter-session -- --session niri"
user = "greeter"
EOF
  echo "    wrote ${GREETD_CONFIG}"
}

disable_other_display_managers() {
  local dm
  for dm in "${OTHER_DISPLAY_MANAGERS[@]}"; do
    if systemctl is-enabled --quiet "${dm}" 2>/dev/null; then
      systemctl disable "${dm}"
      echo "    disabled ${dm} (replaced by greetd)"
    fi
  done
}

# The VT-switch flash during the greeter-to-session handoff is a known,
# long-standing niri issue (niri-wm/niri#254): niri-session calls
# `systemctl --user import-environment` with no variable list, which prints
# a deprecation warning straight to the raw console before niri grabs the
# display. The maintainer declined to change the default but endorsed
# patching niri-session locally. The pacman hook re-applies the patch on
# every niri package update; apply it once by hand here too since the hook
# only fires on a transaction, not on an already-installed niri-session.
install_niri_import_environment_hook() {
  mkdir -p "$(dirname "${NIRI_HOOK}")"
  cat >"${NIRI_HOOK}" <<EOF
[Trigger]
Type = Package
Operation = Install
Operation = Upgrade
Target = niri

[Action]
Description = Patch niri-session import-environment deprecation warning
When = PostTransaction
Exec = /usr/bin/env sed -i "s@ systemctl --user import-environment\$@ # ${NIRI_SESSION_PATCH_MARKER}\\n SYSTEMD_COLORS=true systemctl --user import-environment 2>\\&1 | grep -v 'Calling import-environment without a list of variable names is deprecated.'@" ${NIRI_SESSION}
EOF
  echo "    installed ${NIRI_HOOK}"

  if [[ ! -f "${NIRI_SESSION}" ]]; then
    return
  fi
  if grep -qF "${NIRI_SESSION_PATCH_MARKER}" "${NIRI_SESSION}"; then
    return
  fi
  if [[ ! -f "${NIRI_SESSION_BACKUP}" ]]; then
    cp -a "${NIRI_SESSION}" "${NIRI_SESSION_BACKUP}"
  fi
  sed -i "s@ systemctl --user import-environment\$@ # ${NIRI_SESSION_PATCH_MARKER}\n SYSTEMD_COLORS=true systemctl --user import-environment 2>\&1 | grep -v 'Calling import-environment without a list of variable names is deprecated.'@" \
    "${NIRI_SESSION}"
  echo "    patched ${NIRI_SESSION} (VT-switch text flash fix)"
}

install_login_stack() {
  echo "==> Installing greetd + noctalia-greeter login screen"
  if [[ -z "${SUDO_USER:-}" || "${SUDO_USER}" == "root" ]]; then
    echo "Run this script with sudo from your normal user account:" \
      "need a real user to add to the seat group" >&2
    exit 1
  fi

  pacman -S --needed --noconfirm "${LOGIN_STACK_PACKAGES[@]}"

  add_seat_group "${SUDO_USER}"
  add_seat_group greeter
  systemctl enable --now seatd

  fix_greeter_home
  write_greetd_config
  disable_other_display_managers
  systemctl enable greetd

  install_niri_import_environment_hook

  echo "    Reboot required: seat group membership and the new display" \
    "manager only take effect after a reboot."
  echo "    Rotation and scale for the login screen itself are documented in" \
    "GUIDE.md ('Login screen'); sudo tools/check-status.sh verifies this setup."
}

print_next_steps() {
  echo
  echo "Done — reboot to apply the kernel cmdline."
  echo "thermald needs BIOS changes first, see GUIDE.md."
  if niri_detected; then
    echo "Niri + kernel/VBT rotation: use iio-niri 2.3.3+ and"
    echo "  --transform 90 180 270 normal (right-side-up panel)."
    echo "See docs/iio-sensor-proxy.md for the startup transform."
  fi
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
    echo "    Niri detected: not adding kernel panel rotation."
    echo "    Existing kernel/VBT rotation is kept; see Niri setup instructions."
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
    greeter)
      install_login_stack
      return
      ;;
    *)
      echo "Usage: $0 [all|modules|sensor|cmdline|greeter]" \
        "[--panel-rotation|--no-panel-rotation]" >&2
      exit 1
      ;;
  esac

  print_next_steps
}

main "$@"
