# Makefile for the MegaFlashROM SCC+ SD driver for Nextor 3.
#
# By default builds four ROMs (combining this driver with the Nextor
# kernel base file pointed at by NEXTOR_BASE), placed in `bin/`:
#
#   * bin/Nextor-<ver>.MegaFlashSDSCC.1-slot.ROM             - 1-slot variant
#   * bin/Nextor-<ver>.MegaFlashSDSCC.1-slot.Recovery.ROM    - 1-slot, prefixed with the recovery header
#   * bin/Nextor-<ver>.MegaFlashSDSCC.2-slots.ROM            - 2-slots variant
#   * bin/Nextor-<ver>.MegaFlashSDSCC.2-slots.Recovery.ROM   - 2-slots, prefixed with the recovery header
#
# Intermediate build artifacts (.bin files produced by N80) go to
# `tmp/` and are dropped by `make clean`. The shippable ROMs in `bin/`
# survive `make clean` and are removed only by `make clean-bin` (or
# `make distclean` for both).
#
# <ver> and any variant suffix (e.g. ".NO_UNDOC.SHIFT_INV") are taken
# from the NEXTOR_BASE filename, which must follow the convention
# Nextor-<ver>.base[<suffix>].dat as produced by the Nextor kernel
# Makefile. If NEXTOR_BASE has a non-standard filename, the ROMs are
# named after that filename's stem instead.


### Configurable variables ###################################################

# NEXTOR_BASE: path to the Nextor kernel base .dat file (mandatory for
# every target except `setup` and the clean ones).
ifeq ($(strip $(NEXTOR_BASE)),)
ifeq ($(filter setup clean clean-bin distclean,$(MAKECMDGOALS)),)
$(error NEXTOR_BASE is not set. Point it at a Nextor kernel base .dat file)
endif
else
ifeq ($(wildcard $(NEXTOR_BASE)),)
$(error NEXTOR_BASE points at '$(NEXTOR_BASE)' which does not exist)
endif
endif

# NEXTOR_SDK: path to the Nextor SDK directory (the one containing 'asm/').
# Defaults to the bundled git submodule.
NEXTOR_SDK ?= external/Nextor/sdk

# Tool overrides. Default to invoking the executables from PATH.
N80      ?= N80
MKNEXROM ?= mknexrom

# NO_UNDOC_CPU_INSTRUCTIONS: when set (e.g. =1) the driver is assembled
# with undocumented Z80 opcodes (those operating on ixh/ixl/iyh/iyl)
# replaced with documented equivalents, for compatibility with
# Z180-based MSX machines. Set this whenever NEXTOR_BASE points at a
# .NO_UNDOC. variant; the driver developer is responsible for keeping
# the two consistent.
NO_UNDOC_CPU_INSTRUCTIONS ?=


### Output directories #######################################################

BIN := bin
TMP := tmp


### Filename derivation ######################################################

# Decompose NEXTOR_BASE's basename: 'Nextor-<ver>.base[.<suffix>].dat'.
_BASE_NAME    := $(notdir $(NEXTOR_BASE))
_BASE_STEM    := $(_BASE_NAME:.dat=)
_BASE_VERSION := $(firstword $(subst .base, ,$(_BASE_STEM)))
_BASE_SUFFIX  := $(patsubst $(_BASE_VERSION).base%,%,$(_BASE_STEM))

# If the filename didn't parse (no '.base' found), fall back to using
# the whole stem as the prefix and no variant suffix.
ifeq ($(_BASE_SUFFIX),$(_BASE_STEM))
_DRIVER_PREFIX := $(_BASE_STEM)
_VARIANT       :=
else
_DRIVER_PREFIX := $(_BASE_VERSION).MegaFlashSDSCC
_VARIANT       := $(_BASE_SUFFIX)
endif

ROM_1SLOT           := $(BIN)/$(_DRIVER_PREFIX).1-slot$(_VARIANT).ROM
ROM_1SLOT_RECOVERY  := $(BIN)/$(_DRIVER_PREFIX).1-slot.Recovery$(_VARIANT).ROM
ROM_2SLOTS          := $(BIN)/$(_DRIVER_PREFIX).2-slots$(_VARIANT).ROM
ROM_2SLOTS_RECOVERY := $(BIN)/$(_DRIVER_PREFIX).2-slots.Recovery$(_VARIANT).ROM


### Assembly flags ###########################################################

N80_FLAGS := --no-string-escapes --no-show-banner --verbosity 0 \
             --build-type abs --output-file-extension bin \
             --output-file-case lower \
             --include-directory $(NEXTOR_SDK)

_DEFINES_NO_UNDOC := $(if $(NO_UNDOC_CPU_INSTRUCTIONS),--define-symbols NO_UNDOC_CPU_INSTRUCTIONS)


### Default target ###########################################################

.PHONY: all clean clean-bin distclean setup
all: $(ROM_1SLOT) $(ROM_1SLOT_RECOVERY) $(ROM_2SLOTS) $(ROM_2SLOTS_RECOVERY)

# Order-only prereqs for outputs that live in the build directories:
# ensure the directory exists without making its mtime affect rebuild
# decisions.
$(BIN) $(TMP):
	@mkdir -p $@


### Driver binaries (intermediates) ##########################################

# `driver.asm` (which also includes `romdisk.asm`) is assembled twice
# with different NUM_SLOTS to produce the 1-slot and 2-slots variants.

$(TMP)/driver.1slot.bin: driver.asm romdisk.asm | $(TMP)
	$(N80) driver.asm $(TMP)/driver.1slot.bin $(N80_FLAGS) $(_DEFINES_NO_UNDOC) --define-symbols NUM_SLOTS=1

$(TMP)/driver.2slots.bin: driver.asm romdisk.asm | $(TMP)
	$(N80) driver.asm $(TMP)/driver.2slots.bin $(N80_FLAGS) $(_DEFINES_NO_UNDOC) --define-symbols NUM_SLOTS=2

# The bank-switching routine for the ASCII8 mapper is the standard one
# shipped by the Nextor SDK.

$(TMP)/chgbnk.bin: $(NEXTOR_SDK)/asm/chgbnk/ascii8.asm | $(TMP)
	$(N80) $(NEXTOR_SDK)/asm/chgbnk/ascii8.asm $(TMP)/chgbnk.bin $(N80_FLAGS) $(_DEFINES_NO_UNDOC)

# 512-byte header prepended to the Recovery variants.

$(TMP)/recovery_header.bin: recovery_header.asm | $(TMP)
	$(N80) recovery_header.asm $(TMP)/ $(N80_FLAGS) $(_DEFINES_NO_UNDOC)


### ROM combination via mknexrom (final outputs) #############################

$(ROM_1SLOT): $(TMP)/driver.1slot.bin $(TMP)/chgbnk.bin | $(BIN)
	$(MKNEXROM) $(NEXTOR_BASE) $@ /d:$(TMP)/driver.1slot.bin /m:$(TMP)/chgbnk.bin

$(ROM_2SLOTS): $(TMP)/driver.2slots.bin $(TMP)/chgbnk.bin | $(BIN)
	$(MKNEXROM) $(NEXTOR_BASE) $@ /d:$(TMP)/driver.2slots.bin /m:$(TMP)/chgbnk.bin

# Recovery ROMs are the corresponding regular ROM with the recovery
# header concatenated in front.

$(ROM_1SLOT_RECOVERY): $(ROM_1SLOT) $(TMP)/recovery_header.bin | $(BIN)
	cat $(TMP)/recovery_header.bin $(ROM_1SLOT) > $@

$(ROM_2SLOTS_RECOVERY): $(ROM_2SLOTS) $(TMP)/recovery_header.bin | $(BIN)
	cat $(TMP)/recovery_header.bin $(ROM_2SLOTS) > $@


### Housekeeping #############################################################

# `make clean` keeps the shippable ROMs in bin/, only wipes intermediates.
clean:
	rm -rf $(TMP)

# `make clean-bin` removes the shippable ROMs.
clean-bin:
	rm -rf $(BIN)

# `make distclean` removes both.
distclean: clean clean-bin


### One-time setup ###########################################################

# `make setup` initializes the Nextor SDK submodule as a blobless
# partial clone with sparse-checkout for the `sdk/` directory only, so
# that the full Nextor repository is never fetched. Run this once,
# right after cloning this repo, instead of using `git clone
# --recurse-submodules`.
setup:
	@echo "Setting up the Nextor SDK submodule (blobless + sparse-checkout for sdk/ only)..."
	git submodule init external/Nextor
	git submodule update --init --filter=blob:none external/Nextor
	git -C external/Nextor sparse-checkout init --cone
	git -C external/Nextor sparse-checkout set sdk
	git -C external/Nextor checkout
	@echo "Done. Set NEXTOR_BASE and run 'make' to build."
