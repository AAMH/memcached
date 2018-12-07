#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include "shm_malloc.h"
#include <pthread.h>

int main() {
    int tracker_fd;
    struct tracker* track;
    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    printf("tracker fd: %d\n",tracker_fd);
    if (tracker_fd == -1) {
        //can't find tracker
        printf("tracker file not found\n");
        exit(1);
    }
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        exit(1);
    }
    printf("current max memory: %lu\n", track->max_size);
    printf("current allocation size: %lu\n", track->head_offset);

}

