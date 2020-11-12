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

    double   min_score;          
    int    min_id;

    double   max_score;          
    int    max_id;

    double    preset_share[4];
};

char tracker_name[20], semaph_name[20], slab_name[20];
void init_shared_names(int x);

void * shm_malloc(size_t n);    // MAIN ALLOCATION FUNCTION
void * shm_mallocAt(size_t n, int id);  // allocates the spare slab

void * set_spare_mem(void * ptr, size_t n, int cls, int id);
int    get_spare_clsid();


bool   set_scores(double sc1, double sc2, int id);
bool   compare_minID(int id,double sc);
bool   compare_maxID(int id,double sc);
bool   req_spare();
bool   spare_needed();
bool   reset_locks();           // resets spare availability
bool   lock_spare();
bool   unlock_spare();
bool   is_spare_avail();
bool   signal_alloc_free(int id, size_t size);

extern struct tracker get_tracker(void);    // function that each instance can use to get the most recent status of the tracker