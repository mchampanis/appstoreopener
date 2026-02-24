CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -mwindows
LDFLAGS = -lshell32
TARGET  = appstoreopener.exe

$(TARGET): appstoreopener.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
