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
#include <semaphore.h>

struct tracker shm_malloc(size_t n) {
//    pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
    int tracker_fd;
    int slabs_fd;
    sem_t * mutex;
    struct tracker* track;
    struct tracker track2;
    void* rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;
    printf("page size: %lu\n", PAGESIZE); 

    // open semaphore

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
    perror("semaphore failed!");
    exit(1);
    }
    sem_wait(mutex);
	
    // open tracker
    
    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    printf("tracker fd: %d\n",tracker_fd);    
    if (tracker_fd == -1) {
        //can't find tracker
        printf("tracker file not foundi\n");
        return track2;
    }

//    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return track2;
    }
//   pthread_mutex_unlock(&shm_lock);
    

    // open shared slab memory region
	
    slabs_fd = shm_open("/slabs", O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return track2;
    }
    printf("slabs fd: %d\n",slabs_fd);
	
    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }
	
    /* align to nearest page */
	
 //   pthread_mutex_lock(&shm_lock);	
    printf("current max memory %lu\n", track->max_size);
    printf("requested allocation: %lu\n", n);
	
    int index = n/PAGESIZE;
    n = (index+1) * PAGESIZE;    
    if (write (slabs_fd, "", 1) != 1)
    {
	printf ("write error");
     	return track2;
    }	

    /* get the next available address of the shared memory */
	
    rptr = mmap( ((char*)track->start_address) , n,
         PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd, track->allocated_size);

    if (rptr == MAP_FAILED) {
        printf("mmap failed\n");
 	return track2;
    }

//    pthread_mutex_unlock(&shm_lock);
	
    printf("return pointer %p\n", rptr);
    track->start_address = (long *) track->start_address - (n/8);
    track->last_address = rptr;	
    printf("total allocated mem(all): %lu\n", track->allocated_size);    

    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->allocated_size + n) {
        return track2;
    }
    /* update allocated memory size */
	
//    pthread_mutex_lock(&shm_lock);
    track->allocated_size = track->allocated_size + (off_t)n;
//    munmap(rptr, n);          

//    pthread_mutex_unlock(&shm_lock);
    track2 = *track;
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    close(slabs_fd);
    sem_post(mutex); 
    return track2;
}

struct tracker get_tracker(void) {
	
    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    struct tracker track2;

    // open semaphore

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
    perror("semaphore failed!");
    exit(1);
    }
	
    sem_wait(mutex);
    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        //can't find tracker
        printf("tracker file not found!\n");
        return track2;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ , MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return track2;
    }
	
    track2 = *track;
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return track2;
}
