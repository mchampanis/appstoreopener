CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -mwindows -D_WIN32_WINNT=0x0602 -I.
LDFLAGS = -lshell32 -lkernel32 -ladvapi32
TARGET  = appstoreopener.exe

# VERSION can be overridden from the command line (e.g., make VERSION=v1.0.0)
VERSION ?= dev

# Extract W, X, Y from a vW.X.Y version tag; default to 0 if not a semver string
_VER_NUMS := $(shell echo "$(VERSION)" | sed 's/^v//' | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' | tr '.' ' ')
VER_MAJOR := $(if $(word 1,$(_VER_NUMS)),$(word 1,$(_VER_NUMS)),0)
VER_MINOR := $(if $(word 2,$(_VER_NUMS)),$(word 2,$(_VER_NUMS)),0)
VER_PATCH := $(if $(word 3,$(_VER_NUMS)),$(word 3,$(_VER_NUMS)),0)

.PHONY: all clean FORCE

all: $(TARGET)

# Generate version.h so both the C compiler and windres get version info without
# command-line string quoting issues (windres handles quoted -D values poorly)
version.h: FORCE
	@printf '#define VERSION "%s"\n#define VER_MAJOR %s\n#define VER_MINOR %s\n#define VER_PATCH %s\n' \
	    "$(VERSION)" "$(VER_MAJOR)" "$(VER_MINOR)" "$(VER_PATCH)" > version.h

$(TARGET): appstoreopener.c appstoreopener.res version.h
	$(CC) $(CFLAGS) -o $@ appstoreopener.c appstoreopener.res $(LDFLAGS)

appstoreopener.res: appstoreopener.rc appstoreopener.manifest version.h
	windres appstoreopener.rc -O coff -o appstoreopener.res

clean:
	rm -f $(TARGET) appstoreopener.res version.h
