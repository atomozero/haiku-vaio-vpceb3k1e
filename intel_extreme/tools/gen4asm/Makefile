## Makefile for gen4asm (Intel EU assembler) — Haiku port
##
## Builds the intel-gen4asm and intel-gen4disasm command-line tools.
## These are BUILD-TIME tools used by the intel_extreme accelerant to
## assemble .g4a kernel sources into .g4b.gen5 binaries that are then
## #include'd as uint32_t arrays into the driver code.
##
## Upstream: igt-gpu-tools assembler/ directory, MIT licensed.
## Imported 2026-04 for Haiku VAIO VPCEB3K1E Gen5 driver project.

CC    = /boot/system/develop/tools/bin/gcc
FLEX  = /bin/flex
BISON = /bin/bison

CFLAGS = -O2 -g \
	-I. \
	-Wno-declaration-after-statement \
	-Wno-discarded-qualifiers \
	-Wno-missing-format-attribute \
	-Wno-old-style-definition \
	-Wno-redundant-decls \
	-Wno-shadow \
	-Wno-incompatible-pointer-types-discards-qualifiers \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-Wno-pointer-sign

LIB_BRW_SRC = \
	brw_context.c \
	brw_disasm.c \
	brw_eu.c \
	brw_eu_compact.c \
	brw_eu_debug.c \
	brw_eu_emit.c \
	brw_eu_util.c \
	gen8_disasm.c \
	gen8_instruction.c \
	ralloc.c

LIB_BRW_OBJ = $(LIB_BRW_SRC:.c=.o)

GENERATED = gram.c gram.h lex.c

ASM_OBJ   = main.o gram.o lex.o
DIS_OBJ   = disasm-main.o

all: gen4asm gen4disasm

gen4asm: $(ASM_OBJ) $(LIB_BRW_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

gen4disasm: $(DIS_OBJ) $(LIB_BRW_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# Flex / Bison rules
gram.c gram.h: gram.y
	$(BISON) --defines=gram.h --output=gram.c gram.y

lex.c: lex.l gram.h
	$(FLEX) -i -o lex.c lex.l

# Object rules
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies — flex/bison generated files need to exist before compile
main.o: gram.h
lex.o: gram.h
gram.o: gram.h

clean:
	rm -f *.o gen4asm gen4disasm $(GENERATED)

test: gen4asm
	@./test/run-test.sh test/mov || echo "test/mov FAILED"

.PHONY: all clean test
