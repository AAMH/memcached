#include "assert.h"
#include "memcached.h"
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>


shadow_item* create_shadow_item(item *it,u_int8_t clsid,u_int8_t nkey)
{
    shadow_item* shadow_it = (shadow_item*) malloc(sizeof(struct _shadow_item_t));
    memset(shadow_it,0,sizeof(struct _shadow_item_t));
    assert(shadow_it && it);
    shadow_it->key = (char*)malloc(nkey*sizeof(char));
    memcpy(shadow_it->key, ITEM_key(it), nkey);
    shadow_it->nkey = nkey;
    shadow_it->next = NULL;
    shadow_it->prev = NULL;
    shadow_it->h_next = NULL;
    shadow_it->slabs_clsid = clsid;
    shadow_it->page = 0;

    return shadow_it;
}

void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid) {
    assert(elem);
    pthread_mutex_lock(&shadow_lock);
    elem->next = NULL;
    elem->prev = NULL;
    elem->slabs_clsid = slabs_clsid;
    elem->page = 0;

    shadow_item *old_shadowq_head = get_shadowq_head(slabs_clsid);
    if (old_shadowq_head) {
        elem->next = old_shadowq_head;
        old_shadowq_head->prev = elem;
    } else { //empty queue
        set_shadowq_tail(elem, slabs_clsid);
    }
    set_shadowq_head(elem, slabs_clsid);
    inc_shadowq_size(slabs_clsid);

    update_shadow_page_list(slabs_clsid,0,true,NULL);

    if (get_shadowq_size(slabs_clsid) > get_shadowq_max_items(slabs_clsid)) {
        //printf("Evicting ------ slabs_clsid: %d\n ",slabs_clsid);
        shadow_item *shadowq_tail = get_shadowq_tail(slabs_clsid);
        evict_shadowq_item(shadowq_tail);
    }
    pthread_mutex_unlock(&shadow_lock);
}

void remove_shadowq_item(shadow_item *elem) {
    pthread_mutex_lock(&shadow_lock);
    shadow_item *shadowq_tail = get_shadowq_tail(elem->slabs_clsid);
    assert(shadowq_tail);
    if (shadowq_tail == elem)
        set_shadowq_tail(elem->prev, elem->slabs_clsid);

    shadow_item *shadowq_head = get_shadowq_head(elem->slabs_clsid);
    assert(shadowq_head);
    if (shadowq_head == elem)
        set_shadowq_head(elem->next, elem->slabs_clsid);

    if (elem->prev)
        elem->prev->next = elem->next;
    if (elem->next)
        elem->next->prev = elem->prev;

    dec_shadowq_size(elem->slabs_clsid);

    update_shadow_page_list(elem->slabs_clsid,elem->page,false,elem);

    elem->prev = NULL;
    elem->next = NULL;

    pthread_mutex_unlock(&shadow_lock);
}

void evict_shadowq_item(shadow_item *elem) {
   remove_shadowq_item(elem);
   uint32_t hv = hash(elem->key, elem->nkey);
   shadow_assoc_delete(elem->key, elem->nkey, hv);

   free(elem->key);
   free(elem);
}

bool is_on_first_page(shadow_item *elem, int perslab){
    shadow_item *shadowq_tail = get_shadowq_tail(elem->slabs_clsid);
    shadow_item *shadowq_head = get_shadowq_head(elem->slabs_clsid);
    shadow_item *elem2 = shadowq_head;

    for(int i = 0;i < perslab;i++){
        if(elem2 == elem)
            return true;
        else if(elem2 == shadowq_tail || elem2 == NULL)
            return false;
        else
            elem2 = elem2->next;
    }

    return false;
}