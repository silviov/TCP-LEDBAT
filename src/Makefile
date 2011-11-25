obj-m := tcp_ledbat.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	rm -rf Module.markers modules.order Module.symvers tcp_ledbat.ko tcp_ledbat.mod.c tcp_ledbat.mod.o tcp_ledbat.o
