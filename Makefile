obj-m = my_lkm.o
all:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

unload:
	#sudo rm /dev/my_lkm
	sudo rmmod my_lkm
	dmesg 
	sudo dmesg -C

load:
	sudo insmod my_lkm.ko
	dmesg