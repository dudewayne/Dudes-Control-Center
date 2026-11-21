# Makefile for ite_829x kernel module
# Builds keyboard backlight driver for Clevo laptops
# Compatible with both standalone build and DKMS

PWD := $(shell pwd)

all:
	@echo "Building ite_829x kernel module..."
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M='$(PWD)' modules
	@echo "Module built successfully: ite_829x.ko"

install: all
	@echo "Installing kernel module..."
	cp -v ite_829x.ko /lib/modules/$(shell uname -r)/kernel/drivers/hid/
	depmod -a

clean:
	rm -f *.ko *.o *.mod.c *.mod.o *.symvers *.order Module.symvers .*.cmd
	find . -type d -name '.tmp_*' -exec rm -rf {} + 2>/dev/null || true

.PHONY: all install clean