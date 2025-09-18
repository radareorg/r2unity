# Simple Makefile for the r2unity project

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -I./third_party/radare2/libr/include $(shell pkg-config --cflags r_core r_util 2>/dev/null || echo "")
LDFLAGS = $(shell pkg-config --libs r_core r_util 2>/dev/null || echo "")

# Project name
EXEC = r2unity

# Source files and object files
SRCS = $(wildcard src/*.c) $(wildcard src/lib/*.c)
OBJS = $(SRCS:.c=.o)

# Default target
all: $(EXEC)

# Linking the executable
$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compiling source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean up build artifacts
clean:
	rm -f $(EXEC) $(OBJS)

# Phony targets
.PHONY: all clean
