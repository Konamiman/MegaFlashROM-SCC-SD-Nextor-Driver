# MegaFlashROM SCC+ SD driver for Nextor

This repository contains the [MegaFlashROM SCC+ SD](https://www.msxcartridgeshop.com/) (a.k.a. MFRSD) driver for [Nextor](https://github.com/Konamiman/Nextor). It produces a Nextor ROM image that combines a Nextor kernel (v3.0 or newer) base file with this driver, ready to be flashed to the MegaFlashROM SCC+ SD cartridge.

The driver was originally developed by Manuel Pazos, who kindly provided the source code and gave permission for its publication.

Four variants are built by default:

| Output                                                  | Notes                                                                             |
| ------------------------------------------------------- | --------------------------------------------------------------------------------- |
| `Nextor-<ver>.MegaFlashSDSCC.1-slot.ROM`                | One SD card slot.                                                                 |
| `Nextor-<ver>.MegaFlashSDSCC.1-slot.Recovery.ROM`       | Same as above with a 512-byte header for use as the cartridge's recovery payload. |
| `Nextor-<ver>.MegaFlashSDSCC.2-slots.ROM`               | Two SD card slots.                                                                |
| `Nextor-<ver>.MegaFlashSDSCC.2-slots.Recovery.ROM`      | Same as above with the recovery header.                                           |

`<ver>` is the kernel version reported by the Nextor SDK (`nextor-kernel-version.txt`), and any kernel-base variant suffix (e.g. `.NO_UNDOC`) is picked up automatically from the `NEXTOR_BASE` filename, see [Building](#building) below.

The 1-slot vs 2-slots distinction matches the hardware configuration of the cartridge. The Recovery variants are intended to be saved on the SD card and loaded from the cartridge's recovery menu; the regular variants are flashed directly to the cartridge ROM.

## Repository contents

| File                  | Purpose                                                                                              |
| --------------------- | ---------------------------------------------------------------------------------------------------- |
| `driver.asm`          | The MFRSD driver. Pulls in `romdisk.asm` via an `include`.                                           |
| `romdisk.asm`         | ROM-disk feature included from `driver.asm`.                                                         |
| `recovery_header.asm` | 512-byte identification header prepended to the Recovery variants.                                   |
| `Makefile`            | Build rules; see below.                                                                              |
| `docker-build.sh`     | Wrapper that builds the ROMs in the Nextor dev Docker image (no local toolchain needed).             |
| `build-all.sh`        | Builds the ROMs against every kernel base-file variant found in a directory, with the local toolchain. |
| `tools/`              | `MFRFLASH.COM`, a tool to flash the ROMs from the MSX itself (C source and its own Makefile); see [The MFRFLASH tool](#the-mfrflash-tool). |
| `external/Nextor`     | Git submodule pointing at the Nextor repo, sparse-checkout to the `sdk/` directory only.             |

The ASCII8 bank-switching routine consumed by the cartridge mapper comes from the Nextor SDK (`asm/chgbnk/ascii8.asm`), so it isn't vendored in this repo.

## The MFRFLASH tool

`bin/MFRFLASH.COM` (built from `tools/mfrflash.c`) is an MSX-DOS 2 / Nextor command that flashes a ROM file into the Nextor area of a MegaFlashROM SCC+ SD cartridge, and it can do so **even when the cartridge is the DOS controller the computer booted from** (the original `OPFXSD` tool can't, since the kernel it runs on would disappear midway). Usage:

```
MFRFLASH <file> <slot>[-<subslot>]|0 [/f] [/s]
```

`<slot>-<subslot>` is the slot where the Nextor kernel of the cartridge lives (subslot 3 of the cartridge slot, e.g. `2-3` for a cartridge in slot 2), and `0` stands for the primary DOS controller slot, whatever it is. `/f` skips the check that the slot actually contains a MegaFlashROM SCC+ SD. `/s` skips all the confirmation prompts (see below).

How it works: the entire file is first read and cached in RAM (the TPA, plus as many 16K mapped RAM segments as needed, from the primary mapper first and from any other mapper in the system afterwards) and, if that isn't enough, in the VRAM not used by the text screen (48K on machines with 64K of VRAM, 112K with 128K; MSX1 computers and non-text screen modes excluded). Once the file is cached the tool asks for confirmation (showing the actual slot to be flashed, also when `0` was specified); nothing has been written to the flash yet at that point. Only then, with interrupts disabled and without any further disk access, the flash is erased (only as many 64K blocks as the file needs, so a ROM disk installed after a 128K kernel survives the flashing of another 128K kernel) and programmed from the cache, verifying each block after writing it. One dot is printed per 1K read or flashed.

If the flashed slot is a DOS controller in use (the primary one or any other one listed in the disk driver table) the program ends by asking you to reset the computer and hangs there, since the DOS kernel it runs under is gone; otherwise it just returns to DOS. Flash the regular `.ROM` variants, not the `.Recovery.ROM` ones (those are for the recovery menu of the cartridge and carry an extra header): the tool asks for confirmation before flashing a file that doesn't start with the `AB` ROM signature (with `/s` it just prints a warning). The file can't be bigger than the Nextor area of the flash (1024K).

The slot check requires the target to be subslot 3 of an expanded slot (the other subslots of the cartridge hold the recovery ROM, the game area and the RAM, and would be damaged), reads the flash chip ID (a Micron/Numonyx M29W640 with manufacturer code 20h and device code 7Eh, FDh or 5Bh) and verifies that the slot behaves as an ASCII8 mapper, which is how the Nextor area of the cartridge looks like. The mapper part of the check compares the contents of the first two 8K banks, so it fails on a cartridge whose Nextor area has been erased: use `/f` in that case, and only in that case, since with `/f` the tool writes wherever it is pointed at. Before flashing, the tool writes the default value (03h) to the configuration register of the cartridge (in subslot 1 of the same slot) to make sure flash writes are enabled, as `OPFXSD` does.

The tool needs MSX-DOS 2 or Nextor (it relies on the mapper support routines) and any MSX; the VRAM cache is only used on MSX2 and higher.

## Development environment

The quickest path needs **nothing but Docker**: see [Building with the Nextor dev Docker image](#building-with-the-nextor-dev-docker-image) below, which supplies the toolchain, the SDK and the kernel base files for you (no submodule or base file to fetch). To build with a local toolchain instead, you need:

- [**Nestor80**](https://github.com/Konamiman/Nestor80) (`N80`) on your `PATH`, or pointed at via the `N80` make variable.
- **`mknexrom`** on your `PATH`, or pointed at via the `MKNEXROM` make variable. The source lives in the Nextor repository under `buildtools/sources/mknexrom.c`.
- A POSIX **`make`** and `cat`.
- A Nextor kernel base file and the Nextor SDK (the `external/Nextor` submodule, set up with `make setup`).
- For `MFRFLASH.COM` only: [**SDCC**](https://sdcc.sourceforge.net) (`sdcc` and `sdasz80`) and **`objcopy`** (GNU binutils), on your `PATH` or pointed at via the `SDCC`, `SDASZ80` and `OBJCOPY` make variables. `make roms` builds just the ROMs if you don't have them.

## Cloning the repository

This repository uses a git submodule to pull in the Nextor SDK; clone with `--recurse-submodules` and then configure the submodule for a sparse checkout of the `sdk/` directory (the only thing this driver consumes from Nextor):

```sh
git clone --recurse-submodules https://github.com/Konamiman/MegaFlashROM-SD-Nextor-driver.git [<target-dir>]
cd <target-dir>/external/Nextor
git sparse-checkout init --cone
git sparse-checkout set sdk
cd ../..
```

If you already cloned without `--recurse-submodules`, run `git submodule update --init` first.

If you have a local clone of Nextor and want the submodule to point at it (e.g. while developing the SDK locally), override the URL once:

```sh
git config submodule.external/Nextor.url /path/to/your/local/Nextor
git submodule sync
git submodule update --init
```

### If you'd rather not fetch the full Nextor repository

The sequence above clones the entire Nextor repository before the sparse-checkout limits the working tree. If you'd rather only fetch the SDK files (typically <100 KB instead of tens of MB), clone the driver *without* `--recurse-submodules` and then set up the submodule as a blobless partial clone with sparse-checkout from the start:

```sh
git clone https://github.com/Konamiman/MegaFlashROM-SD-Nextor-driver.git [<target-dir>]
cd <target-dir>
git submodule init external/Nextor
git submodule update --init --filter=blob:none external/Nextor
git -C external/Nextor sparse-checkout init --cone
git -C external/Nextor sparse-checkout set sdk
git -C external/Nextor checkout
```

...or, equivalently, just `make setup`:

```sh
git clone https://github.com/Konamiman/MegaFlashROM-SD-Nextor-driver.git [<target-dir>]
cd <target-dir>
make setup
```

## Building

There are two ways to build: with the **Nextor dev Docker image** (no local toolchain, SDK or kernel base file needed) or with a **local toolchain**.

### Building with the Nextor dev Docker image

The [`nextor-dev`](https://github.com/Konamiman/Nextor/pkgs/container/nextor-dev) image bundles `N80`, `mknexrom`, SDCC, the Nextor SDK and both kernel base-file variants, and presets `NEXTOR_BASE` / `NEXTOR_SDK`, so a build needs nothing else on your machine - not even the `external/Nextor` submodule. The `docker-build.sh` wrapper runs the build in a container, mounting this repository and writing the ROMs (and `MFRFLASH.COM`) into `bin/` owned by you (not root):

```sh
./docker-build.sh                       # all four ROMs and MFRFLASH.COM, default kernel base
./docker-build.sh tools                 # just MFRFLASH.COM
./docker-build.sh --variant NO_UNDOC    # build against the NO_UNDOC kernel base
./docker-build.sh --variant all         # build against every base variant
./docker-build.sh clean                 # any extra args are passed to make
```

`--variant <suffix>` selects one of the image's kernel base files (`kernel_base<suffix>.dat`). There is a single suffix, `NO_UNDOC` (no undocumented Z80 opcodes, for Z180-based machines); the other variant is the default, suffix-less base, selected by omitting `--variant`. For the `NO_UNDOC` variant the Makefile assembles the driver undoc-free to match, and the variant suffix is reflected in the output ROM names, exactly as with a local build. This repo builds four ROMs per base variant, so `--variant all` builds against every base file the image ships in a single container (eight ROMs in all; this runs `build-all.sh`, described below, inside the image). Run `./docker-build.sh --help` for the full list.

The image tag used by default is the kernel version this driver is built for (`3.0.0-beta2`); override it with `--image <ref>` or the `NEXTOR_IMAGE` environment variable. Note that the image's `latest` tag tracks stable kernel releases only, so it is not what you want while the driver targets a prerelease.

### Building with a local toolchain

The build needs a Nextor kernel base file, supplied via `NEXTOR_BASE`:

```sh
NEXTOR_BASE=/path/to/Nextor-3.0.0.base.dat make
```

That produces all four ROM variants, plus `MFRFLASH.COM`, in the `bin/` directory. `make roms` builds only the ROMs and `make tools` only the tool (the latter needs SDCC but no kernel base file; it can also be built standalone with `make -C tools`).

For an undoc-instruction-free build (compatible with Z180-based MSX machines), just point `NEXTOR_BASE` at an undoc-free kernel base:

```sh
NEXTOR_BASE=/path/to/Nextor-3.0.0.base.NO_UNDOC.dat make
```

The Nextor base filename's variant suffix (e.g. `.NO_UNDOC`) is mirrored in the output ROM filenames (the version in them comes from the SDK, not from the base filename), and a `NO_UNDOC` in it makes the Makefile assemble the driver without undocumented opcodes (`NO_UNDOC_CPU_INSTRUCTIONS=1`) so that it matches the kernel. The inference only works when the base file follows one of the two naming conventions (`Nextor-<ver>.base[<suffix>].dat` or `kernel_base[<suffix>].dat`); with a base file named otherwise, set `NO_UNDOC_CPU_INSTRUCTIONS` by hand. An explicit value on the command line or in the environment always wins over the inference.

#### Building against every kernel base variant

To build the ROMs for all the kernel base-file variants at once (the local counterpart of `docker-build.sh --variant all`), point `NEXTOR_KERNEL_BASE_DIR` at the directory holding the base files and run `build-all.sh`:

```sh
NEXTOR_KERNEL_BASE_DIR=/path/to/Nextor/bin/kernel-base ./build-all.sh
NEXTOR_KERNEL_BASE_DIR=/path/to/Nextor/bin/kernel-base ./build-all.sh clean-bin all   # extra args go to make
```

There is no list of variants to maintain: the script scans the directory and builds against every `.dat` file there named by either convention the Makefile understands (`Nextor-<ver>.base[<suffix>].dat`, as built by the Nextor repository, or `kernel_base[<suffix>].dat`, as shipped in the Docker image), ordering the builds so the driver is reassembled only once when crossing into the undoc-free (`*NO_UNDOC*`) group. With the two variants of Nextor 3.0 that is eight ROMs. If the directory mixes base files from several kernel versions, the ones matching the SDK's version are used (and the script stops if none do). Run `./build-all.sh --help` for the details.

### Building without `make`

The Makefile is the recommended way, but each ROM is produced by just three tool invocations: two `N80` calls (one for the driver, one for the bank-switching routine) and one `mknexrom` call that combines them with the kernel base. The Recovery variants then prepend a small header binary on top. Here's the sequence for the regular 1-slot variant:

```sh
mkdir -p tmp

# Assemble the driver for 1 SD slot  ->  tmp/driver.1slot.bin
N80 driver.asm tmp/driver.1slot.bin \
    --no-string-escapes --build-type abs --output-file-extension bin \
    --include-directory external/Nextor/sdk \
    --define-symbols NUM_SLOTS=1

# Assemble the ASCII8 bank-switching routine from the SDK  ->  tmp/chgbnk.bin
N80 external/Nextor/sdk/asm/chgbnk/ascii8.asm tmp/chgbnk.bin \
    --no-string-escapes --build-type abs --output-file-extension bin \
    --include-directory external/Nextor/sdk

# Combine kernel base + driver + chgbnk  ->  Nextor-<ver>.MegaFlashSDSCC.1-slot.ROM
mknexrom /path/to/Nextor-<ver>.base.dat Nextor-<ver>.MegaFlashSDSCC.1-slot.ROM \
    /d:tmp/driver.1slot.bin /m:tmp/chgbnk.bin
```

The other three variants are slight modifications of the same recipe:

- **2-slots**: change `--define-symbols NUM_SLOTS=1` to `NUM_SLOTS=2`, write the driver to a different filename (e.g. `tmp/driver.2slots.bin`), and pick the matching `Nextor-<ver>.MegaFlashSDSCC.2-slots.ROM` output name.
- **Recovery**: assemble `recovery_header.asm` into a small `.bin`, then concatenate it in front of the corresponding regular ROM:
  ```sh
  N80 recovery_header.asm tmp/ \
      --no-string-escapes --build-type abs --output-file-extension bin \
      --include-directory external/Nextor/sdk
  cat tmp/recovery_header.bin Nextor-<ver>.MegaFlashSDSCC.1-slot.ROM \
      > Nextor-<ver>.MegaFlashSDSCC.1-slot.Recovery.ROM
  ```
- **NO_UNDOC**: add `--define-symbols NO_UNDOC_CPU_INSTRUCTIONS` to *every* `N80` call (regardless of which variant you're building), and use a `Nextor-<ver>.base.NO_UNDOC.dat` kernel base. The driver-side and base-side undoc settings must match.

## Make variables

| Variable                    | Purpose                                                              | Default                  |
| --------------------------- | -------------------------------------------------------------------- | ------------------------ |
| `NEXTOR_BASE`               | Path to the Nextor kernel base `.dat` file (mandatory).              | _(unset; error)_         |
| `NEXTOR_SDK`                | Path to the Nextor SDK directory (the one containing `asm/`).        | `external/Nextor/sdk`    |
| `N80`                       | Path to the Nestor80 assembler.                                      | `N80` (from `PATH`)      |
| `MKNEXROM`                  | Path to the `mknexrom` tool.                                         | `mknexrom` (from `PATH`) |
| `NO_UNDOC_CPU_INSTRUCTIONS` | If non-empty (e.g. `=1`), assemble the driver without undocumented opcodes. | _inferred: `1` if the `NEXTOR_BASE` filename's variant suffix contains `NO_UNDOC`, unset otherwise_ |
| `SDCC`, `SDASZ80`, `OBJCOPY` | Paths to the SDCC compiler and assembler and to GNU `objcopy`, used to build `MFRFLASH.COM` only. | `sdcc`, `sdasz80`, `objcopy` (from `PATH`) |

Build and cleanup targets:

| Target           | Effect                                                                                                          |
| ---------------- | --------------------------------------------------------------------------------------------------------------- |
| `make` / `make all` | Builds the four ROMs and `MFRFLASH.COM`.                                                                     |
| `make roms`      | Builds only the four ROMs.                                                                                      |
| `make tools`     | Builds only `MFRFLASH.COM` (no `NEXTOR_BASE` needed).                                                           |
| `make clean`     | Removes `tmp/` (intermediate `.bin` files, the SDCC output and helper artifacts). `bin/` and the shippable files in it are kept. |
| `make clean-bin` | Removes `bin/` (the shippable ROMs and tool).                                                                   |
| `make distclean` | Removes both `tmp/` and `bin/`.                                                                                 |

## License

MIT - see [LICENSE](LICENSE). Note that [Nextor itself has a different license](https://github.com/Konamiman/Nextor/blob/master/LICENSE.md).
