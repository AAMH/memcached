#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shm_malloc.h"

void* shm_malloc(size_t n) {
    
    int tracker_fd;
    int slabs_fd;
    struct tracker* track;
    void* slab_start;
    void* rptr;         // return pointer

    // open tracker
    tracker_fd = shm_open("/tracker", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        //can't find tracker
        return NULL;
    }

    
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }

    printf("current allocated memory %lu\n", track->max_size);
    printf("requested allocation: %lu\n", n);

    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->allocated_size + n) {
        return NULL;
    }
    
    // open shared slab memory region
    slabs_fd = shm_open("/slabs", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }

    /* get the beginning of the shared slab memory */
    slab_start = mmap(NULL, 1,
        PROT_READ | PROT_WRITE, MAP_SHARED, slabs_fd, 0);
    if (slab_start == MAP_FAILED) {
        return NULL;
    }

    /* return pointer to allocated memory */
    rptr = slab_start + track->allocated_size;
    /* update allocated memory size */
    track->allocated_size += n;

    /* printfs for debugging purposes */
    printf("current max_size: %lu\n", track->max_size);
    printf("current allocated_mem: %lu\n", track->allocated_size);

    return rptr;
}

/* test main */
int main() {
    char *test = shm_malloc(12*sizeof(char));
    strcpy(test, "testingonly");

    char *test2 = shm_malloc(13*sizeof(char));
    strcpy(test2, "testingonly2");

    printf("The test string is: %s\n", test);
    printf("The second test string is: %s\n", test2);
    
}
