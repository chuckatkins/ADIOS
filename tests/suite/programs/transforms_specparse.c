/*
 * transforms_specparse.c
 *
 * Tests the "specparse" functionality, which parses the string passed as transform="..." into
 * the transform ID and a list of key-value pairs.
 *
 *  Created on: Jul 25, 2013
 *      Author: David A. Boyuka II
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "core/transforms/adios_transforms_specparse.h"

struct specparse_test {
    const char *specstr;
    struct adios_transform_spec expected;
} TESTS[] = {
    {   .specstr = "identity:a=123,b,c=321,,,f=12321",
        .expected = {
            .transform_type     = adios_transform_identity,
            .transform_type_str = "identity",
            .param_count        = 6,
            .params             = (struct adios_transform_spec_kv_pair[]) {
                { .key = "a", .value = "123"  },
                { .key = "b", .value = NULL   },
                { .key = "c", .value = "321"  },
                { .key = "",  .value = NULL   },
                { .key = "",  .value = NULL   },
                { .key = "f", .value = "12321"},
            }
        }
    },
    {   .specstr = "identity",
        .expected = {
            .transform_type     = adios_transform_identity,
            .transform_type_str = "identity",
            .param_count        = 0,
            .params             = NULL
        }
    },
    {   .specstr = "none:a=123,b,c=321,,,f=12321",
        .expected = {
            .transform_type     = adios_transform_none,
            .transform_type_str = "none",
            .param_count        = 0,
            .params             = NULL
        }
    },
    {   .specstr = "raw:a=123,b,c=321,,,f=12321",
        .expected = {
            .transform_type     = adios_transform_none,
            .transform_type_str = "raw",
            .param_count        = 0,
            .params             = NULL
        }
    },
    {   .specstr = "***impossible-transform-name***:a=123,b,c=321,,,f=12321",
        .expected = {
            .transform_type     = adios_transform_unknown,
            .transform_type_str = "***impossible-transform-name***",
            .param_count        = 0,
            .params             = NULL
        }
    },
};

const int NUM_TESTS = sizeof(TESTS)/sizeof(TESTS[0]);

void test1() {
    // Parse a spec string
    struct adios_transform_spec *origspec = adios_transform_parse_spec("identity:a=123,b,c=321,,,f=12321");
    struct adios_transform_spec *origspecToClear = origspec;

    // Duplicate then delete the original, thus testing both copy() and free()
    struct adios_transform_spec *spec = adios_transform_spec_copy(origspec);
    adios_transform_free_spec(&origspec);
    memset(origspecToClear, 0, sizeof(*origspecToClear));

    // Ensure the content of the parsed spec is as expected
    assert(spec->transform_type == adios_transform_identity);
    assert(strcmp(spec->transform_type_str, "identity") == 0);

    assert(spec->param_count == 6);
    assert(spec->params);

    assert(strcmp(spec->params[0].key, "a") == 0);
    assert(strcmp(spec->params[0].value, "123") == 0);

    assert(strcmp(spec->params[1].key, "b") == 0);
    assert(spec->params[1].value == NULL);

    assert(strcmp(spec->params[2].key, "c") == 0);
    assert(strcmp(spec->params[2].value, "321") == 0);

    assert(strcmp(spec->params[3].key, "") == 0);
    assert(spec->params[3].value == NULL);

    assert(strcmp(spec->params[4].key, "") == 0);
    assert(spec->params[4].value == NULL);

    assert(strcmp(spec->params[5].key, "f") == 0);
    assert(strcmp(spec->params[5].value, "12321") == 0);
}

void run_test(struct specparse_test *test) {
    const struct adios_transform_spec *actual = adios_transform_parse_spec(test->specstr);
    const struct adios_transform_spec *expected = &test->expected;

    // Check transform type ID
    assert(actual->transform_type == expected->transform_type);

    // Check transform type string
    assert(actual->transform_type_str && expected->transform_type_str);
    assert(strcmp(actual->transform_type_str, expected->transform_type_str) == 0);

    // Check parameter count, and ensure parameter list exists for both or neither
    assert(actual->param_count == expected->param_count);
    assert((actual->params != NULL) == (expected->params != NULL));

    // If there is a parameter list, check it
    if (expected->params) {
        int i;
        for (i = 0; i < expected->param_count; i++) {
            const struct adios_transform_spec_kv_pair *actual_p = &actual->params[i];
            const struct adios_transform_spec_kv_pair *expected_p = &expected->params[i];

            // Check that the keys are non-NULL and match
            assert(actual_p->key && expected_p->key);
            assert(strcmp(actual_p->key, expected_p->key) == 0);

            // Check that values are either both or neither present, and if the former, that they match
            assert((actual_p->value != NULL) == (expected_p->value != NULL));
            if (expected_p->value != NULL)
                assert(strcmp(actual_p->value, expected_p->value) == 0);
        }
    }

    adios_transform_free_spec(&actual);
}

void run_tests() {
    int i;
    for (i = 0; i < NUM_TESTS; i++) {
        run_test(&TESTS[i]);
    }
}

int main(int argc, char **argv) {
    test1();
    run_tests();
}


