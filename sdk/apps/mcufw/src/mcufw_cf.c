/*
 * mcufw_cf.c -- Color Flow (start_cf) skeleton for mcufw phase 3.5.
 *
 * Provides the dispatch path and parameter validation for Yeelight's
 * start_cf command. Actual interpolation/easing/gamma is NOT implemented
 * here -- that is phase 3.6/4 work. This file ensures the RPC dispatcher
 * can route start_cf and validate its arguments.
 */
#include "mcufw_cf.h"

int mcufw_cf_parse(const char *params, size_t len, mcufw_cf_params_t *out)
{
    if (!params || !out || len == 0)
        return -1;

    out->count = 0;
    out->action = MCUFW_CF_ACTION_OFF;
    out->expr_len = 0;

    /* Store raw expression for phase 3.6 parser. */
    size_t copy = len;
    if (copy > sizeof(out->expr) - 1)
        copy = sizeof(out->expr) - 1;
    for (size_t i = 0; i < copy; i++)
        out->expr[i] = params[i];
    out->expr[copy] = '\0';
    out->expr_len = copy;

    return 0;
}

int mcufw_cf_start(const mcufw_cf_params_t *params)
{
    if (!params)
        return -1;
    if (params->action > MCUFW_CF_ACTION_RECOVER)
        return -2;

    /* Phase 3.5: accept but do not animate. The real timer-driven
     * interpolation engine lands in phase 3.6. */
    return 0;
}

void mcufw_cf_stop(void)
{
    /* No-op in phase 3.5. */
}
