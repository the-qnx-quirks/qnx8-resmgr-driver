#ifndef HW_MAP_H
#define HW_MAP_H

#include <stdint.h>
#include <stddef.h>
#include <sys/neutrino.h>

/* ---- Target-specific Hardware Register Map -------------------------- */
#if defined(__aarch64__)
    /* Raspberry Pi 4 (BCM2711) System Timer */
    #define SENSOR_HW_PHYS_BASE     0xFE003000ULL
    #define SENSOR_HW_MEM_SIZE      0x1000U
    #define SENSOR_REG_CTRL_OFFSET  0x00U   /* CS  : Control/Status      */
    #define SENSOR_REG_DATA_OFFSET  0x04U   /* CLO : Lower 32-bit counter*/
    #define SENSOR_REG_IRQ_MASK     0x02U   /* Match 1 interrupt bit     */
    #define SENSOR_HW_IRQ           97      /* BCM2711 Timer 1 GIC IRQ   */
#else
    /* x86_64 (VMware / QEMU) HPET Base Address */
    #define SENSOR_HW_PHYS_BASE     0xFED00000ULL
    #define SENSOR_HW_MEM_SIZE      0x1000U
    #define SENSOR_REG_CTRL_OFFSET  0x10U   /* General Configuration     */
    #define SENSOR_REG_DATA_OFFSET  0xF0U   /* Main Counter Value (low)  */
    #define SENSOR_REG_IRQ_MASK     0x01U
    #define SENSOR_HW_IRQ           0       /* Use timer pulse on VM     */
#endif

/* ---- MMIO API (mmio.c) ---------------------------------------------- */
int      hw_mmio_init(void);
uint32_t hw_mmio_read_sample(void);
void     hw_mmio_clear_irq(void);
void     hw_mmio_cleanup(void);

/* ---- Interrupt & Timer Pulse API (irq_pulse.c) ---------------------- */
int  hw_irq_attach(int chid, int irq_num);
void hw_irq_ack(int irq_num, int iid);
void hw_irq_detach(int iid);

int  hw_timer_start(int chid, uint32_t rate_hz);
void hw_timer_stop(void);

#endif /* HW_MAP_H */
