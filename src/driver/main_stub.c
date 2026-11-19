/* main_stub.c -- minimal main for the resource manager.
 * Privilege lockdown and root drop already happen inside sensor_resmgr_init(),
 * so main() has nothing else to do. Replace only if the team adds a real main.c. */
#include <stdlib.h>
#include "resmgr.h"

int main(void)
{
    dispatch_t *dpp = sensor_resmgr_init();
    if (dpp == NULL) return EXIT_FAILURE;
    return sensor_resmgr_run(dpp) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
