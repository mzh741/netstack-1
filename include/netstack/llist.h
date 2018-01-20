#ifndef NETSTACK_LLIST_H
#define NETSTACK_LLIST_H

#include <stdio.h>
#include <pthread.h>

#define LLIST_INITIALISER { \
        .lock = PTHREAD_MUTEX_INITIALIZER, \
        .head = NULL, \
        .tail = NULL, \
        .length = 0 \
    }

struct llist_elem {
    void *data;
    struct llist_elem *next, *prev;
};
typedef struct llist {
    pthread_mutex_t lock;
    struct llist_elem *head, *tail;
    size_t length;
} llist_t;

/*!
 * Iterate over each element in a loop.
 * Call llist_elem_data() to get the pointer to the element data
 * @param list list iterate over
 */
#define for_each_llist(list) \
    for (struct llist_elem *elem = (list)->head, \
        *next = (elem ? elem->next : NULL); elem != NULL; \
        elem = next, next = (elem ? elem->next : NULL))

#define llist_iter(list, fn) \
    for_each_llist(list) \
        fn(llist_elem_data())

/*!
 * For use in a for_each_llist(list) block.
 * Retrieves and casts the list entry data to the specified \a type
 */
#define llist_elem_data() ((elem)->data)


/* Add queue compatibility */
#define queue_push llist_append
#define queue_pop  llist_pop

/*!
 * Remove every element from the list, emptying it.
 * Caution: This will not deallocate the element data,
 *          that must be done beforehand
 */
void llist_clear(llist_t *list);

/*!
 * Adds a new element to the end of the list
 */
void llist_append(llist_t *list, void *data);

/*!
 * Inserts a new element to the start of the list (prepend)
 * @param list list to prepend to
 * @param data data to prepend to the start of the list
 */
void llist_push(llist_t *list, void *data);

/*!
 * Fetch and remove the first element from the list
 * @return data from the first element of the list
 */
void *llist_pop(llist_t *list);

/*!
 * Fetch and remove the last element from the list
 * @return data from the last element of the list
 */
void *llist_pop_last(llist_t *list);

/*!
 * Checks if a list contains an data element
 * Comparison is made by strict pointer checking
 * @return the index of the element if found, -1 otherwise
 */
ssize_t llist_contains(llist_t *list, void *data);

/*!
 * Removes a data element from the list if it exists.
 * Only removes the first instance of an element if it
 * resides in the list more than once.
 * Comparison is made by strict pointer checking.
 * @return 0 if the element was removed, -1 otherwise
 */
ssize_t llist_remove(llist_t *list, void *data);


#endif //NETSTACK_LLIST_H

