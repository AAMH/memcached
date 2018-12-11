#include <stdio.h>
#define SHADOWQ_HIT_THRESHOLD 256

typedef struct _shadow_item_t {
    struct _shadow_item_t *next;
    struct _shadow_item_t *prev;
    struct _shadow_item_t *h_next;    /* hash chain next */
    uint8_t         nkey;       /* key length, w/terminating null and padding */
    uint8_t         slabs_clsid;
    char      *key;
} shadow_item;

void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid);
void remove_shadowq_item(shadow_item *elem);
void evict_shadowq_item(shadow_item *shadowq_it);

