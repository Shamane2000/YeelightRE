/*
 * host_test.c -- runs the mcufw self-test on the build host (x86-64).
 *
 * Same translation units as the firmware image (mcufw_crypto.c, mcufw_miio.c,
 * mcufw_selftest.c), so a green run here and a green run on the bench are two
 * views of one implementation.  Exit code 0 = PASS.
 */
#include <stdio.h>

#include "mcufw_selftest.h"

int main(void)
{
	uint32_t fail = mcufw_selftest();
	unsigned n = mcufw_selftest_count();
	unsigned passed = 0;

	for (unsigned i = 0; i < n; i++) {
		int ok = (fail & (1u << i)) == 0u;

		printf("  %-22s %s\n", mcufw_selftest_name(i), ok ? "PASS" : "FAIL");
		if (ok)
			passed++;
	}

	printf("mcufw selftest: %u/%u PASS%s\n", passed, n,
	       fail == 0u ? "" : " -- FAILURES PRESENT");
	return fail == 0u ? 0 : 1;
}
