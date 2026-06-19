
#include <linux/kernel.h>
#include "version.h"

void print_driver_version(void)
{
    pr_info("W522A: driver version: %s\n", DRIVERVERSION);
    pr_info("W522A: Armbian port: NOT_AMLOGIC_PLATFORM / LINUX_PLATFORM\n");
    pr_info("W522A: Chip: Amlogic W155S1 / Fn-Link K255B-SR\n");
}
