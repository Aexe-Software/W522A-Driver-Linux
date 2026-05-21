/*
 * Auto-generated version file for w1-aml Armbian port.
 * Original: created by create_version_file.pl during build.
 */
#include <linux/kernel.h>
#include "version.h"

void print_driver_version(void)
{
    pr_info("w1-aml WiFi driver version: %s\n", DRIVERVERSION);
    pr_info("w1-aml Armbian port: NOT_AMLOGIC_PLATFORM / LINUX_PLATFORM\n");
    pr_info("w1-aml Chip: Amlogic W155S1 / Fn-Link K255B-SR\n");
}
