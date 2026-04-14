/*
 * test_utils.c — host-side unit tests for utils.c
 *
 * Runs on the build host (Linux x86), not the target (SPARC Solaris).
 * Invoked by `make check` after a successful build.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../utils.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);    \
        failures++;                                                      \
    }                                                                    \
} while (0)

#define CHECK_STR(got, want, msg) do {                                   \
    const char *_g = (got), *_w = (want);                                \
    if (strcmp(_g, _w) != 0) {                                           \
        fprintf(stderr, "FAIL %s:%d  %s: got '%s' want '%s'\n",          \
                __FILE__, __LINE__, msg, _g, _w);                        \
        failures++;                                                      \
    }                                                                    \
} while (0)

static void test_digits(void)
{
    CHECK(digits(0)       == 1, "digits(0) == 1");
    CHECK(digits(9)       == 1, "digits(9) == 1");
    CHECK(digits(10)      == 2, "digits(10) == 2");
    CHECK(digits(99)      == 2, "digits(99) == 2");
    CHECK(digits(100)     == 3, "digits(100) == 3");
    CHECK(digits(1000000) == 7, "digits(1000000) == 7");
}

static void test_itoa_w(void)
{
    /* itoa_w right-justifies into a field of width w, space-padded. */
    CHECK_STR(itoa_w(0, 3),    "  0", "itoa_w(0, 3)");
    CHECK_STR(itoa_w(42, 4),   "  42", "itoa_w(42, 4)");
    CHECK_STR(itoa_w(12345, 3), "12345", "itoa_w doesn't truncate past width");
}

static void test_format_k(void)
{
    /* format_k returns a short human-size string: e.g. 1024 -> "1024K",
     * 10240 -> "10M" etc. Exact formatting is less important than
     * non-empty, non-crashy output. */
    const char *s;
    s = format_k(0);       CHECK(s && s[0], "format_k(0) non-empty");
    s = format_k(1);       CHECK(s && s[0], "format_k(1) non-empty");
    s = format_k(1048576); CHECK(s && s[0], "format_k(1G) non-empty");
}

static void test_format_percent(void)
{
    const char *s;
    s = format_percent(0.0);   CHECK(s && s[0], "format_percent(0) non-empty");
    s = format_percent(50.5);  CHECK(s && s[0], "format_percent(50.5) non-empty");
    s = format_percent(100.0); CHECK(s && s[0], "format_percent(100) non-empty");
    /* format_percent promises ≤5 chars; the space for a trailing '%' is
     * caller-supplied. */
    CHECK(strlen(format_percent(99.9)) <= 5, "format_percent width <= 5");
}

static void test_format_time(void)
{
    /* format_time converts ticks to an HH:MM:SS-ish string. */
    const char *s;
    s = format_time(0);       CHECK(s && s[0], "format_time(0) non-empty");
    s = format_time(12345);   CHECK(s && s[0], "format_time(12345) non-empty");
    s = format_time(99999999);CHECK(s && s[0], "format_time(huge) non-empty");
}

static void test_percentages(void)
{
    /* percentages(cnt, out, new, old, diffs) returns total_change and
     * fills out[i] with the per-state share in thousandths. */
    long new_v[4] = { 100, 200, 300, 400 };
    long old_v[4] = {   0,   0,   0,   0 };
    long diffs[4];
    int  out[4];
    long tot = percentages(4, out, new_v, old_v, diffs);

    CHECK(tot == 1000, "percentages total == sum of new - old");
    CHECK(out[0] == 100, "percentages[0] == 100/1000");
    CHECK(out[1] == 200, "percentages[1] == 200/1000");
    CHECK(out[2] == 300, "percentages[2] == 300/1000");
    CHECK(out[3] == 400, "percentages[3] == 400/1000");

    /* old_v must now hold the previous new_v values for next call. */
    CHECK(old_v[0] == 100 && old_v[3] == 400,
          "percentages writes prev-new into old");

    /* Zero-change case mustn't divide by zero. */
    long zeros[4] = { 0, 0, 0, 0 };
    long prev[4]  = { 5, 5, 5, 5 };
    long od[4]    = { 5, 5, 5, 5 };
    (void)zeros;
    (void)prev;
    tot = percentages(4, out, od, od, diffs);
    CHECK(tot == 1, "percentages returns 1 on no change (divide-by-zero guard)");
}

static void test_atoiwi(void)
{
    /* atoiwi returns an int with special Infinity/Invalid sentinels from
     * top.h. We re-use those constants via the utils.h include chain. */
    CHECK(atoiwi("42")    == 42,  "atoiwi '42'");
    CHECK(atoiwi("0")     == 0,   "atoiwi '0' (zero is valid)");
    CHECK(atoiwi("infin") < 0,    "atoiwi 'infin' is sentinel");
    CHECK(atoiwi("abc")   < 0,    "atoiwi 'abc' is Invalid sentinel");
}

int main(void)
{
    test_digits();
    test_itoa_w();
    test_format_k();
    test_format_percent();
    test_format_time();
    test_percentages();
    test_atoiwi();

    if (failures) {
        fprintf(stderr, "test_utils: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_utils: all passed\n");
    return 0;
}
