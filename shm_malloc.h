struct tracker {        /* Shared memory structure to keep track of current pointer */
    size_t max_size;
    size_t used_size;

    void * start_address;       // start of the shared memory
    void * avail_address;       // address returned to tenants when asked for a slab

    void * spare_mem_start;     // victim slab address
    int    spare_mem_clsid;     // victim slab class
    bool   spare_mem_avail;     // indicating if a tenant already released one of its slabs
    bool   spare_lock;          // lock preventing more than one tenant to access the spare slab
};

void * shm_malloc(size_t n);    // MAIN ALLOCATION FUNCTION
void * shm_mallocAt(size_t n);  // allocates the spare slab

void * get_spare_mem();         // return the spare slab and resets it
void * set_spare_mem(void * ptr, int id);
int    get_spare_clsid();


bool   reset_locks();           // resets bool variables
bool   is_spare_locked();
bool   is_spare_avail();

extern struct tracker get_tracker(void);    // function that each instance can use to get the most recent status of the tracker

