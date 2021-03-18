PREFIX = /usr/local
DESTDIR =

OBJCOPY = objcopy
OBJCOPY_ARCH = default
LIMINE_HDD_BIN = limine-hdd.bin
BUILD_DIR = $(shell realpath .)

CFLAGS = -O2 -pipe -Wall -Wextra

.PHONY: all clean

all: limine-install

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -s limine-install $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share
	install -m 644 limine.sys $(DESTDIR)$(PREFIX)/share/
	install -m 644 limine-cd.bin $(DESTDIR)$(PREFIX)/share/
	install -m 644 limine-eltorito-efi.bin $(DESTDIR)$(PREFIX)/share/
	install -m 644 limine-pxe.bin $(DESTDIR)$(PREFIX)/share/
	install -m 644 BOOTX64.EFI $(DESTDIR)$(PREFIX)/share/

clean:
	rm -f limine-hdd.o limine-install

limine-install: limine-install.c limine-hdd.o
	$(CC) $(CFLAGS) -std=c11 $^ -o $@

limine-hdd.o: $(LIMINE_HDD_BIN)
	cd `dirname $^` && \
	  $(OBJCOPY) -B i8086 -I binary -O $(OBJCOPY_ARCH) `basename $^` $(BUILD_DIR)/$@
