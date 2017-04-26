#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "shm_malloc.h"

/* test main */
int main() {
    char *test = shm_malloc(12*sizeof(char));
    if(test == NULL)
	exit(1);
    strcpy(test, "testingonly");

    char *test2 = shm_malloc(13*sizeof(char));
    strcpy(test2, "testingonly2");

    printf("The test string is: %s\n", test);
    printf("The second test string is: %s\n", test2);
    
}
