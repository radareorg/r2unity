# Simple Makefile for the r2unity project

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g $(shell pkg-config --cflags r_util 2>/dev/null || echo "")
LDFLAGS = $(shell pkg-config --libs r_util 2>/dev/null || echo "")

# r_core plugin flags (full radare2)
PLUGIN_CFLAGS = -Wall -Wextra -g -fPIC $(shell pkg-config --cflags r_core 2>/dev/null || echo "")
PLUGIN_LDFLAGS = $(shell pkg-config --libs r_core 2>/dev/null || echo "")

# Shared-library extension per platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SOEXT = dylib
PLUGIN_SHFLAGS = -dynamiclib
else
SOEXT = so
PLUGIN_SHFLAGS = -shared
endif

# Project name
EXEC = r2unity
PLUGIN = src/r2/core_r2unity.$(SOEXT)

# Install location (radare2 user plugin dir)
R2_USER_PLUGINS = $(shell r2 -H R2_USER_PLUGINS 2>/dev/null)

# Source files and object files
LIB_SRCS = $(wildcard src/lib/*.c)
LIB_OBJS = $(LIB_SRCS:.c=.o)
CLI_SRCS = src/main.c
CLI_OBJS = $(CLI_SRCS:.c=.o)
OBJS = $(CLI_OBJS) $(LIB_OBJS)

R2PM_BINDIR=$(shell r2pm -H r2PM_BINDIR)

# Default target
all: $(EXEC)

# Plugin target
plugin: $(PLUGIN)

# Install plugin into the user plugins directory
install-plugin: $(PLUGIN)
	@[ -n "$(R2_USER_PLUGINS)" ] || (echo "r2 not found; cannot resolve R2_USER_PLUGINS"; exit 1)
	mkdir -p "$(R2_USER_PLUGINS)"
	cp $(PLUGIN) "$(R2_USER_PLUGINS)/"

uninstall-plugin:
	@[ -n "$(R2_USER_PLUGINS)" ] || (echo "r2 not found; cannot resolve R2_USER_PLUGINS"; exit 1)
	rm -f "$(R2_USER_PLUGINS)/core_r2unity.$(SOEXT)"

user-install: install-plugin
	mkdir -p "$(R2PM_BINDIR)"
	cp -f $(EXEC) "$(R2PM_BINDIR)"

user-uninstall: uninstall-plugin
	rm -f "$(R2PM_BINDIR)/$(EXEC)"

# check target wraps r2r with a timeout wrapper

# Linking the executable
$(EXEC): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Linking the r_core plugin (re-compiles lib sources with PLUGIN_CFLAGS)
$(PLUGIN): src/r2/core_r2unity.c $(LIB_SRCS)
	$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_SHFLAGS) -o $@ $^ $(PLUGIN_LDFLAGS)

# Compiling source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean up build artifacts
clean:
	rm -f $(EXEC) $(OBJS) $(PLUGIN)

# Phony targets
.PHONY: all clean plugin install-plugin uninstall-plugin
