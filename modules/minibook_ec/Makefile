DKMS_NAME = minibook_ec
DKMS_VERSION = 1.0
DKMS_DIR = /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

SOURCES = minibook_ec_core.c minibook_ec_sysfs.c minibook_ec_thermal.c \
          minibook_ec_fan.c minibook_ec_i2ec.c minibook_ec_kbd_backlight.c \
          minibook_ec.h

all:
	@echo "Nothing to build (DKMS builds against the running kernel at install time)"

install:
	rm -rf "$(DKMS_DIR)"
	mkdir -p "$(DKMS_DIR)"
	cp Kbuild dkms.conf $(SOURCES) "$(DKMS_DIR)/"
	dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null || true
	dkms add -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms build -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms install -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"

uninstall:
	dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null || true
	rm -rf "$(DKMS_DIR)"

clean:
	rm -f *.o *.ko *.mod *.mod.c *.mod.o modules.order Module.symvers .*.cmd
	rm -f compile_commands.json

compile_commands.json:
	make -C /lib/modules/$(shell uname -r)/build \
		M=$(CURDIR) LLVM=1 modules
	/lib/modules/$(shell uname -r)/build/scripts/clang-tools/gen_compile_commands.py \
		-d $(CURDIR) -o compile_commands.json

install-arch:
	cd packaging/arch && makepkg -f
	-sudo dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null
	-sudo rm -rf "$(DKMS_DIR)"
	sudo pacman -U --noconfirm packaging/arch/$(DKMS_NAME)-dkms-$(DKMS_VERSION)-*.pkg.tar.zst

enable:
	modprobe "$(DKMS_NAME)"
	echo "$(DKMS_NAME)" > /etc/modules-load.d/$(DKMS_NAME).conf

disable:
	rm -f /etc/modules-load.d/$(DKMS_NAME).conf
	modprobe -r "$(DKMS_NAME)" 2>/dev/null || true

.PHONY: all install install-arch uninstall clean enable disable compile_commands.json
