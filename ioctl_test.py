from fcntl import ioctl

filename = "/dev/my_lkm"

fd = open(filename, "wb")
ioctl(fd, int(48), 128)
fd.close()