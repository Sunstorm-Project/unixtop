/*
 * test_hash.c — host-side unit tests for hash.c (pid flavor).
 *
 * hash.c is a bucketed linked-list hash table generated from hash.m4h.
 * The pid variant is the one m_solaris.c and m_linux.c both depend on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "../hash.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);    \
        failures++;                                                      \
    }                                                                    \
} while (0)

static void test_basic_add_lookup(void)
{
    hash_table *ht = hash_create(31);
    int values[] = { 10, 20, 30, 40 };
    pid_t pids[] = { 1, 500, 12345, 99999 };
    int i;

    CHECK(ht != NULL, "hash_create returns non-NULL");

    for (i = 0; i < 4; i++)
        hash_add_pid(ht, pids[i], &values[i]);

    for (i = 0; i < 4; i++) {
        int *v = (int *)hash_lookup_pid(ht, pids[i]);
        CHECK(v == &values[i], "lookup returns inserted value ptr");
    }

    CHECK(hash_lookup_pid(ht, 7777) == NULL, "lookup of absent key is NULL");
}

static void test_replace(void)
{
    hash_table *ht = hash_create(31);
    int a = 1, b = 2;

    hash_add_pid(ht, 42, &a);
    void *prev = hash_replace_pid(ht, 42, &b);
    CHECK(prev == &a, "replace returns previous value");
    CHECK(hash_lookup_pid(ht, 42) == &b, "replace installs new value");
}

static void test_remove(void)
{
    hash_table *ht = hash_create(31);
    int a = 1;

    hash_add_pid(ht, 42, &a);
    CHECK(hash_lookup_pid(ht, 42) == &a, "pre-remove lookup");

    void *removed = hash_remove_pid(ht, 42);
    CHECK(removed == &a, "remove returns the stored value");
    CHECK(hash_lookup_pid(ht, 42) == NULL, "post-remove lookup is NULL");

    /* Removing something that isn't there must not crash. */
    CHECK(hash_remove_pid(ht, 42) == NULL, "double-remove is NULL");
}

static void test_iteration(void)
{
    hash_table *ht = hash_create(31);
    int values[] = { 10, 20, 30, 40, 50 };
    pid_t pids[] = { 1, 17, 256, 4096, 65535 };
    int seen[5] = { 0, 0, 0, 0, 0 };
    int count = 0;
    int i;

    for (i = 0; i < 5; i++)
        hash_add_pid(ht, pids[i], &values[i]);

    hash_pos pos;
    hash_item_pid *hi = hash_first_pid(ht, &pos);
    while (hi != NULL) {
        for (i = 0; i < 5; i++) {
            if (hi->key == pids[i]) {
                CHECK(hi->value == &values[i], "iter value matches key");
                seen[i]++;
                break;
            }
        }
        count++;
        hi = hash_next_pid(&pos);
    }

    CHECK(count == 5, "iteration visits every entry exactly once");
    for (i = 0; i < 5; i++)
        CHECK(seen[i] == 1, "each key seen once");
}

static void test_remove_during_iteration(void)
{
    /* get_process_info() in m_solaris.c removes dead entries via
     * hash_remove_pos_pid while iterating — make sure the iterator
     * survives it. */
    hash_table *ht = hash_create(31);
    int values[] = { 1, 2, 3, 4, 5 };
    pid_t pids[] = { 10, 20, 30, 40, 50 };
    int i, count = 0;

    for (i = 0; i < 5; i++)
        hash_add_pid(ht, pids[i], &values[i]);

    hash_pos pos;
    hash_item_pid *hi = hash_first_pid(ht, &pos);
    while (hi != NULL) {
        if (hi->key == 30)
            hash_remove_pos_pid(&pos);
        count++;
        hi = hash_next_pid(&pos);
    }

    CHECK(count == 5, "iteration still visits all 5 when one is removed");
    CHECK(hash_lookup_pid(ht, 30) == NULL, "removed key gone post-iter");
    CHECK(hash_lookup_pid(ht, 10) != NULL, "untouched key survives");
    CHECK(hash_lookup_pid(ht, 50) != NULL, "untouched key survives");
}

static void test_collisions(void)
{
    /* Tiny table forces collisions; make sure the linked-list bucket
     * still stores and retrieves every key. */
    hash_table *ht = hash_create(3);
    int values[20];
    int i;

    for (i = 0; i < 20; i++) {
        values[i] = i;
        hash_add_pid(ht, (pid_t)(100 + i), &values[i]);
    }

    for (i = 0; i < 20; i++) {
        int *v = (int *)hash_lookup_pid(ht, (pid_t)(100 + i));
        CHECK(v == &values[i], "collision-bucket lookup");
    }
}

int main(void)
{
    test_basic_add_lookup();
    test_replace();
    test_remove();
    test_iteration();
    test_remove_during_iteration();
    test_collisions();

    if (failures) {
        fprintf(stderr, "test_hash: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_hash: all passed\n");
    return 0;
}
