#include "w1_sdio.h"
#include <linux/gfp.h>
#include <linux/irqflags.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/rwsem.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>

#include "chip_pmu_reg.h"
#include "chip_intf_reg.h"
#include "wifi_intf_addr.h"
#include "wifi_drv_reg_ops.h"
#include "rf_d_top_reg.h"
#include "wifi_coex_addr.h"

struct amlw1_hwif_sdio w1_g_w1_hwif_sdio;
struct amlw1_hif_ops w1_g_w1_hif_ops;

unsigned char w1_sdio_wifi_bt_alive;
unsigned char w1_w1_sdio_driver_insmoded;
unsigned char w1_w1_sdio_after_porbe;
unsigned char w1_wifi_in_insmod;
unsigned char w1_wifi_in_rmmod;
unsigned char  w1_wifi_sdio_access = 1;
unsigned char w1_wifi_irq_enable = 0;
volatile unsigned char w1_wifi_hal_probe_done = 0;
static unsigned int shutdown_i = 0;
#define  I2C_CLK_QTR   0x4

static DEFINE_MUTEX(wifi_bt_sdio_mutex);

/* Serializes the multi-step FUNC5 banked-window sequence (set MISC_CTRL bit23
 * -> program RG_SCFG_FUNC5_BADDR_A -> SRAM access). The bank base is a single
 * piece of chip state shared by BT word access and the WiFi AON/fwlog/DPD
 * paths; without this lock a concurrent caller can clobber the base between
 * steps and the access lands at the wrong chip address. This lock is held
 * ACROSS the whole sequence; the per-transaction wifi_bt_sdio_mutex (taken by
 * the low-level read/write ops) is separate, so there is no recursion. */
static DEFINE_MUTEX(func5_bank_mutex);

void w1_func5_bank_lock(void)
{
    mutex_lock(&func5_bank_mutex);
}
EXPORT_SYMBOL(w1_func5_bank_lock);

void w1_func5_bank_unlock(void)
{
    mutex_unlock(&func5_bank_mutex);
}
EXPORT_SYMBOL(w1_func5_bank_unlock);

/* WiFi<->BT exclusive radio arbiter. On W155S1/S2 the 2.4 GHz WiFi and BT
 * share the same RF front-end; this token lets only one subsystem own the
 * radio at a time. 0 = free, otherwise holds W522A_RADIO_WIFI / _BT.
 * try_claim() returns 0 on success (free, or already owned by 'who') and
 * -EBUSY when the other radio holds it. release() only clears the token if
 * 'who' actually owns it, so an unbalanced release from one side can't free
 * the other side's claim. */
static atomic_t w1_radio_owner = ATOMIC_INIT(0);

int w1_wifi_bt_try_claim(int who)
{
    int prev = atomic_cmpxchg(&w1_radio_owner, 0, who);

    if (prev == 0 || prev == who)
        return 0;

    return -EBUSY;
}
EXPORT_SYMBOL(w1_wifi_bt_try_claim);

void w1_wifi_bt_release(int who)
{
    atomic_cmpxchg(&w1_radio_owner, who, 0);
}
EXPORT_SYMBOL(w1_wifi_bt_release);

#define AML_W1_BT_WIFI_MUTEX_ON() do {\
    mutex_lock(&wifi_bt_sdio_mutex);\
} while (0)

#define AML_W1_BT_WIFI_MUTEX_OFF() do {\
    mutex_unlock(&wifi_bt_sdio_mutex);\
} while (0)

#define SDIO_RD_YIELD_BURST  4
#define SDIO_WR_YIELD_BURST  4
static int w522a_sdio_read_yield_burst  = SDIO_RD_YIELD_BURST;
module_param_named(sdio_read_yield_burst, w522a_sdio_read_yield_burst, int, 0644);
MODULE_PARM_DESC(sdio_read_yield_burst, "W522A yield after N SDIO reads (0=disabled)");

static int w522a_sdio_write_yield_burst = SDIO_WR_YIELD_BURST;
module_param_named(sdio_write_yield_burst, w522a_sdio_write_yield_burst, int, 0644);
MODULE_PARM_DESC(sdio_write_yield_burst, "W522A yield after N SDIO writes (0=disabled)");

static int w522a_sdio_yield_min_us = 25;
module_param_named(sdio_yield_min_us, w522a_sdio_yield_min_us, int, 0644);
MODULE_PARM_DESC(sdio_yield_min_us, "W522A SDIO fairness yield minimum usec");

static int w522a_sdio_yield_max_us = 30;
module_param_named(sdio_yield_max_us, w522a_sdio_yield_max_us, int, 0644);
MODULE_PARM_DESC(sdio_yield_max_us, "W522A SDIO fairness yield maximum usec");
static atomic_t sdio_rd_burst = ATOMIC_INIT(0);
static atomic_t sdio_wr_burst = ATOMIC_INIT(0);

static inline void sdio_burst_after_read(void)
{
    int burst = READ_ONCE(w522a_sdio_read_yield_burst);
    int min_us = READ_ONCE(w522a_sdio_yield_min_us);
    int max_us = READ_ONCE(w522a_sdio_yield_max_us);

    atomic_set(&sdio_wr_burst, 0);
    if (burst <= 0)
        return;
    if (atomic_inc_return(&sdio_rd_burst) >= burst) {
        atomic_set(&sdio_rd_burst, 0);
        min_us = clamp_t(int, min_us, 0, 20000);
        max_us = clamp_t(int, max_us, min_us, 50000);
        if (max_us > 0)
            usleep_range(min_us, max_us);
    }
}

static inline void sdio_burst_after_write(void)
{
    int burst = READ_ONCE(w522a_sdio_write_yield_burst);
    int min_us = READ_ONCE(w522a_sdio_yield_min_us);
    int max_us = READ_ONCE(w522a_sdio_yield_max_us);

    atomic_set(&sdio_rd_burst, 0);
    if (burst <= 0)
        return;
    if (atomic_inc_return(&sdio_wr_burst) >= burst) {
        atomic_set(&sdio_wr_burst, 0);
        min_us = clamp_t(int, min_us, 0, 20000);
        max_us = clamp_t(int, max_us, min_us, 50000);
        if (max_us > 0)
            usleep_range(min_us, max_us);
    }
}

static DECLARE_RWSEM(wifi_sdio_power_rwsem);

#define SDIO_DMA_BUF_SIZE    PAGE_SIZE
#define W1_SDIO_SCAT_BOUNCE_SIZE    (511 * FUNC4_BLKSIZE)
#define W1_FUNC6_RX_BOUNCE_SIZE  ((64 * 1024) + PAGE_SIZE)
static void *sdio_dma_buf[SDIO_MAX_FUNCS];
static unsigned char *w1_func6_rx_bounce_buf;
static size_t w1_func6_rx_bounce_buf_size;

static inline bool aml_w1_sdio_is_rx_bounce_func(unsigned char func_num)
{
    return func_num == SDIO_FUNC6 || func_num == SDIO_FUNC7;
}

static bool aml_w1_sdio_func_live(struct sdio_func *func,
                                  unsigned char func_num,
                                  const char *where)
{
    if (unlikely(!func || !func->card || !func->card->host)) {
        pr_warn_ratelimited("W522A: sdio-guard: %s func%d stale func=%p card=%p host=%p access=%u rmmod=%u\n",
                            where, func_num, func,
                            func ? func->card : NULL,
                            (func && func->card) ? func->card->host : NULL,
                            READ_ONCE(w1_wifi_sdio_access),
                            READ_ONCE(w1_wifi_in_rmmod));
        return false;
    }

    if (unlikely(func_num != SDIO_FUNC0 && func->num != func_num)) {
        pr_warn_ratelimited("W522A: sdio-guard: %s func mismatch requested=%u actual=%u\n",
                            where, func_num, func->num);
        return false;
    }

    /* w1_wifi_in_rmmod is set only by the WiFi module's unload path
     * (wifi_hal_platform.c). It must NOT gate BT traffic: FUNC5 (BT banked
     * SRAM) and FUNC0 (shared CCCR) have to keep working when WiFi unloads
     * while BT stays up. Only reject the WiFi data/reg functions here. */
    if (unlikely(READ_ONCE(w1_wifi_in_rmmod)) &&
        func_num != SDIO_FUNC0 && func_num != SDIO_FUNC5) {
        pr_warn_ratelimited("W522A: sdio-guard: %s wifi-func%d rejected during wifi rmmod\n",
                            where, func_num);
        return false;
    }

    return true;
}

static bool aml_w1_sdio_can_sleep_io(const char *where, unsigned char func_num)
{
    if (unlikely(irqs_disabled())) {
        pr_err_ratelimited("W522A: sdio-guard: %s func%d called with IRQs disabled; dropping SDIO transaction\n",
                           where, func_num);
        return false;
    }

    if (unlikely(in_atomic())) {
        pr_err_ratelimited("W522A: sdio-guard: %s func%d called from atomic context; dropping SDIO transaction\n",
                           where, func_num);
        return false;
    }

    return true;
}

static void aml_w1_sdio_release_host_safe(struct sdio_func *func,
                                          unsigned char func_num,
                                          const char *where)
{
    
    if (likely(func && func->card && func->card->host &&
               (func_num == SDIO_FUNC0 || func->num == func_num))) {
        sdio_release_host(func);
        return;
    }

    pr_err_ratelimited("W522A: sdio-guard: %s func%d skip sdio_release_host on stale SDIO function\n",
                       where, func_num);
}

static void *aml_w1_sdio_dma_alloc(size_t size)
{
    return (void *)__get_free_pages(GFP_KERNEL | GFP_DMA | __GFP_ZERO, get_order(size));
}

static void aml_w1_sdio_dma_free(void *buf, size_t size)
{
    if (buf)
        free_pages((unsigned long)buf, get_order(size));
}

void w1_aml_wifi_sdio_power_lock(void)
{
    down_write(&wifi_sdio_power_rwsem);
}

void w1_aml_wifi_sdio_power_unlock(void)
{
    up_write(&wifi_sdio_power_rwsem);
}

static inline void w1_aml_wifi_sdio_io_lock(void)
{
    down_read(&wifi_sdio_power_rwsem);
}

static inline void w1_aml_wifi_sdio_io_unlock(void)
{
    up_read(&wifi_sdio_power_rwsem);
}

unsigned char (*w1_host_wake_w1_req)(void);
int (*w1_host_suspend_req)(struct device *device);
int (*w1_host_resume_req)(struct device *device);

static struct sdio_func *aml_priv_to_func(int func_n)
{
    ASSERT(func_n >= 0 &&  func_n < SDIO_FUNCNUM_MAX);
    return w1_g_w1_hwif_sdio.sdio_func_if[func_n];
}

static int wifi_oob_irq_override = -1;
module_param_named(wifi_oob_irq, wifi_oob_irq_override, int, 0644);
MODULE_PARM_DESC(wifi_oob_irq,
    "Force OOB host-wake Linux IRQ virq (-1=auto map via gpio_intc)");

static int wifi_oob_hwirq = 71;
module_param(wifi_oob_hwirq, int, 0644);
MODULE_PARM_DESC(wifi_oob_hwirq, "gpio-intc hwirq for host-wake (default 71=GPIOX_7)");

static int wifi_oob_irqtype = 8; 
module_param(wifi_oob_irqtype, int, 0644);
MODULE_PARM_DESC(wifi_oob_irqtype, "host-wake IRQ type: 8=level-low 4=level-high 2=falling 1=rising");

int wifi_irq_num(void)
{
    struct device_node *intc;
    struct of_phandle_args fwspec;
    int irq = -1;

    if (wifi_oob_irq_override >= 0)
        return wifi_oob_irq_override;

    intc = of_find_node_by_path("/soc/bus@ffd00000/interrupt-controller@f080");
    if (!intc)
        intc = of_find_compatible_node(NULL, NULL, "amlogic,meson-gpio-intc");
    if (!intc) {
        pr_err("W522A: wifi_irq_num: gpio-intc node not found\n");
        return -ENODEV;
    }

    memset(&fwspec, 0, sizeof(fwspec));
    fwspec.np = intc;
    fwspec.args_count = 2;
    fwspec.args[0] = wifi_oob_hwirq;
    fwspec.args[1] = wifi_oob_irqtype;

    irq = irq_create_of_mapping(&fwspec);
    of_node_put(intc);

    pr_err("W522A: wifi_irq_num: hwirq=%d type=%d -> virq=%d\n",
           wifi_oob_hwirq, wifi_oob_irqtype, irq);
    return irq > 0 ? irq : -EINVAL;
}
EXPORT_SYMBOL(wifi_irq_num);

int wifi_irq_trigger_level(void)
{
    
    return 1;
}
EXPORT_SYMBOL(wifi_irq_trigger_level);

static unsigned int aml_w1_bt_hi_read_word(unsigned int addr)
{
    unsigned int regdata = 0;
    unsigned int reg_tmp;

    mutex_lock(&func5_bank_mutex);

    reg_tmp = w1_g_w1_hif_ops.hi_read_word( RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23))) {
        reg_tmp |= BIT(23);
        w1_g_w1_hif_ops.hi_write_word( RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }

    w1_g_w1_hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);
    w1_g_w1_hif_ops.bt_hi_read_sram((unsigned char*)(SYS_TYPE)&regdata,

        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));

    mutex_unlock(&func5_bank_mutex);
    return regdata;
}

static void aml_w1_bt_hi_write_word(unsigned int addr,unsigned int data)
{
    unsigned int reg_tmp;

    mutex_lock(&func5_bank_mutex);

    reg_tmp = w1_g_w1_hif_ops.hi_read_word( RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23))) {
        reg_tmp |= BIT(23);
        w1_g_w1_hif_ops.hi_write_word( RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }

    w1_g_w1_hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);

    w1_g_w1_hif_ops.bt_hi_write_sram((unsigned char *)&data,

        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));

    mutex_unlock(&func5_bank_mutex);
}

static void aml_w1_bt_sdio_read_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC5, ((SYS_TYPE)addr & SDIO_ADDR_MASK),
        buf, len, (len > 8 ? SDIO_OPMODE_INCREMENT : SDIO_OPMODE_FIXED));
}

static void aml_w1_bt_sdio_write_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_write(SDIO_FUNC5, ((SYS_TYPE)addr & SDIO_ADDR_MASK),
        buf, len, (len > 8 ? SDIO_OPMODE_INCREMENT : SDIO_OPMODE_FIXED));
}

static int aml_w1_sdio_write_reg32(unsigned long sram_addr, unsigned long sramdata)
{
    return w1_g_w1_hif_ops.hi_bottom_write(SDIO_FUNC1, sram_addr&SDIO_ADDR_MASK,
        (unsigned char *)&sramdata,  sizeof(unsigned long), SDIO_OPMODE_INCREMENT);
}

static unsigned int aml_w1_aon_read_reg(unsigned int addr)
{
    unsigned int regdata = 0;

    regdata = w1_g_w1_hif_ops.bt_hi_read_word((addr));
    return regdata;
}

static void aml_w1_aon_write_reg(unsigned int addr,unsigned int data)
{
    w1_g_w1_hif_ops.bt_hi_write_word((addr), data);
}

static int _aml_w1_sdio_request_byte(unsigned char func_num,
    unsigned char write, unsigned int reg_addr, unsigned char *byte)
{
    int err_ret;
    struct sdio_func * func = aml_priv_to_func(func_num);
    unsigned char *kmalloc_buf = NULL;
    unsigned char len = sizeof(unsigned char);

#if defined(DBG_PRINT_COST_TIME)
    struct timespec64 now, before;
    ktime_get_real_ts64(&before); 
#endif 

    ASSERT(func != NULL);
    ASSERT(byte != NULL);
    ASSERT(func->num == func_num);

    if (!aml_w1_sdio_can_sleep_io(__func__, func_num) ||
        !aml_w1_sdio_func_live(func, func_num, __func__))
        return SDIOH_API_RC_FAIL;

    AML_W1_BT_WIFI_MUTEX_ON();

    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        return SDIOH_API_RC_FAIL;
    }
    sdio_claim_host(func);
    kmalloc_buf = sdio_dma_buf[func_num];
    memcpy(kmalloc_buf, byte, len);

    if (write) {
        
        sdio_writeb(func, *kmalloc_buf, reg_addr, &err_ret);
    }
    else {
        
        *byte = sdio_readb(func, reg_addr, &err_ret);
    }

    aml_w1_sdio_release_host_safe(func, func_num, __func__);

#if defined(DBG_PRINT_COST_TIME)
    ktime_get_real_ts64(&now); 

    pr_debug("[sdio byte]: len=1 cost=%lds %luus\n",
        now.tv_sec-before.tv_sec, now.tv_nsec/1000 - before.tv_nsec/1000);
#endif 

    AML_W1_BT_WIFI_MUTEX_OFF();
    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

static size_t aml_w1_sdio_align_size(struct sdio_func *func, unsigned int nbytes,
                                     unsigned char write, unsigned int fix_incr)
{
    bool fifo = (fix_incr == SDIO_OPMODE_FIXED);

    if (write && !fifo) {
        
        return sdio_align_size(func, nbytes);
    } else if (write) {
        
        return nbytes;
    } else if (fifo) {
        
        return nbytes;
    } else {
        
        return sdio_align_size(func, nbytes);
    }
}

static int _aml_w1_sdio_request_buffer(unsigned char func_num,
    unsigned int fix_incr, unsigned char write, unsigned int addr, void *buf, unsigned int nbytes)
{
    int err_ret;
    int align_nbytes = nbytes;
    struct sdio_func * func = aml_priv_to_func(func_num);
    bool fifo = (fix_incr == SDIO_OPMODE_FIXED);

    ASSERT(buf != NULL);
    ASSERT(func != NULL);
    ASSERT(fix_incr == SDIO_OPMODE_FIXED|| fix_incr == SDIO_OPMODE_INCREMENT);
    ASSERT(func->num == func_num);

    if (write && !fifo)
    {
        
        align_nbytes = sdio_align_size(func, nbytes);
        err_ret = sdio_memcpy_toio(func, addr, buf, align_nbytes);
    }
    else if (write)
    {
        
        err_ret = sdio_writesb(func, addr, buf, align_nbytes);
    }
    else if (fifo)
    {
        
        err_ret = sdio_readsb(func, buf, addr, align_nbytes);
    }
    else
    {
        
        align_nbytes = sdio_align_size(func, nbytes);
        err_ret = sdio_memcpy_fromio(func, buf, addr, align_nbytes);
    }

    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

unsigned char aml_w1_sdio_bottom_read8(unsigned char  func_num, int addr);

static atomic_t aml_w1_rx_bounce_hits;
static atomic_t aml_w1_rx_bounce_persistent;
static atomic_t aml_w1_rx_bounce_fallback;

static atomic_t aml_w1_rx_bounce_hits_f6;
static atomic_t aml_w1_rx_bounce_hits_f7;

static int aml_w1_sdio_bottom_read(unsigned char func_num, int addr, void *buf, size_t len, int incr_addr)
{
    void *kmalloc_buf = NULL;
    int result;
    int align_len = 0;
    struct sdio_func * func = aml_priv_to_func(func_num);
    bool need_free_buf = false;
    bool func6_bounce = false;

    ASSERT(func_num != SDIO_FUNC0);

    if (!aml_w1_sdio_can_sleep_io(__func__, func_num) ||
        !aml_w1_sdio_func_live(func, func_num, __func__))
        return SDIOH_API_RC_FAIL;

    if (!w1_wifi_sdio_access) {
        if (func_num == SDIO_FUNC5) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC5, func num %d, addr 0x%08x\n", func_num, addr);

        } else if (func_num == SDIO_FUNC1) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC1, func num %d, addr 0x%08x\n", func_num, addr);

        }  else if ((func_num == SDIO_FUNC2) && (addr == 0x00005080)) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC1, func num %d, addr 0x%08x\n", func_num, addr);

        } else {
            ERROR_DEBUG_OUT("fw recovery downloading, func num %d, addr 0x%08x\n", func_num, addr);
            return -1;
        }
    }

    w1_aml_wifi_sdio_io_lock();

    if (w1_host_wake_w1_req != NULL)
    {
        if (w1_host_wake_w1_req() == 0)
        {
            w1_aml_wifi_sdio_io_unlock();
            ERROR_DEBUG_OUT("W522A: host wake w1 fail\n");
            return -1;
        }
    }
    
    AML_W1_BT_WIFI_MUTEX_ON();

    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        w1_aml_wifi_sdio_io_unlock();
        return SDIOH_API_RC_FAIL;
    }
    sdio_claim_host(func);

    if (!aml_w1_sdio_is_rx_bounce_func(func_num))
    {
        if (incr_addr == SDIO_OPMODE_INCREMENT)
        {
            struct sdio_func * func = aml_priv_to_func(func_num);
            align_len = sdio_align_size(func, len);
        }
        else
            align_len = len;

        if (align_len > SDIO_DMA_BUF_SIZE) {
            need_free_buf = true;
            kmalloc_buf = aml_w1_sdio_dma_alloc(align_len);
        } else {
            kmalloc_buf = sdio_dma_buf[func_num];
        }
    }
    else
    {
        align_len = sdio_align_size(func, len);
        if (w1_func6_rx_bounce_buf != NULL &&
            (size_t)align_len <= w1_func6_rx_bounce_buf_size)
        {
            kmalloc_buf = w1_func6_rx_bounce_buf;
            func6_bounce = true;
            atomic_inc(&aml_w1_rx_bounce_persistent);
        }
        else
        {
            kmalloc_buf = aml_w1_sdio_dma_alloc(align_len);
            if (kmalloc_buf) {
                func6_bounce = true;
                need_free_buf = true;
                atomic_inc(&aml_w1_rx_bounce_fallback);
            }
        }
        atomic_inc(&aml_w1_rx_bounce_hits);

        if (func_num == SDIO_FUNC6)
            atomic_inc(&aml_w1_rx_bounce_hits_f6);
        else if (func_num == SDIO_FUNC7) {
            if (atomic_inc_return(&aml_w1_rx_bounce_hits_f7) == 1) {
                pr_info("W522A: func6-7-bounce-diag: FIRST FUNC7 RX cmd53 bounced "
                        "addr=0x%08x len=%zu align_len=%d "
                        "(persistent_buf=%p persistent_size=%zu) — "
                        "W522A: bounce path is engaged for FUNC7\n",
                        addr, len, align_len,
                        w1_func6_rx_bounce_buf,
                        w1_func6_rx_bounce_buf_size);
            }
        }

        pr_debug("W522A: func6-7-bounce-64k: FUNC6/7 RX fullbounce hits=%d "
                 "(f6=%d f7=%d persistent=%d fallback=%d)\n",
                 atomic_read(&aml_w1_rx_bounce_hits),
                 atomic_read(&aml_w1_rx_bounce_hits_f6),
                 atomic_read(&aml_w1_rx_bounce_hits_f7),
                 atomic_read(&aml_w1_rx_bounce_persistent),
                 atomic_read(&aml_w1_rx_bounce_fallback));
    }

    if (kmalloc_buf == NULL)
    {
        ERROR_DEBUG_OUT("kmalloc buf fail\n");

        aml_w1_sdio_release_host_safe(func, func_num, __func__);

        AML_W1_BT_WIFI_MUTEX_OFF();
        w1_aml_wifi_sdio_io_unlock();
        return SDIOH_API_RC_FAIL;
    }

    result = _aml_w1_sdio_request_buffer(func_num, incr_addr, SDIO_READ, addr, kmalloc_buf,
                                         aml_w1_sdio_is_rx_bounce_func(func_num) ? (size_t)align_len : len);

    if (!aml_w1_sdio_is_rx_bounce_func(func_num))
    {
        memcpy(buf, kmalloc_buf, len);
        if (need_free_buf)
            aml_w1_sdio_dma_free(kmalloc_buf, align_len);
    }
    else if (func6_bounce)
    {
        
        memcpy(buf, kmalloc_buf, len);
        if (need_free_buf)
            aml_w1_sdio_dma_free(kmalloc_buf, align_len);
    }

    aml_w1_sdio_release_host_safe(func, func_num, __func__);

    AML_W1_BT_WIFI_MUTEX_OFF();
    w1_aml_wifi_sdio_io_unlock();
    
    sdio_burst_after_read();
    return result;
}

static int aml_w1_sdio_bottom_write(unsigned char func_num, int addr, void *buf, size_t len, int incr_addr)
{
    void *kmalloc_buf;
    int result;
    struct sdio_func * func = aml_priv_to_func(func_num);
    size_t sdio_aligned_len;

    if (!aml_w1_sdio_can_sleep_io(__func__, func_num) ||
        !aml_w1_sdio_func_live(func, func_num, __func__))
        return SDIOH_API_RC_FAIL;

    if (!w1_wifi_sdio_access) {
        if (func_num == SDIO_FUNC5) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC5, func num %d, addr 0x%08x\n", func_num, addr);

        } else if (func_num == SDIO_FUNC1) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC1, func num %d, addr 0x%08x\n", func_num, addr);

        }  else if ((func_num == SDIO_FUNC2) && (addr == 0x00005080)) {
            
            ERROR_DEBUG_OUT("SDIO_FUNC2, func num %d, addr 0x%08x\n", func_num, addr);

        } else {
            ERROR_DEBUG_OUT("fw recovery downloading, func num %d, addr 0x%08x\n", func_num, addr);
            return -1;
        }
    }

    w1_aml_wifi_sdio_io_lock();
    ASSERT(func_num != SDIO_FUNC0);

    if (w1_host_wake_w1_req != NULL)
    {
        if (w1_host_wake_w1_req() == 0)
        {
            w1_aml_wifi_sdio_io_unlock();
            ERROR_DEBUG_OUT("W522A: host wake w1 fail\n");
            return -1;
        }
    }

    AML_W1_BT_WIFI_MUTEX_ON();
    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        w1_aml_wifi_sdio_io_unlock();
        return SDIOH_API_RC_FAIL;
    }
    sdio_claim_host(func);

    sdio_aligned_len = aml_w1_sdio_align_size(func, len, SDIO_WRITE, incr_addr);

    if (sdio_aligned_len > SDIO_DMA_BUF_SIZE) {
        kmalloc_buf = aml_w1_sdio_dma_alloc(sdio_aligned_len);
        if (!kmalloc_buf) {
            ERROR_DEBUG_OUT("kmalloc buf fail\n");
            aml_w1_sdio_release_host_safe(func, func_num, __func__);
            AML_W1_BT_WIFI_MUTEX_OFF();
            w1_aml_wifi_sdio_io_unlock();
            return SDIOH_API_RC_FAIL;
        }
    } else {
        kmalloc_buf = sdio_dma_buf[func_num];
    }
    memcpy(kmalloc_buf, buf, len);

    result = _aml_w1_sdio_request_buffer(func_num, incr_addr, SDIO_WRITE, addr, kmalloc_buf, sdio_aligned_len);
    if (sdio_aligned_len > SDIO_DMA_BUF_SIZE)
        aml_w1_sdio_dma_free(kmalloc_buf, sdio_aligned_len);

    aml_w1_sdio_release_host_safe(func, func_num, __func__);

    AML_W1_BT_WIFI_MUTEX_OFF();
    w1_aml_wifi_sdio_io_unlock();
    
    sdio_burst_after_write();
    return result;
}

static void aml_w1_sdio_read_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC2, (SYS_TYPE)addr&SDIO_ADDR_MASK,
        buf, len, (len > 8 ? SDIO_OPMODE_INCREMENT : SDIO_OPMODE_FIXED));
}

static void aml_w1_sdio_write_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_write(SDIO_FUNC2, (SYS_TYPE)addr&SDIO_ADDR_MASK,
        buf, len, (len > 8 ? SDIO_OPMODE_INCREMENT : SDIO_OPMODE_FIXED));
}

static unsigned int aml_w1_sdio_read_word(unsigned int addr)
{
    unsigned int regdata = 0;

    if ((addr & 0x00f00000) == 0x00f00000) {
        regdata = aml_w1_aon_read_reg(addr);
    }
    else if(((addr & 0x00f00000) == 0x00b00000)||
        ((addr & 0x00f00000) == 0x00d00000)||
        ((addr & 0x00f00000) == 0x00900000))
    {
        regdata = aml_w1_aon_read_reg(addr);
    }
    else if(((addr & 0x00f00000) == 0x00200000)||
        ((addr & 0x00f00000) == 0x00300000)||
        ((addr & 0x00f00000) == 0x00400000))
    {
        regdata = aml_w1_aon_read_reg(addr);
    }
    else
    {
        aml_w1_sdio_read_sram((unsigned char*)(SYS_TYPE)&regdata,
            (unsigned char*)(SYS_TYPE)(addr), sizeof(unsigned int));
    }
    return regdata;
}

static void aml_w1_sdio_write_word(unsigned int addr, unsigned int data)
{
    
    if ((addr & 0x00f00000) == 0x00f00000) {
        aml_w1_aon_write_reg(addr, data);
    }
    else if(((addr & 0x00f00000) == 0x00b00000)||
        ((addr & 0x00f00000) == 0x00d00000)||
        ((addr & 0x00f00000) == 0x00900000))
    {
        aml_w1_aon_write_reg(addr, data);
    }
    else if(((addr & 0x00f00000) == 0x00200000)||
        ((addr & 0x00f00000) == 0x00300000)||
        ((addr & 0x00f00000) == 0x00400000))
    {
        aml_w1_aon_write_reg((addr), data);
    }
    else
    {
        aml_w1_sdio_write_sram((unsigned char *)&data,
            (unsigned char*)(SYS_TYPE)(addr), sizeof(unsigned int));
    }
}

static int aml_w1_sdio_bottom_write8(unsigned char  func_num, int addr, unsigned char data)
{
    int ret = 0;

    ASSERT(func_num != SDIO_FUNC0);
    ret =  _aml_w1_sdio_request_byte(func_num, SDIO_WRITE, addr, &data);

    return ret;
}

unsigned char aml_w1_sdio_bottom_read8(unsigned char  func_num, int addr)
{
    unsigned char sramdata;

    _aml_w1_sdio_request_byte(func_num, SDIO_READ, addr, &sramdata);
    return sramdata;
}

static void aml_w1_sdio_bottom_write8_func0(unsigned long sram_addr, unsigned char sramdata)
{
    _aml_w1_sdio_request_byte(SDIO_FUNC0, SDIO_WRITE, sram_addr, &sramdata);
}

static unsigned char aml_w1_sdio_bottom_read8_func0(unsigned long sram_addr)
{
    unsigned char sramdata;

    _aml_w1_sdio_request_byte(SDIO_FUNC0, SDIO_READ, sram_addr, &sramdata);
    return sramdata;
}

static void aml_w1_sdio_write_cmd32(unsigned long sram_addr, unsigned long sramdata)
{
#if defined (HAL_SIM_VER)
    aml_sdio_read_write(sram_addr&SDIO_ADDR_MASK,	(unsigned char *)&sramdata, 4,
                        SDIO_FUNC3,SDIO_RW_FLAG_WRITE,SDIO_F_SYNCHRONOUS);
#elif defined (HAL_FPGA_VER)
    w1_g_w1_hif_ops.hi_bottom_write(SDIO_FUNC3, sram_addr&SDIO_ADDR_MASK,
        (unsigned char *)&sramdata, sizeof(unsigned long), SDIO_OPMODE_INCREMENT);
#endif 
}

static int aml_w1_sdio_suspend(unsigned int suspend_enable)
{
    mmc_pm_flag_t flags;
    struct sdio_func *func = NULL;
    int ret = 0, i;

    if (suspend_enable == 0)
    {
        
        return ret;
    }

    AML_W1_BT_WIFI_MUTEX_ON();
    
    for (i = SDIO_FUNC1; i <= FUNCNUM_SDIO_LAST; i++)
    {
        func = aml_priv_to_func(i);
        if (func == NULL)
            continue;
        flags = sdio_get_host_pm_caps(func);

        if ((flags & MMC_PM_KEEP_POWER) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);

        if (ret != 0) {
            AML_W1_BT_WIFI_MUTEX_OFF();
            return -1;
        }

        if ((flags & MMC_PM_WAKE_SDIO_IRQ) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_WAKE_SDIO_IRQ);

        if (ret != 0) {
            AML_W1_BT_WIFI_MUTEX_OFF();
            return -1;
        }
    }

    AML_W1_BT_WIFI_MUTEX_OFF();
    return ret;
}

static unsigned long  aml_w1_sdio_read_reg8(unsigned long sram_addr )
{
    unsigned char regdata[8] = {0};

    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC1, sram_addr&SDIO_ADDR_MASK, regdata, 1, SDIO_OPMODE_INCREMENT);
    return regdata[0];
}

static void   aml_w1_sdio_write_reg8(unsigned long sram_addr, unsigned long sramdata)
{
    w1_g_w1_hif_ops.hi_bottom_write(SDIO_FUNC1, sram_addr&SDIO_ADDR_MASK,
        (unsigned char *)&sramdata, sizeof(unsigned long), SDIO_OPMODE_INCREMENT);
}

static unsigned long  aml_w1_sdio_read_reg32(unsigned long sram_addr)
{
    unsigned long sramdata = 0;

    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC1, sram_addr&SDIO_ADDR_MASK, &sramdata, 4, SDIO_OPMODE_INCREMENT);
    return sramdata;
}

static struct amlw_hif_scatter_req *aml_w1_sdio_scatter_req_get(void)
{
    struct amlw1_hwif_sdio *hif_sdio = &w1_g_w1_hwif_sdio;

    struct amlw_hif_scatter_req *scat_req = NULL;

    ASSERT(hif_sdio != NULL);

    if (hif_sdio == NULL || hif_sdio->scat_req == NULL)
        return NULL;

    scat_req = hif_sdio->scat_req;

    if (scat_req->free)
    {
        scat_req->free = false;
    }
    else if (scat_req->scat_count != 0) 
    {
        scat_req = NULL;
    }

    return scat_req;
}

static int amlw_w1_sdio_alloc_prep_scat_req(struct amlw1_hwif_sdio *hif_sdio)
{
    struct amlw_hif_scatter_req * scat_req = NULL;

    ASSERT(hif_sdio != NULL);

    scat_req = kzalloc(sizeof(struct amlw_hif_scatter_req), GFP_KERNEL);
    if (scat_req == NULL)
    {
        ERROR_DEBUG_OUT("[sdio sg alloc_scat_req]: no mem\n");
        return 1;
    }

    scat_req->free = true;
    hif_sdio->scat_req = scat_req;
#ifdef NOT_AMLOGIC_PLATFORM
    hif_sdio->bounce_buf_size = W1_SDIO_SCAT_BOUNCE_SIZE;
    hif_sdio->bounce_buf = aml_w1_sdio_dma_alloc(hif_sdio->bounce_buf_size);
    if (!hif_sdio->bounce_buf) {
        kfree(scat_req);
        hif_sdio->scat_req = NULL;
        hif_sdio->bounce_buf_size = 0;
        ERROR_DEBUG_OUT("[sdio sg alloc_bounce]: no mem\n");
        return -ENOMEM;
    }
    
    pr_info("W522A: aml_sdio: bounce_buf=%p size=%u align=0x%lx (RX-BOUNCE-FIX armed)\n",
            hif_sdio->bounce_buf, hif_sdio->bounce_buf_size,
            (unsigned long)hif_sdio->bounce_buf & (PAGE_SIZE - 1));
#else
    pr_warn("W522A: aml_sdio: NOT_AMLOGIC_PLATFORM not defined, RX-BOUNCE-FIX disabled\n");
#endif

    atomic_set(&aml_w1_rx_bounce_hits, 0);
    atomic_set(&aml_w1_rx_bounce_persistent, 0);
    atomic_set(&aml_w1_rx_bounce_fallback, 0);

    return 0;
}

static int aml_w1_sdio_enable_scatter(void)
{
    struct amlw1_hwif_sdio *hif_sdio = &w1_g_w1_hwif_sdio;
    int ret;

    ASSERT(hif_sdio != NULL);

    if (hif_sdio->scatter_enabled)
        return 0;

    ret = amlw_w1_sdio_alloc_prep_scat_req(&w1_g_w1_hwif_sdio);
    if (ret == 0)
        hif_sdio->scatter_enabled = true;
    return ret;
}

static int aml_w1_sdio_scat_rw(struct scatterlist *sg_list, unsigned int sg_num, unsigned int blkcnt,
        unsigned char func_num, unsigned int addr, unsigned char write)
{
    struct mmc_request mmc_req;
    struct mmc_command mmc_cmd;
    struct mmc_data    mmc_dat;
    struct sdio_func *func = aml_priv_to_func(func_num);
    int ret = 0;

    if (!aml_w1_sdio_can_sleep_io(__func__, func_num) ||
        !aml_w1_sdio_func_live(func, func_num, __func__))
        return -EIO;

    AML_W1_BT_WIFI_MUTEX_ON();
    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        return -EIO;
    }
    memset(&mmc_req, 0, sizeof(struct mmc_request));
    memset(&mmc_cmd, 0, sizeof(struct mmc_command));
    memset(&mmc_dat, 0, sizeof(struct mmc_data));

    mmc_dat.sg     = sg_list;
    mmc_dat.sg_len = sg_num;
    mmc_dat.blksz  = FUNC4_BLKSIZE;
    mmc_dat.blocks = blkcnt;
    mmc_dat.flags  = write ? MMC_DATA_WRITE : MMC_DATA_READ;

    mmc_cmd.opcode = SD_IO_RW_EXTENDED;
    mmc_cmd.arg    = write ? 1 << 31 : 0;
    mmc_cmd.arg   |= (func_num & 0x7) << 28;
    mmc_cmd.arg   |= 1 << 27;	
    mmc_cmd.arg   |= 0 << 26;	   	
    mmc_cmd.arg   |= (addr & 0x1ffff)<< 9;
    mmc_cmd.arg   |= blkcnt & 0x1ff;
    mmc_cmd.flags  = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

    mmc_req.cmd = &mmc_cmd;
    mmc_req.data = &mmc_dat;

    sdio_claim_host(func);
    mmc_set_data_timeout(&mmc_dat, func->card);
    mmc_wait_for_req(func->card->host, &mmc_req);
    aml_w1_sdio_release_host_safe(func, func_num, __func__);

    if (mmc_cmd.error || mmc_dat.error) {
        pr_err("ERROR CMD53 %s cmd_error = %d data_error=%d\n",
        write ? "write" : "read", mmc_cmd.error, mmc_dat.error);
        ret  = mmc_cmd.error;
    }

    AML_W1_BT_WIFI_MUTEX_OFF();
    return ret;
}

static void aml_w1_sdio_scat_complete (struct amlw_hif_scatter_req * scat_req)
{
    int  i;
    struct amlw1_hwif_sdio *hif_sdio = &w1_g_w1_hwif_sdio;

    ASSERT(scat_req != NULL);
    ASSERT(hif_sdio != NULL);

    if (scat_req == NULL)
        return;

    if (scat_req->complete)
    {
        for (i = 0; i < scat_req->scat_count; i++)
        {
            (scat_req->complete)(scat_req->scat_list[i].skbbuf);
            scat_req->scat_list[i].skbbuf = NULL;
        }
    }
    else
    {
        ERROR_DEBUG_OUT("error: no complete function\n");
    }

    scat_req->free = true;
    scat_req->scat_count = 0;
    scat_req->len = 0;
    scat_req->addr = 0;
    memset(scat_req->sgentries, 0, SDIO_MAX_SG_ENTRIES * sizeof(struct scatterlist));
}

static void aml_w1_sdio_cleanup_scatter(void)
{
    struct amlw1_hwif_sdio *hif_sdio = &w1_g_w1_hwif_sdio;
    pr_debug("[sdio sg cleanup]: enter\n");

    ASSERT(hif_sdio != NULL);

    if (!hif_sdio->scatter_enabled)
        return;

    hif_sdio->scatter_enabled = false;

#ifdef NOT_AMLOGIC_PLATFORM
    aml_w1_sdio_dma_free(hif_sdio->bounce_buf, hif_sdio->bounce_buf_size);
    hif_sdio->bounce_buf = NULL;
    hif_sdio->bounce_buf_size = 0;
#endif
    kfree(hif_sdio->scat_req);
    hif_sdio->scat_req = NULL;
    pr_debug("[sdio sg cleanup]: exit\n");

    return;
}

static void aml_w1_sdio_recv_frame (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC6, ((SYS_TYPE)addr & SDIO_ADDR_MASK),
        buf, len, SDIO_OPMODE_INCREMENT);
}

static void aml_w1_sdio_init_ops(void)
{
    struct amlw1_hif_ops* ops = &w1_g_w1_hif_ops;

    ops->hi_bottom_write8 = aml_w1_sdio_bottom_write8;
    ops->hi_bottom_read8 = aml_w1_sdio_bottom_read8;
    ops->hi_bottom_read = aml_w1_sdio_bottom_read;
    ops->hi_bottom_write = aml_w1_sdio_bottom_write;
    ops->hi_write8_func0 = aml_w1_sdio_bottom_write8_func0;
    ops->hi_read8_func0 = aml_w1_sdio_bottom_read8_func0;

    ops->hi_enable_scat = aml_w1_sdio_enable_scatter;
    ops->hi_cleanup_scat = aml_w1_sdio_cleanup_scatter;
    ops->hi_get_scatreq = aml_w1_sdio_scatter_req_get;
    ops->hi_rcv_frame = aml_w1_sdio_recv_frame;

    ops->hi_read_reg8 = aml_w1_sdio_read_reg8;
    ops->hi_write_reg8 = aml_w1_sdio_write_reg8;
    ops->hi_read_reg32 = aml_w1_sdio_read_reg32;
    ops->hi_write_reg32 = aml_w1_sdio_write_reg32;
    ops->hi_write_cmd = aml_w1_sdio_write_cmd32;
    ops->hi_write_sram = aml_w1_sdio_write_sram;
    ops->hi_read_sram = aml_w1_sdio_read_sram;

    ops->hi_write_word = aml_w1_sdio_write_word;
    ops->hi_read_word = aml_w1_sdio_read_word;

    ops->bt_hi_write_sram = aml_w1_bt_sdio_write_sram;
    ops->bt_hi_read_sram = aml_w1_bt_sdio_read_sram;
    ops->bt_hi_write_word = aml_w1_bt_hi_write_word;
    ops->bt_hi_read_word = aml_w1_bt_hi_read_word;
    ops->hif_suspend = aml_w1_sdio_suspend;

    w1_w1_sdio_after_porbe = 1;

    w1_host_wake_w1_req = NULL;
    w1_host_suspend_req = NULL;
}

static int aml_w1_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
    int ret = 0;
    static struct sdio_func sdio_func_0;

    sdio_claim_host(func);
    ret = sdio_enable_func(func);
    if (ret)
        goto sdio_enable_error;

    {
        int blksz_ret = sdio_set_block_size(func, 512);
        if (blksz_ret) {
            pr_warn("W522A: sdio_set_block_size(512) failed on func%d ret=%d, retrying 128\n",
                func->num, blksz_ret);
            blksz_ret = sdio_set_block_size(func, 128);
            if (blksz_ret) {
                pr_warn("W522A: sdio_set_block_size(128) also failed on func%d ret=%d; throughput will be DEGRADED\n",
                    func->num, blksz_ret);
            }
        }
    }

    pr_debug("%s(%d): func->num %d sdio block size=%d, \n", __func__, __LINE__,
        func->num,  func->cur_blksize);

    if (func->num == 1)
    {
        sdio_func_0.num = 0;
        sdio_func_0.card = func->card;
        w1_g_w1_hwif_sdio.sdio_func_if[0] = &sdio_func_0;
    }
    w1_g_w1_hwif_sdio.sdio_func_if[func->num] = func;
    pr_debug("%s(%d): func->num %d sdio_func=%p, \n", __func__, __LINE__,
        func->num,  func);

    sdio_release_host(func);
    sdio_set_drvdata(func, (void *)(&w1_g_w1_hwif_sdio));
    if (func->num != FUNCNUM_SDIO_LAST)
    {
        pr_debug("%s(%d):func_num=%d, last func num=%d\n", __func__, __LINE__,
            func->num, FUNCNUM_SDIO_LAST);
        return 0;
    }

    WRITE_ONCE(w1_wifi_sdio_access, 1);
    WRITE_ONCE(w1_wifi_in_rmmod, 0);
    aml_w1_sdio_init_ops();

    return ret;

sdio_enable_error:
    pr_err("sdio_enable_error:  line %d\n",__LINE__);
    sdio_release_host(func);

    return ret;
}

static void  aml_w1_sdio_remove(struct sdio_func *func)
{
    int ret = 0;
    int err = 0;
    u8 abort;

    if (func== NULL)
    {
        return ;
    }

    WRITE_ONCE(w1_wifi_in_rmmod, 1);
    WRITE_ONCE(w1_wifi_sdio_access, 0);

    pr_info("W522A: enter func->num=%d FUNCNUM_SDIO_LAST=%d\n",
            func->num, FUNCNUM_SDIO_LAST);

    sdio_claim_host(func);

    if (func->num == FUNCNUM_SDIO_LAST && func->card) {
        ret = mmc_hw_reset(func->card);
        if (ret == 0) {
            pr_info("W522A: mmc_hw_reset OK — chip ready for next modprobe\n");
        } else if (ret == 1) {
            pr_info("W522A: mmc_hw_reset scheduled async rescan (ret=1) — expected for W155S1\n");
        } else {
            pr_warn("W522A: mmc_hw_reset failed: %d — falling back to CCCR RES\n", ret);
            {
                u32 prev_quirks = func->card->quirks;
                func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
                abort = sdio_f0_readb(func, SDIO_CCCR_ABORT, &err);
                if (err)
                    abort = 0x00;
                abort |= 0x08;
                sdio_f0_writeb(func, abort, SDIO_CCCR_ABORT, &err);
                if (err)
                    pr_warn("W522A: fallback CCCR RES write failed: %d\n", err);
                else
                    pr_info("W522A: fallback CCCR RES issued\n");
                func->card->quirks = prev_quirks;
            }
        }
        msleep(10);
    }

    sdio_disable_func(func);
    w1_g_w1_hwif_sdio.sdio_func_if[func->num] = NULL;
    sdio_release_host(func);

    w1_host_wake_w1_req = NULL;
    w1_host_suspend_req = NULL;
    w1_host_resume_req = NULL;
}

static int aml_sdio_pm_suspend(struct device *device)
{
    if (w1_host_suspend_req != NULL)
        return w1_host_suspend_req(device);
    else
        return aml_w1_sdio_suspend(1);
}

static int aml_sdio_pm_resume(struct device *device)
{
    if (w1_host_resume_req != NULL)
        return w1_host_resume_req(device);
    else
        return 0;
}

static void write_byte_8ba(unsigned char Bus, unsigned char SlaveAddr,
    unsigned char RegAddr, unsigned char Data)
{
    unsigned int tmp,cnt = 0;

    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp = (I2C_END  << 16)             |
            (I2C_DATA << 12)             |    
            (I2C_DATA << 8)              |    
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,(Data << 8) | (RegAddr << 0));
    
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    do {
        tmp  = aml_w1_sdio_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 1000) {
            pr_err("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));

}

static unsigned char read_byte_8ba(unsigned char Bus, unsigned char SlaveAddr, unsigned char RegAddr)
{
    
    unsigned int tmp,cnt = 0;

    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END  << 24)             |
            (I2C_DATA_LAST << 20)        |  
            (I2C_SLAVE_ADDR_READ << 16)  |
            (I2C_START << 12)            |
            (I2C_DATA << 8)              |  
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,(RegAddr << 0));
    
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    do {
        tmp  = aml_w1_sdio_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 1000) {
            pr_err("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));

    tmp  = aml_w1_sdio_read_word(I2C_TOKEN_RDATA_REG0) & 0xff;
    return (unsigned char)tmp;
}

static void write_word_32ba(unsigned char Bus, unsigned char SlaveAddr,
    unsigned int StartToken, unsigned int Data)
{
    
    unsigned int tmp,cnt = 0;

    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END << 28)              |    
            (I2C_DATA << 24)             |    
            (I2C_DATA << 20)             |    
            (I2C_DATA << 16)             |    
            (I2C_DATA << 12)             |    
            (I2C_DATA << 8)              |    
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,StartToken | (Data<<8));
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG1,(Data >> 24));
    
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    tmp = 0;
    do {
        tmp  = aml_w1_sdio_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 100000) {
            ERROR_DEBUG_OUT("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));
}

static unsigned int read_word_32ba(unsigned int SlaveAddr, unsigned int RegAddr)
{
    
    unsigned int tmp,cnt = 0;

    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =    (I2C_DATA  << 28)            |
             (I2C_DATA  << 24)            |
             (I2C_DATA  << 20)            |
             (I2C_SLAVE_ADDR_READ  << 16) |  
             (I2C_START << 12)            |  
             (I2C_DATA  << 8)             |  
             (I2C_SLAVE_ADDR_WRITE << 4)  |  
             (I2C_START << 0);               
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG1);
    tmp = (I2C_END       << 4) | (I2C_DATA_LAST << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG1, tmp);

    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,RegAddr << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG1,0);
    
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    tmp = 0;
    do {
        tmp  = aml_w1_sdio_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 100000) {
            ERROR_DEBUG_OUT("-------[ERR]-----> i2c[R] err\n");
            break;
        }
    } while( tmp & (1 << 2));

    tmp = aml_w1_sdio_read_word(I2C_TOKEN_RDATA_REG0);
    return tmp;
}

unsigned int rf_i2c_read(unsigned int reg_addr)
{
    unsigned char bus = 0;
    unsigned int slave_addr = 0x7a;
    unsigned int read_data = 0;
    unsigned int start_token;

    start_token = 0x04;

    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    read_data = read_word_32ba(slave_addr, 0x0);
    return read_data;
}

void rf_i2c_write(unsigned int reg_addr, unsigned int data)
{
    unsigned char bus = 0;
    unsigned int slave_addr = 0x7a;
    unsigned int start_token;

    start_token = 0x00;  
    write_word_32ba(bus, slave_addr, 0x00, data);

    start_token = 0x04;  
    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    write_byte_8ba(bus, slave_addr, 0x8, bus);
}

static void config_pmu_reg_off(void)
{
    RG_AON_A30_FIELD_T reg_aon30_data;
    RG_AON_A29_FIELD_T reg_aon29_data;

    unsigned int reg_val = 0;

    reg_val = rf_i2c_read(RG_TOP_A2);
    reg_val = reg_val &(~0x1f);
    
    reg_val |= 0x15;
    rf_i2c_write(RG_TOP_A2, reg_val);

    reg_val  = aml_w1_sdio_read_word(RG_PMU_A16);
    reg_val &= (~BIT(31));
    aml_w1_sdio_write_word(RG_PMU_A16, reg_val);

    reg_val  = aml_w1_sdio_read_word(RG_COEX_WF_OWNER_CTRL);
    reg_val &= (~BIT(28) );
    aml_w1_sdio_write_word(RG_COEX_WF_OWNER_CTRL, reg_val);

    {
        int value_pmu_A12 = aml_w1_sdio_read_word(RG_PMU_A12);
        int value_pmu_A15 = aml_w1_sdio_read_word(RG_PMU_A15);
        int value_pmu_A17 = aml_w1_sdio_read_word(RG_PMU_A17);
        int value_pmu_A18 = aml_w1_sdio_read_word(RG_PMU_A18);
        int value_pmu_A20 = aml_w1_sdio_read_word(RG_PMU_A20);
        int value_pmu_A22 = aml_w1_sdio_read_word(RG_PMU_A22);
        int value_pmu_A24 = aml_w1_sdio_read_word(RG_PMU_A24);
        int value_aon30   = aml_w1_sdio_read_word(RG_AON_A30);
        unsigned char host_req_status;

        pr_debug("%s power off: before write A12=0x%x, A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x, AON30=0x%x\n",
            __func__, value_pmu_A12,value_pmu_A15,value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22,value_pmu_A24, value_aon30);

        aml_w1_sdio_write_word(RG_INTF_CPU_CLK, 0x4f070001);

        reg_aon29_data.data = aml_w1_sdio_read_word(RG_AON_A29);
        reg_aon29_data.b.rg_ana_bpll_cfg |= BIT(1) | BIT(0);
        aml_w1_sdio_write_word(RG_AON_A29, reg_aon29_data.data);

        aml_w1_sdio_write_word(RG_PMU_A12, 0x9ea2e); 
        aml_w1_sdio_write_word(RG_PMU_A14, 0x1);
        aml_w1_sdio_write_word(RG_PMU_A16, 0x0);
        aml_w1_sdio_write_word(RG_PMU_A22, 0x707);
        aml_w1_sdio_write_word(RG_PMU_A18, 0x8700);
        aml_w1_sdio_write_word(RG_PMU_A20, 0x3ff01ff);
        aml_w1_sdio_write_word(RG_PMU_A17, 0x703);
        
        reg_aon30_data.data = aml_w1_sdio_read_word(RG_AON_A30);
        
        reg_aon30_data.b.rg_always_on_cfg4 |= BIT(12);
        aml_w1_sdio_write_word(RG_AON_A30, reg_aon30_data.data);

        value_pmu_A12 = aml_w1_sdio_read_word(RG_PMU_A12);
        value_pmu_A15 = aml_w1_sdio_read_word(RG_PMU_A15);
        value_pmu_A17 = aml_w1_sdio_read_word(RG_PMU_A17);
        value_pmu_A18 = aml_w1_sdio_read_word(RG_PMU_A18);
        value_pmu_A20 = aml_w1_sdio_read_word(RG_PMU_A20);
        value_pmu_A22 = aml_w1_sdio_read_word(RG_PMU_A22);
        value_pmu_A24 = aml_w1_sdio_read_word(RG_PMU_A24);
        value_aon30   = aml_w1_sdio_read_word(RG_AON_A30);
        pr_debug("%s power off: after write A12=0x%x, A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x, AON30=0x%x\n",
            __func__, value_pmu_A12,value_pmu_A15,value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22,value_pmu_A24, value_aon30);

        host_req_status = (0x8 << 1)| BIT(0);
        aml_w1_sdio_bottom_write8(SDIO_FUNC1, 0x221, host_req_status);
    }
}

extern int wifi_irq_num(void);
static void aml_sdio_shutdown(struct device *device)
{
    pr_debug("===>>>> enter %s <<<===\n", __func__);
    if (w1_wifi_irq_enable == 1) {
#if (USE_SDIO_IRQ==1)
        struct sdio_func *func = w1_g_w1_hwif_sdio.sdio_func_if[SDIO_FUNC1];
        sdio_claim_host(func);
        sdio_release_irq(func);
        sdio_release_host(func);
#elif (USE_GPIO_IRQ==1)
        unsigned int irq_num = wifi_irq_num();
        disable_irq(irq_num);
#endif
        w1_wifi_irq_enable = 0;
    }
    shutdown_i += 1;
    if (shutdown_i == 1) {
        if (w1_wifi_hal_probe_done) {
            config_pmu_reg_off();
        }
    } else if (shutdown_i == 7) {
        shutdown_i = 0;
        pr_debug("===>>>> end <<<===\n");
    } else {
        ;
    }
    pr_debug("=== shutdown_i:%d ===\n", shutdown_i);
}

static SIMPLE_DEV_PM_OPS(aml_sdio_pm_ops, aml_sdio_pm_suspend,
                     aml_sdio_pm_resume);

static const struct sdio_device_id aml_w1_sdio[] =
{
    {SDIO_DEVICE(W1_VENDOR_AMLOGIC,W1_PRODUCT_AMLOGIC) },
    {SDIO_DEVICE(W1_VENDOR_AMLOGIC_EFUSE,W1_PRODUCT_AMLOGIC_EFUSE)},
    {}
};

static struct sdio_driver aml_w1_sdio_driver =
{
    .name = "aml_w1_sdio",
    .id_table = aml_w1_sdio,
    .probe = aml_w1_sdio_probe,
    .remove = aml_w1_sdio_remove,
    .drv.pm = &aml_sdio_pm_ops,
    .drv.shutdown = aml_sdio_shutdown,
};

#ifdef NOT_AMLOGIC_PLATFORM

static void *wlan_preallocated_rx_buf;
static void *wlan_preallocated_tx_desc_buf;

#define AML_RX  11
#define AML_TX  20

#define RX_BUF_LEN    459776

/* Backing store for AML_TX section. Must cover sizeof(struct drv_txdesc) *
 * DRV_TXDESC_NUM (224 * 1280 = 286720 with SYSTEM64). The old 246272 was
 * smaller than that request, so w1_aml_mem_prealloc() returned NULL. 512K is
 * exactly what get_order() rounds this allocation up to anyway. */
#define TX_DESC_BUF_LEN  (512 * 1024)

void *w1_aml_mem_prealloc(int section, unsigned long size)
{
    switch (section) {
        case AML_RX:
            if (size > RX_BUF_LEN)
                return NULL;

            return wlan_preallocated_rx_buf;
        case AML_TX:
            if (size > TX_DESC_BUF_LEN)
                return NULL;

            return wlan_preallocated_tx_desc_buf;
        default:
                return NULL;
    }
}
EXPORT_SYMBOL(w1_aml_mem_prealloc);

static int aml_init_wlan_mem(void)
{
    
    wlan_preallocated_rx_buf = (void *)__get_free_pages(
        GFP_KERNEL | GFP_DMA | __GFP_ZERO, get_order(RX_BUF_LEN));
    if (!wlan_preallocated_rx_buf)
        return -ENOMEM;

    wlan_preallocated_tx_desc_buf = (void *)__get_free_pages(
        GFP_KERNEL | GFP_DMA | __GFP_ZERO, get_order(TX_DESC_BUF_LEN));
    if (!wlan_preallocated_tx_desc_buf) {
        free_pages((unsigned long)wlan_preallocated_rx_buf, get_order(RX_BUF_LEN));
        wlan_preallocated_rx_buf = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void aml_deinit_wlan_mem(void)
{
    if (wlan_preallocated_rx_buf) {
        free_pages((unsigned long)wlan_preallocated_rx_buf, get_order(RX_BUF_LEN));
        wlan_preallocated_rx_buf = NULL;
    }
    if (wlan_preallocated_tx_desc_buf) {
        free_pages((unsigned long)wlan_preallocated_tx_desc_buf, get_order(TX_DESC_BUF_LEN));
        wlan_preallocated_tx_desc_buf = NULL;
    }
}
#endif

int  w1_aml_w1_sdio_init(void)
{
    int err = 0;
    int i;

    for (i = 0; i < ARRAY_SIZE(sdio_dma_buf); i++) {
        sdio_dma_buf[i] = aml_w1_sdio_dma_alloc(SDIO_DMA_BUF_SIZE);
        if (!sdio_dma_buf[i]) {
            err = -ENOMEM;
            goto err_out;
        }
    }

    w1_func6_rx_bounce_buf_size = W1_FUNC6_RX_BOUNCE_SIZE;
    w1_func6_rx_bounce_buf = aml_w1_sdio_dma_alloc(w1_func6_rx_bounce_buf_size);
    if (w1_func6_rx_bounce_buf) {
        pr_info("W522A: func6-7-bounce-64k: FUNC6/7 RX fullbounce armed at %p size=%zu page_off=0x%lx\n",
                w1_func6_rx_bounce_buf, w1_func6_rx_bounce_buf_size,
                (unsigned long)w1_func6_rx_bounce_buf & (PAGE_SIZE - 1));
    } else {
        pr_warn("W522A: func6-7-bounce-64k: persistent FUNC6/7 RX bounce alloc failed, using per-call fallback\n");
        w1_func6_rx_bounce_buf_size = 0;
    }

    err = sdio_register_driver(&aml_w1_sdio_driver);
    if (err) {
        
        pr_err("failed to register sdio driver: %d \n", err);
        goto err_out;
    }
    w1_w1_sdio_driver_insmoded = 1;
    w1_wifi_in_insmod = 0;
    w1_wifi_in_rmmod = 0;
    PRINT("*****************aml sdio common driver is insmoded********************\n");

    return 0;

err_out:
    if (w1_func6_rx_bounce_buf) {
        aml_w1_sdio_dma_free(w1_func6_rx_bounce_buf, w1_func6_rx_bounce_buf_size);
        w1_func6_rx_bounce_buf = NULL;
        w1_func6_rx_bounce_buf_size = 0;
    }
    for (i = 0; i < ARRAY_SIZE(sdio_dma_buf); i++) {
        aml_w1_sdio_dma_free(sdio_dma_buf[i], SDIO_DMA_BUF_SIZE);
        sdio_dma_buf[i] = NULL;
    }

    return err;
}
EXPORT_SYMBOL(w1_aml_w1_sdio_init);

void  w1_aml_w1_sdio_exit(void)
{
    int i = 0;
    PRINT("w1_aml_w1_sdio_exit++ \n");
    if (w1_w1_sdio_driver_insmoded) {
        sdio_unregister_driver(&aml_w1_sdio_driver);
        w1_w1_sdio_driver_insmoded = 0;
    }
    w1_w1_sdio_after_porbe = 0;
    PRINT("*****************aml sdio common driver is rmmoded********************\n");
    for (i = 0; i < ARRAY_SIZE(sdio_dma_buf); i++) {
        aml_w1_sdio_dma_free(sdio_dma_buf[i], SDIO_DMA_BUF_SIZE);
        sdio_dma_buf[i] = NULL;
    }
    if (w1_func6_rx_bounce_buf) {
        pr_info("W522A: func6-7-bounce-64k: FUNC6/7 RX stats at exit: bounce=%d (persistent=%d fallback=%d)\n",
                atomic_read(&aml_w1_rx_bounce_hits),
                atomic_read(&aml_w1_rx_bounce_persistent),
                atomic_read(&aml_w1_rx_bounce_fallback));
        aml_w1_sdio_dma_free(w1_func6_rx_bounce_buf, w1_func6_rx_bounce_buf_size);
        w1_func6_rx_bounce_buf = NULL;
        w1_func6_rx_bounce_buf_size = 0;
    }
}

EXPORT_SYMBOL(w1_w1_sdio_driver_insmoded);
EXPORT_SYMBOL(w1_wifi_in_insmod);
EXPORT_SYMBOL(w1_wifi_in_rmmod);
EXPORT_SYMBOL(w1_w1_sdio_after_porbe);
EXPORT_SYMBOL(w1_host_wake_w1_req);
EXPORT_SYMBOL(w1_host_suspend_req);
EXPORT_SYMBOL(w1_host_resume_req);
EXPORT_SYMBOL(w1_wifi_sdio_access);
EXPORT_SYMBOL(w1_wifi_irq_enable);
EXPORT_SYMBOL(w1_wifi_hal_probe_done);

EXPORT_SYMBOL(w1_aml_wifi_sdio_power_lock);
EXPORT_SYMBOL(w1_aml_wifi_sdio_power_unlock);

void w1_set_wifi_bt_sdio_driver_bit(bool is_register, int shift)
{
    AML_W1_BT_WIFI_MUTEX_ON();
    if (is_register) {
        w1_sdio_wifi_bt_alive |= (1 << shift);
        PRINT("Insmod %s sdio driver!\n", (shift ? "WiFi":"BT"));
    } else {
        PRINT("Rmmod %s sdio driver!\n", (shift ? "WiFi":"BT"));
        w1_sdio_wifi_bt_alive &= ~(1 << shift);
        if (!w1_sdio_wifi_bt_alive) {
            w1_aml_w1_sdio_exit();
        }
    }
    AML_W1_BT_WIFI_MUTEX_OFF();
}
EXPORT_SYMBOL(w1_set_wifi_bt_sdio_driver_bit);
EXPORT_SYMBOL(w1_g_w1_hwif_sdio);
EXPORT_SYMBOL(w1_g_w1_hif_ops);

static int aml_w1_sdio_insmod(void)
{
#ifdef NOT_AMLOGIC_PLATFORM
    int ret;
    ret = aml_init_wlan_mem();
    if (ret) {
        PRINT("aml_init_wlan_mem err: %d \n", ret);
        return -ENOMEM;
    }
#endif
    
    {
        int err = w1_aml_w1_sdio_init();
        if (err) {
            PRINT("w1_aml_w1_sdio_init failed: %d\n", err);
#ifdef NOT_AMLOGIC_PLATFORM
            aml_deinit_wlan_mem();
#endif
            return err;
        }
    }
    pr_debug("%s(%d) start...\n", __func__, __LINE__);
    return 0;
}

static void aml_w1_sdio_rmmod(void)
{
    w1_aml_w1_sdio_exit();
#ifdef NOT_AMLOGIC_PLATFORM
    aml_deinit_wlan_mem();
#endif
}

module_init(aml_w1_sdio_insmod);
module_exit(aml_w1_sdio_rmmod);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amlogic / Fn-Link");
MODULE_DESCRIPTION("Amlogic W155S1 / Fn-Link K255B-SR SDIO bus driver");

MODULE_DEVICE_TABLE(sdio, aml_w1_sdio);
