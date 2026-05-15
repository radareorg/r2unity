EXEC = r2unity
CONFIG_H = r2unity_config.h
PROJECT_VERSION = $(shell ./release.sh version)
ifeq ($(PROJECT_VERSION),)
	error
endif

SOEXT = $(shell r2 -H R2_LIBEXT)
ifeq ($(SOEXT),dylib)
PLUGIN_SHFLAGS = -dynamiclib
else
PLUGIN_SHFLAGS = -shared
endif

CORE_PLUGIN = src/r2/core_r2unity.$(SOEXT)
BIN_PLUGIN = src/r2/bin_r2unity.$(SOEXT)
PLUGINS = $(CORE_PLUGIN) $(BIN_PLUGIN)
CC = gcc

CFLAGS = -Wall -Wextra -g -I. $(shell pkg-config --cflags r_util 2>/dev/null || echo "")
LDFLAGS = $(shell pkg-config --libs r_util 2>/dev/null || echo "")

# r_core plugin flags (full radare2)
CORE_PLUGIN_CFLAGS = -Wall -Wextra -g -fPIC $(shell pkg-config --cflags r_core 2>/dev/null || echo "")
CORE_PLUGIN_LDFLAGS = $(shell pkg-config --libs r_core 2>/dev/null || echo "")

# r_bin plugin flags
BIN_PLUGIN_CFLAGS = -Wall -Wextra -g -fPIC $(shell pkg-config --cflags r_bin 2>/dev/null || echo "")
BIN_PLUGIN_LDFLAGS = $(shell pkg-config --libs r_bin 2>/dev/null || echo "")

R2_USER_PLUGINS = $(shell r2 -H R2_USER_PLUGINS 2>/dev/null)

LIB_SRCS = $(wildcard src/lib/*.c)
LIB_OBJS = $(LIB_SRCS:.c=.o)
CLI_SRCS = src/main.c
CLI_OBJS = $(CLI_SRCS:.c=.o)
OBJS = $(CLI_OBJS) $(LIB_OBJS)

R2PM_BINDIR=$(shell r2pm -H R2PM_BINDIR)

all: $(EXEC)

plugin: $(PLUGINS)

install-plugin: $(PLUGINS)
	@[ -n "$(R2_USER_PLUGINS)" ] || (echo "r2 not found; cannot resolve R2_USER_PLUGINS"; exit 1)
	mkdir -p "$(R2_USER_PLUGINS)"
	cp -f $(PLUGINS) "$(R2_USER_PLUGINS)/"

uninstall-plugin:
	@[ -n "$(R2_USER_PLUGINS)" ] || (echo "r2 not found; cannot resolve R2_USER_PLUGINS"; exit 1)
	rm -f "$(R2_USER_PLUGINS)/core_r2unity.$(SOEXT)"
	rm -f "$(R2_USER_PLUGINS)/bin_r2unity.$(SOEXT)"

fmt:
	clang-format-radare2 src/**/*.c

user-install: install-plugin
	mkdir -p "$(R2PM_BINDIR)"
	cp -f $(EXEC) "$(R2PM_BINDIR)"

user-uninstall: uninstall-plugin
	rm -f "$(R2PM_BINDIR)/$(EXEC)"

$(EXEC): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(CONFIG_H): meson.build
	echo '#define R2UNITY_VERSION "$(PROJECT_VERSION)"\n' > $@

$(CLI_OBJS): %.o: %.c $(CONFIG_H)
	$(CC) $(CFLAGS) -c -o $@ $<

$(CORE_PLUGIN): src/r2/core_r2unity.c $(LIB_SRCS)
	$(CC) $(CORE_PLUGIN_CFLAGS) $(PLUGIN_SHFLAGS) -o $@ $^ $(CORE_PLUGIN_LDFLAGS)

$(BIN_PLUGIN): src/r2/bin_r2unity.c $(LIB_SRCS)
	$(CC) $(BIN_PLUGIN_CFLAGS) $(PLUGIN_SHFLAGS) -o $@ $^ $(BIN_PLUGIN_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(EXEC) $(OBJS) $(PLUGINS) $(CONFIG_H)


.PHONY: all clean plugin install-plugin uninstall-plugin fmt
