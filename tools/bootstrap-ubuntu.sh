#!/bin/bash
# SPDX-License-Identifier: 0BSD
#
# Bootstrap a CHUWI MiniBook X on Ubuntu/Debian. Idempotent: safe to re-run.
#
#   sudo tools/bootstrap-ubuntu.sh            # all but the refresh rate
#   sudo tools/bootstrap-ubuntu.sh modules    # DKMS modules only
#   sudo tools/bootstrap-ubuntu.sh sensor     # iio-sensor-proxy only
#   sudo tools/bootstrap-ubuntu.sh cmdline    # kernel cmdline only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly REPO_DIR="${SCRIPT_DIR}/.."
readonly GRUB_CONF="/etc/default/grub"
readonly APT_PIN="/etc/apt/preferences.d/iio-sensor-proxy-fork.pref"

readonly GOODIX_FIRMWARE="/lib/firmware/goodix_9110_cfg.bin"
readonly DRACUT_GOODIX_CONF="/etc/dracut.conf.d/91-goodix-fw.conf"
readonly INITRAMFS_GOODIX_HOOK="/etc/initramfs-tools/hooks/goodix-fw"

readonly MODULES=(i2c_designware_spklen goodix_ts minibook_ec dptf_enabler)

# Rotate in the kernel so the boot splash and LUKS prompt come up upright too.
readonly CMDLINE_ARGS=(
  "video=DSI-1:panel_orientation=right_side_up"
  "mem_sleep_default=deep"
  "i915.enable_psr=0"
)

# Console rotation does not compose additively with the panel orientation, so
# try all four values rather than deriving one. See GUIDE.md.
readonly FBCON_ROTATE=1
readonly FBCON_TMPFILES="/etc/tmpfiles.d/fbcon-rotate.conf"

readonly APT_PACKAGES=(
  build-essential meson ninja-build pkgconf clang git curl patch
  dkms "linux-headers-$(uname -r)" acpi-call-dkms
  libglib2.0-dev libgudev-1.0-dev libpolkit-gobject-1-dev systemd-dev
  libudev-dev
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

write_dracut_goodix_conf() {
  echo "install_items+=\" ${GOODIX_FIRMWARE} \"" >"${DRACUT_GOODIX_CONF}"
}

write_initramfs_tools_goodix_hook() {
  cat >"${INITRAMFS_GOODIX_HOOK}" <<EOF
#!/bin/sh
[ "\$1" = prereqs ] && { echo; exit 0; }
. /usr/share/initramfs-tools/hook-functions
copy_file firmware "${GOODIX_FIRMWARE}"
EOF
  chmod +x "${INITRAMFS_GOODIX_HOOK}"
}

rebuild_initramfs() {
  if command -v update-initramfs &>/dev/null; then
    update-initramfs -u -k all
  else
    dracut -f --regenerate-all
  fi
}

# goodix_ts probes inside the initramfs, long before the real root exists, so
# its OEM config has to travel with it or the controller silently falls back to
# the built-in one.
bundle_goodix_firmware() {
  echo "==> Bundling goodix config into the initramfs"

  if [[ -d /etc/dracut.conf.d ]]; then
    write_dracut_goodix_conf
  elif [[ -d /etc/initramfs-tools/hooks ]]; then
    write_initramfs_tools_goodix_hook
  else
    echo "    neither dracut nor initramfs-tools found, skipping" >&2
    return
  fi

  rebuild_initramfs
}

pin_distro_sensor_proxy() {
  cat >"${APT_PIN}" <<'EOF'
Package: iio-sensor-proxy
Pin: release *
Pin-Priority: -1
EOF
}

install_sensor_proxy() {
  echo "==> Installing patched iio-sensor-proxy"

  # The distro package installs to the same paths and would shadow the fork.
  apt-get remove -y -o DPkg::Lock::Timeout=180 iio-sensor-proxy || true
  pin_distro_sensor_proxy

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
    if grep -qF -- "${arg}" "${GRUB_CONF}"; then
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

main() {
  require_root

  case "${1:-all}" in
    all)
      install_packages
      enable_acpi_call
      install_modules
      install_sensor_proxy
      update_cmdline
      rotate_consoles
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
      rotate_consoles
      ;;
    *)
      echo "Usage: $0 [all|modules|sensor|cmdline]" >&2
      exit 1
      ;;
  esac

  print_next_steps
}

main "$@"
