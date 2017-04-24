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

struct tracker {        /* Shared memory structure to keep track of current pointer */
    size_t cur_size;
    size_t allocated_size;
};

int fd1;
int fd2;

struct tracker* track;
void* slab_start;

int main(int argc, char **argv)
{
    size_t mem_allocated;
    printf("argc: %d\n", argc);
    if(argc == 1) {
        printf("No memory size specified, defaulting to 2GB");
        mem_allocated = 2147483648;
    }
    else if(argc > 2) {
        printf("Too many arguments, please only input the amount of shared memory in MB");
    }
    else {
        mem_allocated = atoi(argv[1])*1024*1024;
    }
    /* Get maximum system shared memory*/
    FILE *f = fopen("cat /proc/sys/kernel/shmmax", "r");
    size_t max_sz = 0;
    char line[100];
    if (f == NULL)
    {
        printf("no shared memory max file found, defaulting to 2GB");
        max_sz = 2147483648;
    }
    while( fgets(line, sizeof(line), f) != NULL );
    max_sz = atoi( line );
    printf("max: %d\n", (int) max_sz);
    fclose(f);

    if (mem_allocated > max_sz-sizeof(struct tracker))
    {
        printf("exceeded maximum shared memory size, current max (in bytes): %d\n",(int)(max_sz-sizeof(struct tracker)));
        exit(0);
    }

    printf("memory allocated: %d\n", (int) mem_allocated);
    fd1 = shm_open("/memcached/tracker", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    fd2 = shm_open("/memcached/slabs", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd1 == -1)
        perror("couldn\'t create tracker struct");
    if (fd2 == -1)
        perror("couldn\'t create shared slab");
    if (ftruncate(fd1, sizeof(struct tracker)) == -1)
        perror("error truncating tracker");
    if (ftruncate(fd2, mem_allocated) == -1)
        perror("error truncating shared slab");    
    /* Map shared memory tracker object */
    track = mmap(NULL, sizeof(struct tracker),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    
    track->cur_size = mem_allocated;
    track->allocated_size = 0;

    printf("tracking segment initialized");
    /* Map shared memory slab segment */
    slab_start = mmap(NULL, mem_allocated, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0); 

    if (track == MAP_FAILED) {
        perror("mapping tracker failed");
    }
    if (slab_start == MAP_FAILED) {
        perror("mapping slab failed");
    }
    printf("shared slab initialized");
}
