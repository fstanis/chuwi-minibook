CC = clang
CFLAGS = -Wall -Wextra -O2

KERNEL_GIT = https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
I915_DISPLAY = drivers/gpu/drm/i915/display
HEADERS = intel_vbt_defs.h intel_dsi_vbt_defs.h

vbt_patch: vbt_patch.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ vbt_patch.c

$(HEADERS):
	curl -fsSL -o $@ "$(KERNEL_GIT)/$(I915_DISPLAY)/$@"

all: vbt_patch

clean:
	rm -f vbt_patch

distclean: clean
	rm -f $(HEADERS)

.PHONY: all install uninstall clean distclean
