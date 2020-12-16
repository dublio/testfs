# testfs
	A simple block device based filesystem, most of code was copied from ext2.
	It just a demo to show how to learn to write a file system step by step.

## support functions
	create file
	remove file
	read file
	write file
	mkdir
## need support functions
	rmdir
	symlink
	attribute
	...
## Environment
	linux kernel v4.10 and newer

## Usage
	Suppose /test is the target mount point

	make

	dd if=/dev/zero of=disk.img bs=1M count=64 status=none
	./mktestfs disk.img

	umount /test
	rmmod testfs
	insmod testfs.ko

	mount -t testfs -o loop disk.img /test

	echo "hello testfs" > /test/hello.txt
	cat /test/hello.txt
