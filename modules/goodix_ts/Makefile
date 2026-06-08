DKMS_NAME = goodix_ts
KVER ?= $(shell uname -r | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+')
KVER_MM ?= $(shell uname -r | grep -oE '^[0-9]+\.[0-9]+')
DKMS_VERSION = $(KVER)-minibook1
DKMS_DIR = /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
KERNEL_GIT = https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain
TOUCHSCREEN_DIR = drivers/input/touchscreen
SOURCES = goodix.c goodix.h goodix_fwupload.c

all: .patched.stamp

$(SOURCES):
	curl -fsSL -o $@ "$(KERNEL_GIT)/$(TOUCHSCREEN_DIR)/$@?h=linux-$(KVER_MM).y"

.patched.stamp: $(SOURCES) goodix_resume.patch
	patch -N -p1 < goodix_resume.patch || true
	touch $@

install: .patched.stamp
	install -Dm644 goodix_cfg.bin /lib/firmware/goodix_9110_cfg.bin
	rm -rf "$(DKMS_DIR)"
	mkdir -p "$(DKMS_DIR)"
	cp Kbuild dkms.conf $(SOURCES) "$(DKMS_DIR)/"
	sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="$(DKMS_VERSION)"/' "$(DKMS_DIR)/dkms.conf"
	dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null || true
	dkms add -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms build -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms install --force -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"

uninstall:
	dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null || true
	rm -rf "$(DKMS_DIR)"
	rm -f /lib/firmware/goodix_9110_cfg.bin

clean:
	rm -f $(SOURCES) .patched.stamp

enable:
	modprobe "$(DKMS_NAME)"
	echo "$(DKMS_NAME)" > /etc/modules-load.d/$(DKMS_NAME).conf

disable:
	rm -f /etc/modules-load.d/$(DKMS_NAME).conf
	modprobe -r "$(DKMS_NAME)" 2>/dev/null || true

.PHONY: all install uninstall clean enable disable
