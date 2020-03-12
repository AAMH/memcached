#include <stdio.h>
#include <stdbool.h>
#define SHADOWQ_HIT_THRESHOLD 256

typedef struct _shadow_item_t {
    struct _shadow_item_t *next;
    struct _shadow_item_t *prev;
    struct _shadow_item_t *h_next;    /* hash chain next */
    uint8_t                nkey;      /* key length, w/terminating null and padding */
    uint8_t                slabs_clsid;
    char                   *key;
    int                    page;      /*shadow page number*/
} shadow_item;

typedef struct _que_item_t{
    unsigned int tt;
    unsigned int page_number;
    unsigned int op_code;
    unsigned int shadowqsize;
    struct _que_item_t *next;
    shadow_item *curr_tail;
    shadow_item *curr_it;
    shadow_item *curr_nextit;
} que_item;

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

    //uint8_t *shadow_op_qu;
    //int shadow_op_qu_index;

/*** shadow queue Additions ***/
    shadow_item *shadowq_head;
    shadow_item *shadowq_tail;
    unsigned int shadowq_size;
    uint32_t shadowq_max_items;
    uint32_t shadowq_hits[4000];
    //uint32_t shadowq_hits;
    uint32_t q_misses;
    shadow_item **shadow_page_list;
    
    uint32_t shadow_insert_count;
    uint32_t shadow_remove_count;
    que_item *shadow_update_head;
    que_item *shadow_update_tail;

} slabclass_t;

static pthread_mutex_t extra_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t shadow_lock = PTHREAD_MUTEX_INITIALIZER;

volatile int shadow_update_signal;
volatile int shadow_update_signal2;

extern int shad_id, shad_id2, shadow_pageN;
extern shadow_item *elemthread;

void reinsert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid);
void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid);
void remove_shadowq_item(shadow_item *elem);
void evict_shadowq_item(shadow_item *shadowq_it);

bool is_on_first_page(shadow_item *elem, int perslab);
shadow_item *get_nextlimit(shadow_item *it, int perslab);