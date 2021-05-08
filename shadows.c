#include "assert.h"
#include "memcached.h"
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

node_t *new_tree_node(struct timeval key) 
{
	node_t *node = (node_t *)malloc(sizeof(struct _avl_node_t));
    node->parent = NULL;
	node->time = key; 
	node->left = NULL; 
	node->right = NULL; 
    node->weight = 0;
	node->height = 1; // new node is initially added as leaf 

	return node; 
} 

tree_t *new_tree()
{
    tree_t *t = (tree_t *)malloc(sizeof(struct _avl_node_t));
    t->root = NULL;

    return t;
}

int max(int a, int b)
{
    if(a > b)
        return a;
    return b;
}

int height(node_t *n)
{
    if(n == NULL)
        return -1;
    return n->height;
}

node_t *minimum(tree_t *t, node_t *x)
{
    while(x->left != NULL)
        x = x->left;
    return x;
}

void left_rotate(tree_t *t, node_t *x)
{
    node_t *y = x->right;
    x->right = y->left;

    if(y->left != NULL){
        y->left->parent = x;
    }

    y->parent = x->parent;

    if(x->parent == NULL){ // x is root
        t->root = y;
    }
    else if(x == x->parent->left){ // x is left child
        x->parent->left = y;
    }
    else { // x is right child
        x->parent->right = y;
    }

    y->left = x;
    x->parent = y;

    x->height = 1 + max(height(x->left), height(x->right));
    y->height = 1 + max(height(y->left), height(y->right));
}

void right_rotate(tree_t *t, node_t *x)
{
    node_t *y = x->left;
    x->left = y->right;

    if(y->right != NULL){
        y->right->parent = x;
    }
    
    y->parent = x->parent;
    
    if(x->parent == NULL){ // x is root
        t->root = y;
    }
    else if(x == x->parent->right){ // x is left child
        x->parent->right = y;
    }
    else { // x is right child
        x->parent->left = y;
    }

    y->right = x;
    x->parent = y;

    x->height = 1 + max(height(x->left), height(x->right));
    y->height = 1 + max(height(y->left), height(y->right));
}

int balance_factor(node_t *n)
{
    if(n == NULL)
        return 0;
    return(height(n->left) - height(n->right));
}

void insert_tree_node(tree_t *t, node_t *n)
{
    node_t *y = NULL;
    node_t *temp = t->root;

    while(temp != NULL){
        y = temp;
        if(temp->time.tv_sec > n->time.tv_sec || (temp->time.tv_sec == n->time.tv_sec && temp->time.tv_usec > n->time.tv_usec))
            temp = temp->left;
        else{
            temp->weight++;
            temp = temp->right;
        }
    }
    n->parent = y;

    if(y == NULL) //newly added node is root
        t->root = n;
    else if(y->time.tv_sec > n->time.tv_sec || (y->time.tv_sec == n->time.tv_sec && y->time.tv_usec > n->time.tv_usec))
        y->left = n;
    else
        y->right = n;

    node_t *z = n;

    while(y != NULL){
        y->height = 1 + max(height(y->left), height(y->right));

        node_t *x = y->parent;

        if(balance_factor(x) <= -2 || balance_factor(x) >= 2){ //grandparent is unbalanced

            if(y == x->left){
                if(z == x->left->left) //case 1
                    right_rotate(t, x);

                else if(z == x->left->right){ //case 3
                    left_rotate(t, y);
                    right_rotate(t, x);
                }
            }
            else if(y == x->right){
                if(z == x->right->right) //case 2
                    left_rotate(t, x);

                else if(z == x->right->left){ //case 4
                    right_rotate(t, y);
                    left_rotate(t, x);
                }
            }
            break;
        }

        y = y->parent;
        z = z->parent;
    }
}

void transplant(tree_t *t, node_t *u, node_t *v) // replaces u by v
{
    if(u->parent == NULL) // u is root
        t->root = v;
    else if(u == u->parent->left) // u is left child
        u->parent->left = v;
    else // u is right child
        u->parent->right = v;

    if(v != NULL)
        v->parent = u->parent;
}

void delete_fixup(tree_t *t, node_t *n)
{
    node_t *p = n;

    while(p != NULL){
        p->height = 1 + max(height(p->left), height(p->right));

        if(balance_factor(p) <= -2 || balance_factor(p) >= 2){ //grandparent is unbalanced
            node_t *x, *y, *z;
            x = p;

            //taller child of x will be y
            if(x->left && x->right){
                if(x->left->height > x->right->height)
                    y = x->left;
                else
                    y = x->right;
            } 
            else if(!x->left)
                y = x->right;
            else if(!x->right)
                y = x->left;

            //taller child of y will be z
            if(y->left && y->right){
                if(y->left->height > y->right->height){
                    z = y->left;
                }
                else if(y->left->height < y->right->height){
                    z = y->right;
                }
                else { //same height, go for single rotation
                    if(y == x->left)
                        z = y->left;
                    else
                        z = y->right;
                }
            }
            else if(!y->left)
                z = y->right;
            else if(!y->right)
                z = y->left;

            if(y == x->left){
                if(z == x->left->left) //case 1
                    right_rotate(t, x);

                else if(z == x->left->right){//case 3
                    left_rotate(t, y);
                    right_rotate(t, x);
                }
            }
            else if(y == x->right){
                if(z == x->right->right) //case 2
                    left_rotate(t, x);

                else if(z == x->right->left){//case 4
                    right_rotate(t, y);
                    left_rotate(t, x);
                }
            }
        }
        p = p->parent;
    }
}

void delete_tree_node(tree_t *t, node_t *z)
{
    if(z->left == NULL) {
        transplant(t, z, z->right);
        if(z->right != NULL)
            delete_fixup(t, z->right);
        free(z);
    }
    else if(z->right == NULL) {
        transplant(t, z, z->left);
        if(z->left != NULL)
            delete_fixup(t, z->left);
        free(z);
    }
    else {
        node_t *y = minimum(t, z->right); //minimum element in right subtree
        if(y->parent != z){
            transplant(t, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(t, z, y);
        y->left = z->left;
        y->left->parent = y;
        if(y != NULL)
            delete_fixup(t, y);
        free(z);
    }
}

node_t *search_tree(node_t *root, struct timeval key)
{
    // Base Cases: root is null or key is present at root 
    if (root == NULL || (root->time.tv_sec == key.tv_sec && root->time.tv_usec == key.tv_usec))
        return root;
    // Key is greater than root's key 
    if (root->time.tv_sec < key.tv_sec || (root->time.tv_sec == key.tv_sec && root->time.tv_usec < key.tv_usec)) 
        return search_tree(root->right, key); 
    // Key is smaller than root's key     
    else if(root->time.tv_sec > key.tv_sec || (root->time.tv_sec == key.tv_sec && root->time.tv_usec > key.tv_usec))
        return search_tree(root->left, key); 
}

// A utility function for preorder traversal of the tree. 
// The function returns the weight of the sub-tree
int preOrder(node_t *node) 
{
	if (node == NULL)
        return 0;
    else
        return (1+ preOrder(node->left) + preOrder(node->right)); 
} 

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

    return shadow_it;
}

void insert_shadowq_item(shadow_item *elem, unsigned int slabs_clsid)
{
    assert(elem);
    pthread_mutex_lock(&shadow_lock);
    elem->next = NULL;
    elem->prev = NULL;
    elem->slabs_clsid = slabs_clsid;

    gettimeofday(&elem->last_seen_time, NULL);

    //pthread_mutex_lock(&tree_lock);
    slabclass_t *p = (slabclass_t *) get_slabclass(slabs_clsid);    
    insert_tree_node(p->tree ,new_tree_node(elem->last_seen_time));
    //pthread_mutex_unlock(&tree_lock);

    shadow_item *old_shadowq_head = get_shadowq_head(slabs_clsid);
    if (old_shadowq_head) {
        elem->next = old_shadowq_head;
        old_shadowq_head->prev = elem;
    } else { //empty queue
        set_shadowq_tail(elem, slabs_clsid);
    }
    set_shadowq_head(elem, slabs_clsid);
    inc_shadowq_size(slabs_clsid);

    pthread_mutex_unlock(&shadow_lock);

    if (get_shadowq_size(slabs_clsid) > get_shadowq_max_items(slabs_clsid)) {
        //printf("Evicting ------ slabs_clsid: %d\n ",slabs_clsid);
        shadow_item *shadowq_tail = get_shadowq_tail(slabs_clsid);
        evict_shadowq_item(shadowq_tail);
    }
}

void remove_shadowq_item(shadow_item *elem,node_t *node)
{
    pthread_mutex_lock(&shadow_lock);
    shadow_item *shadowq_tail = get_shadowq_tail(elem->slabs_clsid);
    assert(shadowq_tail);
    if (shadowq_tail == elem)
        set_shadowq_tail(elem->prev, elem->slabs_clsid);

    shadow_item *shadowq_head = get_shadowq_head(elem->slabs_clsid);
    assert(shadowq_head);
    if (shadowq_head == elem)
        set_shadowq_head(elem->next, elem->slabs_clsid);
        
    if(node){    
        //pthread_mutex_lock(&tree_lock);
        slabclass_t *p = (slabclass_t *) get_slabclass(elem->slabs_clsid);
        fix_weights(p->tree->root,node);
        delete_tree_node(p->tree,node);
        //pthread_mutex_unlock(&tree_lock);
    }

    if (elem->prev)
        elem->prev->next = elem->next;
    if (elem->next)
        elem->next->prev = elem->prev;

    dec_shadowq_size(elem->slabs_clsid);

    elem->prev = NULL;
    elem->next = NULL;

    pthread_mutex_unlock(&shadow_lock);
}

void evict_shadowq_item(shadow_item *elem)
{
   remove_shadowq_item(elem, NULL);
   uint32_t hv = hash(elem->key, elem->nkey);
   shadow_assoc_delete(elem->key, elem->nkey, hv);

   free(elem->key);
   free(elem);
}

void fix_weights(node_t *root, node_t *node)
{
    while(node != root){
        if(node == (node->parent)->right)
            node->parent->weight--;
        node = node->parent;
    }
}

int calculate_reuse_distance(node_t *root, node_t *node)
{
    int x = node->weight;
    while(node != root){
	if(node->parent == NULL)
            break;
        if(node == (node->parent)->left)
            x += (node->parent)->weight + 1;
        node = node->parent;
        //printf("%p -", node); 
    }
    //printf("\n"); 
    return x;
}

shadow_item* slabs_shadowq_lookup(char *key, const size_t nkey)
{
    uint32_t hv = hash(key, nkey);
    shadow_item* shadow_it = shadow_assoc_find(key, nkey, hv);

    if (shadow_it){   
        int x = -1;
        pthread_mutex_lock(&tree_lock);
        slabclass_t *p = (slabclass_t *) get_slabclass(shadow_it->slabs_clsid);
        node_t *search = search_tree(p->tree->root,shadow_it->last_seen_time);
        if(search){
            int y = calculate_reuse_distance(p->tree->root, search);
            x = y / p->perslab;
            //printf("reuse_distance: %d, page: %d\n",y,x);

            //pthread_mutex_unlock(&tree_lock);

            //move to head
            remove_shadowq_item(shadow_it,search);
            insert_shadowq_item(shadow_it, shadow_it->slabs_clsid);

            //update shadowq hit counters
            if(x >= 0 && x <= 3999)
                p->shadowq_hits[x]++;
            p->q_misses++;
            pthread_mutex_unlock(&tree_lock);
        }
        else
        {
            //printf("Not Found!    \n");
            pthread_mutex_unlock(&tree_lock);
            return NULL;
        }
    }

    return shadow_it;
}