obj-m := eth_uart.o
KERNEL_DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:eth_uart.c
	make -C $(KERNEL_DIR) SUBDIRS=$(PWD) modules
clean:
	rm -f *.o *.ko *.mod.c *.symvers *.order

.PHONY:clean
