/*
 * sensor_client.c - QNX Quirks /dev/sensor0 telemetry client.
 *
 * Uses the frozen SENSOR_IFACE_VERSION 1 contract from include/driver_interface.h.
 * Does not modify the resource manager or its interface.
 *
 * Build (QNX SDP 8.0, ARM64):
 *   qcc -Vgcc_ntoaarch64le -Wall -Wextra -Iinclude \
 *       -o bin/qnx_sensor_client src/client/sensor_client.c
 *
 * Examples:
 *   ./qnx_sensor_client --status
 *   ./qnx_sensor_client --reset
 *   ./qnx_sensor_client --inject 42
 *   ./qnx_sensor_client --latest
 *   ./qnx_sensor_client --read
 *   ./qnx_sensor_client --watch 10 --interval-ms 250
 *
 * --watch polls a NONBLOCKING read at fixed intervals. It does not assume
 * continuous samples exist when the hardware producer is not yet connected.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver_interface.h"

enum action {
    ACT_UNSET,
    ACT_STATUS,
    ACT_LATEST,
    ACT_READ,
    ACT_EXPECT_EMPTY,
    ACT_INJECT,
    ACT_RATE,
    ACT_RESET,
    ACT_WATCH
};

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [--device PATH] ACTION [--interval-ms MS]\n"
            "Actions (choose exactly one):\n"
            "  --status          Get driver counters, rate and interface version\n"
            "  --latest          Get latest sensor sample using devctl()\n"
            "  --read            Read one queued sample without blocking\n"
            "  --expect-empty    Succeed only if nonblocking read returns EAGAIN\n"
            "  --inject VALUE    Inject a uint32 sample using write()\n"
            "  --rate HZ         Set sample rate (0..SENSOR_MAX_RATE_HZ)\n"
            "  --reset           Clear buffer and counters (affects all clients)\n"
            "  --watch COUNT     Poll nonblocking read COUNT times (default 250ms)\n"
            "  --help            Show help\n",
            program);
}

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+')
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static int select_action(enum action *current, enum action candidate)
{
    if (*current != ACT_UNSET)
        return -1;
    *current = candidate;
    return 0;
}

static void print_sample(const sensor_sample_t *s)
{
    printf("sample seq=%" PRIu32 " timestamp_ns=%" PRIu64 " value=%" PRIu32 "\n",
           s->seq, s->timestamp_ns, s->value);
}

static int checked_devctl(int fd, int command, void *data, size_t length,
                          const char *label)
{
    int err = devctl(fd, command, data, length, NULL);
    if (err != EOK) {
        fprintf(stderr, "%s failed: %s (code=%d)\n", label,
                strerror(err), err);
        return -1;
    }
    return 0;
}

static int read_one(int fd, int expect_empty)
{
    sensor_sample_t sample;
    ssize_t bytes = read(fd, &sample, sizeof(sample));
    if (bytes == -1 && errno == EAGAIN) {
        puts("empty");
        return expect_empty ? 0 : 2;
    }
    if (bytes == -1) {
        perror("read");
        return 1;
    }
    if (bytes != (ssize_t)sizeof(sample)) {
        fprintf(stderr, "read returned %ld bytes; expected %lu\n",
                (long)bytes, (unsigned long)sizeof(sample));
        return 1;
    }
    if (expect_empty) {
        fputs("expected an empty buffer, but received: ", stderr);
        fprintf(stderr, "seq=%" PRIu32 " value=%" PRIu32 "\n",
                sample.seq, sample.value);
        return 1;
    }
    print_sample(&sample);
    return 0;
}

static int watch_samples(int fd, uint32_t count, uint32_t interval_ms)
{
    uint32_t i;
    struct timespec pause;
    pause.tv_sec = (time_t)(interval_ms / 1000u);
    pause.tv_nsec = (long)((interval_ms % 1000u) * 1000000u);

    for (i = 0; i < count; i++) {
        sensor_sample_t sample;
        ssize_t bytes = read(fd, &sample, sizeof(sample));
        if (bytes == (ssize_t)sizeof(sample)) {
            print_sample(&sample);
        } else if (bytes == -1 && errno == EAGAIN) {
            puts("no_sample");
        } else if (bytes == -1) {
            perror("watch read");
            return 1;
        } else {
            fprintf(stderr, "watch read returned unexpected byte count %ld\n",
                    (long)bytes);
            return 1;
        }
        fflush(stdout);
        if (i + 1u < count) {
            while (nanosleep(&pause, &pause) == -1 && errno == EINTR)
                ;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = SENSOR_DEV_PATH;
    enum action action = ACT_UNSET;
    uint32_t argument = 0, interval_ms = 250;
    int fd, i, exit_code = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        enum action next = ACT_UNSET;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(arg, "--device") == 0) {
            if (++i >= argc) {
                fputs("--device requires a path\n", stderr);
                return 2;
            }
            device = argv[i];
            continue;
        } else if (strcmp(arg, "--interval-ms") == 0) {
            if (++i >= argc || parse_u32(argv[i], &interval_ms) != 0 ||
                interval_ms > 60000u) {
                fputs("--interval-ms must be an integer from 0 to 60000\n", stderr);
                return 2;
            }
            continue;
        } else if (strcmp(arg, "--status") == 0) {
            next = ACT_STATUS;
        } else if (strcmp(arg, "--latest") == 0) {
            next = ACT_LATEST;
        } else if (strcmp(arg, "--read") == 0) {
            next = ACT_READ;
        } else if (strcmp(arg, "--expect-empty") == 0) {
            next = ACT_EXPECT_EMPTY;
        } else if (strcmp(arg, "--reset") == 0) {
            next = ACT_RESET;
        } else if (strcmp(arg, "--inject") == 0 ||
                   strcmp(arg, "--rate") == 0 ||
                   strcmp(arg, "--watch") == 0) {
            if (strcmp(arg, "--inject") == 0)
                next = ACT_INJECT;
            else if (strcmp(arg, "--rate") == 0)
                next = ACT_RATE;
            else
                next = ACT_WATCH;
            if (++i >= argc || parse_u32(argv[i], &argument) != 0 ||
                (next == ACT_WATCH && (argument == 0 || argument > 100000u))) {
                fprintf(stderr, "%s needs a valid unsigned integer\n", arg);
                return 2;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            usage(stderr, argv[0]);
            return 2;
        }
        if (select_action(&action, next) != 0) {
            fputs("Choose exactly one action\n", stderr);
            return 2;
        }
    }

    if (action == ACT_UNSET) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (action != ACT_WATCH && interval_ms != 250u) {
        fputs("--interval-ms is only meaningful with --watch\n", stderr);
        return 2;
    }
    if (action == ACT_RATE && argument > SENSOR_MAX_RATE_HZ) {
        fprintf(stderr, "Rate must be 0..%u Hz\n", SENSOR_MAX_RATE_HZ);
        return 2;
    }

    fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return 1;
    }

    switch (action) {
    case ACT_STATUS: {
        sensor_status_t status;
        memset(&status, 0, sizeof(status));
        if (checked_devctl(fd, DCMD_SENSOR_GET_STATUS, &status,
                           sizeof(status), "GET_STATUS") != 0) {
            exit_code = 1;
            break;
        }
        printf("status irq_count=%" PRIu32 " overruns=%" PRIu32
               " rate_hz=%" PRIu32 " iface_version=%" PRIu32 "\n",
               status.irq_count, status.overruns,
               status.rate_hz, status.iface_version);
        if (status.iface_version != SENSOR_IFACE_VERSION) {
            fprintf(stderr, "Interface mismatch: expected %u\n",
                    SENSOR_IFACE_VERSION);
            exit_code = 1;
        }
        break;
    }
    case ACT_LATEST: {
        sensor_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        if (checked_devctl(fd, DCMD_SENSOR_GET_SAMPLE, &sample,
                           sizeof(sample), "GET_SAMPLE") != 0) {
            exit_code = 1;
            break;
        }
        print_sample(&sample);
        break;
    }
    case ACT_READ:
        exit_code = read_one(fd, 0);
        break;
    case ACT_EXPECT_EMPTY:
        exit_code = read_one(fd, 1);
        break;
    case ACT_INJECT: {
        ssize_t bytes = write(fd, &argument, sizeof(argument));
        if (bytes != (ssize_t)sizeof(argument)) {
            if (bytes < 0) perror("write");
            else fprintf(stderr, "write returned %ld bytes\n", (long)bytes);
            exit_code = 1;
        } else {
            printf("injected value=%" PRIu32 "\n", argument);
        }
        break;
    }
    case ACT_RATE:
        if (checked_devctl(fd, DCMD_SENSOR_SET_RATE, &argument,
                           sizeof(argument), "SET_RATE") != 0)
            exit_code = 1;
        else
            printf("rate_set_hz=%" PRIu32 "\n", argument);
        break;
    case ACT_RESET:
        if (checked_devctl(fd, DCMD_SENSOR_RESET, NULL, 0,
                           "RESET") != 0)
            exit_code = 1;
        else
            puts("reset_ok");
        break;
    case ACT_WATCH:
        exit_code = watch_samples(fd, argument, interval_ms);
        break;
    default:
        exit_code = 2;
        break;
    }

    if (close(fd) == -1) {
        perror("close");
        if (exit_code == 0) exit_code = 1;
    }
    return exit_code;
}
