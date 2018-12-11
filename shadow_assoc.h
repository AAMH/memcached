/**
 * Structure for storing items within memcached.
 */
#include <stdint.h>
#include <stddef.h>
#include "shadows.h"
//assaf - FIXME - move to shadows.h
//typedef struct _shadow_item_t {
//    struct _shadow_item_t *next;
 //   struct _shadow_item_t *prev;
 //   struct _shadow_item_t *h_next;    /* hash chain next */
 //   uint8_t         nkey;       /* key length, w/terminating null and padding */
 //   uint8_t         slabs_clsid;
//    char      *key;
//} shadow_item;

/* associative array */
void shadow_assoc_init(const int hashpower_init);
shadow_item *shadow_assoc_find(const char *key, const size_t nkey, const uint32_t hv);
int shadow_assoc_insert(shadow_item *item, const uint32_t hv);
void shadow_assoc_delete(const char *key, const size_t nkey, const uint32_t hv);
void shadow_do_assoc_move_next_shadow(void);
int shadow_start_assoc_maintenance_thread(void);
void shadow_stop_assoc_maintenance_thread(void);
