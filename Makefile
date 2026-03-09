CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -mwindows -D_WIN32_WINNT=0x0602
LDFLAGS = -lshell32 -lkernel32 -ladvapi32
TARGET  = appstoreopener.exe

# VERSION can be overridden from the command line (e.g., make VERSION=v1.0.0)
VERSION ?= dev
CFLAGS += -DVERSION=\"$(VERSION)\"

# Derive a FILEVERSION quad (W,X,Y,0) from a vW.X.Y version tag for the RC file
_VER_QUAD := $(shell echo "$(VERSION)" | sed 's/^v//' | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' | awk -F. '{print $$1","$$2","$$3",0"}')
FILEVERSION_QUAD := $(if $(_VER_QUAD),$(_VER_QUAD),0,0,0,0)

$(TARGET): appstoreopener.c appstoreopener.res
	$(CC) $(CFLAGS) -o $@ appstoreopener.c appstoreopener.res $(LDFLAGS)

appstoreopener.res: appstoreopener.rc appstoreopener.manifest
	windres -DVERSION=\"$(VERSION)\" -DFILEVERSION_QUAD=$(FILEVERSION_QUAD) appstoreopener.rc -O coff -o appstoreopener.res

clean:
	rm -f $(TARGET) appstoreopener.res
