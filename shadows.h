#include <stdio.h>
#include <stdbool.h>
#define SHADOWQ_HIT_THRESHOLD 256

static pthread_mutex_t shadow_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct _shadow_item_t {
    struct _shadow_item_t *next;
    struct _shadow_item_t *prev;
    struct _shadow_item_t *h_next;    /* hash chain next */
    uint8_t                nkey;      /* key length, w/terminating null and padding */
    uint8_t                slabs_clsid;
    char                   *key;
    int                    page;      /*shadow page number*/
} shadow_item;

void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid);
void remove_shadowq_item(shadow_item *elem);
void evict_shadowq_item(shadow_item *shadowq_it);

bool is_on_first_page(shadow_item *elem, int perslab);