#include "w1_sdio.h"
#include <linux/gfp.h>
#include <linux/irqflags.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/rwsem.h>
#include <linux/vmalloc.h>

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
/* Accessed from multiple contexts (probe, insmod, pm): use volatile to
 * prevent the compiler from caching reads. For single-byte types on arm64
 * this is sufficient — byte loads/stores are atomic at the hardware level. */
volatile unsigned char w1_wifi_hal_probe_done = 0;
unsigned int  shutdown_i = 0;
#define  I2C_CLK_QTR   0x4

static DEFINE_MUTEX(wifi_bt_sdio_mutex);

#define AML_W1_BT_WIFI_MUTEX_ON() do {\
    mutex_lock(&wifi_bt_sdio_mutex);\
} while (0)

#define AML_W1_BT_WIFI_MUTEX_OFF() do {\
    mutex_unlock(&wifi_bt_sdio_mutex);\
} while (0)

/* v15g SDIO RX/TX FAIRNESS (tightened from v15f):
 *
 * Under sustained AP-RX load (phone uploading), aml_w1_sdio_bottom_read() can
 * be called dozens of times per millisecond by the RX path while TX cmd53
 * writes (TCP-ACKs back to phone, beacons, 802.11 ACKs) sit waiting on the
 * same wifi_bt_sdio_mutex. Even though the kernel mutex is fair, rx_thread
 * re-acquires the lock so quickly that TX side observes long stretches of
 * starvation in practice.
 *
 * v15f used SDIO_RD_YIELD_BURST=16, usleep_range(50,100) and only yielded
 * on the read side. On single-CPU Amlogic boxes this still left TX starved:
 * 16 reads at FUNC4_BLKSIZE=512 = ~8KB monopolised the mutex for ~800us at
 * a time, and a 50us yield window is barely larger than one TX cmd53 round-
 * trip (claim_host + memcpy + transfer + release ~= 150-250us), so the
 * waking TX-side often raced and lost.
 *
 * v15g tunes the parameters and adds a symmetric write-side yield so that
 * neither direction can monopolise the bus indefinitely:
 *
 *   - SDIO_RD_YIELD_BURST: 16 -> 4   (RX gives up the mutex 4x more often)
 *   - usleep_range:        50-100us -> 200-400us (large enough that a
 *                          waiting TX cmd53 actually fits inside it)
 *   - sdio_burst_after_write() now also yields after SDIO_WR_YIELD_BURST
 *     consecutive writes, so a burst of mgmt/ACK TX cannot starve RX
 *     processing of incoming data either.
 *
 * The burst counters are independent. Any RX call resets the write counter
 * and vice versa, so the yield only kicks in during single-direction bursts
 * and has near-zero cost in mixed traffic. */
#define SDIO_RD_YIELD_BURST  4
#define SDIO_WR_YIELD_BURST  8
static atomic_t sdio_rd_burst = ATOMIC_INIT(0);
static atomic_t sdio_wr_burst = ATOMIC_INIT(0);

static inline void sdio_burst_after_read(void)
{
    /* RX served -- reset write burst counter so TX isn't penalised
     * for the next write after RX has already had its window. */
    atomic_set(&sdio_wr_burst, 0);
    if (atomic_inc_return(&sdio_rd_burst) >= SDIO_RD_YIELD_BURST) {
        atomic_set(&sdio_rd_burst, 0);
        usleep_range(200, 400);
    }
}

static inline void sdio_burst_after_write(void)
{
    /* TX served -- reset read burst counter symmetrically. */
    atomic_set(&sdio_rd_burst, 0);
    if (atomic_inc_return(&sdio_wr_burst) >= SDIO_WR_YIELD_BURST) {
        atomic_set(&sdio_wr_burst, 0);
        usleep_range(200, 400);
    }
}

/* v15c: power-state lock is rwsem so cmd53 callers (read-side) don't
 * serialize on each other. */
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
        pr_warn_ratelimited("v17m-sdio-guard: %s func%d stale func=%p card=%p host=%p access=%u rmmod=%u\n",
                            where, func_num, func,
                            func ? func->card : NULL,
                            (func && func->card) ? func->card->host : NULL,
                            READ_ONCE(w1_wifi_sdio_access),
                            READ_ONCE(w1_wifi_in_rmmod));
        return false;
    }

    if (unlikely(func_num != SDIO_FUNC0 && func->num != func_num)) {
        pr_warn_ratelimited("v17m-sdio-guard: %s func mismatch requested=%u actual=%u\n",
                            where, func_num, func->num);
        return false;
    }

    if (unlikely(READ_ONCE(w1_wifi_in_rmmod))) {
        pr_warn_ratelimited("v17m-sdio-guard: %s func%d rejected during rmmod\n",
                            where, func_num);
        return false;
    }

    return true;
}

static bool aml_w1_sdio_can_sleep_io(const char *where, unsigned char func_num)
{
    if (unlikely(irqs_disabled())) {
        pr_err_ratelimited("v17m-sdio-guard: %s func%d called with IRQs disabled; dropping SDIO transaction\n",
                           where, func_num);
        return false;
    }

    if (unlikely(in_atomic())) {
        pr_err_ratelimited("v17m-sdio-guard: %s func%d called from atomic context; dropping SDIO transaction\n",
                           where, func_num);
        return false;
    }

    return true;
}

static void aml_w1_sdio_release_host_safe(struct sdio_func *func,
                                          unsigned char func_num,
                                          const char *where)
{
    /*
     * Release must be weaker than aml_w1_sdio_func_live(): an in-flight SDIO
     * transaction may have claimed the host just before remove set
     * w1_wifi_in_rmmod=1. New transactions are rejected by func_live(), but an
     * already claimed host still has to be released or the MMC core can wedge.
     */
    if (likely(func && func->card && func->card->host &&
               (func_num == SDIO_FUNC0 || func->num == func_num))) {
        sdio_release_host(func);
        return;
    }

    pr_err_ratelimited("v17m-sdio-guard: %s func%d skip sdio_release_host on stale SDIO function\n",
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

static unsigned int aml_w1_bt_hi_read_word(unsigned int addr)
{
    unsigned int regdata = 0;
    unsigned int reg_tmp;
    /*
     * make sure function 5 section address-mapping feature is disabled,
     * when this feature is disabled,
     * all 128k space in one sdio-function use only
     * one address-mapping: 32-bit AHB Address = BaseAddr + cmdRegAddr
     */

    reg_tmp = w1_g_w1_hif_ops.hi_read_word( RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23))) {
        reg_tmp |= BIT(23);
        w1_g_w1_hif_ops.hi_write_word( RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }

    /*config msb 15 bit address in BaseAddr Register*/
    w1_g_w1_hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);
    w1_g_w1_hif_ops.bt_hi_read_sram((unsigned char*)(SYS_TYPE)&regdata,
        /*sdio cmd 52/53 can only take 17 bit address*/
        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));

    return regdata;
}

static void aml_w1_bt_hi_write_word(unsigned int addr,unsigned int data)
{
    unsigned int reg_tmp;
    /*
     * make sure function 5 section address-mapping feature is disabled,
     * when this feature is disabled,
     * all 128k space in one sdio-function use only
     * one address-mapping: 32-bit AHB Address = BaseAddr + cmdRegAddr
     */
    reg_tmp = w1_g_w1_hif_ops.hi_read_word( RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23))) {
        reg_tmp |= BIT(23);
        w1_g_w1_hif_ops.hi_write_word( RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }
    /*config msb 15 bit address in BaseAddr Register*/
    w1_g_w1_hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);

    w1_g_w1_hif_ops.bt_hi_write_sram((unsigned char *)&data,
        /*sdio cmd 52/53 can only take 17 bit address*/
        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));
}

static void aml_w1_bt_sdio_read_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    w1_g_w1_hif_ops.hi_bottom_read(SDIO_FUNC5, ((SYS_TYPE)addr & SDIO_ADDR_MASK),
        buf, len, (len > 8 ? SDIO_OPMODE_INCREMENT : SDIO_OPMODE_FIXED));
}

/*For BT use only start */
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

/* aon module address from 0x00f00000, we need read/write by sdio func5 */
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
    ktime_get_real_ts64(&before); /* KP-4 FIX */
#endif /* End of DBG_PRINT_COST_TIME */

    ASSERT(func != NULL);
    ASSERT(byte != NULL);
    ASSERT(func->num == func_num);

    if (!aml_w1_sdio_can_sleep_io(__func__, func_num) ||
        !aml_w1_sdio_func_live(func, func_num, __func__))
        return SDIOH_API_RC_FAIL;

    AML_W1_BT_WIFI_MUTEX_ON();

    /* Claim host controller */
    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        return SDIOH_API_RC_FAIL;
    }
    sdio_claim_host(func);
    kmalloc_buf = sdio_dma_buf[func_num];
    memcpy(kmalloc_buf, byte, len);

    if (write) {
        /* CMD52 Write */
        sdio_writeb(func, *kmalloc_buf, reg_addr, &err_ret);
    }
    else {
        /* CMD52 Read */
        *byte = sdio_readb(func, reg_addr, &err_ret);
    }

    /* Release host controller */
    aml_w1_sdio_release_host_safe(func, func_num, __func__);

#if defined(DBG_PRINT_COST_TIME)
    ktime_get_real_ts64(&now); /* KP-4 FIX */

    pr_debug("[sdio byte]: len=1 cost=%lds %luus\n",
        now.tv_sec-before.tv_sec, now.tv_nsec/1000 - before.tv_nsec/1000);
#endif /* End of DBG_PRINT_COST_TIME */

    AML_W1_BT_WIFI_MUTEX_OFF();
    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

static size_t aml_w1_sdio_align_size(struct sdio_func *func, unsigned int nbytes,
                                     unsigned char write, unsigned int fix_incr)
{
    bool fifo = (fix_incr == SDIO_OPMODE_FIXED);

    if (write && !fifo) {
        /* write, increment */
        return sdio_align_size(func, nbytes);
    } else if (write) {
        /* write, fifo */
        return nbytes;
    } else if (fifo) {
        /* read */
        return nbytes;
    } else {
        /* read */
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
        /* write, increment */
        align_nbytes = sdio_align_size(func, nbytes);
        err_ret = sdio_memcpy_toio(func, addr, buf, align_nbytes);
    }
    else if (write)
    {
        /* write, fifo */
        err_ret = sdio_writesb(func, addr, buf, align_nbytes);
    }
    else if (fifo)
    {
        /* read */
        err_ret = sdio_readsb(func, buf, addr, align_nbytes);
    }
    else
    {
        /* read */
        align_nbytes = sdio_align_size(func, nbytes);
        err_ret = sdio_memcpy_fromio(func, buf, addr, align_nbytes);
    }

    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

unsigned char aml_w1_sdio_bottom_read8(unsigned char  func_num, int addr);

/* v15v RX-BOUNCE-FIX accounting: how many times the FUNC6 RX hot path had to
 * route through the page-aligned bounce buffer because the caller-supplied
 * ring-buffer pointer was not page-aligned, and how many of those landed on
 * the persistent bounce vs the per-call __get_free_pages() fallback.
 *
 * Surfaced via pr_info_ratelimited inside aml_w1_sdio_bottom_read() so the
 * user can see in dmesg that the fix is active without flooding the log. */
static atomic_t aml_w1_rx_bounce_hits;
static atomic_t aml_w1_rx_bounce_persistent;
static atomic_t aml_w1_rx_bounce_fallback;
/*
 * v16u FUNC6-7-BOUNCE-DIAG:
 *
 * v16t added FUNC7 to aml_w1_sdio_is_rx_bounce_func() so func7
 * RX cmd53 transactions also pass through the same persistent
 * 64K bounce buffer (DMA-coherent page-aligned region). But
 * because the variable name "w1_func6_rx_bounce_buf" and the
 * log tag "v16t-func6-7-bounce-64k" did not distinguish the
 * two funcs, there was no way to *confirm from dmesg* that
 * func7 RX traffic actually exercises the bounce path. The
 * user-reported "transactions switch to mmc0:0000:7 and the
 * bounce mechanism is ignored" hypothesis stayed un-disprovable.
 *
 * Add per-func atomic hit counters. The total aml_w1_rx_bounce_hits
 * is kept for backward log compatibility but the per-func counters
 * give a smoking-gun signal in dmesg: if aml_w1_rx_bounce_hits_f7
 * stays 0 while data is moving, the bounce really is being
 * bypassed for FUNC7 (and we have a real bug to chase). If
 * both counters increment, v16t is doing the right thing.
 */
static atomic_t aml_w1_rx_bounce_hits_f6;
static atomic_t aml_w1_rx_bounce_hits_f7;

//cmd53
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
            /*SDIO_FUNC5 ignore*/
            ERROR_DEBUG_OUT("SDIO_FUNC5, func num %d, addr 0x%08x\n", func_num, addr);

        } else if (func_num == SDIO_FUNC1) {
            /*SDIO_FUNC1 ignore*/
            ERROR_DEBUG_OUT("SDIO_FUNC1, func num %d, addr 0x%08x\n", func_num, addr);

        }  else if ((func_num == SDIO_FUNC2) && (addr == 0x00005080)) {
            /*SDIO_FUNC2 && 0x00005080 ignore*/
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
            ERROR_DEBUG_OUT("aml_w1_sdio_bottom_read, host wake w1 fail\n");
            return -1;
        }
    }
    /* v13 PERF/NOISE FIX: drop the per-cmd53 fw_st sanity probe.
     *
     * Before this fix, every cmd53 read first issued an extra cmd52 to
     * SDIO_FUNC1 reg 0x23c just to sanity-check the firmware-active flag
     * (fw_st == 6). Two problems:
     *   1. During calibration / FW download fw_st is legitimately != 6,
     *      so this fired pr_err("BUG! fw_st 8") hundreds of times per boot,
     *      flooding dmesg.
     *   2. On the steady-state RX hot path the check is dead weight: it adds
     *      an extra cmd52 + sdio_claim_host/release per data transaction.
     *      Real FW failures are caught by the SDIO host returning an error
     *      from the cmd53 itself a few lines below. */
    AML_W1_BT_WIFI_MUTEX_ON();

    if (!aml_w1_sdio_func_live(func, func_num, __func__)) {
        AML_W1_BT_WIFI_MUTEX_OFF();
        w1_aml_wifi_sdio_io_unlock();
        return SDIOH_API_RC_FAIL;
    }
    sdio_claim_host(func);

    /* read block mode */
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

        /*
         * v16u FUNC6-7-BOUNCE-DIAG: per-func counter so dmesg can
         * confirm/refute the hypothesis that FUNC7 RX traffic bypasses
         * the bounce path. First time we see FUNC7 through here, emit
         * a one-shot info line so the boot log carries proof that
         * v16t's helper actually routes FUNC7 reads through the
         * 64K page-aligned DMA bounce.
         */
        if (func_num == SDIO_FUNC6)
            atomic_inc(&aml_w1_rx_bounce_hits_f6);
        else if (func_num == SDIO_FUNC7) {
            if (atomic_inc_return(&aml_w1_rx_bounce_hits_f7) == 1) {
                pr_info("v16u-func6-7-bounce-diag: FIRST FUNC7 RX cmd53 bounced "
                        "addr=0x%08x len=%zu align_len=%d "
                        "(persistent_buf=%p persistent_size=%zu) — "
                        "v16t bounce path is engaged for FUNC7\n",
                        addr, len, align_len,
                        w1_func6_rx_bounce_buf,
                        w1_func6_rx_bounce_buf_size);
            }
        }

        if ((atomic_read(&aml_w1_rx_bounce_hits) & 0xFFF) == 0) {
            pr_info("v16u-func6-7-bounce-64k: FUNC6/7 RX fullbounce hits=%d "
                    "(f6=%d f7=%d persistent=%d fallback=%d)\n",
                    atomic_read(&aml_w1_rx_bounce_hits),
                    atomic_read(&aml_w1_rx_bounce_hits_f6),
                    atomic_read(&aml_w1_rx_bounce_hits_f7),
                    atomic_read(&aml_w1_rx_bounce_persistent),
                    atomic_read(&aml_w1_rx_bounce_fallback));
        }
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
        memcpy(buf, kmalloc_buf, align_len);
        if (need_free_buf)
            aml_w1_sdio_dma_free(kmalloc_buf, align_len);
    }

    aml_w1_sdio_release_host_safe(func, func_num, __func__);

    AML_W1_BT_WIFI_MUTEX_OFF();
    w1_aml_wifi_sdio_io_unlock();
    /* v15g: yield briefly after a burst of pure reads so TX cmd53 can grab
     * the mutex; see SDIO_RD_YIELD_BURST comment at top of file. */
    sdio_burst_after_read();
    return result;
}


//cmd53
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
            /*SDIO_FUNC5 ignore*/
            ERROR_DEBUG_OUT("SDIO_FUNC5, func num %d, addr 0x%08x\n", func_num, addr);

        } else if (func_num == SDIO_FUNC1) {
            /*SDIO_FUNC1 ignore*/
            ERROR_DEBUG_OUT("SDIO_FUNC1, func num %d, addr 0x%08x\n", func_num, addr);

        }  else if ((func_num == SDIO_FUNC2) && (addr == 0x00005080)) {
            /*SDIO_FUNC2 && 0x00005080 ignore*/
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
            ERROR_DEBUG_OUT("aml_w1_sdio_bottom_write, host wake w1 fail\n");
            return -1;
        }
    }

    /* v13 PERF/NOISE FIX: see matching comment in aml_w1_sdio_bottom_read. */

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
    /* v15g: TX served; reset RX burst counter and yield after a write burst. */
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

// for bt access always on reg
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
    // for bt access always on reg
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

//cmd52
static int aml_w1_sdio_bottom_write8(unsigned char  func_num, int addr, unsigned char data)
{
    int ret = 0;

    ASSERT(func_num != SDIO_FUNC0);
    ret =  _aml_w1_sdio_request_byte(func_num, SDIO_WRITE, addr, &data);

    return ret;
}

//cmd52
unsigned char aml_w1_sdio_bottom_read8(unsigned char  func_num, int addr)
{
    unsigned char sramdata;

    _aml_w1_sdio_request_byte(func_num, SDIO_READ, addr, &sramdata);
    return sramdata;
}

/* cmd52
   buff[7:0],     // funcIoNum
   buff[25:8],    // regAddr
   buff[39:32]);  // regValue
*/
static void aml_w1_sdio_bottom_write8_func0(unsigned long sram_addr, unsigned char sramdata)
{
    _aml_w1_sdio_request_byte(SDIO_FUNC0, SDIO_WRITE, sram_addr, &sramdata);
}

//cmd52
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
#endif /* End of HAL_SIM_VER */
}

static int aml_w1_sdio_suspend(unsigned int suspend_enable)
{
    mmc_pm_flag_t flags;
    struct sdio_func *func = NULL;
    int ret = 0, i;

    if (suspend_enable == 0)
    {
        /* when enable == 0 that's resume operation
        and we do nothing for resume now. */
        return ret;
    }

    /* just clear sdio clock value for emmc init when resume */
    //amlwifi_set_sdio_host_clk(0);

    AML_W1_BT_WIFI_MUTEX_ON();
    /* we shall suspend all card for sdio. */
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

        /*
         * if we don't use sdio irq, we can't get functions' capability with
         * MMC_PM_WAKE_SDIO_IRQ, so we don't need set irq for wake up
         * sdio for upcoming suspend.
         */
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
    unsigned long sramdata;

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
    else if (scat_req->scat_count != 0) // get scat_req, but not build scatter list
    {
        scat_req = NULL;
    }

    return scat_req;
}

static int amlw_w1_sdio_alloc_prep_scat_req(struct amlw1_hwif_sdio *hif_sdio)
{
    struct amlw_hif_scatter_req * scat_req = NULL;

    ASSERT(hif_sdio != NULL);

    /* allocate the scatter request */
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
    /* v15v: surface the bounce-buf state at insmod so the user can see in
     * dmesg whether the RX-BOUNCE-FIX is armed.  Both vmac/wifi_sdio.c TX
     * bounce (FUNC4) and w1_sdio/w1_sdio.c RX bounce (FUNC6) share this
     * page-aligned region. */
    pr_info("v15y aml_sdio: bounce_buf=%p size=%u align=0x%lx (RX-BOUNCE-FIX armed)\n",
            hif_sdio->bounce_buf, hif_sdio->bounce_buf_size,
            (unsigned long)hif_sdio->bounce_buf & (PAGE_SIZE - 1));
#else
    pr_warn("v15y aml_sdio: NOT_AMLOGIC_PLATFORM not defined, RX-BOUNCE-FIX disabled\n");
#endif

    /* reset RX bounce-hit counters every fresh scatter init */
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

    // TODO : getting hw_config to configure scatter number;

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
    mmc_cmd.arg   |= 1 << 27;	/* block basic */
    mmc_cmd.arg   |= 0 << 26;	/* 1 << 26;*/   	/*0 fix address */
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

    /* empty the free list */
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

    //ops->hif_get_sts = hif_get_sts;
    //ops->hif_pt_rx_start = hif_pt_rx_start;
    //ops->hif_pt_rx_stop = hif_pt_rx_stop;
    w1_w1_sdio_after_porbe = 1;

    // check and wake w1 firstly.
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

    /* v16c (B18): sdio_set_block_size() can fail under host controller
     * saturation or unsupported block size. Pre-v16c the return value was
     * silently ignored. If the call fails the func keeps the controller
     * default (often 16 B), causing every subsequent CMD53 to be split into
     * many short transfers — observed as ~3-5x throughput drop with no log
     * indicating why. Retry once with the standard SDIO 2.0 minimum (128 B)
     * which all hosts support, and warn loudly on failure so the user can
     * spot the cause in dmesg instead of guessing about a "slow Wi-Fi". */
    {
        int blksz_ret = sdio_set_block_size(func, 512);
        if (blksz_ret) {
            pr_warn("aml_w1_sdio: sdio_set_block_size(512) failed on func%d ret=%d, retrying 128\n",
                func->num, blksz_ret);
            blksz_ret = sdio_set_block_size(func, 128);
            if (blksz_ret) {
                pr_warn("aml_w1_sdio: sdio_set_block_size(128) also failed on func%d ret=%d; throughput will be DEGRADED\n",
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

    /* v15y: diagnostic so the user can see which path runs and whether the
     * reset block is entered at all. v15x logs used the PRINT macro which
     * maps to pr_debug — invisible in production. Use pr_info here. */
    pr_info("aml_w1_sdio_remove: enter func->num=%d FUNCNUM_SDIO_LAST=%d\n",
            func->num, FUNCNUM_SDIO_LAST);

    sdio_claim_host(func);

    /* v15y: Problem #1 fix — take 3.
     *
     * v15v had no reset on unload. Next modprobe hit:
     *   mmc0: tuning execution failed: -5
     *   Host HAL: write ICCM ERROR!!!
     *   drv_dev_probe LINE 2566 : init hal error
     * and only a physical reboot recovered.
     *
     * v15w tried CCCR RES via sdio_f0_writeb(func, val, SDIO_CCCR_ABORT, ...).
     * The kernel API returned -22 EINVAL on every rmmod — sdio_f0_writeb()
     * refuses any address below 0xF0 unless MMC_QUIRK_LENIENT_FN0 is set.
     *
     * v15x added LENIENT_FN0 quirk before the write. The write went through
     * at the API layer, but on the W155S1 / meson-gx-mmc combo the chip
     * stayed in a state where modprobe vlsicomm came back with -ENODEV
     * (see w522a-v15x-analysis.md section 4.2). Either the chip ignores the
     * CCCR RES bit or the host controller's tuned state is incompatible
     * with the post-reset card state.
     *
     * v15y: use mmc_hw_reset(func->card) — exported by mmc/core/core.c.
     * For SDIO this invokes mmc_sdio_hw_reset() which power-cycles the
     * card, re-tunes the host and re-enumerates SDIO functions. This is
     * what brcmfmac (drivers/net/wireless/broadcom/.../sdio.c:4168),
     * mwifiex (drivers/net/wireless/marvell/mwifiex/sdio.c:2693) and
     * ath10k (drivers/net/wireless/ath/ath10k/sdio.c:1637) all do, with
     * the exact same sdio_claim_host / mmc_hw_reset / sdio_release_host
     * pattern. mmc_hw_reset is much more aggressive than CCCR RES and
     * leaves the host+card pair in a known-good post-power-on state ready
     * for the next modprobe.
     *
     * On rare hosts that don't implement bus_ops->hw_reset, fall back to
     * the v15x CCCR RES path so we still get something. */
    if (func->num == FUNCNUM_SDIO_LAST && func->card) {
        ret = mmc_hw_reset(func->card);
        if (ret == 0) {
            pr_info("aml_w1_sdio_remove: mmc_hw_reset OK (synchronous, single-func path) — chip ready for next modprobe\n");
        } else if (ret == 1) {
            /* v15z: ret=1 documented — mmc/core/sdio.c::mmc_sdio_hw_reset()
             * returns 1 when sdio_funcs_probed > 1 (always true for the
             * W155S1's seven SDIO functions). The core marks the card as
             * removed and schedules a host rescan; the chip is power-cycled
             * via the rescan path. This is the expected normal-path result
             * for a multi-function SDIO card, not an error. */
            pr_info("aml_w1_sdio_remove: mmc_hw_reset scheduled async rescan (ret=1; sdio_funcs_probed>1) — expected for W155S1\n");
        } else {
            pr_warn("aml_w1_sdio_remove: mmc_hw_reset failed: %d — falling back to CCCR RES\n",
                    ret);

            /* Fallback: CCCR RES with LENIENT_FN0 quirk (the v15x path). */
            {
                u32 prev_quirks = func->card->quirks;

                func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

                abort = sdio_f0_readb(func, SDIO_CCCR_ABORT, &err);
                if (err)
                    abort = 0x00;
                abort |= 0x08; /* RES — software reset of I/O portion */
                sdio_f0_writeb(func, abort, SDIO_CCCR_ABORT, &err);
                if (err)
                    pr_warn("aml_w1_sdio_remove: fallback CCCR RES write failed: %d\n", err);
                else
                    pr_info("aml_w1_sdio_remove: fallback CCCR RES issued (bit 0x08 at offset 0x06)\n");

                func->card->quirks = prev_quirks;
            }
        }
        /* Give the card a few ms to settle before the next CMD52 from
         * whoever re-probes it (modprobe path). */
        msleep(10);
    }

    sdio_disable_func(func);
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

    // Set the I2C bus to 100khz
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    // Set the I2C Address
    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    // Fill the token registers
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp = (I2C_END  << 16)             |
            (I2C_DATA << 12)             |    // Write Data
            (I2C_DATA << 8)              |    // Write RegAddr
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    // Fill the write data registers
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,(Data << 8) | (RegAddr << 0));
    // Start and Wait
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
    //struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    // Set the I2C bus to 100khz
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    // Set the I2C Address
    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    // Fill the token registers
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END  << 24)             |
            (I2C_DATA_LAST << 20)        |  // this is a data read
            (I2C_SLAVE_ADDR_READ << 16)  |
            (I2C_START << 12)            |
            (I2C_DATA << 8)              |  // This is a data write
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    // Fill the write data registers
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,(RegAddr << 0));
    // Start and Wait
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


//void write_rdaddr_32ba(U8 Bus, U8 SlaveAddr, U32 TkData0, U32 TkData1)
static void write_word_32ba(unsigned char Bus, unsigned char SlaveAddr,
    unsigned int StartToken, unsigned int Data)
{
    //struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    //pr_debug("%s(%d) token 0x%x data 0x%x\n", __func__, __LINE__, StartToken,Data);

    // Set the I2C bus to 100khz
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    // Set the I2C Address
    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    // Fill the token registers
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END << 28)              |    //
            (I2C_DATA << 24)             |    //
            (I2C_DATA << 20)             |    //
            (I2C_DATA << 16)             |    //
            (I2C_DATA << 12)             |    //
            (I2C_DATA << 8)              |    //  This shall be related to start token (token = 0x04 for receiving paddr,  token = 0x00 for receiving pwdata)
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    //Fill the write data registers
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,StartToken | (Data<<8));
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG1,(Data >> 24));
    //Start and Wait
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
    //struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    // Set the I2C bus to 100khz
    tmp = aml_w1_sdio_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    aml_w1_sdio_write_word(I2C_CONTROL_REG, tmp);

    // Set the I2C Address
    aml_w1_sdio_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    // Fill the token registers
    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG0);
    tmp =    (I2C_DATA  << 28)            |
             (I2C_DATA  << 24)            |
             (I2C_DATA  << 20)            |
             (I2C_SLAVE_ADDR_READ  << 16) |  // the second burst is for READ
             (I2C_START << 12)            |  // after addr pointer has been reset to 0x00, we start a new burst for reading data out
             (I2C_DATA  << 8)             |  // This shall be related a 0x00 written into rf i2c in order to reset addr pointer to 0x00
             (I2C_SLAVE_ADDR_WRITE << 4)  |  // the first burst is for WRITE token 0x00 to reset addr pointer
             (I2C_START << 0);               // start the first burst
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG0, tmp);

    tmp = aml_w1_sdio_read_word(I2C_TOKEN_LIST_REG1);
    tmp = (I2C_END       << 4) | (I2C_DATA_LAST << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_LIST_REG1, tmp);

    // Fill the write data registers
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG0,RegAddr << 0);
    aml_w1_sdio_write_word(I2C_TOKEN_WDATA_REG1,0);
    // Start and Wait
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

    /* 0x4-0x7 save address of read register in slave device*/
    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    /* 0x0-0x3 save address of read data in slave device*/
    read_data = read_word_32ba(slave_addr, 0x0);
    return read_data;
}

void rf_i2c_write(unsigned int reg_addr, unsigned int data)
{
    unsigned char bus = 0;
    unsigned int slave_addr = 0x7a;
    unsigned int start_token;

    /* 0x0-0x3 save address of write data in slave device*/
    start_token = 0x00;  //for data to be written in
    write_word_32ba(bus, slave_addr, 0x00, data);

    //system_wait(1);

    /* 0x4-0x7 save address of write register in slave device*/
    start_token = 0x04;  //for address to be written to
    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    write_byte_8ba(bus, slave_addr, 0x8, bus);
}

static void config_pmu_reg_off(void)
{
    RG_AON_A30_FIELD_T reg_aon30_data;
    RG_AON_A29_FIELD_T reg_aon29_data;

    unsigned int reg_val = 0;

    //reg_val = rf_read_register(RG_TOP_A2);
    reg_val = rf_i2c_read(RG_TOP_A2);
    reg_val = reg_val &(~0x1f);
    /*RF enter sx mode*/
    // reg_val = reg_val |(0x15 );

    /*RF enter sleep mode*/
    //reg_val = reg_val |(0x10 );

    /*2.4G sx mode*/
    reg_val |= 0x15;
    rf_i2c_write(RG_TOP_A2, reg_val);

    /*infor BT ,wifi not work*/
    reg_val  = aml_w1_sdio_read_word(RG_PMU_A16);
    reg_val &= (~BIT(31));
    aml_w1_sdio_write_word(RG_PMU_A16, reg_val);

    /*close all WIFI coexist thread*/
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

        aml_w1_sdio_write_word(RG_PMU_A12, 0x9ea2e); //set dpll_val(bit16) for sdio resp_timeout
        aml_w1_sdio_write_word(RG_PMU_A14, 0x1);
        aml_w1_sdio_write_word(RG_PMU_A16, 0x0);
        aml_w1_sdio_write_word(RG_PMU_A22, 0x707);
        aml_w1_sdio_write_word(RG_PMU_A18, 0x8700);
        aml_w1_sdio_write_word(RG_PMU_A20, 0x3ff01ff);
        aml_w1_sdio_write_word(RG_PMU_A17, 0x703);
        //aml_w1_sdio_write_word(RG_PMU_A24, 0x3f20000);

        reg_aon30_data.data = aml_w1_sdio_read_word(RG_AON_A30);
        /* change rf to idle mode */
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

        //force wifi pmu fsm to sleep mode
        host_req_status = (0x8 << 1)| BIT(0);
        aml_w1_sdio_bottom_write8(SDIO_FUNC1, 0x221, host_req_status);
    }
}

extern int wifi_irq_num(void);
static void aml_sdio_shutdown(struct device *device)
{
    pr_debug("===>>> enter %s <<<===\n", __func__);
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
        pr_debug("===>>> end <<<===\n");
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


/* Two defines below taken from aml_static_buf.c. */
#define AML_RX  11
#define AML_TX  20
/* This is value '(RX_FIFO_SIZE + 2 * FUNC6_BLKSIZE)'
 * from function 'hi_rx_fifo_init()' in wifi_hif.c.
 */
#define RX_BUF_LEN    459776

/* This is value of variable 'bsize' from function
 * 'wifi_mac_tx_init()' in wifi_mac_if.c. Note, it
 * depends on size of 'struct drv_txdesc', so we take
 * biggest size between 32 and 64 bits build.
 */
#define TX_DESC_BUF_LEN  246272

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
    /* DMA-FIX: vmalloc() produces virtually-mapped but physically non-contiguous
     * pages. meson_mmc performs real DMA through the scatter-gather list, so it
     * needs physically contiguous, cache-coherent memory. Using vmalloc() here
     * caused the "unaligned sg offset 3076" warning followed by a kernel Oops in
     * meson_mmc_irq_thread -> sg_copy_from_buffer -> flush_dcache_page because
     * the sg entry points into a vmalloc page with no valid DMA flush path.
     *
     * __get_free_pages(GFP_KERNEL | GFP_DMA | __GFP_ZERO, order) returns a
     * physically contiguous, page-aligned region — the same allocator already
     * used in aml_w1_sdio_dma_alloc() for per-transfer bounce buffers. */
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

    //amlwifi_set_sdio_host_clk(200000000);//200MHZ
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
        pr_info("v16t-func6-7-bounce-64k: FUNC6/7 RX fullbounce armed at %p size=%zu page_off=0x%lx\n",
                w1_func6_rx_bounce_buf, w1_func6_rx_bounce_buf_size,
                (unsigned long)w1_func6_rx_bounce_buf & (PAGE_SIZE - 1));
    } else {
        pr_warn("v16t-func6-7-bounce-64k: persistent FUNC6/7 RX bounce alloc failed, using per-call fallback\n");
        w1_func6_rx_bounce_buf_size = 0;
    }

    err = sdio_register_driver(&aml_w1_sdio_driver);
    if (err) {
        /* БАГ 1 fix: прапор встановлюємо ТІЛЬКИ після успішної реєстрації.
         * Оригінал ставив w1_w1_sdio_driver_insmoded=1 до перевірки err,
         * що призводило до sdio_unregister_driver() для незареєстрованого
         * драйвера при rmmod → kernel panic. */
        PRINT("failed to register sdio driver: %d \n", err);
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
        pr_info("v16t-func6-7-bounce-64k: FUNC6/7 RX stats at exit: bounce=%d (persistent=%d fallback=%d)\n",
                atomic_read(&aml_w1_rx_bounce_hits),
                atomic_read(&aml_w1_rx_bounce_persistent),
                atomic_read(&aml_w1_rx_bounce_fallback));
        aml_w1_sdio_dma_free(w1_func6_rx_bounce_buf, w1_func6_rx_bounce_buf_size);
        w1_func6_rx_bounce_buf = NULL;
        w1_func6_rx_bounce_buf_size = 0;
    }
}
//EXPORT_SYMBOL(w1_aml_w1_sdio_exit);

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
/*w1_set_wifi_bt_sdio_driver_bit() is used to determine whether to unregister sdio power driver.
  *Only when w1_sdio_wifi_bt_alive is 0, then call w1_aml_w1_sdio_exit().
*/
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
    /* БАГ 2 fix: перевіряємо return value w1_aml_w1_sdio_init().
     * Оригінал ігнорував помилку і завжди повертав 0 → insmod "успішний"
     * але WiFi не працює без жодного повідомлення. */
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

/* FIX: expose SDIO module-alias to udev so the kernel auto-loads
 * this module when the SDIO bus enumerates the chip (just like
 * mt7601u, ath10k_sdio etc do). Without this, the user has to
 * manually `modprobe aml_sdio` after every boot.
 */
MODULE_DEVICE_TABLE(sdio, aml_w1_sdio);
