obj-m := tcp_ledbat.o
IDIR= /lib/modules/$(shell uname -r)/kernel/net/ipv4/
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install:
	install -v -m 644 tcp_ledbat.ko $(IDIR)
	depmod
	modprobe tcp_ledbat
	sysctl -w net.ipv4.tcp_allowed_congestion_control="cubic reno ledbat"

clean:
	rm -rf Module.markers modules.order Module.symvers tcp_ledbat.ko tcp_ledbat.mod.c tcp_ledbat.mod.o tcp_ledbat.o
