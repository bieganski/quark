# quark version
VERSION = 0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# flags
CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -Os
LDFLAGS  =  -lpthread -s -Wl,--export-dynamic,--dynamic-linker=/usr/lib/ld-linux.so.3 -Wl,--as-needed 

# compiler and linker
CC = ./compiler/bin/arm-linux-gnueabi-gcc
