#!/bin/bash
# SPDX-License-Identifier: 0BSD
#
# Bootstrap a CHUWI MiniBook X on Ubuntu/Debian. Idempotent: safe to re-run.
#
#   sudo tools/bootstrap-ubuntu.sh            # everything except the VBT refresh rate
#   sudo tools/bootstrap-ubuntu.sh modules    # DKMS modules only
#   sudo tools/bootstrap-ubuntu.sh sensor     # iio-sensor-proxy only
#   sudo tools/bootstrap-ubuntu.sh cmdline    # kernel cmdline only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_DIR="${SCRIPT_DIR}/.."
readonly GRUB_CONF="/etc/default/grub"
readonly APT_PIN="/etc/apt/preferences.d/iio-sensor-proxy-fork.pref"

readonly MODULES=(i2c_designware_spklen goodix_ts minibook_ec dptf_enabler)

# Rotation is handled by the kernel so that the boot splash and LUKS prompt are
# upright too; iio-sensor-proxy then reports orientation relative to that.
readonly CMDLINE_ARGS=(
  "video=DSI-1:panel_orientation=right_side_up"
  "mem_sleep_default=deep"
  "i915.enable_psr=0"
)

# Text consoles need their own rotation on top of the panel orientation, and
# fbcon=rotate: on the cmdline is ignored under i915's fbdev emulation -- it
# leaves fbcon/rotate at 0. Writing rotate_all at runtime does work.
readonly FBCON_ROTATE=1
readonly FBCON_TMPFILES="/etc/tmpfiles.d/fbcon-rotate.conf"

readonly APT_PACKAGES=(
  build-essential meson ninja-build pkgconf clang git curl patch
  dkms "linux-headers-$(uname -r)" acpi-call-dkms
  libglib2.0-dev libgudev-1.0-dev libpolkit-gobject-1-dev systemd-dev libudev-dev
)

require_root() {
  if (( EUID != 0 )); then
    echo "This script must be run as root" >&2
    exit 1
  fi
}

install_packages() {
  echo "==> Installing packages"
  apt-get update -qq
  apt-get install -y -o DPkg::Lock::Timeout=180 "${APT_PACKAGES[@]}"
}

enable_acpi_call() {
  echo "==> Enabling acpi_call"
  modprobe acpi_call
  echo acpi_call >/etc/modules-load.d/acpi_call.conf
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

# goodix_ts is force-loaded by its modules-load.d entry, so dracut bundles it and
# it probes inside the initramfs -- long before the real root appears, which on
# an encrypted disk waits on the passphrase. Its OEM config has to travel with
# it or request_firmware fails and the controller falls back to its built-in
# config.
bundle_goodix_firmware() {
  if [[ ! -d /etc/dracut.conf.d ]]; then
    return
  fi

  echo "==> Bundling goodix config into the initramfs"
  echo 'install_items+=" /lib/firmware/goodix_9110_cfg.bin "' \
    >/etc/dracut.conf.d/91-goodix-fw.conf
  update-initramfs -u -k all
}

install_sensor_proxy() {
  echo "==> Installing patched iio-sensor-proxy"

  # The distro package installs to the same paths and would shadow the fork.
  apt-get remove -y -o DPkg::Lock::Timeout=180 iio-sensor-proxy || true
  cat >"${APT_PIN}" <<'EOF'
Package: iio-sensor-proxy
Pin: release *
Pin-Priority: -1
EOF

  make -C "${REPO_DIR}/iio-sensor-proxy"
  make -C "${REPO_DIR}/iio-sensor-proxy" install

  systemctl daemon-reload
  udevadm control --reload-rules && udevadm trigger
  systemctl restart iio-sensor-proxy
}

update_cmdline() {
  echo "==> Updating kernel cmdline"
  local arg changed=0

  cp -a "${GRUB_CONF}" "${GRUB_CONF}.bak-$(date +%F)"

  for arg in "${CMDLINE_ARGS[@]}"; do
    if grep -q -- "${arg}" "${GRUB_CONF}"; then
      continue
    fi
    sed -i "/^GRUB_CMDLINE_LINUX_DEFAULT=/s|\"$| ${arg}\"|" "${GRUB_CONF}"
    echo "    added ${arg}"
    changed=1
  done

  if (( changed )); then
    update-grub
  else
    echo "    already up to date"
  fi

  rotate_consoles
}

rotate_consoles() {
  echo "==> Rotating text consoles"
  echo "w /sys/class/graphics/fbcon/rotate_all - - - - ${FBCON_ROTATE}" \
    >"${FBCON_TMPFILES}"
  systemd-tmpfiles --create "${FBCON_TMPFILES}" || true
}

main() {
  require_root

  case "${1:-all}" in
  all)
    install_packages
    enable_acpi_call
    install_modules
    install_sensor_proxy
    update_cmdline
    ;;
  modules)
    install_packages
    enable_acpi_call
    install_modules
    ;;
  sensor)
    install_sensor_proxy
    ;;
  cmdline)
    update_cmdline
    ;;
  *)
    echo "Usage: $0 [all|modules|sensor|cmdline]" >&2
    exit 1
    ;;
  esac

  echo
  echo "Done — reboot to apply the kernel cmdline."
  echo "Refresh rate is separate: sudo tools/update-vbt-clock.sh 90"
  echo "thermald needs BIOS changes first, see GUIDE.md."
}

main "$@"
