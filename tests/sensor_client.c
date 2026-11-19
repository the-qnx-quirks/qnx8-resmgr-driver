/*
 * sensor_client.c -- self-checking test for /dev/sensor0
 * Build: qcc -V gcc_ntox86_64 -Wall -Iinclude -o sensor_client tests/sensor_client.c
 * Run  : ./sensor_resmgr &   then   ./sensor_client      (prints PASS/FAIL per check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "driver_interface.h"

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s (errno=%d %s)\n", name, errno, strerror(errno)); fails++; } \
} while (0)

int main(void)
{
    sensor_sample_t s;
    sensor_status_t st;
    uint32_t v, hz;
    int rc;

    int fd = open(SENSOR_DEV_PATH, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open " SENSOR_DEV_PATH); return 1; }

    /* 1. empty + non-blocking -> EAGAIN */
    errno = 0;
    CHECK(read(fd, &s, sizeof(s)) == -1 && errno == EAGAIN, "non-blocking read on empty buffer gives EAGAIN");

    /* 2. write injects a sample, read returns it */
    v = 0xCAFE;
    CHECK(write(fd, &v, sizeof(v)) == (ssize_t)sizeof(v), "write 4 bytes accepted");
    CHECK(read(fd, &s, sizeof(s)) == (ssize_t)sizeof(s) && s.value == 0xCAFE, "read returns the injected value");

    /* 3. bad sizes -> EINVAL */
    errno = 0;
    CHECK(write(fd, &v, 2) == -1 && errno == EINVAL, "write of wrong size gives EINVAL");
    errno = 0;
    CHECK(read(fd, &s, 4) == -1 && errno == EINVAL, "read into too-small buffer gives EINVAL");

    /* 4. devctl range check */
    hz = 20000;
    CHECK(devctl(fd, DCMD_SENSOR_SET_RATE, &hz, sizeof(hz), NULL) == EINVAL, "SET_RATE 20000 Hz gives EINVAL");
    hz = 1000;
    CHECK(devctl(fd, DCMD_SENSOR_SET_RATE, &hz, sizeof(hz), NULL) == EOK, "SET_RATE 1000 Hz accepted");

    /* 5. latest sample + status via devctl */
    rc = devctl(fd, DCMD_SENSOR_GET_SAMPLE, &s, sizeof(s), NULL);
    CHECK(rc == EOK && s.value == 0xCAFE, "GET_SAMPLE returns latest value");
    rc = devctl(fd, DCMD_SENSOR_GET_STATUS, &st, sizeof(st), NULL);
    CHECK(rc == EOK && st.rate_hz == 1000 && st.iface_version == SENSOR_IFACE_VERSION, "GET_STATUS shows rate and version");

    /* 6. overflow: write more than the buffer holds, overruns must count */
    for (v = 0; v < SENSOR_RING_SIZE + 5; v++) write(fd, &v, sizeof(v));
    devctl(fd, DCMD_SENSOR_GET_STATUS, &st, sizeof(st), NULL);
    CHECK(st.overruns == 5, "buffer overflow counted in status.overruns");

    /* 7. reset */
    CHECK(devctl(fd, DCMD_SENSOR_RESET, NULL, 0, NULL) == EOK, "RESET accepted");
    errno = 0;
    CHECK(read(fd, &s, sizeof(s)) == -1 && errno == EAGAIN, "buffer empty after RESET");

    close(fd);
    printf("\n%s (%d failure%s)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
