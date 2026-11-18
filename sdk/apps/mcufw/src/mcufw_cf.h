#ifndef MCUFW_CF_H
#define MCUFW_CF_H

#include <stdint.h>
#include <stddef.h>

/* Color Flow (start_cf) dispatcher skeleton for mcufw phase 3.5.
 *
 * Phase 3.5 scope: dispatch path + placeholder handler only.
 * Interpolation, easing curves, gamma correction are phase 3.6/4.
 * The handler acknowledges the command but does NOT animate yet.
 */

#define MCUFW_CF_ACTION_OFF   0
#define MCUFW_CF_ACTION_ON    1
#define MCUFW_CF_ACTION_RECOVER 2

/* Parsed start_cf parameters. */
typedef struct {
    uint32_t count;      /* number of transitions (0 = infinite) */
    uint32_t action;     /* 0=off, 1=on, 2=recover after flow */
    /* flow expression entries stored raw; parsing deferred to phase 3.6 */
    char expr[512];
    size_t expr_len;
} mcufw_cf_params_t;

/* Parse start_cf params from a JSON params array substring.
 * Returns 0 on success, negative on error. */
int mcufw_cf_parse(const char *params, size_t len, mcufw_cf_params_t *out);

/* Dispatch a start_cf command. Returns 0 = accepted, negative = error.
 * Phase 3.5: validates params and returns OK without starting animation. */
int mcufw_cf_start(const mcufw_cf_params_t *params);

/* Stop any running color flow. No-op in phase 3.5. */
void mcufw_cf_stop(void);

#endif /* MCUFW_CF_H */
