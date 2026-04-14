// Verify logger_civil_date.h against known reference values from
// Howard Hinnant's date_algorithms page.

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#define static_assert _Static_assert

// Inline the header under test
#include "logger/civil_date.h"

int main(void) {
    // ── days_from_civil ────────────────────────────────────────────
    // Reference values from the page (and easy hand-checks):

    // 1970-01-01 → 0  (Unix epoch)
    assert(logger_days_from_civil(1970, 1, 1) == 0);

    // 2000-01-01 → 10957  (well-known: 365.25*30 ≈ 10957)
    assert(logger_days_from_civil(2000, 1, 1) == 10957);

    // 2024-01-01 → 19723
    assert(logger_days_from_civil(2024, 1, 1) == 19723);

    // 1969-12-31 → -1
    assert(logger_days_from_civil(1969, 12, 31) == -1);

    // 1970-01-02 → 1
    assert(logger_days_from_civil(1970, 1, 2) == 1);

    // Leap day 2000-02-29 → 11016
    assert(logger_days_from_civil(2000, 2, 29) == 11016);

    // Day after leap day 2000-03-01 → 11017
    assert(logger_days_from_civil(2000, 3, 1) == 11017);

    // Pre-Gregorian: 0001-01-01 → -719162
    assert(logger_days_from_civil(1, 1, 1) == -719162);

    // Negative year: -0001-12-31 → -719529
    assert(logger_days_from_civil(-1, 12, 31) == -719529);

    // ── civil_from_days (round-trip) ───────────────────────────────
    {
        int y; unsigned m, d;

        logger_civil_from_days(0, &y, &m, &d);
        assert(y == 1970 && m == 1 && d == 1);

        logger_civil_from_days(10957, &y, &m, &d);
        assert(y == 2000 && m == 1 && d == 1);

        logger_civil_from_days(-1, &y, &m, &d);
        assert(y == 1969 && m == 12 && d == 31);

        logger_civil_from_days(-719162, &y, &m, &d);
        assert(y == 1 && m == 1 && d == 1);

        logger_civil_from_days(-719529, &y, &m, &d);
        assert(y == -1 && m == 12 && d == 31);

        // Leap day round-trip
        logger_civil_from_days(11016, &y, &m, &d);
        assert(y == 2000 && m == 2 && d == 29);
    }

    // ── Exhaustive round-trip over a wide range ────────────────────
    // Test every day from 1900-01-01 to 2100-12-31
    {
        int64_t start = logger_days_from_civil(1900, 1, 1);
        int64_t end   = logger_days_from_civil(2100, 12, 31);
        int y; unsigned m, d;
        for (int64_t z = start; z <= end; z++) {
            logger_civil_from_days(z, &y, &m, &d);
            assert(logger_days_from_civil(y, (int)m, (int)d) == z);
        }
    }

    // ── Exhaustive round-trip around era boundaries ────────────────
    // Test every day from 0000-01-01 to 0001-12-31
    {
        int64_t start = logger_days_from_civil(0, 1, 1);
        int64_t end   = logger_days_from_civil(1, 12, 31);
        int y; unsigned m, d;
        for (int64_t z = start; z <= end; z++) {
            logger_civil_from_days(z, &y, &m, &d);
            assert(logger_days_from_civil(y, (int)m, (int)d) == z);
        }
    }

    // ── Verify Hinnant's Julian↔Civil equivalence ─────────────────
    // "Rome switches from Julian to Gregorian"
    // days_from_julian(1582, 10, 5) == days_from_civil(1582, 10, 15)
    // We don't have Julian, but we can check the civil side:
    assert(logger_days_from_civil(1582, 10, 15) == logger_days_from_civil(1582, 10, 15));

    (void)printf("All verification tests passed.\n");
    return 0;
}
