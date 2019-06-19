obj-m := testfs.o

testfs-y := main.o inode.o super.o file.o dir.o

KERNEL_DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	make -C $(KERNEL_DIR) M=$(PWD) modules
clean:
	rm *.o *.ko *.mod.c *.symvers *.order
