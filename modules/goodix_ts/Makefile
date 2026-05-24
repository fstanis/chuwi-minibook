DKMS_NAME = goodix_ts
# Extraemos la versión para la carpeta de DKMS
KVER_FULL ?= $(shell uname -r | cut -d+ -f1)
DKMS_VERSION = $(KVER_FULL)-minibook1
DKMS_DIR = /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

KERNEL_GIT = https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
TOUCHSCREEN_DIR = drivers/input/touchscreen
SOURCES = goodix.c goodix.h goodix_fwupload.c

# Ruta de compilación para Debian
KDIR ?= /lib/modules/$(shell uname -r)/build

all: .patched.stamp
	make -C $(KDIR) M=$(PWD) modules

# Descarga limpia (la que te funcionó)
$(SOURCES):
	@echo "Descargando $@ desde el repositorio de Torvalds..."
	curl -fsSL -o $@ "$(KERNEL_GIT)/$(TOUCHSCREEN_DIR)/$@"

# Aplicar el parche de Chuwi (resume fix)
.patched.stamp: $(SOURCES) goodix_resume.patch
	@echo "Aplicando parches de Chuwi para el táctil..."
	patch -N -p1 < goodix_resume.patch || true
	touch $@

# Instalación estilo Debian/DKMS
install: .patched.stamp
	install -Dm644 goodix_cfg.bin /lib/firmware/goodix_9110_cfg.bin
	rm -rf "$(DKMS_DIR)"
	mkdir -p "$(DKMS_DIR)"
	cp Kbuild dkms.conf $(SOURCES) "$(DKMS_DIR)/"
	# Quitamos LLVM=1 para Debian en el dkms.conf copiado
	sed -i 's/LLVM=1 //g' "$(DKMS_DIR)/dkms.conf"
	sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="$(DKMS_VERSION)"/' "$(DKMS_DIR)/dkms.conf"
	dkms add -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms build -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"
	dkms install --force -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)"

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f $(SOURCES) .patched.stamp

uninstall:
	dkms remove -m "$(DKMS_NAME)" -v "$(DKMS_VERSION)" --all 2>/dev/null || true
	rm -rf "$(DKMS_DIR)"
	rm -f /lib/firmware/goodix_9110_cfg.bin

.PHONY: all install uninstall clean
