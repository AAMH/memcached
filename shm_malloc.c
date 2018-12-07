#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include "shm_malloc.h"
#include <pthread.h>
#include <sys/stat.h>
#include <semaphore.h>

void * shm_malloc(size_t n) {
//    pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
    int tracker_fd;
    int slabs_fd;

    sem_t * mutex;
    struct tracker* track;
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
        printf("tracker file not found\n");
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
  
    // open shared slab memory region
    slabs_fd = shm_open("/slabs", O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
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
     	return NULL;
    }	

    /* get the next available address of the shared memory */  
    rptr = mmap( ((char*)track->start_address) , n,
        PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd, track->used_size);

    if (rptr == MAP_FAILED) {
        printf("mmap failed\n");
    return NULL;
    }

//    pthread_mutex_unlock(&shm_lock);
	
    /* requested size is larger than what the allocator is capabable of allocating */
    if (track->max_size < track->used_size + n) {
        return NULL;
    }

    /* update allocated memory size */
    track->start_address = (long *) track->start_address - (n/8);
    track->avail_address = rptr;	  
	track->used_size = track->used_size + (size_t)n;

    printf("return pointer %p\n", rptr);
    printf("total allocated mem(all): %lu\n", track->used_size);

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    close(slabs_fd);
    sem_post(mutex); 
    return rptr;
}

void * shm_mallocAt(size_t n){

    int slabs_fd;
    int tracker_fd;

    struct tracker* track;
    sem_t * mutex;
    void* rptr;         // return pointer
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    struct stat buffer;
    int ret;

    // open semaphore
    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);
  
    // open shared slab memory region
    slabs_fd = shm_open("/slabs", O_RDWR, S_IRUSR | S_IWUSR);
    if (slabs_fd == -1) {
        return NULL;
    }

    // open tracker  
    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found\n");
        return NULL;
    }

//    pthread_mutex_lock(&shm_lock);
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    if((ret = fstat(slabs_fd,&buffer)) < 0) {
        printf("fstat() on shared memory failed with errno %d\n", errno);
    }
  
    rptr = mmap(track->spare_mem_start , n,
        PROT_READ | PROT_WRITE, MAP_SHARED , slabs_fd, 0);

    if (rptr == MAP_FAILED) {
        printf("mmap failed\n");
        return NULL;
    }
    close(tracker_fd);
    close(slabs_fd);
    sem_post(mutex); 
    return rptr;
}

void * get_spare_mem(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    void * victim;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    victim = track->spare_mem_start;
    track->spare_mem_start = NULL;
    track->spare_mem_avail = false;
    track->spare_mem_clsid = -1;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return victim;
}

void * set_spare_mem(void * ptr, int id){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    void * victim;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    track->spare_mem_start = ptr;
    track->spare_mem_clsid = id;
    track->spare_mem_avail = true;
    track->spare_lock = false;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return victim;
}

int get_spare_clsid(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    int victim_id = -1;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return NULL;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return NULL;
    }
	
    victim_id = track->spare_mem_clsid;
    
    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return victim_id;
}

bool is_spare_avail(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    b = track->spare_mem_avail;
    track->spare_lock = true;       // locks after the first access

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool reset_locks(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    track->spare_mem_avail = false;
    track->spare_lock = false;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

bool is_spare_locked(){

    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    bool b = false;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        printf("tracker file not found!\n");
        return false;
    }

    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    if (track == MAP_FAILED) {
        return false;
    }
	
    b = track->spare_lock;

    munmap(track, sizeof(struct tracker));
    close(tracker_fd);
    sem_post(mutex);  
    return b;
}

struct tracker get_tracker(void) {
	
    int tracker_fd;
    sem_t * mutex;
    struct tracker* track;
    struct tracker track2;

    if ((mutex = sem_open("/semaph", 0)) == SEM_FAILED) {
        perror("semaphore failed!");
        exit(1);
    }
    sem_wait(mutex);

    tracker_fd = shm_open("/tracker",  O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
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
