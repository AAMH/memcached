struct tracker {        /* Shared memory structure to keep track of current pointer */
    size_t max_size;
    size_t used_size;
    size_t spare_off;

    void * start;               // start of the shared memory
    void * start_address;       // address returned to tenants when asked for a slab
    void * avail_address;       

    void * spare_mem_start;     // victim slab address
    int    spare_mem_clsid;     // victim slab class
    bool   spare_mem_avail;     // indicating if a tenant already released one of its slabs
    bool   spare_lock;          // lock preventing more than one tenant to access the spare slab
    bool   spare_requested;

    long   min_misses;          // shared variable used to find the minimum number of shadowq hits among tenants
    int    min_id;
};

char tracker_name[20], semaph_name[20], slab_name[20];
void init_shared_names(int x);

void * shm_malloc(size_t n);    // MAIN ALLOCATION FUNCTION
void * shm_mallocAt(size_t n);  // allocates the spare slab

void * set_spare_mem(void * ptr, int id);
int    get_spare_clsid();


bool   set_min_miss(long misses,int id);
bool   req_spare();
bool   spare_needed();
bool   reset_locks();           // resets spare availability
bool   lock_spare();
bool   unlock_spare();
bool   is_spare_avail();

extern struct tracker get_tracker(void);    // function that each instance can use to get the most recent status of the tracker