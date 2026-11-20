#include "hw_map.h"
/*#include "perf_trace.h"*/

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/slog2.h>

static volatile uint8_t *g_mmio_base = NULL;
static uint32_t          g_sim_counter = 0x1000U;

int hw_mmio_init(void)
{
    void *ptr = mmap_device_memory(
        NULL,
        SENSOR_HW_MEM_SIZE,
        PROT_READ | PROT_WRITE | PROT_NOCACHE,
        0,
        SENSOR_HW_PHYS_BASE
    );

    if (ptr == MAP_FAILED) {
        slog2f(NULL, 0, SLOG2_ERROR,
               "mmio: mmap_device_memory(0x%llx) failed (errno=%d); using simulated fallback",
               (unsigned long long)SENSOR_HW_PHYS_BASE, errno);
        g_mmio_base = NULL;
        return -1;
    }

    g_mmio_base = (volatile uint8_t *)ptr;
    slog2f(NULL, 0, SLOG2_INFO,
           "mmio: mapped phys 0x%llx (%u bytes) -> %p",
           (unsigned long long)SENSOR_HW_PHYS_BASE,
           SENSOR_HW_MEM_SIZE,
           (void *)g_mmio_base);
    return 0;
}

uint32_t hw_mmio_read_sample(void)
{
    uint32_t val;

    if (g_mmio_base != NULL) {
        volatile uint32_t *data_reg =
            (volatile uint32_t *)(g_mmio_base + SENSOR_REG_DATA_OFFSET);
        val = *data_reg;
    } else {
        /* Fallback when running in a VM without exposed MMIO hardware */
        val = ++g_sim_counter;
    }

    /*TRACE_STAMP_MMIO_DONE();*/
    return val;
}

void hw_mmio_clear_irq(void)
{
    if (g_mmio_base != NULL) {
        volatile uint32_t *ctrl_reg =
            (volatile uint32_t *)(g_mmio_base + SENSOR_REG_CTRL_OFFSET);
        *ctrl_reg = SENSOR_REG_IRQ_MASK;
    }
}

void hw_mmio_cleanup(void)
{
    if (g_mmio_base != NULL) {
        munmap_device_memory((void *)g_mmio_base, SENSOR_HW_MEM_SIZE);
        g_mmio_base = NULL;
    }
}

