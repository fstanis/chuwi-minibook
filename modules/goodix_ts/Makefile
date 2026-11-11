DKMS_NAME = goodix_ts
KVER ?= $(shell uname -r | grep -oE '^[0-9]+\.[0-9]+(\.[0-9]+)?')
SOURCE_VERSION = $(shell echo '$(KVER)' | sed -E 's/^([0-9]+\.[0-9]+)\.0$$/\1/')
PATCH_REVISION = $(shell cat goodix_resume.patch Kbuild dkms.conf | sha256sum | cut -c1-12)
DKMS_VERSION = $(SOURCE_VERSION).minibook1.$(PATCH_REVISION)
SOURCE_DIR = .sources/$(DKMS_VERSION)
SOURCES = goodix.c goodix.h goodix_fwupload.c

all: $(SOURCE_DIR)/.patched.stamp

$(SOURCE_DIR)/.patched.stamp: goodix_resume.patch Kbuild dkms.conf prepare_sources.sh
	bash prepare_sources.sh "$(SOURCE_VERSION)" "$(SOURCE_DIR)" goodix_resume.patch

install: all
	bash manage_dkms.sh install "$(DKMS_VERSION)" "$(SOURCE_DIR)"

uninstall:
	bash manage_dkms.sh uninstall

clean:
	rm -rf .sources
	rm -f $(SOURCES) .patched.stamp

dkms-version:
	@echo "$(DKMS_VERSION)"

source-dir:
	@echo "$(SOURCE_DIR)"

enable:
	modprobe "$(DKMS_NAME)"
	echo "$(DKMS_NAME)" > /etc/modules-load.d/$(DKMS_NAME).conf

disable:
	rm -f /etc/modules-load.d/$(DKMS_NAME).conf
	modprobe -r "$(DKMS_NAME)" 2>/dev/null || true

.PHONY: all install uninstall clean dkms-version source-dir enable disable
