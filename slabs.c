//* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
//#define DEBUG_SLAB_MOVER
/* powers-of-N allocation structures */

typedef struct {
    unsigned int size;      /* sizes of items */
    unsigned int perslab;   /* how many items per slab */

    void *slots;           /* list of item ptrs */
    unsigned int sl_curr;   /* total free items in list */

    unsigned int slabs;     /* how many slabs were allocated for this class */

    void **slab_list;       /* array of slab pointers */
    unsigned int list_size; /* size of prev array */

    size_t requested; /* The number of requested bytes */

    uint32_t hits[1000];

/*** shadow queue Additions ***/
    shadow_item *shadowq_head;
    shadow_item *shadowq_tail;
    unsigned int shadowq_size;
    uint32_t shadowq_max_items;
    uint32_t shadowq_hits[1000];
    //uint32_t shadowq_hits;
    uint32_t q_misses;
    shadow_item **shadow_page_list;
} slabclass_t;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
/* If the memory limit has been hit once. Used as a hint to decide when to
 * early-wake the LRU maintenance thread */
static bool mem_limit_reached = false;
static int power_largest;

static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;
static int prev_victim = POWER_SMALLEST;
static int slab_shadowq_dec_victim = POWER_SMALLEST;
static bool greedy = false;

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;


static bool rebal_source_ext = false;       // indicating if reassigning is being used to take memory from an external source
static bool rebal_dest_ext = false;         // indicating if reassigning is being used to give memory to an external destination
static bool is_taken_careof = false;
static int check_counter = 0;
static int pages_to_request = -1;
static int pages_to_release = -1;
static int slab_to_release = -1;

void checkForSpare();
void waitForSpare();
void update_shadow_page_list(int id, int pageN, bool is_increasing, shadow_item *elem);
/*
 * Forward Declarations
 */
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0 || size > settings.item_size_max)
        return 0;
    while (size > slabclass[res].size)
        if (res++ == power_largest)     /* won't fit in the biggest slab */
            return power_largest;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes, const bool isgreedy) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;
    greedy = isgreedy;
    mem_limit = limit;
    
    if (prealloc) {
        /* Allocate everything in a big chunk with malloc */ 
        void * ptr = shm_malloc(mem_limit);
        struct tracker  trck = get_tracker();
        mem_base = trck.avail_address;
        if(greedy)
            mem_limit = trck.max_size;
        if (mem_base != NULL) {
            mem_current = mem_base;
            if(greedy)
                mem_avail = trck.max_size - trck.used_size;
            else
                mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
    }

    memset(slabclass, 0, sizeof(slabclass));

    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
        if (slab_sizes != NULL) {
            if (slab_sizes[i-1] == 0)
                break;
            size = slab_sizes[i-1];
        } else if (size >= settings.slab_chunk_size_max / factor) {
            break;
        }
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        slabclass[i].size = size;
        slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
        slabclass[i].shadowq_max_items = settings.shadowq_size / slabclass[i].size;
        printf("shadowq_max_items: queue %d, slab_size %d, perslab %d, shadowq_size %d\n",i,size,slabclass[i].perslab, slabclass[i].shadowq_max_items);
        slabclass[i].shadow_page_list = (shadow_item**) malloc(1000*sizeof(shadow_item*));

        if (slab_sizes == NULL)
            size *= factor;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }

    power_largest = i;
    slabclass[power_largest].size = settings.slab_chunk_size_max;
    slabclass[power_largest].perslab = settings.slab_page_size / settings.slab_chunk_size_max;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            mem_malloced = (size_t)atol(t_initial_malloc);
        }

    }

    if (prealloc) {
        slabs_preallocate(power_largest);
    }
}

static void slabs_preallocate (const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
        if (++prealloc > maxslabs)
            return;
        if (do_slabs_newslab(i) == 0) {
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max memory must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }

}

static int grow_slab_list (const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    if (p->slabs == p->list_size) {
        size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
        p->list_size = new_size;
        p->slab_list = new_list;
    }
    return 1;
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    int x;
    for (x = 0; x < p->perslab; x++) {
        do_slabs_free(ptr, 0, id);
        ((item *)ptr)->page_id = p->slabs;
        ptr += p->size;
    }
}

/* Fast FIFO queue */
static void *get_page_from_global_pool(void) {
    slabclass_t *p = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    if (p->slabs < 1) {
        return NULL;
    }
    char *ret = p->slab_list[p->slabs - 1];
    p->slabs--;
    return ret;
}

static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
        ? settings.slab_page_size
        : p->size * p->perslab;
    char *ptr; 

    if(greedy && !mem_limit_reached){   
        struct tracker trck = get_tracker();     
        mem_avail = trck.max_size - trck.used_size;
        mem_limit = mem_malloced + mem_avail ;
    }

    if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0
        && g->slabs == 0)) {
            mem_limit_reached = true;
            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return 0;
        }

    if(mem_limit_reached)
        return 0;
    
    if ((grow_slab_list(id) == 0) ||
        (((ptr = get_page_from_global_pool()) == NULL) &&
        ((ptr = memory_allocate((size_t)len)) == 0))) {

        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);
    split_slab_page_into_freelist(ptr, id);

    p->slab_list[p->slabs++] = ptr;
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

/*@null@*/
static void *do_slabs_alloc(const size_t size, unsigned int id, uint64_t *total_bytes,
        unsigned int flags) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }
    p = &slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);
    if (total_bytes != NULL) {
        *total_bytes = p->requested;
    }

    assert(size <= p->size);
    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (p->sl_curr == 0 && flags != SLABS_ALLOC_NO_NEWPAGE) {
        do_slabs_newslab(id);
    }

    if (p->sl_curr != 0) {
        /* return off our freelist */
        it = (item *)p->slots;
        p->slots = it->next;
        if (it->next) it->next->prev = 0;
        /* Kill flag and initialize refcount here for lock safety in slab
         * mover's freeness detection. */
        it->it_flags &= ~ITEM_SLABBED;
        it->refcount = 1;
        p->sl_curr--;
        ret = (void *)it;
    } else {
        ret = NULL;
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

static void do_slabs_free_chunked(item *it, const size_t size) {
    item_chunk *chunk = (item_chunk *) ITEM_data(it);
    slabclass_t *p;

    it->it_flags = ITEM_SLABBED;
    it->slabs_clsid = 0;
    it->prev = 0;
    // header object's original classid is stored in chunk.
    p = &slabclass[chunk->orig_clsid];
    if (chunk->next) {
        chunk = chunk->next;
        chunk->prev = 0;
    } else {
        // header with no attached chunk
        chunk = NULL;
    }

    // return the header object.
    // TODO: This is in three places, here and in do_slabs_free().
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
    p->sl_curr++;
    // TODO: macro
    p->requested -= it->nkey + 1 + it->nsuffix + sizeof(item) + sizeof(item_chunk);
    if (settings.use_cas) {
        p->requested -= sizeof(uint64_t);
    }

    item_chunk *next_chunk;
    while (chunk) {
        assert(chunk->it_flags == ITEM_CHUNK);
        chunk->it_flags = ITEM_SLABBED;
        p = &slabclass[chunk->slabs_clsid];
        chunk->slabs_clsid = 0;
        next_chunk = chunk->next;

        chunk->prev = 0;
        chunk->next = p->slots;
        if (chunk->next) chunk->next->prev = chunk;
        p->slots = chunk;
        p->sl_curr++;
        p->requested -= chunk->size + sizeof(item_chunk);

        chunk = next_chunk;
    }

    return;
}


static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

    it = (item *)ptr;
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        it->it_flags = ITEM_SLABBED;
        it->slabs_clsid = 0;
        it->prev = 0;
        it->next = p->slots;
        if (it->next) it->next->prev = it;
        p->slots = it;

        p->sl_curr++;
        p->requested -= size;
    } else {
        do_slabs_free_chunked(it, size);
    }
    return;
}

static int nz_strcmp(int nzlength, const char *nz, const char *z) {
    int zlength=strlen(z);
    return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    if (add_stats != NULL) {
        if (!stat_type) {
            /* prepare general statistics for the engine */
            STATS_LOCK();
            APPEND_STAT("bytes", "%llu", (unsigned long long)stats_state.curr_bytes);
            APPEND_STAT("curr_items", "%llu", (unsigned long long)stats_state.curr_items);
            APPEND_STAT("total_items", "%llu", (unsigned long long)stats.total_items);
            STATS_UNLOCK();
            if (settings.slab_automove > 0) {
                pthread_mutex_lock(&slabs_lock);
                APPEND_STAT("slab_global_page_pool", "%u", slabclass[SLAB_GLOBAL_PAGE_POOL].slabs);
                pthread_mutex_unlock(&slabs_lock);
            }
            item_stats_totals(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "items") == 0) {
            item_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "slabs") == 0) {
            slabs_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes") == 0) {
            item_stats_sizes(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_enable") == 0) {
            item_stats_sizes_enable(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_disable") == 0) {
            item_stats_sizes_disable(add_stats, c);
        } else {
            ret = false;
        }
    } else {
        ret = false;
    }

    return ret;
}

/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;

     //if(!is_spare_avail())
       // checkForSpare();
    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);

    total = 0;
    for(i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;
            
            for(int j = 0;j < p->slabs; j++){
            APPEND_NUM_STAT(i, "slab pointer", "%p", p->slab_list[j]);
            } 

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            APPEND_NUM_STAT(i, "used_chunks", "%u",
                            slabs*perslab - p->sl_curr);
            APPEND_NUM_STAT(i, "free_chunks", "%u", p->sl_curr);
            /* Stat is dead, but displaying zero instead of removing it. */
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", 0);
            APPEND_NUM_STAT(i, "mem_requested", "%llu",
                            (unsigned long long)p->requested);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "shadowq_hits", "%llu",
                     (unsigned long long)thread_stats.slab_stats[i].shadowq_hits);
            APPEND_NUM_STAT(i, "q_misses", "%llu",
                     (unsigned long long)thread_stats.slab_stats[i].q_misses);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)mem_malloced);
    add_stats(NULL, 0, NULL, 0, c);
}

// void force_release() {

//     int i = 0;
//     int j = MAX_NUMBER_OF_SLAB_CLASSES-1;
//     slabclass_t *sc = &slabclass[j];
//     while (sc->slabs < 1 && j >= 0) {
//             sc = &slabclass[--j];        
//     }
//     for(int i = 0; i < 100; i++){
//         for(int k = sc->slabs - 1; k >= 0;k--){
//             printf("Number : %d From class : %d\n",i,j);
//             void *p = sc->slab_list[k];
//             sc->slabs--;
             
//             mem_malloced -= 1051936; 
//             add_released(p, 1051936);
//             struct tracker trck = get_tracker();
//             mem_avail = trck.max_size - trck.used_size;
//             mem_limit = mem_malloced + mem_avail ;

//             printf("PAGE RELEASED at %p! \n",p);
//             if(k > 0)
//                 i++;
//             if(i>=100)
//                 break;
//      //   free(p);
//         }
//         sc = &slabclass[--j];
//         while (sc->slabs < 1 && j >= 0) {
//             sc = &slabclass[--j];        
//         }
//         if(j < 0)
//             break;
//     }
// }

// void force_allocate(){

//     int id = 3;
//     slabclass_t *p = &slabclass[id];

//     int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size)
//         ? settings.slab_page_size
//         : p->size * p->perslab;

//     //size_t size = (size_t) len;
//     size_t size = (size_t) 1047840;

//     for(int i = 0;i < 100; i++){
//         void *ptr = shm_malloc2(size);
//         if(ptr != NULL){
//             struct tracker trck = get_tracker();
            
//             mem_avail = trck.max_size - trck.used_size;
//             mem_malloced += size;
//             mem_limit = mem_malloced + mem_avail ;
            
//             printf("total allocated mem(this): %lu\n", mem_malloced);  

//         //  memset(ptr, 0, size);
//         //  split_slab_page_into_freelist(ptr, id);

//             p->slab_list[p->slabs++] = ptr;
//             MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);
//         }
//     }
// }

static void *memory_allocate(size_t size) {
    void *ret;
    
    if (mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */  
        ret = shm_malloc(size);
        size_t PAGESIZE = sysconf(_SC_PAGESIZE);
        int index = size/PAGESIZE;
        size = (index+1) * PAGESIZE;   
        mem_malloced += size;
        printf("total allocated mem(this): %lu\n", mem_malloced);  
    
    } else {
        ret = mem_current;

        if (size > mem_avail) {
            return NULL;
        }

        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }
    if(greedy){
        struct tracker trck = get_tracker();
        mem_avail = trck.max_size - trck.used_size;
        mem_limit = mem_malloced + mem_avail ;
    }
    return ret;
}

/* Must only be used if all pages are item_size_max */
static void memory_release() {
    void *p = NULL;
    if (mem_base != NULL)
        return;

    if (!settings.slab_reassign)
        return;

    while (mem_malloced > mem_limit &&
            (p = get_page_from_global_pool()) != NULL) {
    //    free(p);
        munmap(p,settings.item_size_max);
        mem_malloced -= settings.item_size_max;
    }
}



void *slabs_alloc(size_t size, unsigned int id, uint64_t *total_bytes,
        unsigned int flags) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id, total_bytes, flags);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_stats(add_stats, c);
    pthread_mutex_unlock(&slabs_lock);
}

static bool do_slabs_adjust_mem_limit(size_t new_mem_limit) {
    /* Cannot adjust memory limit at runtime if prealloc'ed */
    if (mem_base != NULL)
        return false;
    settings.maxbytes = new_mem_limit;
    mem_limit = new_mem_limit;
    mem_limit_reached = false; /* Will reset on next alloc */
    memory_release(); /* free what might already be in the global pool */
    return true;
}

bool slabs_adjust_mem_limit(size_t new_mem_limit) {
    bool ret;
    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_adjust_mem_limit(new_mem_limit);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)
{
    pthread_mutex_lock(&slabs_lock);
    slabclass_t *p;
    if (id < POWER_SMALLEST || id > power_largest) {
        fprintf(stderr, "Internal error! Invalid slab class\n");
        abort();
    }

    p = &slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&slabs_lock);
}

unsigned int slabs_available_chunks(const unsigned int id, bool *mem_flag,
        uint64_t *total_bytes, unsigned int *chunks_perslab) {
    unsigned int ret;
    slabclass_t *p;

    pthread_mutex_lock(&slabs_lock);
    p = &slabclass[id];
    ret = p->sl_curr;
    if (mem_flag != NULL)
        *mem_flag = mem_limit_reached;
    if (total_bytes != NULL)
        *total_bytes = p->requested;
    if (chunks_perslab != NULL)
        *chunks_perslab = p->perslab;
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

/* The slabber system could avoid needing to understand much, if anything,
 * about items if callbacks were strategically used. Due to how the slab mover
 * works, certain flag bits can only be adjusted while holding the slabs lock.
 * Using these functions, isolate sections of code needing this and turn them
 * into callbacks when an interface becomes more obvious.
 */
void slabs_mlock(void) {
    pthread_mutex_lock(&slabs_lock);
}

void slabs_munlock(void) {
    pthread_mutex_unlock(&slabs_lock);
}

static pthread_cond_t slab_rebalance_cond = PTHREAD_COND_INITIALIZER;
static volatile int do_run_slab_thread = 1;
static volatile int do_run_slab_rebalance_thread = 1;

#define DEFAULT_SLAB_BULK_CHECK 1
int slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;

static int slab_rebalance_start(void) {
    slabclass_t *s_cls;
    int no_go = 0;

    pthread_mutex_lock(&slabs_lock);

    /* source is not external AND it's not just releasing for someone else*/
    if(!rebal_source_ext && !rebal_dest_ext){
        if (slab_rebal.s_clsid < POWER_SMALLEST ||
            slab_rebal.s_clsid > power_largest  ||
            slab_rebal.d_clsid < SLAB_GLOBAL_PAGE_POOL ||
            slab_rebal.d_clsid > power_largest  ||
            slab_rebal.s_clsid == slab_rebal.d_clsid)
            no_go = -2;

         s_cls = &slabclass[slab_rebal.s_clsid];

        if (s_cls->slabs < 2)
            no_go = -3;

        if (!grow_slab_list(slab_rebal.d_clsid)) {
            no_go = -1;
        }
    }
    if(rebal_source_ext)
        if(!grow_slab_list(slab_rebal.d_clsid)) {
            no_go = -1;
        }
    s_cls = &slabclass[slab_rebal.s_clsid];

    if(rebal_source_ext && s_cls->slabs == 0)
        s_cls = &slabclass[slab_rebal.d_clsid];

    if (no_go != 0) {
        pthread_mutex_unlock(&slabs_lock);
        return no_go; /* Should use a wrapper function... */
    }

    /* Always kill the first available slab page as it is most likely to
        * contain the oldest items
        */
    if(rebal_dest_ext)
        slab_rebal.slab_start = s_cls->slab_list[slab_to_release];
    else
        slab_rebal.slab_start = s_cls->slab_list[0];

    slab_rebal.slab_end   = (char *)slab_rebal.slab_start +
        (s_cls->size * s_cls->perslab);
    slab_rebal.slab_pos   = slab_rebal.slab_start;
    slab_rebal.done       = 0;

    /* Also tells do_item_get to search for items in this slab */
    slab_rebalance_signal = 2;
        
    if(rebal_dest_ext && !is_taken_careof){
        //set_spare_mem(slab_rebal.slab_start,slab_rebal.s_clsid);
        printf("releasing address range: %p to %p \n",slab_rebal.slab_start,slab_rebal.slab_end);
        is_taken_careof = true;
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "Started a slab rebalance\n");
    }

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats_state.slab_reassign_running = true;
    STATS_UNLOCK();

    return 0;
}

/* CALLED WITH slabs_lock HELD */
static void *slab_rebalance_alloc(const size_t size, unsigned int id) {
    slabclass_t *s_cls;
    s_cls = &slabclass[slab_rebal.s_clsid];
    int x;
    item *new_it = NULL;

    for (x = 0; x < s_cls->perslab; x++) {
        new_it = do_slabs_alloc(size, id, NULL, SLABS_ALLOC_NO_NEWPAGE);
        /* check that memory isn't within the range to clear */
        if (new_it == NULL) {
            break;
        }
        if ((void *)new_it >= slab_rebal.slab_start
            && (void *)new_it < slab_rebal.slab_end) {
            /* Pulled something we intend to free. Mark it as freed since
             * we've already done the work of unlinking it from the freelist.
             */
            s_cls->requested -= size;
            new_it->refcount = 0;
            new_it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
            memcpy(ITEM_key(new_it), "deadbeef", 8);
#endif
            new_it = NULL;
            slab_rebal.inline_reclaim++;
        } else {
            break;
        }
    }
    return new_it;
}

/* CALLED WITH slabs_lock HELD */
/* detatches item/chunk from freelist. */
static void slab_rebalance_cut_free(slabclass_t *s_cls, item *it) {
    /* Ensure this was on the freelist and nothing else. */
    assert(it->it_flags == ITEM_SLABBED);
    if (s_cls->slots == it) {
        s_cls->slots = it->next;
    }
    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    s_cls->sl_curr--;
}

enum move_status {
    MOVE_PASS=0, MOVE_FROM_SLAB, MOVE_FROM_LRU, MOVE_BUSY, MOVE_LOCKED
};

/* refcount == 0 is safe since nobody can incr while item_lock is held.
 * refcount != 0 is impossible since flags/etc can be modified in other
 * threads. instead, note we found a busy one and bail. logic in do_item_get
 * will prevent busy items from continuing to be busy
 * NOTE: This is checking it_flags outside of an item lock. I believe this
 * works since it_flags is 8 bits, and we're only ever comparing a single bit
 * regardless. ITEM_SLABBED bit will always be correct since we're holding the
 * lock which modifies that bit. ITEM_LINKED won't exist if we're between an
 * item having ITEM_SLABBED removed, and the key hasn't been added to the item
 * yet. The memory barrier from the slabs lock should order the key write and the
 * flags to the item?
 * If ITEM_LINKED did exist and was just removed, but we still see it, that's
 * still safe since it will have a valid key, which we then lock, and then
 * recheck everything.
 * This may not be safe on all platforms; If not, slabs_alloc() will need to
 * seed the item key while holding slabs_lock.
 */
static int slab_rebalance_move(void) {
    slabclass_t *s_cls;
    int x;
    int was_busy = 0;
    int refcount = 0;
    uint32_t hv;
    void *hold_lock;
    enum move_status status = MOVE_PASS;

    pthread_mutex_lock(&slabs_lock);

    if(!rebal_source_ext){
    s_cls = &slabclass[slab_rebal.s_clsid];

    for (x = 0; x < slab_bulk_check; x++) {
        hv = 0;
        hold_lock = NULL;
        item *it = slab_rebal.slab_pos;
        item_chunk *ch = NULL;
        status = MOVE_PASS;
        if (it->it_flags & ITEM_CHUNK) {
            /* This chunk is a chained part of a larger item. */
            ch = (item_chunk *) it;
            /* Instead, we use the head chunk to find the item and effectively
             * lock the entire structure. If a chunk has ITEM_CHUNK flag, its
             * head cannot be slabbed, so the normal routine is safe. */
            it = ch->head;
            assert(it->it_flags & ITEM_CHUNKED);
        }
        /* ITEM_FETCHED when ITEM_SLABBED is overloaded to mean we've cleared
         * the chunk for move. Only these two flags should exist.
         */
        if (it->it_flags != (ITEM_SLABBED|ITEM_FETCHED)) {
            /* ITEM_SLABBED can only be added/removed under the slabs_lock */
            if (it->it_flags & ITEM_SLABBED) {
                assert(ch == NULL);
                slab_rebalance_cut_free(s_cls, it);
                status = MOVE_FROM_SLAB;
            } else if ((it->it_flags & ITEM_LINKED) != 0) {
                /* If it doesn't have ITEM_SLABBED, the item could be in any
                 * state on its way to being freed or written to. If no
                 * ITEM_SLABBED, but it's had ITEM_LINKED, it must be active
                 * and have the key written to it already.
                 */
                hv = hash(ITEM_key(it), it->nkey);
                if ((hold_lock = item_trylock(hv)) == NULL) {
                    status = MOVE_LOCKED;
                } else {
                    refcount = refcount_incr(it);
                    if (refcount == 2) { /* item is linked but not busy */
                        /* Double check ITEM_LINKED flag here, since we're
                         * past a memory barrier from the mutex. */
                        if ((it->it_flags & ITEM_LINKED) != 0) {
                            status = MOVE_FROM_LRU;
                        } else {
                            /* refcount == 1 + !ITEM_LINKED means the item is being
                             * uploaded to, or was just unlinked but hasn't been freed
                             * yet. Let it bleed off on its own and try again later */
                            status = MOVE_BUSY;
                        }
                    } else {
                        if (settings.verbose > 2) {
                            fprintf(stderr, "Slab reassign hit a busy item: refcount: %d (%d -> %d)\n",
                                it->refcount, slab_rebal.s_clsid, slab_rebal.d_clsid);
                        }
                        status = MOVE_BUSY;
                    }
                    /* Item lock must be held while modifying refcount */
                    if (status == MOVE_BUSY) {
                        refcount_decr(it);
                        item_trylock_unlock(hold_lock);
                    }
                }
            } else {
                /* See above comment. No ITEM_SLABBED or ITEM_LINKED. Mark
                 * busy and wait for item to complete its upload. */
                status = MOVE_BUSY;
            }
        }

        int save_item = 0;
        item *new_it = NULL;
        size_t ntotal = 0;
        switch (status) {
            case MOVE_FROM_LRU:
                /* Lock order is LRU locks -> slabs_lock. unlink uses LRU lock.
                 * We only need to hold the slabs_lock while initially looking
                 * at an item, and at this point we have an exclusive refcount
                 * (2) + the item is locked. Drop slabs lock, drop item to
                 * refcount 1 (just our own, then fall through and wipe it
                 */
                /* Check if expired or flushed */
                ntotal = ITEM_ntotal(it);
                /* REQUIRES slabs_lock: CHECK FOR cls->sl_curr > 0 */
                if (ch == NULL && (it->it_flags & ITEM_CHUNKED)) {
                    /* Chunked should be identical to non-chunked, except we need
                     * to swap out ntotal for the head-chunk-total. */
                    ntotal = s_cls->size;
                }
                if ((it->exptime != 0 && it->exptime < current_time)
                    || item_is_flushed(it)) {
                    /* Expired, don't save. */
                    save_item = 0;
                } else if (ch == NULL &&
                        (new_it = slab_rebalance_alloc(ntotal, slab_rebal.s_clsid)) == NULL) {
                    /* Not a chunk of an item, and nomem. */
                    save_item = 0;
                    slab_rebal.evictions_nomem++;
                } else if (ch != NULL &&
                        (new_it = slab_rebalance_alloc(s_cls->size, slab_rebal.s_clsid)) == NULL) {
                    /* Is a chunk of an item, and nomem. */
                    save_item = 0;
                    slab_rebal.evictions_nomem++;
                } else {
                    /* Was whatever it was, and we have memory for it. */
                    save_item = 1;
                }
                pthread_mutex_unlock(&slabs_lock);
                unsigned int requested_adjust = 0;
                if (save_item) {
                    if (ch == NULL) {
                        assert((new_it->it_flags & ITEM_CHUNKED) == 0);
                        /* if free memory, memcpy. clear prev/next/h_bucket */
                        memcpy(new_it, it, ntotal);
                        new_it->prev = 0;
                        new_it->next = 0;
                        new_it->h_next = 0;
                        /* These are definitely required. else fails assert */
                        new_it->it_flags &= ~ITEM_LINKED;
                        new_it->refcount = 0;
                        do_item_replace(it, new_it, hv);
                        /* Need to walk the chunks and repoint head  */
                        if (new_it->it_flags & ITEM_CHUNKED) {
                            item_chunk *fch = (item_chunk *) ITEM_data(new_it);
                            fch->next->prev = fch;
                            while (fch) {
                                fch->head = new_it;
                                fch = fch->next;
                            }
                        }
                        it->refcount = 0;
                        it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
                        memcpy(ITEM_key(it), "deadbeef", 8);
#endif
                        slab_rebal.rescues++;
                        requested_adjust = ntotal;
                    } else {
                        item_chunk *nch = (item_chunk *) new_it;
                        /* Chunks always have head chunk (the main it) */
                        ch->prev->next = nch;
                        if (ch->next)
                            ch->next->prev = nch;
                        memcpy(nch, ch, ch->used + sizeof(item_chunk));
                        ch->refcount = 0;
                        ch->it_flags = ITEM_SLABBED|ITEM_FETCHED;
                        slab_rebal.chunk_rescues++;
#ifdef DEBUG_SLAB_MOVER
                        memcpy(ITEM_key((item *)ch), "deadbeef", 8);
#endif
                        refcount_decr(it);
                        requested_adjust = s_cls->size;
                    }
                } else {
                    /* restore ntotal in case we tried saving a head chunk. */
                    ntotal = ITEM_ntotal(it);
                    do_item_unlink(it, hv);
                    slabs_free(it, ntotal, slab_rebal.s_clsid);
                    /* Swing around again later to remove it from the freelist. */
                    slab_rebal.busy_items++;
                    was_busy++;
                }
                item_trylock_unlock(hold_lock);
                pthread_mutex_lock(&slabs_lock);
                /* Always remove the ntotal, as we added it in during
                 * do_slabs_alloc() when copying the item.
                 */
                s_cls->requested -= requested_adjust;
                break;
            case MOVE_FROM_SLAB:
                it->refcount = 0;
                it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
                memcpy(ITEM_key(it), "deadbeef", 8);
#endif
                break;
            case MOVE_BUSY:
            case MOVE_LOCKED:
                slab_rebal.busy_items++;
                was_busy++;
                break;
            case MOVE_PASS:
                break;
        }

        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }

    if (slab_rebal.slab_pos >= slab_rebal.slab_end) {
        /* Some items were busy, start again from the top */
        if (slab_rebal.busy_items) {
            slab_rebal.slab_pos = slab_rebal.slab_start;
            STATS_LOCK();
            stats.slab_reassign_busy_items += slab_rebal.busy_items;
            STATS_UNLOCK();
            slab_rebal.busy_items = 0;
        } else {
            slab_rebal.done++;
        }
    }
    }
    else{
        slab_rebal.done++;      
    }

    pthread_mutex_unlock(&slabs_lock);

    return was_busy;
}

static void slab_rebalance_finish(void) {
    slabclass_t *s_cls;
    slabclass_t *d_cls;
    int x;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;
    uint32_t chunk_rescues;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &slabclass[slab_rebal.s_clsid];
    if(slab_rebal.d_clsid != -2)        // When destination is external
        d_cls = &slabclass[slab_rebal.d_clsid];

#ifdef DEBUG_SLAB_MOVER
    /* If the algorithm is broken, live items can sneak in. */
    slab_rebal.slab_pos = slab_rebal.slab_start;
    while (1) {
        item *it = slab_rebal.slab_pos;
        assert(it->it_flags == (ITEM_SLABBED|ITEM_FETCHED));
        assert(memcmp(ITEM_key(it), "deadbeef", 8) == 0);
        it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }
#endif

    /* At this point the stolen slab is completely clear.
     * We always kill the "first"/"oldest" slab page in the slab_list, so
     * shuffle the page list backwards and decrement.
     */

    void * pp = NULL;
    if(!rebal_source_ext){      // No need to do this when we are in receiver side of reassigning
        s_cls->slabs--;
        pp = s_cls->slab_list[slab_to_release];
        for (x = slab_to_release; x < s_cls->slabs; x++) {
            s_cls->slab_list[x] = s_cls->slab_list[x + 1];
            s_cls->hits[x] = s_cls->hits[x + 1];
        }
        s_cls->slab_list[s_cls->slabs] = NULL;
        s_cls->hits[s_cls->slabs] = 0;

        if(rebal_dest_ext){
            set_spare_mem(pp,slab_rebal.s_clsid);
            printf("    address: %p \n",pp);
            size_t size  = (size_t)(s_cls->size * s_cls->perslab);
            munmap(pp,size);
        }
    }
    /* taking care of destination */
    if(!rebal_dest_ext){
        if(rebal_source_ext){
            item * ppp = (item *)shm_mallocAt((size_t)(d_cls->size * d_cls->perslab));
            memset(ppp, 0, (size_t)(d_cls->size * d_cls->perslab));
            d_cls->slab_list[d_cls->slabs++] = ppp;
            printf("    address: %p \n",ppp); 
            int temp = d_cls->sl_curr;
            split_slab_page_into_freelist(d_cls->slab_list[d_cls->slabs-1], slab_rebal.d_clsid);
            printf("%d items added\n",d_cls->sl_curr - temp);
        }
        else{   
            d_cls->slab_list[d_cls->slabs++] = slab_rebal.slab_start;
            /* Don't need to split the page into chunks if we're just storing it */
            if (slab_rebal.d_clsid > SLAB_GLOBAL_PAGE_POOL) {
                memset(d_cls->slab_list[d_cls->slabs-1], 0, (size_t)(s_cls->size * s_cls->perslab));
                split_slab_page_into_freelist(d_cls->slab_list[d_cls->slabs-1], slab_rebal.d_clsid);
            } else if (slab_rebal.d_clsid == SLAB_GLOBAL_PAGE_POOL) {
                /* mem_malloc'ed might be higher than mem_limit. */
                memory_release();
            }
        }
    }
    
    /* adjust memory limit since a slab was reassigned to/from external source/destination */
    if(rebal_source_ext || rebal_dest_ext){
        if(rebal_source_ext)
           mem_malloced += (size_t)(s_cls->size * s_cls->perslab);
        else
           mem_malloced -= (size_t)(s_cls->size * s_cls->perslab);
        struct tracker trck = get_tracker();     
        mem_avail = trck.max_size - trck.used_size;
        mem_limit = mem_malloced + mem_avail ;
        unlock_spare();
        printf("total allocated mem(this) : %ld\n",mem_malloced);
    }

    slab_rebal.done       = 0;
    slab_rebal.s_clsid    = 0;
    slab_rebal.slab_start = NULL;
    slab_rebal.slab_end   = NULL;
    slab_rebal.slab_pos   = NULL;
    evictions_nomem    = slab_rebal.evictions_nomem;
    inline_reclaim = slab_rebal.inline_reclaim;
    rescues   = slab_rebal.rescues;
    chunk_rescues = slab_rebal.chunk_rescues;
    slab_rebal.evictions_nomem    = 0;
    slab_rebal.inline_reclaim = 0;
    slab_rebal.rescues  = 0;


    if(rebal_source_ext){
        reset_locks();
        slab_rebalance_signal = 10;
    }
    else{
        slab_to_release = -1;
        slab_rebalance_signal = 0;
        slab_rebal.d_clsid    = 0;
    }
    rebal_source_ext = false;
    rebal_dest_ext = false;

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slabs_moved++;
    stats.slab_reassign_rescues += rescues;
    stats.slab_reassign_evictions_nomem += evictions_nomem;
    stats.slab_reassign_inline_reclaim += inline_reclaim;
    stats.slab_reassign_chunk_rescues += chunk_rescues;
    stats_state.slab_reassign_running = false;
    STATS_UNLOCK();

    if (settings.verbose > 1) {
        fprintf(stderr, "finished a slab move\n");
    }
}

/* Slab mover thread.
 * Sits waiting for a condition to jump off and shovel some memory about
 */
static void *slab_rebalance_thread(void *arg) {
    int was_busy = 0;
    /* So we first pass into cond_wait with the mutex held */
    mutex_lock(&slabs_rebalance_lock);

    while (do_run_slab_rebalance_thread) {
        if (slab_rebalance_signal == 1) {
            if (slab_rebalance_start() < 0) {
                /* Handle errors with more specifity as required. */
                slab_rebalance_signal = 0;
            }

            was_busy = 0;
        } else if(slab_rebalance_signal == 10){
            waitForSpare();
        } else if(slab_rebalance_signal && slab_rebal.slab_start != NULL) {
            was_busy = slab_rebalance_move();
        }

        if (slab_rebal.done) {
            slab_rebalance_finish();
        } else if (was_busy) {
            /* Stuck waiting for some items to unlock, so slow down a bit
             * to give them a chance to free up */
            usleep(50);
        }
        if (slab_rebalance_signal == 0) {
            /* always hold this lock while we're running */
            pthread_cond_wait(&slab_rebalance_cond, &slabs_rebalance_lock);
        }
    }
    return NULL;
}

/* Iterate at most once through the slab classes and pick a "random" source.
 * I like this better than calling rand() since rand() is slow enough that we
 * can just check all of the classes once instead.
 */
static int slabs_reassign_pick_any(int dst) {
    static int cur = POWER_SMALLEST - 1;
    int tries = power_largest - POWER_SMALLEST + 1;
    for (; tries > 0; tries--) {
        cur++;
        if (cur > power_largest)
            cur = POWER_SMALLEST;
        if (cur == dst)
            continue;
        if (slabclass[cur].slabs > 1) {
            return cur;
        }
    }
    return -1;
}

static enum reassign_result_type do_slabs_reassign(int src, int dst) {
    if (slab_rebalance_signal != 0)
        return REASSIGN_RUNNING;

    if(src == 1000){    //  does not know the source clsid, thread should enter the state of "waiting" for a page to be released 
        slab_rebalance_signal = 10;
        slab_rebal.d_clsid = dst;
        pthread_cond_signal(&slab_rebalance_cond);
        return REASSIGN_OK;
    }

    if (src == dst)
        if(!rebal_source_ext)   // two slabs being in the same class is not a problem when they are from different tenants 
            return REASSIGN_SRC_DST_SAME;

    /* Special indicator to choose ourselves. */
    if (src == -1) {
        src = slabs_reassign_pick_any(dst);
        /* TODO: If we end up back at -1, return a new error type */
    }

    /* Indicating that victim is from an external source */
    if(rebal_source_ext){
         if (dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
            return REASSIGN_BADCLASS;
    }
    else if(rebal_dest_ext){
        if (src < POWER_SMALLEST        || src > power_largest )
            return REASSIGN_BADCLASS;

        if (slabclass[src].slabs < 2)
            return REASSIGN_NOSPARE;
    }
    else{
        if (src < POWER_SMALLEST        || src > power_largest ||
            dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
            return REASSIGN_BADCLASS;

        if (slabclass[src].slabs < 2)
            return REASSIGN_NOSPARE;

    }

    slab_rebal.s_clsid = src;
    slab_rebal.d_clsid = dst;

    slab_rebalance_signal = 1;
    pthread_cond_signal(&slab_rebalance_cond);
    
    return REASSIGN_OK;
}

enum reassign_result_type slabs_reassign(int src, int dst) {
    enum reassign_result_type ret;
    if (pthread_mutex_trylock(&slabs_rebalance_lock) != 0) {
        return REASSIGN_RUNNING;
    }
    ret = do_slabs_reassign(src, dst);
    pthread_mutex_unlock(&slabs_rebalance_lock);
    return ret;
}

/* If we hold this lock, rebalancer can't wake up or move */
void slabs_rebalancer_pause(void) {
    pthread_mutex_lock(&slabs_rebalance_lock);
}

void slabs_rebalancer_resume(void) {
    pthread_mutex_unlock(&slabs_rebalance_lock);
}

static pthread_t rebalance_tid;

int start_slab_maintenance_thread(void) {
    int ret;
    slab_rebalance_signal = 0;
    slab_rebal.slab_start = NULL;
    char *env = getenv("MEMCACHED_SLAB_BULK_CHECK");
    if (env != NULL) {
        slab_bulk_check = atoi(env);
        if (slab_bulk_check == 0) {
            slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;
        }
    }

    if (pthread_cond_init(&slab_rebalance_cond, NULL) != 0) {
        fprintf(stderr, "Can't intiialize rebalance condition\n");
        return -1;
    }
    pthread_mutex_init(&slabs_rebalance_lock, NULL);

    if ((ret = pthread_create(&rebalance_tid, NULL,
                              slab_rebalance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

int start_slab_rebalance_thread(void) {
     int ret;
     slab_rebalance_signal = 0;
     slab_rebal.slab_start = NULL;

     if ((ret = pthread_create(&rebalance_tid, NULL,
                           slab_rebalance_thread, NULL)) != 0) {
         fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
         return -1;
     }
     return 0;
 }


/* The maintenance thread is on a sleep/loop cycle, so it should join after a
 * short wait */
void stop_slab_maintenance_thread(void) {
    mutex_lock(&slabs_rebalance_lock);
    do_run_slab_thread = 0;
    do_run_slab_rebalance_thread = 0;
    pthread_cond_signal(&slab_rebalance_cond);
    pthread_mutex_unlock(&slabs_rebalance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(rebalance_tid, NULL);
}

void stop_slab_rebalance_thread(void) {
     mutex_lock(&slabs_rebalance_lock);
     do_run_slab_thread = 0;
     do_run_slab_rebalance_thread = 0;
     pthread_cond_signal(&slab_rebalance_cond);
     pthread_mutex_unlock(&slabs_rebalance_lock);
     pthread_join(rebalance_tid, NULL);
}

void slabs_stats_file_write(FILE *f, FILE *f2,struct thread_stats thread_stats){

    bool b = false;
    unsigned long long total_req = 0;

    threadlocal_stats_aggregate(&thread_stats);

    if(thread_stats.slab_stats[5].get_hits != 0)
        fprintf(f2,"%.2f,",(double)mem_malloced / 1047840);

    for(int i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];

        if (p->slabs != 0) {
            total_req = thread_stats.slab_stats[i].q_misses + thread_stats.slab_stats[i].get_hits;
            if(total_req){
                b = true;
                fprintf(f,"%.2f,",(double)thread_stats.slab_stats[i].get_hits / (double)total_req * 100);
                fflush(f);
                fprintf(f2,"%.2f,",(double)(p->slabs * p->perslab * p->size) / 1047840);
                fflush(f2);
            }
        }
    }

    if(b){
        fseek(f,-1,SEEK_CUR);
        fprintf(f,"\n");
        fseek(f2,-1,SEEK_CUR);
        fprintf(f2,"\n");
    }
}

void force_reassign(int d){
    rebal_source_ext = false; 
    rebal_dest_ext   = false;
    int counter = 0; //make sure we are not stuck in case there are no more slabs left
    do {
        if (counter++ > power_largest - POWER_SMALLEST + 1)
            break;
        prev_victim = ((prev_victim + 1 - POWER_SMALLEST) % (power_largest - POWER_SMALLEST + 1)) + POWER_SMALLEST; //round robin
    } while ((prev_victim == d) || (slabclass[prev_victim].slabs <= 1));

    printf("Internal REASSIGN: class %d  TO  class %d\n", prev_victim, d); 
    do_slabs_reassign(prev_victim,d);
}

int find_victim_slab(){

    int min_hit = 1000000;
    int min_id = 0;

    for(int i = POWER_SMALLEST;i <= power_largest;i++){
        if(slabclass[i].slabs >= 2 && slabclass[i].q_misses >= 0)
            if(slabclass[i].q_misses < min_hit){
                min_hit = slabclass[i].q_misses;
                min_id = i;
            }
    }
    
    return min_id;
}

void waitForSpare(){ // wait for a fresh page

    if(pages_to_request == 0){
        slab_rebalance_signal = 0;  
        rebal_source_ext = false;
        rebal_dest_ext   = false;  
    }
    else
    {
        if(is_spare_avail()){
            lock_spare();
            pages_to_request--;
            printf("\nExternal REASSIGN: Received 1 page(s) for class %d, Needs %d more\n", slab_rebal.d_clsid,pages_to_request);
            slab_rebal.s_clsid = get_spare_clsid();
            rebal_source_ext = true;
            rebal_dest_ext   = false;
            is_taken_careof  = false;    
            slab_rebalance_signal = 1;
        }
        else
            req_spare();
    }

}

void checkForSpare(){

    long total_shadowq_hits = 0;

    if(spare_needed()){ // Someone needs a page, we should release one!
        if((slab_rebalance_signal == 0 /*|| slab_rebalance_signal == 10*/)/* && lock_spare()*/){

            for(int i = POWER_SMALLEST;i <= power_largest;i++){
                if(slabclass[i].slabs > 0 && slabclass[i].shadowq_hits[0] > 0)
                    total_shadowq_hits += slabclass[i].shadowq_hits[0];
            }
            if(set_min_miss(total_shadowq_hits,settings.port)) // found his id as the candidate
                check_counter++;
            else
            {
                check_counter = 0;
            }
            
            if(check_counter >= 3){
                //printf("\nspare is needed. Releasing..\n");
                lock_spare();
                check_counter = 0;
                is_taken_careof = false;
		
                if(slab_rebalance_signal == 0){
                    rebal_dest_ext  = true;
                    prev_victim = find_victim_slab();
                    printf("\nExternal REASSIGN: Releasing Memory from class %d\n", prev_victim);
                    do_slabs_reassign(prev_victim,-2);  // -2 means it's located in another tenant
        	    }    
	        }
        }
    }
}

void find_lowest_mu(){

    double lowest_mu = 1000000,mu = 0;
    int temp = 0,cls_id = 0,page_id = -1;


    if(spare_needed()){
        for(int i = POWER_SMALLEST;i <= power_largest;i++)
            if(slabclass[i].slabs > 10)
                for(int j = 0;j <= slabclass[i].slabs - 10;j++){
                    //printf("page %d : %d\n",j,slabclass[5].shadowq_hits[j]);
                    if(slabclass[i].hits[j] == 0){
                        cls_id = i;
                        page_id = j;
                        break;
                    }
                    else{
                        // temp = 0;
                        // for(int k = j;k >= 0;k--)
                        //     temp += slabclass[i].hits[k];
                        // mu = (double) temp / (j+1);
                        if(slabclass[i].hits[j] < lowest_mu){
                            lowest_mu = slabclass[i].hits[j];
                            cls_id = i;
                            page_id = j;
                        }
                    }
                }

        if(cls_id != 0 && page_id != -1 && slab_rebalance_signal == 0 && lock_spare()){
            is_taken_careof = false;
            rebal_dest_ext  = true;
            slab_to_release = page_id;
            printf("\nExternal REASSIGN: Releasing Memory from class %d (slab %d)\n", cls_id, page_id);
            do_slabs_reassign(cls_id,-2);  // -2 means it's located in another tenant
        }   
        
    }
    
}

void find_highest_mu(){

    double highest_mu = 0,mu = 0;
    int temp = 0,cls_id = 0,shadow_page_id = -1;


    if(slab_rebalance_signal == 0){

        if(!is_spare_avail())
            //checkForSpare();
            find_lowest_mu();

        for(int i = POWER_SMALLEST;i <= power_largest;i++)
            if(slabclass[i].slabs > 0)
                for(int j = 999;j >= 0;j--){
                    //printf("page %d : %d\n",j,slabclass[5].shadowq_hits[j]);
                    if(slabclass[i].shadowq_hits[j] > SHADOWQ_HIT_THRESHOLD){
                        temp = 0;
                        for(int k = j;k >= 0;k--)
                            temp += slabclass[i].shadowq_hits[k];
                        mu = (double) temp / (j+1);
                        if(mu > highest_mu){
                            highest_mu = mu;
                            cls_id = i;
                            shadow_page_id = j;
                        }
                    }
                }

        if(cls_id != 0 && shadow_page_id != -1 && req_spare()){
            
            for(int k = shadow_page_id;k >= 0;k--)
                slabclass[cls_id].shadowq_hits[k] = 0;
            
            pages_to_request = shadow_page_id + 1;
            printf("\nExternal REASSIGN: Requested %d page(s) for class %d\n",pages_to_request, cls_id);
            do_slabs_reassign(1000,cls_id);

        }
    }
}

shadow_item* slabs_shadowq_lookup(char *key, const size_t nkey) {
    
    uint32_t hv = hash(key, nkey);
    shadow_item* shadow_it = shadow_assoc_find(key, nkey, hv);

    if (shadow_it){   
        int x = shadow_it->page;
        
        //move to head
        remove_shadowq_item(shadow_it);
        insert_shadowq_item(shadow_it, shadow_it->slabs_clsid);

        //update shadowq hit counters
        do {
            slab_shadowq_dec_victim = ((slab_shadowq_dec_victim + 1 - POWER_SMALLEST) % (power_largest - POWER_SMALLEST + 1)) + POWER_SMALLEST; //round robin
        } while (slab_shadowq_dec_victim == shadow_it->slabs_clsid);
        //slabclass[slab_shadowq_dec_victim].shadowq_hits--;
        slabclass[shadow_it->slabs_clsid].shadowq_hits[x]++;
        slabclass[shadow_it->slabs_clsid].q_misses++;

    }
/*    //uncomment for ONLY local reallocation
    if (shadow_it) {
        
        //move to head
        remove_shadowq_item(shadow_it);
        insert_shadowq_item(shadow_it, shadow_it->slabs_clsid);

        //update shadowq hit counters
        do {
            slab_shadowq_dec_victim = ((slab_shadowq_dec_victim + 1 - POWER_SMALLEST) % (power_largest - POWER_SMALLEST + 1)) + POWER_SMALLEST; //round robin
        } while (slab_shadowq_dec_victim == shadow_it->slabs_clsid);
        slabclass[slab_shadowq_dec_victim].shadowq_hits--;
        slabclass[shadow_it->slabs_clsid].shadowq_hits++;
        slabclass[shadow_it->slabs_clsid].q_misses++;

        //Slab rebalance
        if ((slabclass[shadow_it->slabs_clsid].shadowq_hits) > SHADOWQ_HIT_THRESHOLD) {
            
            // if(slab_rebalance_signal == 0){
            //     if(req_spare()){    // Request a page
            //         printf("\nExternal REASSIGN: Requested memory for class %d\n", shadow_it->slabs_clsid);
            //         slabclass[shadow_it->slabs_clsid].shadowq_hits = 0; 
            //         do_slabs_reassign(1000,shadow_it->slabs_clsid);
            //     }
            //     else{   //  Failed to request a page for some reason, use your own slabs        
                    rebal_source_ext = false; 
                    rebal_dest_ext   = false;
                    int counter = 0; //make sure we are not stuck in case there are no more slabs left
                    do {
                        if (counter++ > power_largest - POWER_SMALLEST + 1)
                            break;
                        prev_victim = ((prev_victim + 1 - POWER_SMALLEST) % (power_largest - POWER_SMALLEST + 1)) + POWER_SMALLEST; //round robin
                    } while ((prev_victim == shadow_it->slabs_clsid) || (slabclass[prev_victim].slabs <= 1));

                    printf("\nInternal REASSIGN: class %d  TO  class %d\n", prev_victim, shadow_it->slabs_clsid);
                    slabclass[shadow_it->slabs_clsid].shadowq_hits = 0; 
                    do_slabs_reassign(prev_victim,shadow_it->slabs_clsid);
                // }
            // }
        }
    }
*/
    return shadow_it;
}

void update_shadow_page_list(int id, int pageN, bool inserting, shadow_item *elem){
    slabclass_t *p = &slabclass[id];
    pthread_mutex_lock(&shadow_lock);

    if(inserting){
        p->shadow_page_list[0] = get_shadowq_head(id);

        for(int i = 1;i <= p->shadowq_size / p->perslab;i++){
            if(p->shadow_page_list[i]){
                p->shadow_page_list[i] = p->shadow_page_list[i]->prev;
                p->shadow_page_list[i]->page = i;
            }
            else if(i == p->shadowq_size / p->perslab && p->shadowq_size % p->perslab == 1){
                p->shadow_page_list[i] = get_shadowq_tail(id);
                p->shadow_page_list[i]->page = i;
            }
            // else
            //     printf("ERROR\n");
        }
    }
    else
    {
        if(p->shadow_page_list[pageN] == elem)
            p->shadow_page_list[pageN] = elem->next;
        
        for(int i = pageN + 1;i <= p->shadowq_size / p->perslab;i++){
            if(p->shadow_page_list[i]){
                p->shadow_page_list[i]->page = i - 1;
                if(p->shadow_page_list[i] != get_shadowq_tail(id))
                    p->shadow_page_list[i] = p->shadow_page_list[i]->next;
                else if(p->shadow_page_list[i] == get_shadowq_tail(id))
                    p->shadow_page_list[i] = NULL;
            }
            // else
            //     printf("     ERROR\n");
            
        }
    }

    pthread_mutex_unlock(&shadow_lock);
}

void incr_slab_hits(uint8_t clsid, uint8_t slabid){
    if(slabid < 1000){
        slabclass_t *p = &slabclass[clsid];
        p->hits[slabid]++;
    }
}

 shadow_item* get_shadowq_head(unsigned int id) { return (slabclass[id].shadowq_head); }
 void set_shadowq_head(shadow_item *elem, unsigned int id) { slabclass[id].shadowq_head = elem; }
 shadow_item* get_shadowq_tail(unsigned int id) { return (slabclass[id].shadowq_tail); }
 void set_shadowq_tail(shadow_item *elem, unsigned int id) { slabclass[id].shadowq_tail = elem; }
 unsigned int get_shadowq_max_items(unsigned int id) { return (slabclass[id].shadowq_max_items); }
 unsigned int get_shadowq_size(unsigned int id) { return (slabclass[id].shadowq_size); }
 void dec_shadowq_size(unsigned int id) { slabclass[id].shadowq_size--; }
 void inc_shadowq_size(unsigned int id) { slabclass[id].shadowq_size++; }