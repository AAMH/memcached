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
#include <sys/stat.h>

void* shm_malloc(size_t n) {
    pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
    int tracker_fd;
    int slabs_fd;
    struct tracker* track;
    void* rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;
    printf("page size: %lu\n", PAGESIZE);   
    // open tracker
    
    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    printf("tracker fd: %d\n",tracker_fd);    
    if (tracker_fd == -1) {
        //can't find tracker
        printf("tracker file not foundi\n");
        return NULL;
    }
    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
    pthread_mutex_unlock(&shm_lock);

    printf("current max memory %lu\n", track->max_size);
    printf("requested allocation: %lu\n", n);

    // open shared slab memory regioni
    slabs_fd = shm_open("/slabs", O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }

    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    	}
    else
    {
        printf("fstat() on shared memory succeeded\n");
        printf("mode = %d\n", buffer.st_mode);
        printf("size = %lu\n", buffer.st_size);
    }
    /* align to nearest page */
    pthread_mutex_lock(&shm_lock);
    int index = n/PAGESIZE;
    n = (index+1) * PAGESIZE;    
    if (write (slabs_fd, "", 1) != 1)
    {
	printf ("write error");
     	return NULL;
    }
    
    printf("actual allocated: %lu\n", n);

    /* get the beginning of the shared slab memory */
    rptr = mmap(0, n,
        PROT_READ | PROT_WRITE, MAP_SHARED, slabs_fd, track->allocated_size);
    
//    rptr = mmap(NULL, n,
//        PROT_READ | PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, slabs_fd, 0);

    if (rptr == MAP_FAILED) {
        printf("mmap failed\n");
 	return NULL;
    }
    pthread_mutex_unlock(&shm_lock);

    printf("return_ptr: %p\n", rptr);
    /* printfs for debugging purposes */
    printf("current max_size: %lu\n", track->max_size);
    printf("current allocated_mem: %lu\n", track->allocated_size);    

    /* return pointer to allocated memory */
    //rptr = (void*) slab_start + track->allocated_size;
    //  printf("allocated size: %d\n", n);

    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->allocated_size + n) {
        return NULL;
    }
    /* update allocated memory size */
    
    pthread_mutex_lock(&shm_lock);
    track->allocated_size = track->allocated_size + (off_t)n;
//    munmap(rptr, n);          
    
    pthread_mutex_unlock(&shm_lock);
    printf("return pointer %p\n", rptr); 
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    close(slabs_fd);
    return rptr;
}




