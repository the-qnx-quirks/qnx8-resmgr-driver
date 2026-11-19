/*
 * ham_guard.c -- register a program with the QNX High Availability Manager
 *                so that HAM starts it and restarts it whenever it dies.
 *
 * Usage:   ham_guard <entity-name> <full-path-to-program> [arguments...]
 * Example: ham_guard sensor /proc/boot/my_driver
 *
 * What it does (the pattern from the QNX HAM "Simple restart" example):
 *   1. ham_connect()        connect to the running ham
 *   2. ham_attach()         register the program as an "entity". With pid -1
 *                           HAM itself starts the program and watches it
 *   3. ham_condition()      add a condition that fires when the entity dies
 *   4. ham_action_restart() add the action: run the program again
 *   5. exit                 HAM keeps watching after this program has gone
 *
 * Requirements: `ham` must already be running (start it first), and we must
 * be root. Build:  qcc -Vgcc_ntox86_64 src/driver/ham_guard.c -lham -o ham_guard
 *
 * The entity name may not contain '/'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/netmgr.h>
#include <ha/ham.h>

int main(int argc, char *argv[])
{
    ham_entity_t    *ent;
    ham_condition_t *cond;
    ham_action_t    *act;
    char             cmdline[512];
    size_t           used = 0;
    int              i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <entity-name> <full-path> [args...]\n", argv[0]);
        return 2;
    }
    if (strchr(argv[1], '/') != NULL) {
        fprintf(stderr, "ham_guard: entity name must not contain '/'\n");
        return 2;
    }

    /* HAM takes one command line: the full path followed by its arguments. */
    cmdline[0] = '\0';
    for (i = 2; i < argc; i++) {
        int n = snprintf(cmdline + used, sizeof(cmdline) - used, "%s%s",
                         (i > 2) ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(cmdline) - used) {
            fprintf(stderr, "ham_guard: command line too long\n");
            return 2;
        }
        used += (size_t)n;
    }

    if (ham_connect(0) == -1) {
        fprintf(stderr, "ham_guard: cannot connect to ham (%s). Is ham running?\n",
                strerror(errno));
        return 1;
    }

    /* nd = 0 (this node), pid = -1 (HAM starts the program itself). */
    ent = ham_attach(argv[1], 0, -1, cmdline, 0);
    if (ent == NULL) {
        fprintf(stderr, "ham_guard: ham_attach failed (%s)\n", strerror(errno));
        ham_disconnect(0);
        return 1;
    }

    cond = ham_condition(ent, CONDDEATH, "death", HREARMAFTERRESTART);
    if (cond == NULL) {
        fprintf(stderr, "ham_guard: ham_condition failed (%s)\n", strerror(errno));
        ham_disconnect(0);
        return 1;
    }

    act = ham_action_restart(cond, "restart", cmdline, HREARMAFTERRESTART);
    if (act == NULL) {
        fprintf(stderr, "ham_guard: ham_action_restart failed (%s)\n", strerror(errno));
        ham_disconnect(0);
        return 1;
    }

    printf("ham_guard: '%s' is now supervised (restart on death): %s\n",
           argv[1], cmdline);
    ham_disconnect(0);
    return 0;
}