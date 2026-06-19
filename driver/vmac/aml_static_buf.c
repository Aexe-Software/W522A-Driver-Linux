
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include "wifi_common.h"
#include <linux/skbuff.h>
#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0))
#include <linux/wlan_plat.h>
#elif !defined(NOT_AMLOGIC_PLATFORM)
#include <linux/amlogic/wlan_plat.h>
#endif

#ifdef NOT_AMLOGIC_PLATFORM
extern void *w1_aml_mem_prealloc(int section, unsigned long size);
#else
extern void *bcmdhd_mem_prealloc(int section, unsigned long size);
#endif
enum aml_prealloc_index {
    AML_RX_FIFO = 0,
    AML_TX_DESC_BUF = 1
};
#define AML_RX   11
#define AML_TX   20
#define AML_RX_FIFO_SIZE   (1290 * 1024)
/* Must hold sizeof(struct drv_txdesc) * DRV_TXDESC_NUM. With SYSTEM64 that is
 * 224 * 1280 = 286720 bytes, which overflowed the old 256K limit and made
 * wifi_mem_prealloc() return NULL. 512K matches the order-7 page block that
 * __get_free_pages(get_order()) actually allocates for the backing buffer. */
#define AML_TX_DESC_BUF_SIZE            (512 * 1024)

void *wifi_mem_prealloc(int section, unsigned long size)
{
    pr_info("sectoin %d, size %ld\n", section, size);
    if (section == AML_RX_FIFO ) {
        if (size > AML_RX_FIFO_SIZE) {
            pr_err("request AML_RX_FIFO (%lu) > %d\n",
               size, AML_RX_FIFO_SIZE);
            return NULL;
        }

#ifdef NOT_AMLOGIC_PLATFORM
        return w1_aml_mem_prealloc(AML_RX, size);
#else
        return bcmdhd_mem_prealloc(AML_RX, size);
#endif
    }
    if (section == AML_TX_DESC_BUF) {
        if (size > AML_TX_DESC_BUF_SIZE) {
            pr_err("request AML_TX_DESC_BUF(%lu) > %d\n",
                size, AML_TX_DESC_BUF_SIZE);
            return NULL;
        }

#ifdef NOT_AMLOGIC_PLATFORM
        return w1_aml_mem_prealloc(AML_TX, size);
#else
        return bcmdhd_mem_prealloc(AML_TX, size);
#endif
    }
    return NULL;
}
