/*
 * rv_selftest - boots, runs the rv_dsp cross-platform self test, and prints the
 * signature. Used to prove the on-device (Xtensa / newlib) numbers match the
 * validated host build (x86 / glibc). Runs in QEMU with no peripherals.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rv_dsp.h"

static void print_f(const char *name, float v)
{
    /* print decimal and raw IEEE-754 bits so host/device can be compared both
     * ways (bits match where no transcendental libm calls differ). */
    uint32_t bits;
    memcpy(&bits, &v, 4);
    printf("%s=%.7f (0x%08x)\n", name, (double)v, (unsigned)bits);
}

void app_main(void)
{
    float sig[4];
    float s = rv_selftest(sig, 4);

    printf("\n==== RV_SELFTEST BEGIN ====\n");
    print_f("signature", s);
    print_f("los_tap_mag", sig[0]);
    print_f("cfo_err_rad", sig[1]);
    print_f("quiet_metric", sig[2]);
    print_f("motion_metric", sig[3]);
    printf("==== RV_SELFTEST END ====\n");
    fflush(stdout);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
