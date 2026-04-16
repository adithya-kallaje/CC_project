CC      := gcc
CFLAGS  := -Wall -Wextra -O2 $(shell pkg-config fuse3 --cflags)
LDLIBS  := $(shell pkg-config fuse3 --libs)

TARGET  := mini_unionfs

$(TARGET): mini_unionfs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
