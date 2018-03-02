struct tracker {        /* Shared memory structure to keep track of current pointer */
    size_t max_size;
    off_t allocated_size;
    void * start_address;
    void * last_address;
};

extern struct tracker shm_malloc(size_t n);
extern struct tracker get_tracker(void);
