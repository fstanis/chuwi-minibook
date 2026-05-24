TARGET = vbt_patch
SRCS = vbt_patch.c
OBJS = $(SRCS:.c=.o)

# Evitamos el error 404 usando la ruta de Torvalds limpia
KERNEL_GIT = https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
I915_DIR = drivers/gpu/drm/i915/display
HEADERS = intel_vbt_defs.h intel_dsi_vbt_defs.h

CC = clang
CFLAGS = -Wall -Wextra -O2

all: $(HEADERS) $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Descarga de cabeceras sin el sufijo +deb13
$(HEADERS):
	@echo "Descargando $@ para el parche de video..."
	curl -fsSL -o $@ "$(KERNEL_GIT)/$(I915_DIR)/$@"

clean:
	rm -f $(TARGET) $(OBJS) $(HEADERS)

.PHONY: all clean
