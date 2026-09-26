# Makefile for MFRFLASH.COM, the MegaFlashROM SCC+ SD cached flash ROM
# loader (see mfrflash.c). Built with SDCC against the Nextor SDK, the
# same way as the C tools in the Nextor repository.
#
#   make            build MFRFLASH.COM (placed in ../bin by default)
#   make clean      remove the intermediate files
#
# Normally invoked from the root Makefile (`make tools` or plain `make`),
# which passes its own NEXTOR_SDK, BIN and TMP; it also works standalone
# from this directory with the defaults below.
#
# Tools needed: sdcc and sdasz80 (SDCC, https://sdcc.sourceforge.net) and
# objcopy (GNU binutils), all on PATH or pointed at via the variables
# below. The Nextor dev Docker image has them preinstalled.

NEXTOR_SDK ?= ../external/Nextor/sdk
BIN        ?= ../bin
TMP        ?= ../tmp/tools

SDCC    ?= sdcc
SDASZ80 ?= sdasz80
OBJCOPY ?= objcopy

COM := $(BIN)/MFRFLASH.COM

SDK_C    := $(NEXTOR_SDK)/C
CFLAGS   := -mz80 --disable-warning 196 --disable-warning 85 -I$(SDK_C)/includes -I$(SDK_C)/code
CRT0     := $(TMP)/crt0_msxdos.rel
SDK_RELS := $(TMP)/asmcall.rel $(TMP)/printf.rel $(TMP)/print_msxdos.rel

.PHONY: all clean
all: $(COM)

# Check that the required tools are available, at parse time so that it
# happens before anything else. Skipped when just cleaning.
ifneq ($(MAKECMDGOALS),clean)
$(foreach exec,$(SDCC) $(SDASZ80) $(OBJCOPY),\
	$(if $(shell which $(exec)),,$(error "ERROR: can't execute $(exec), is it installed/in PATH?")))
endif

$(COM): $(TMP)/mfrflash.ihx | $(BIN)
	$(OBJCOPY) -I ihex -O binary $< $@

# The program is linked at 0x180 (right after the crt0 startup code at
# 0x100) with the global variables placed right after the code, as the
# Nextor tools are. All the intermediate files go to $(TMP).
#
# The program takes the _HEAP_start symbol (defined by the crt0 at the end
# of the last linked area) as the end of its own code and data, and uses
# everything from there up to 7FFFh as a buffer. Check in the map file
# that no area actually ends beyond that symbol, which could happen if
# the linker ever ordered the areas differently.
$(TMP)/mfrflash.ihx: mfrflash.c $(CRT0) $(SDK_RELS) $(SDK_C)/includes/asmcall.h | $(TMP)
	$(SDCC) $(CFLAGS) --code-loc 0x180 --data-loc 0 --no-std-crt0 -o $(TMP)/ $(CRT0) $(SDK_RELS) mfrflash.c
	@set -e; \
	heap=$$(printf %d 0x$$(grep -a '_HEAP_start' $(TMP)/mfrflash.map | head -1 | awk '{print $$1}')); \
	for area in $$(grep -a -E '^_[A-Z0-9_]+ +[0-9A-F]{8} +[0-9A-F]{8} =' $(TMP)/mfrflash.map | grep -v '(ABS' | awk '{print $$1 ":" $$2 ":" $$3}'); do \
		name=$${area%%:*}; rest=$${area#*:}; start=$${rest%%:*}; size=$${rest#*:}; \
		end=$$(( $$(printf %d 0x$$start) + $$(printf %d 0x$$size) )); \
		if [ $$end -gt $$heap ]; then \
			printf 'ERROR: the %s area ends at 0x%04X, beyond _HEAP_start (0x%04X): the buffer start assumption in mfrflash.c no longer holds\n' "$$name" "$$end" "$$heap"; \
			exit 1; \
		fi; \
	done

$(CRT0): $(SDK_C)/code/crt0_msxdos.asm | $(TMP)
	$(SDASZ80) -o $@ $<

# SDK C library modules, compiled with the same flags as in the Nextor repository.
$(TMP)/asmcall.rel: $(SDK_C)/code/asmcall.c $(SDK_C)/includes/asmcall.h | $(TMP)
	$(SDCC) $(CFLAGS) -c -o $@ $<

$(TMP)/printf.rel: $(SDK_C)/code/printf.c $(SDK_C)/includes/msx_bios.h | $(TMP)
	$(SDCC) $(CFLAGS) -c --max-allocs-per-node 100000 --allow-unsafe-read --opt-code-size -DCOM_FILE -DSUPPORT_LONG -o $@ $<

$(TMP)/print_msxdos.rel: $(SDK_C)/code/print_msxdos.c | $(TMP)
	$(SDCC) $(CFLAGS) -c -o $@ $<

$(BIN) $(TMP):
	@mkdir -p $@

clean:
	rm -rf $(TMP)
