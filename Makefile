CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -mwindows -D_WIN32_WINNT=0x0602
LDFLAGS = -lshell32 -lkernel32 -ladvapi32
TARGET  = appstoreopener.exe

# VERSION can be overridden from the command line (e.g., make VERSION=v1.0.0)
VERSION ?= dev
CFLAGS += -DVERSION=\"$(VERSION)\"

$(TARGET): appstoreopener.c appstoreopener.res
	$(CC) $(CFLAGS) -o $@ appstoreopener.c appstoreopener.res $(LDFLAGS)

appstoreopener.res: appstoreopener.rc appstoreopener.manifest
	windres -DVERSION=\"$(VERSION)\" appstoreopener.rc -O coff -o appstoreopener.res

clean:
	rm -f $(TARGET) appstoreopener.res
