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

`<ver>` is the kernel version reported by the Nextor SDK (`nextor-kernel-version.txt`), and any kernel-base variant suffix (e.g. `.NO_UNDOC.SHIFT_INV.KANJI_INV`) is picked up automatically from the `NEXTOR_BASE` filename, see [Building](#building) below.

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
| `external/Nextor`     | Git submodule pointing at the Nextor repo, sparse-checkout to the `sdk/` directory only.             |

The ASCII8 bank-switching routine consumed by the cartridge mapper comes from the Nextor SDK (`asm/chgbnk/ascii8.asm`), so it isn't vendored in this repo.

## Development environment

The quickest path needs **nothing but Docker**: see [Building with the Nextor dev Docker image](#building-with-the-nextor-dev-docker-image) below, which supplies the toolchain, the SDK and the kernel base files for you (no submodule or base file to fetch). To build with a local toolchain instead, you need:

- [**Nestor80**](https://github.com/Konamiman/Nestor80) (`N80`) on your `PATH`, or pointed at via the `N80` make variable.
- **`mknexrom`** on your `PATH`, or pointed at via the `MKNEXROM` make variable. The source lives in the Nextor repository under `buildtools/sources/mknexrom.c`.
- A POSIX **`make`** and `cat`.
- A Nextor kernel base file and the Nextor SDK (the `external/Nextor` submodule, set up with `make setup`).

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

The [`nextor-dev`](https://github.com/Konamiman/Nextor/pkgs/container/nextor-dev) image bundles `N80`, `mknexrom`, the Nextor SDK and all twelve kernel base-file variants, and presets `NEXTOR_BASE` / `NEXTOR_SDK`, so a build needs nothing else on your machine - not even the `external/Nextor` submodule. The `docker-build.sh` wrapper runs the build in a container, mounting this repository and writing the ROMs into `bin/` owned by you (not root):

```sh
./docker-build.sh                       # all four ROMs, default kernel base
./docker-build.sh --variant NO_UNDOC    # build against the NO_UNDOC kernel base
./docker-build.sh --variant CTRL_INV
./docker-build.sh --variant NO_UNDOC.SHIFT_INV
./docker-build.sh --variant KANJI_INV
./docker-build.sh --variant NO_UNDOC.CTRL_INV.KANJI_INV
./docker-build.sh --variant all         # build against every base variant
./docker-build.sh clean                 # any extra args are passed to make
```

`--variant <suffix>` selects one of the image's kernel base files (`kernel_base<suffix>.dat`). The variants combine three independent axes: `NO_UNDOC` (no undocumented Z80 opcodes, for Z180-based machines), `SHIFT_INV` _or_ `CTRL_INV` (the SHIFT or CTRL boot key inverted), and `KANJI_INV` (the "6" boot key inverted, so the Kanji driver is installed at boot unless the key is pressed; always the last component of the suffix). The eleven suffixes are therefore `NO_UNDOC`, `SHIFT_INV`, `CTRL_INV`, `NO_UNDOC.SHIFT_INV`, `NO_UNDOC.CTRL_INV`, each of these with `.KANJI_INV` appended, and plain `KANJI_INV`; the twelfth variant is the default, suffix-less base, selected by omitting `--variant`. For a `*NO_UNDOC*` variant the Makefile assembles the driver undoc-free to match, and the variant suffix is reflected in the output ROM names, exactly as with a local build. This repo builds four ROMs per base variant, so `--variant all` builds against every base file the image ships in a single container (48 ROMs in all; this runs `build-all.sh`, described below, inside the image). Run `./docker-build.sh --help` for the full list.

The image tag used by default is the kernel version this driver is built for (`3.0.0-beta1`); override it with `--image <ref>` or the `NEXTOR_IMAGE` environment variable. Note that the image's `latest` tag tracks stable kernel releases only, so it is not what you want while the driver targets a prerelease.

### Building with a local toolchain

The build needs a Nextor kernel base file, supplied via `NEXTOR_BASE`:

```sh
NEXTOR_BASE=/path/to/Nextor-3.0.0.base.dat make
```

That produces all four ROM variants in the `bin/` directory.

For an undoc-instruction-free build (compatible with Z180-based MSX machines), just point `NEXTOR_BASE` at an undoc-free kernel base:

```sh
NEXTOR_BASE=/path/to/Nextor-3.0.0.base.NO_UNDOC.dat make
```

The Nextor base filename's variant suffix (e.g. `.NO_UNDOC.SHIFT_INV.KANJI_INV`) is mirrored in the output ROM filenames (the version in them comes from the SDK, not from the base filename), and a `NO_UNDOC` in it makes the Makefile assemble the driver without undocumented opcodes (`NO_UNDOC_CPU_INSTRUCTIONS=1`) so that it matches the kernel. The inference only works when the base file follows one of the two naming conventions (`Nextor-<ver>.base[<suffix>].dat` or `kernel_base[<suffix>].dat`); with a base file named otherwise, set `NO_UNDOC_CPU_INSTRUCTIONS` by hand. An explicit value on the command line or in the environment always wins over the inference.

#### Building against every kernel base variant

To build the ROMs for all the kernel base-file variants at once (the local counterpart of `docker-build.sh --variant all`), point `NEXTOR_KERNEL_BASE_DIR` at the directory holding the base files and run `build-all.sh`:

```sh
NEXTOR_KERNEL_BASE_DIR=/path/to/Nextor/bin/kernel-base ./build-all.sh
NEXTOR_KERNEL_BASE_DIR=/path/to/Nextor/bin/kernel-base ./build-all.sh clean-bin all   # extra args go to make
```

There is no list of variants to maintain: the script scans the directory and builds against every `.dat` file there named by either convention the Makefile understands (`Nextor-<ver>.base[<suffix>].dat`, as built by the Nextor repository, or `kernel_base[<suffix>].dat`, as shipped in the Docker image), ordering the builds so the driver is reassembled only once when crossing into the undoc-free (`*NO_UNDOC*`) group. With the twelve variants of Nextor 3.0 that is 48 ROMs. If the directory mixes base files from several kernel versions, the ones matching the SDK's version are used (and the script stops if none do). Run `./build-all.sh --help` for the details.

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

Cleanup targets:

| Target           | Effect                                                                                                          |
| ---------------- | --------------------------------------------------------------------------------------------------------------- |
| `make clean`     | Removes `tmp/` (intermediate `.bin` files and helper artifacts). `bin/` and the shippable ROMs in it are kept.  |
| `make clean-bin` | Removes `bin/` (the shippable ROMs).                                                                            |
| `make distclean` | Removes both `tmp/` and `bin/`.                                                                                 |

## License

MIT - see [LICENSE](LICENSE). Note that [Nextor itself has a different license](https://github.com/Konamiman/Nextor/blob/master/LICENSE.md).
