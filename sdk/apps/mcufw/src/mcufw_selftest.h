/*
 * mcufw_selftest.h -- on-target known-answer tests for the 3.4 network layer.
 *
 * Runs from main() on the bench build and from `make -C sdk/apps/mcufw
 * host-test` on the host: the identical translation units are compiled for
 * arm-none-eabi and for x86-64, so a PASS here is a statement about the code
 * that lands in the container -- not about a mock.
 */
#ifndef MCUFW_SELFTEST_H
#define MCUFW_SELFTEST_H

#include <stdint.h>

/* Bit i of the return value is set when case i FAILED (0 => all pass). */
uint32_t mcufw_selftest(void);

unsigned mcufw_selftest_count(void);
const char *mcufw_selftest_name(unsigned bit);

#endif /* MCUFW_SELFTEST_H */
