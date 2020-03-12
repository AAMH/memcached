#include "assert.h"
#include "memcached.h"
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// typedef struct _que_item_t{
//     unsigned int page_number;

//     struct _que_item_t *next;
// } que_item;

// typedef struct {
//     unsigned int size;      /* sizes of items */
//     unsigned int perslab;   /* how many items per slab */

//     void *slots;           /* list of item ptrs */
//     unsigned int sl_curr;   /* total free items in list */

//     unsigned int slabs;     /* how many slabs were allocated for this class */

//     void **slab_list;       /* array of slab pointers */
//     unsigned int list_size; /* size of prev array */

//     size_t requested; /* The number of requested bytes */

//     uint32_t hits[1000];

// /*** shadow queue Additions ***/
//     shadow_item *shadowq_head;
//     shadow_item *shadowq_tail;
//     unsigned int shadowq_size;
//     uint32_t shadowq_max_items;
//     uint32_t shadowq_hits[4000];
//     //uint32_t shadowq_hits;
//     uint32_t q_misses;
//     shadow_item **shadow_page_list;

//     uint32_t shadow_insert_count;
//     uint32_t shadow_remove_count;
//     que_item *shadow_remove_head;
//     que_item *shadow_remove_tail;

// } slabclass_t;

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

    
    //slabclass_t *p = (slabclass_t *) get_slabclass(slabs_clsid);
    //p->shadow_insert_count++;

    //p->shadow_op_qu[++p->shadow_op_qu_index] = 1;

    que_item *que_it = (que_item*) malloc(sizeof(struct _que_item_t));
    memset(que_it,0,sizeof(struct _que_item_t));
    que_it->page_number = 0;
    que_it->next = NULL;
    que_it->op_code = 0;
    que_it->shadowqsize = get_shadowq_size(slabs_clsid);
    que_it->curr_it = elem;
    que_it->curr_nextit = elem->next;
    que_it->curr_tail = get_shadowq_tail(slabs_clsid);

    pthread_mutex_lock(&extra_lock);
    if(!get_shadowq_update_head(slabs_clsid))
        set_shadowq_update_head(que_it,slabs_clsid);
    if(get_shadowq_update_tail(slabs_clsid))
        insert_shadowq_update(que_it,slabs_clsid);
    set_shadowq_update_tail(que_it,slabs_clsid);
    pthread_mutex_unlock(&extra_lock);

    //p->shadow_page_list[0] = elem;

    // for(int i = 1;i <= p->shadowq_size / p->perslab && i <= 999;i++){
    //         if(i == p->shadowq_size / p->perslab && p->shadowq_size % p->perslab == 0)
    //             break;
    //         // if(p->shadow_page_list[i]->prev == NULL){
    //         //     p->shadow_insert_count = 0;
    //         //     break;
    //         // }
    //             if(p->shadow_page_list[i]){
    //             if(p->shadow_page_list[i]->prev)
    //                 p->shadow_page_list[i] = p->shadow_page_list[i]->prev;
    //             p->shadow_page_list[i]->page = i;
    //         }
    //         else if(i == p->shadowq_size / p->perslab && p->shadowq_size % p->perslab == 1){
    //             p->shadow_page_list[i] = get_shadowq_tail(slabs_clsid);
    //             p->shadow_page_list[i]->page = i;
    //         }
    //     }

    // if(p->shadowq_size / p->perslab > 0){
    //     if(!p->shadow_page_list[p->shadowq_size / p->perslab] && p->shadowq_size % p->perslab == 1){
    //         p->shadow_page_list[p->shadowq_size / p->perslab] = get_shadowq_tail(slabs_clsid);
    //         p->shadow_page_list[p->shadowq_size / p->perslab]->page = p->shadowq_size / p->perslab;
    //     }}
    //     else if(p->shadow_page_list[p->shadowq_size / p->perslab]){
    //         p->shadow_page_list[p->shadowq_size / p->perslab] = p->shadow_page_list[p->shadowq_size / p->perslab]->prev;
    //         p->shadow_page_list[p->shadowq_size / p->perslab]->page = p->shadowq_size / p->perslab;
    //     }
    // }
    

    shadow_update_signal = 1;

    //update_shadow_insert(slabs_clsid);

    pthread_mutex_unlock(&shadow_lock);

    if (get_shadowq_size(slabs_clsid) > get_shadowq_max_items(slabs_clsid)) {
        //printf("Evicting ------ slabs_clsid: %d\n ",slabs_clsid);
        shadow_item *shadowq_tail = get_shadowq_tail(slabs_clsid);
        evict_shadowq_item(shadowq_tail);
    }
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

    slabclass_t *p = (slabclass_t *) get_slabclass(elem->slabs_clsid);
    //p->shadow_remove_count++;

    //p->shadow_op_qu[++p->shadow_op_qu_index] = 2;

    // for(int i = 1;i <= p->shadowq_size / p->perslab && i <= 3999;i++){
    // if(p->shadow_page_list[i] == elem){
    //     //if(elem->next)
    //         p->shadow_page_list[i] = get_nextlimit(p->shadow_page_list[i-1],p->perslab);//elem->next;
    //     // else
    //     //     p->shadow_page_list[elem->page] = NULL;
    //     printf("elem page %d found on %d \n",elem->page,i);
    //     // else
    //     //     p->shadow_page_list[elem->page] = NULL;
    //         //printf("rare case !!! %d\n",elem->page);
    // }}
    // else if(p->shadow_page_list[elem->page-1] == elem){
    //     if(elem->next)
    //         p->shadow_page_list[elem->page-1] = elem->next;
    //     // else
    //     //     p->shadow_page_list[elem->page-1] = NULL;
    //         //printf("rare case !!!\n");
    // }
    //}
    // for(int i = elem->page + 1;i <= p->shadowq_size / p->perslab && i <= 999;i++){
    //     if(i == p->shadowq_size / p->perslab && p->shadowq_size % p->perslab == 0)
    //             break;
    //         if(p->shadow_page_list[i])
    //             p->shadow_page_list[i]->page = i - 1;
    //         if(p->shadowq_size % p->perslab && p->shadow_page_list[i] != get_shadowq_tail(elem->slabs_clsid))
    //                 p->shadow_page_list[i] = p->shadow_page_list[i]->next;
    //             //}
    //             else if(p->shadow_page_list[i] == get_shadowq_tail(elem->slabs_clsid))
    //                 p->shadow_page_list[i] = NULL;
    //        // }
    //        if(i == 999)
    //         printf("FCK %d\n", elem->slabs_clsid);
    //     }

    // if(p->shadow_page_list[p->shadowq_size / p->perslab] == get_shadowq_tail(elem->slabs_clsid)){
    //     p->shadow_page_list[p->shadowq_size / p->perslab]->page--;
    //     p->shadow_page_list[p->shadowq_size / p->perslab] = NULL;
    // }
    // else if(p->shadowq_size % p->perslab && p->shadow_page_list[p->shadowq_size / p->perslab]){ 
    //     p->shadow_page_list[p->shadowq_size / p->perslab]->page--;
    //     p->shadow_page_list[p->shadowq_size / p->perslab] = p->shadow_page_list[p->shadowq_size / p->perslab]->next;
    // }


    que_item *que_it = (que_item*) malloc(sizeof(struct _que_item_t));
    memset(que_it,0,sizeof(struct _que_item_t));
    que_it->page_number = elem->page;
    que_it->next = NULL;
    que_it->op_code = 1;
    que_it->tt = -1;
    que_it->shadowqsize = get_shadowq_size(elem->slabs_clsid);
    que_it->curr_it = elem;
    que_it->curr_nextit = elem->next;
    que_it->curr_tail = get_shadowq_tail(elem->slabs_clsid);

    // for(int i = 1;i <= p->shadowq_size / p->perslab && i <= 3999;i++)
    //     if(p->shadow_page_list[i] == elem){
    //         que_it->tt = i;    
    //         break;
    //     }
    // if(que_it->tt != -1)
    //     p->shadow_page_list[que_it->tt] =  get_nextlimit(p->shadow_page_list[que_it->tt - 1],p->perslab);

    pthread_mutex_lock(&extra_lock);
    if(!get_shadowq_update_head(elem->slabs_clsid))
        set_shadowq_update_head(que_it,elem->slabs_clsid);
    if(get_shadowq_update_tail(elem->slabs_clsid))
        insert_shadowq_update(que_it,elem->slabs_clsid);
    set_shadowq_update_tail(que_it,elem->slabs_clsid);
    pthread_mutex_unlock(&extra_lock);

    shadow_update_signal = 1;

    // update_shadow_remove(elem->slabs_clsid,elem->page,elem);
    
        dec_shadowq_size(elem->slabs_clsid);


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

shadow_item *get_nextlimit(shadow_item *it, int perslab){

    shadow_item *ite = it;
    for(int i = 1;i <= perslab, ite != get_shadowq_tail(it->slabs_clsid);i++)
        ite = ite->next;

    // if(!ite)
    //     printf("WTFFFFFFFFFFFFFFF\n");
    // else
        // printf("limit: %p\n",ite);
    return ite;
}