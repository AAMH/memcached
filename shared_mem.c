/* 
    Toy Program for exploratory testing of POSIX shared memory 
    Creates a shared memory segment and stores a struct in the segment for later access
*/
#include <sys/mman.h>   /* shared memory and mmap() */
#include <unistd.h>     /* for getopt() */
#include <errno.h>      /* errno and perror */
#include <fcntl.h>      /* O_flags */
#include <stdio.h>


#define MAX_LEN 10000
struct region {        /* Defines "structure" of shared memory */
    int len;
    void* ptr;
};

struct test {
    int test;	
};

struct region *rptr;
struct region *rptr2;
struct test *test1;
int fd;
int main()
{
    fd = shm_open("/testregion", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1)
    /* Handle error */;

    if (ftruncate(fd, 2*sizeof(struct region) + sizeof(struct test)) == -1)
    /* Handle error */;


    /* Map shared memory object */

    rptr = mmap(NULL, 2*sizeof(struct region) + sizeof(struct test),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
    rptr->ptr = rptr;
    printf("stored mem: %p\n", rptr);
    rptr2 = rptr+sizeof(struct region);
    printf("stored mem2: %p\n", rptr2);
    rptr2->ptr = rptr2;
    test1 = rptr2+sizeof(struct test);
    test1->test = 1;
    printf("stored test: %d\n", test1->test);
    if (rptr == MAP_FAILED) {;}
    
    else  {
        int pid = fork();
        if (pid == 0)
        {
            rptr->len++;
	    rptr2->len++;
            printf("access shared mem in child: %d\n", rptr->len);
            printf("access shared mem in child2: %d\n", rptr2->len);	 
	    exit(0);
        }
        else {
            rptr->len = 1;
	    rptr2->len = 2;
            printf("access shared mem in parent: %d\n",rptr->len);
            printf("access shared mem in parent2: %d\n", rptr2->len);

        }
    }
    
}
