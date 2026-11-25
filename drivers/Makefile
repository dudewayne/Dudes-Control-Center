# Makefile for the kernel modules (see Kbuild): ite_829x (keyboard
# backlight) and dcc_fan (fans and temperatures)
# Compatible with both standalone build and DKMS

PWD := $(shell pwd)

all:
	@echo "Building ite_829x and dcc_fan kernel modules..."
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M='$(PWD)' modules
	@echo "Modules built: ite_829x.ko dcc_fan.ko"

install: all
	@echo "Installing kernel module..."
	cp -v ite_829x.ko /lib/modules/$(shell uname -r)/kernel/drivers/hid/
	cp -v dcc_fan.ko /lib/modules/$(shell uname -r)/kernel/drivers/hwmon/
	depmod -a

clean:
	rm -f *.ko *.o *.mod.c *.mod.o *.symvers *.order Module.symvers .*.cmd
	find . -type d -name '.tmp_*' -exec rm -rf {} + 2>/dev/null || true

.PHONY: all install clean