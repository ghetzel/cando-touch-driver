obj-m := cando.o

# This should be the absolute path to a copy of the Raspberry Pi Linux kernel source
# for the version of Raspbian et. al. you are running.
#
KDIR ?= /lib/modules/`uname -r`/build

default:
	-rm cando.ko

	ssh root@$(HOST) '\
		[ -d build ] || mkdir build && \
		cd build && \
		[ -L kernel ] || ln -s /usr/src/linux-source-`uname -r | cut -d- -f1` kernel'

	scp cando.c Makefile root@$(HOST):build/
	ssh root@$(HOST) 'cd build && make build && \
		[ -d /lib/modules/`uname -r`/extra ] || mkdir -v /lib/modules/`uname -r`/extra; \
		mv -v cando.ko /lib/modules/`uname -r`/extra/cando.ko; \
		depmod && \
		modprobe -vr cando; modprobe -v cando'

build:
	$(MAKE) -C $(KDIR) M=$$PWD

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

clean:
	-rm *.cmd .*.cmd .tmp_versions
	-rm *.mod.*
	-rm *.o
	-rm *.ko
	-rm *.order *.symvers
