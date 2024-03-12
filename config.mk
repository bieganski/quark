# quark version
VERSION = 0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

ifndef ARCH
$(error ARCH is not defined. Please specify ARCH={riscv|armv})
endif

ifeq ($(ARCH), riscv)
    CC = riscv64-linux-gnu-gcc
    DL = /lib64/ld-linux-riscv64-lp64d.so.1
else ifeq ($(ARCH), arm)
    CC = ./compiler/bin/arm-linux-gnueabi-gcc
    DL = /usr/lib/ld-linux.so.3
else
    $(error Unsupported ARCH: $(ARCH))
endif

# flags
CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Os
LDFLAGS  =  -lpthread -s -Wl,--export-dynamic,--dynamic-linker=/lib64/ld-linux-riscv64-lp64d.so.1 -Wl,--as-needed 

