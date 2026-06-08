# AGENTS.md

Project context for AI coding agents working in this repository.

## Project

Kernel modules, patches and tools for getting Linux fully working on the
**CHUWI MiniBook X** (Intel N150, Alder Lake-N).

## Repository Structure

Each top-level directory is a self-contained component. There is no
unified build system.

| Directory                        | What it is                                                                                                    |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `modules/goodix_ts/`             | DKMS kernel module -- patches upstream Goodix touchscreen driver for resume fix + OEM config                  |
| `modules/minibook_ec/`           | DKMS kernel module -- EC platform driver: touchpad/keyboard toggle, SoC thermal zone, EC version, BIOS unlock |
| `modules/dptf_enabler/`          | DKMS kernel module -- unhides BIOS-gated Intel DPTF devices by setting GNVS flags                             |
| `modules/i2c_designware_spklen/` | DKMS kernel module -- I2C spike suppression for Intel LPSS controllers                                        |
| `thermal_daemon/`                | Fork of intel/thermal_daemon with device-specific config                                                      |
| `iio-sensor-proxy/`              | Fork of iio-sensor-proxy with dual-accelerometer MXC6655 driver for hinge angle detection                     |
| `vbt_patch/`                     | VBT patcher for DSI panel refresh rate                                                                        |
| `tools/`                         | Status check, hardware detection, VBT clock update scripts                                                    |
| `docs/`                          | Hardware documentation: device inventory, EC register map, ACPI tables                                        |

## Code Style

- Avoid comments. If a section needs explanation, extract it into a
  well-named function.
- **Keep functions small**: rarely exceed 20 lines. If a function has
  "sections" (initialization, processing, logging), extract each into
  its own function.
- **Do one thing**: each function performs exactly one task (Single
  Responsibility).
- **Single level of abstraction**: don't mix high-level logic with
  low-level details (regex, buffer manipulation, bit twiddling) in the
  same function.
- **Avoid deep nesting**: use early returns/guard clauses to keep
  indentation shallow.
- No ternary operators. Use explicit `if`/`else`.
- Always use braces for `if`/`else`/`for`/`while` bodies, even
  single-line.
- **Exception**: when working in an existing codebase with different
  conventions (e.g. git subtrees, upstream forks), follow that
  codebase's style instead.

## Conventions

### Installation

Every installable component should have a `Makefile` with `install` and
`uninstall` targets. For kernel modules: `make install` copies source to
`/usr/src/` and runs DKMS (`dkms add/build/install`).

### Kernel modules

- Follow the official Linux kernel coding style
  (`Documentation/process/coding-style.rst`)
- Build with `make` (standard kbuild `obj-m` pattern)
- `dkms.conf` files detect the target kernel's compiler at build time via a
  `LLVM_FLAG=$(grep -sq CC_IS_CLANG=y ${kernel_source_dir}/.config && echo
  LLVM=1)` line; the value (either `LLVM=1` or empty) is interpolated into
  `MAKE[0]`. This picks `LLVM=1` for clang-built kernels (CachyOS, some Arch
  builds, …) and gcc for everyone else (Debian, RHEL, openSUSE, …), and works
  for DKMS auto-rebuild on kernel upgrades because the check reads the *target*
  kernel's `.config`.
- Include `dkms.conf` for DKMS packaging
- When patching an existing upstream driver: `make` downloads the kernel
  source files for the running kernel version and applies a `.patch`;
  `make install` builds via DKMS -- don't carry full copies of upstream
  driver source
- Verify after loading with `dmesg | grep <module-name>`

### Forks / patched upstream code

- Managed as git subtrees (not submodules)
- Follow the upstream project's existing coding style and build system
- Keep changes minimal and isolated to make upstreaming easy

## Bash Style

Based on the Google Shell Style Guide. Apply these rules when writing or
editing any bash script in this repository.

### File Structure

1. `#!/bin/bash` shebang (always bash, never sh/zsh)
1. `# SPDX-License-Identifier: 0BSD`
1. `set -euo pipefail`
1. Constants (`readonly`, `UPPER_SNAKE_CASE`), declared at the top
1. All function definitions (no executable code between functions)
1. `main "$@"` at the bottom (if the script has any functions)

### Formatting

- **2-space indentation**, no tabs
- **80 character** max line length (long paths/URLs are exceptions, but
  put them on their own line or in a variable)
- `; then` and `; do` on the same line as `if`/`for`/`while`
- `else`, `fi`, `done` on their own lines, vertically aligned with the
  opener
- Opening brace `{` on the same line as the function name, no space
  before `()`
- Split long pipelines one segment per line with `\` continuation and
  2-space indent:
  ```bash
  command1 \
    | command2 \
    | command3
  ```
- Split long `||`/`&&` chains the same way

### Naming

- **Functions**: `lower_snake_case()` -- brace on same line, no space
  before parens
- **Variables**: `lower_snake_case`
- **Constants/exports**: `UPPER_SNAKE_CASE`, declared with `readonly` or
  `declare -xr`
- **Filenames**: `lower_snake_case.sh` (or no extension if installed to
  PATH)

### Variables and Quoting

- Always quote strings containing variables, command substitutions,
  spaces or shell metacharacters
- Prefer `"${var}"` over `"$var"` for non-special variables
- Don't brace-delimit single-character specials/positionals (`$1`, `$?`,
  `$@`) unless needed for disambiguation
- Never quote literal integers: `value=32`
- Always quote command substitutions: `result="$(some_command)"`
- Use `"$@"` to pass arguments (not `$*`)

### Functions and Variables

- Use `local` for all function-scoped variables
- Separate `local` declaration from command-substitution assignment (so
  `$?` works):
  ```bash
  local my_var
  my_var="$(some_command)"
  ```
- Loop variables must be declared `local` inside functions

### Control Flow and Tests

- Use `[[ ... ]]` (not `[ ... ]` or `test`)
- Use `==` for string equality (not `=`)
- Use `(( ... ))` for arithmetic comparisons (not `[[ ... ]]` with
  `<`/`>`)
- Use `-z`/`-n` for empty/non-empty string tests

### Arrays

- Use arrays for lists of elements
- Never store lists in strings

### Command Substitution and Arithmetic

- Use `$(command)` (not backticks)
- Use `$(( ... ))` or `(( ... ))` for arithmetic (never `let`, `$[ ]`
  or `expr`)
- Variables inside `$(( ))` don't need `$` or `${}`

### Error Handling

- Errors to stderr: `echo "Error: ..." >&2`
- Always check return values -- use `if ! command; then` or check `$?`
- Be careful with `(( expr ))` under `set -e` -- `(( 0 ))` returns
  non-zero

### Other Rules

- Prefer bash builtins over external commands (parameter expansion over
  `sed`, `(( ))` over `expr`, `[[ =~ ]]` over `grep`)
