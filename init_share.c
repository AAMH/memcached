/* 
    Initializes shared memory sections for memcached
    arguments: amount of memory being allocated in bytes
*/
#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include "shm_malloc.h"

int tracker_fd;
int slabs_fd;

struct tracker* track;
void* slab_start;

int main(int argc, char **argv)
{
    size_t mem_allocated;
    
    //printf("argc: %d\n", argc);
    /* Parse arguments */
    if(argc == 1) {
        printf("No memory size specified, defaulting to 2GB\n");
        mem_allocated = 2147483648;
    }
    else if(argc > 2) {
        printf("Too many arguments, please only input the amount of shared memory in MB\n");
        exit(1);
    }
    else {
        mem_allocated = atoi(argv[1])*1024*1024;
        //printf("%lu\n", mem_allocated);
    }
    /* Get maximum system shared memory*/
    FILE *f = fopen("/proc/sys/kernel/shmmax", "r");
    size_t max_sz = 0;
    char line[100];
    if (f == NULL)
    {
        printf("no shared memory max file found, defaulting to 2GB\n");
        max_sz = 2147483648;
    }
    else {
        fgets(line, sizeof(line), f);
        //printf("line: %s\n", line);
        max_sz = (size_t) atoi( line );
        //printf("max: %lu\n", (long) max_sz);
        fclose(f);
    }
    if (mem_allocated > max_sz-sizeof(struct tracker))
    {
        printf("exceeded maximum shared memory size, current max (in bytes): %d\n",(int)(max_sz-sizeof(struct tracker)));
        exit(1);
    }

    printf("memory allocated (MB): %lu\n", (long) mem_allocated/1024/1024);
    
    /* Initialize tracker and shared memory */
    tracker_fd = shm_open("/tracker", O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    slabs_fd = shm_open("/slabs", O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (tracker_fd == -1) {
        perror("couldn\'t create tracker struct\n");
        exit(1);
    }
    if (slabs_fd == -1) {
        perror("couldn\'t create shared slab\n");
        exit(1);
    }
    if (ftruncate(tracker_fd, sizeof(struct tracker)) == -1) {
        perror("error truncating tracker\n");
        exit(1);
    }
    if (ftruncate(slabs_fd, mem_allocated) == -1) {
        perror("error truncating shared slab\n");
        exit(1);
    }    
    /* Map shared memory tracker object */
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, tracker_fd, 0);
    
    track->max_size = mem_allocated;
    track->allocated_size = 0;

    printf("tracking segment initialized\n");
    /* Map shared memory slab segment */
    slab_start = mmap(NULL, mem_allocated, PROT_READ | PROT_WRITE, MAP_SHARED, slabs_fd, 0); 

    if (track == MAP_FAILED) {
        perror("mapping tracker failed\n");
    }
    if (slab_start == MAP_FAILED) {
        perror("mapping slab failed\n");
    }
    printf("shared slab initialized\n");
}
