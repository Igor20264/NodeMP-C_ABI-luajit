# Build ljhost.dll. LuaJIT sources are fetched on the first run.

ifeq ($(wildcard third_party/LuaJIT/src/Makefile),)

all ljhost.dll:
	git clone --depth 1 https://github.com/LuaJIT/LuaJIT.git third_party/LuaJIT
	$(MAKE) ljhost.dll

else

CC = gcc
LUAJIT_SRC = third_party/LuaJIT/src
CFLAGS = -O2 -Wall -Wextra -I$(LUAJIT_SRC) -Isdk
LIBS = $(LUAJIT_SRC)/libluajit.a -lm

.PHONY: all
all: ljhost.dll

ljhost.dll: src/ljhost.c $(LUAJIT_SRC)/libluajit.a sdk/node.h
	$(CC) $(CFLAGS) -shared -static-libgcc -o $@ src/ljhost.c $(LIBS)

$(LUAJIT_SRC)/libluajit.a:
	$(MAKE) -C $(LUAJIT_SRC) BUILDMODE=static

endif
