# The PS3's system call glue, libsysbase.a (../../README.md): newlib's _open_r and the rest over
# the __syscalls table that PSL1GHT's librt fills, __getreent, realpath, and newlib's retargetable
# locks over lv2's lightweight mutexes. Built with the installed PPU toolchain after newlib;
# `make install` puts the library where GCC's link spec (-lsysbase) finds it.
PS3DEV ?= /usr/local/ps3dev
CC = $(PS3DEV)/ppu/bin/ppu-gcc
AR = $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-ar
BUILD ?= build
LIBDIR = $(PS3DEV)/ppu/powerpc64-ps3-elf/lib

CFLAGS = -O2 -mcpu=cell -ffunction-sections -fdata-sections -include ansi_compat.h -I.

SOURCES = $(wildcard *.c)
OBJECTS = $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))

all: $(BUILD)/libsysbase.a

$(BUILD)/libsysbase.a: $(OBJECTS)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

install: $(BUILD)/libsysbase.a
	install -m 644 $(BUILD)/libsysbase.a $(LIBDIR)/libsysbase.a

clean:
	rm -rf $(BUILD)

.PHONY: all install clean
