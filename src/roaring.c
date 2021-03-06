#include "roaring.h"
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "roaring_array.h"
#include "array_util.h"

roaring_bitmap_t *roaring_bitmap_create() {
    roaring_bitmap_t *ans = (roaring_bitmap_t *)malloc(sizeof(*ans));
    if (!ans) {
        return NULL;
    }
    ans->high_low_container = ra_create();
    if (!ans->high_low_container) {
        free(ans);
        return NULL;
    }
    return ans;
}

roaring_bitmap_t *roaring_bitmap_of_ptr(size_t n_args, uint32_t *vals) {
    // todo: could be greatly optimized
    roaring_bitmap_t *answer = roaring_bitmap_create();
    for (size_t i = 0; i < n_args; i++) {
        roaring_bitmap_add(answer, vals[i]);
    }
    return answer;
}

roaring_bitmap_t *roaring_bitmap_of(size_t n_args, ...) {
    // todo: could be greatly optimized
    roaring_bitmap_t *answer = roaring_bitmap_create();
    va_list ap;
    va_start(ap, n_args);
    for (size_t i = 1; i <= n_args; i++) {
        uint32_t val = va_arg(ap, uint32_t);
        roaring_bitmap_add(answer, val);
    }
    va_end(ap);
    return answer;
}

void roaring_bitmap_printf(roaring_bitmap_t *ra) {
    printf("{");
    for (int i = 0; i < ra->high_low_container->size; ++i) {
        container_printf_as_uint32_array(
            ra->high_low_container->containers[i],
            ra->high_low_container->typecodes[i],
            ((uint32_t)ra->high_low_container->keys[i]) << 16);
        if (i + 1 < ra->high_low_container->size) printf(",");
    }
    printf("}");
}

roaring_bitmap_t *roaring_bitmap_copy(roaring_bitmap_t *r) {
    roaring_bitmap_t *ans = (roaring_bitmap_t *)malloc(sizeof(*ans));
    if (!ans) {
        return NULL;
    }
    ans->high_low_container = ra_copy(r->high_low_container);
    if (!ans->high_low_container) {
        free(ans);
        return NULL;
    }
    return ans;
}

void roaring_bitmap_overwrite(roaring_bitmap_t *dest,
                              const roaring_bitmap_t *src) {
    ra_free(dest->high_low_container);
    dest->high_low_container = ra_copy(src->high_low_container);
    // TODO any better error handling
    assert(dest->high_low_container);
}

void roaring_bitmap_free(roaring_bitmap_t *r) {
    ra_free(r->high_low_container);
    r->high_low_container = NULL;  // paranoid
    free(r);
}

void roaring_bitmap_add(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(r->high_low_container, hb);
    uint8_t typecode;
    if (i >= 0) {
        void *container =
            ra_get_container_at_index(r->high_low_container, i, &typecode);
        uint8_t newtypecode = typecode;
        void *container2 =
            container_add(container, val & 0xFFFF, typecode, &newtypecode);
        if (container2 != container) {
            container_free(container, typecode);
            ra_set_container_at_index(r->high_low_container, i, container2,
                                      newtypecode);
        }
    } else {
        array_container_t *newac = array_container_create();
        void *container = container_add(newac, val & 0xFFFF,
                                        ARRAY_CONTAINER_TYPE_CODE, &typecode);
        // we could just assume that it stays an array container
        ra_insert_new_key_value_at(r->high_low_container, -i - 1, hb, container,
                                   typecode);
    }
}

bool roaring_bitmap_contains(roaring_bitmap_t *r, uint32_t val) {
    const uint16_t hb = val >> 16;
    const int i = ra_get_index(r->high_low_container, hb);
    uint8_t typecode;
    if (i >= 0) {
        void *container =
            ra_get_container_at_index(r->high_low_container, i, &typecode);
        return container_contains(container, val & 0xFFFF, typecode);
    } else {
        return false;
    }
}

// there should be some SIMD optimizations possible here
roaring_bitmap_t *roaring_bitmap_and(roaring_bitmap_t *x1,
                                     roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    roaring_bitmap_t *answer = roaring_bitmap_create();
    const int length1 = x1->high_low_container->size,
              length2 = x2->high_low_container->size;

    int pos1 = 0, pos2 = 0;

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t container_type_1, container_type_2;
            void *c1 = ra_get_container_at_index(x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_and(c1, container_type_1, c2, container_type_2,
                                    &container_result_type);
            if (container_nonzero_cardinality(c, container_result_type)) {
                ra_append(answer->high_low_container, s1, c,
                          container_result_type);
            } else {
                container_free(
                    c, container_result_type);  // otherwise:memory leak!
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {  // s1 < s2
            pos1 = ra_advance_until(x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(x2->high_low_container, s1, pos2);
        }
    }
    return answer;
}

// inplace and (modifies its first argument).
void roaring_bitmap_and_inplace(roaring_bitmap_t *x1,
                                const roaring_bitmap_t *x2) {
    int pos1 = 0, pos2 = 0, intersection_size = 0;
    const int length1 = ra_get_size(x1->high_low_container);
    const int length2 = ra_get_size(x2->high_low_container);

    // any skipped-over or newly emptied containers in x1
    // have to be freed.

    while (pos1 < length1 && pos2 < length2) {
        const uint16_t s1 = ra_get_key_at_index(x1->high_low_container, pos1);
        const uint16_t s2 = ra_get_key_at_index(x2->high_low_container, pos2);

        if (s1 == s2) {
            uint8_t typecode1, typecode2, typecode_result;
            void *c1 = ra_get_container_at_index(x1->high_low_container, pos1,
                                                 &typecode1);
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &typecode2);
            void *c =
                container_iand(c1, typecode1, c2, typecode2, &typecode_result);

            if (container_nonzero_cardinality(c, typecode_result)) {
                ra_replace_key_and_container_at_index(x1->high_low_container,
                                                      intersection_size, s1, c,
                                                      typecode_result);
                intersection_size++;
            } else {
                container_free(c, typecode_result);
            }
            ++pos1;
            ++pos2;
        } else if (s1 < s2) {
            pos1 = ra_advance_until_freeing(x1->high_low_container, s2, pos1);
        } else {  // s1 > s2
            pos2 = ra_advance_until(x2->high_low_container, s1, pos2);
        }
    }

    // if we ended early because x2 ran out, then all remaining in x1 should be
    // freed
    while (pos1 < length1) {
        container_free(x1->high_low_container->containers[pos1],
                       x1->high_low_container->typecodes[pos1]);
        ++pos1;
    }

    // all containers after this have either been copied or freed
    ra_downsize(x1->high_low_container, intersection_size);
}

roaring_bitmap_t *roaring_bitmap_or(roaring_bitmap_t *x1,
                                    roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    roaring_bitmap_t *answer = roaring_bitmap_create();
    const int length1 = x1->high_low_container->size,
              length2 = x2->high_low_container->size;
    if (0 == length1) {
        return roaring_bitmap_copy(x1);
    }
    if (0 == length2) {
        return roaring_bitmap_copy(x2);
    }
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_or(c1, container_type_1, c2, container_type_2,
                                   &container_result_type);
            // since we assume that the initial containers are non-empty, the
            // result here
            // can only be non-empty
            ra_append(answer->high_low_container, s1, c, container_result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            void *c1 = ra_get_container_at_index(x1->high_low_container, pos1,
                                                 &container_type_1);
            c1 = container_clone(c1, container_type_1);  // creating a copy
                                                         // since we do not have
                                                         // COW support
            ra_append(answer->high_low_container, s1, c1, container_type_1);
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &container_type_2);
            c2 = container_clone(c2, container_type_2);  // creating a copy
                                                         // since we do not have
                                                         // COW support
            ra_append(answer->high_low_container, s2, c2, container_type_2);
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(answer->high_low_container, x2->high_low_container,
                             pos2, length2);
    } else if (pos2 == length2) {
        ra_append_copy_range(answer->high_low_container, x1->high_low_container,
                             pos1, length1);
    }
    return answer;
}

// inplace or (modifies its first argument).
// TODO: test it....
void roaring_bitmap_or_inplace(roaring_bitmap_t *x1,
                               const roaring_bitmap_t *x2) {
    uint8_t container_result_type = 0;
    roaring_bitmap_t *answer = roaring_bitmap_create();
    int length1 = x1->high_low_container->size;
    const int length2 = x2->high_low_container->size;

    if (0 == length2) return;

    if (0 == length2) {
        roaring_bitmap_overwrite(x1, x2);
        return;
    }
    int pos1 = 0, pos2 = 0;
    uint8_t container_type_1, container_type_2;
    uint16_t s1 = ra_get_key_at_index(x1->high_low_container, pos1);
    uint16_t s2 = ra_get_key_at_index(x2->high_low_container, pos2);
    while (true) {
        if (s1 == s2) {
            void *c1 = ra_get_container_at_index(x1->high_low_container, pos1,
                                                 &container_type_1);
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c = container_ior(c1, container_type_1, c2, container_type_2,
                                    &container_result_type);

            ra_set_container_at_index(x1->high_low_container, pos1, c,
                                      container_result_type);
            ++pos1;
            ++pos2;
            if (pos1 == length1) break;
            if (pos2 == length2) break;
            s1 = ra_get_key_at_index(x1->high_low_container, pos1);
            s2 = ra_get_key_at_index(x2->high_low_container, pos2);

        } else if (s1 < s2) {  // s1 < s2
            pos1++;
            if (pos1 == length1) break;
            s1 = ra_get_key_at_index(x1->high_low_container, pos1);

        } else {  // s1 > s2
            void *c2 = ra_get_container_at_index(x2->high_low_container, pos2,
                                                 &container_type_2);
            void *c2_clone = container_clone(c2, container_type_2);
            ra_insert_new_key_value_at(x1->high_low_container, pos1, s2,
                                       c2_clone, container_type_2);
            pos1++;
            length1++;
            pos2++;
            if (pos2 == length2) break;
            s2 = ra_get_key_at_index(x2->high_low_container, pos2);
        }
    }
    if (pos1 == length1) {
        ra_append_copy_range(answer->high_low_container, x2->high_low_container,
                             pos2, length2);
    }
}

uint32_t roaring_bitmap_get_cardinality(roaring_bitmap_t *ra) {
    uint32_t ans = 0;
    for (int i = 0; i < ra->high_low_container->size; ++i)
        ans += container_get_cardinality(ra->high_low_container->containers[i],
                                         ra->high_low_container->typecodes[i]);
    return ans;
}

uint32_t *roaring_bitmap_to_uint32_array(roaring_bitmap_t *ra,
                                         uint32_t *cardinality) {
    uint32_t card1 = roaring_bitmap_get_cardinality(ra);

    uint32_t *ans = malloc((card1 + 10) * sizeof(uint32_t));  //+20??
    // TODO Valgrind reports we write beyond the end of this array (?) with an
    // 8-byte write (?)
    // but it may just be an AVX2 instruction needing a little extra space.  Add
    // 40 bytes...seems to fix problem, but adding 8 didn't
    uint32_t ctr = 0;

    for (int i = 0; i < ra->high_low_container->size; ++i) {
        int num_added = container_to_uint32_array(
            ans + ctr, ra->high_low_container->containers[i],
            ra->high_low_container->typecodes[i],
            ((uint32_t)ra->high_low_container->keys[i]) << 16);
        ctr += num_added;
    }
    assert(ctr == card1);
    *cardinality = ctr;
    return ans;
}

/** convert array and bitmap containers to run containers when it is more
 * efficient;
 * also convert from run containers when more space efficient.  Returns
 * true if the result has at least one run container.
*/
bool roaring_bitmap_run_optimize(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container->size; i++) {
        uint8_t typecode_original, typecode_after;
        void *c = ra_get_container_at_index(r->high_low_container, i,
                                            &typecode_original);

        void *c1 = convert_run_optimize(c, typecode_original, &typecode_after);
        if (typecode_after == RUN_CONTAINER_TYPE_CODE) answer = true;
        ra_set_container_at_index(r->high_low_container, i, c1, typecode_after);
    }
    return answer;
}

/**
 *  Remove run-length encoding even when it is more space efficient
 *  return whether a change was applied
 */
bool roaring_bitmap_remove_run_compression(roaring_bitmap_t *r) {
    bool answer = false;
    for (int i = 0; i < r->high_low_container->size; i++) {
        uint8_t typecode_original, typecode_after;
        void *c = ra_get_container_at_index(r->high_low_container, i,
                                            &typecode_original);
        if (typecode_original == RUN_CONTAINER_TYPE_CODE) {
            answer = true;
            int32_t card = run_container_cardinality(c);
            void *c1 =
                convert_to_bitset_or_array_container(c, card, &typecode_after);
            ra_set_container_at_index(r->high_low_container, i, c1,
                                      typecode_after);
        }
    }
    return answer;
}

char *roaring_bitmap_serialize(roaring_bitmap_t *ra, uint32_t *serialize_len) {
    return (ra_serialize(ra->high_low_container, serialize_len));
}

roaring_bitmap_t *roaring_bitmap_deserialize(char *buf, uint32_t buf_len) {
    roaring_bitmap_t *b;
    uint32_t len;

    if (buf_len < 4) return (NULL);

    memcpy(&len, buf, 4);

    if (len != buf_len) return (NULL);

    b = (roaring_bitmap_t *)malloc(sizeof(roaring_bitmap_t *));
    if (b) {
        b->high_low_container = ra_deserialize(&buf[4], buf_len - 4);
        if (b->high_low_container == NULL) {
            free(b);
            b = NULL;
        }
    }

    return (b);
}

void roaring_iterate(roaring_bitmap_t *ra, roaring_iterator iterator, void *ptr) {
  for (int i = 0; i < ra->high_low_container->size; ++i)
    container_iterate(ra->high_low_container->containers[i],
		      ra->high_low_container->typecodes[i],
		      ((uint32_t)ra->high_low_container->keys[i]) << 16,
		      iterator, ptr);
}

