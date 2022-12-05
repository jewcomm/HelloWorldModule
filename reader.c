#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

void main(int argc, char **argv){
    int pipe_fd;
    char *buffer;

    pipe_fd = open("/dev/my_lkm", O_RDONLY);
    if(pipe_fd < 0){
        perror("Error open pipe\n");
        exit(1);
    }
    int bufferSize = 100;
    buffer = (char*)malloc(bufferSize * sizeof(char));
    int readed = read(pipe_fd, buffer, 5);
    printf("%s\n", buffer);

    exit(0);
}