CC      = x86_64-w64-mingw32-gcc
WINDRES = x86_64-w64-mingw32-windres
VERSION = $(shell git describe --tags --dirty 2>/dev/null || echo dev)

export SOURCE_DATE_EPOCH ?= $(shell git show -s --format=%ct HEAD 2>/dev/null || echo 0)

CFLAGS  = -O2 -Wall -Wextra -mwindows -municode -DVERSION_STRING=L\"$(VERSION)\"
LDFLAGS = -lshell32 -luser32 -lkernel32 -ladvapi32
TARGET  = ForceComposedFlip.exe
SRC     = ForceComposedFlip.c
RC      = ForceComposedFlip.rc
RES_OBJ = ForceComposedFlip_res.o

$(TARGET): $(SRC) $(RES_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(RES_OBJ) $(LDFLAGS)

$(RES_OBJ): $(RC) ForceComposedFlip.ico ForceComposedFlip.manifest
	$(WINDRES) $(RC) -o $@

clean:
	rm -f $(TARGET) $(RES_OBJ)

.PHONY: clean
