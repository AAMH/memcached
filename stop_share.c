/* 
    Unlinks the shared memory segment
*/
#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>

int main () {
    shm_unlink("/slabs");
    shm_unlink("/tracker");
    shm_unlink("/semaph");
}
