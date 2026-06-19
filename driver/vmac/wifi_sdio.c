
#include "wifi_hal.h"
#include "wifi_hif.h"
#include "chip_intf_reg.h"
#include "chip_ana_reg.h"
#include <linux/mm.h>

#include "wifi_pt_init.h"
#include "wifi_pt_network.h"

struct amlw_hwif_sdio g_amlw_hwif_sdio;

static inline struct amlw_hwif_sdio *aml_sdio_priv(void)
{
#ifndef SDIO_BUILD_IN
    return &g_amlw_hwif_sdio;
#else
    return &w1_g_w1_hwif_sdio;
#endif
}

#include "wifi_mac_com.h"
#include <linux/gfp.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/card.h>
#include <linux/atomic.h>

#define HIF_SDIO_UNIT_MULTIBLKSZ
#define AML_SDIO_SCAT_BOUNCE_SIZE    (SDIO_MAX_BLK_CNT * FUNC4_BLKSIZE)

static void *aml_sdio_dma_alloc(size_t size)
{
    return (void *)__get_free_pages(GFP_KERNEL | GFP_DMA | __GFP_ZERO, get_order(size));
}

static void aml_sdio_dma_free(void *buf, size_t size)
{
    if (buf)
        free_pages((unsigned long)buf, get_order(size));
}

#ifdef    DRIVER_FOR_BT  
int sdioclk = 3000000;
#else
#ifdef RF_T9026
    
    int sdioclk = 200000000;
#else
    int sdioclk = 200000000;
#endif
#endif
module_param(sdioclk, int, 0644);
MODULE_PARM_DESC(sdioclk, "SDIO clock frequency in Hz (default/max: 200000000 for SDR104 test profile).");

#define AML_SDIO_CLK_SAFE_MAX 200000000

static inline bool aml_sdio_is_rx_bounce_func(unsigned char func_num)
{
    return func_num == SDIO_FUNC6 || func_num == SDIO_FUNC7;
}

struct sdio_func *aml_priv_to_func(int func_n)
{
    ASSERT(func_n >= 0 &&  func_n < SDIO_FUNCNUM_MAX);

#ifndef SDIO_BUILD_IN
    return g_amlw_hwif_sdio.sdio_func_if[func_n];
#else
    return w1_g_w1_hwif_sdio.sdio_func_if[func_n];
#endif
}

#ifdef NOT_AMLOGIC_PLATFORM
static int aml_sdio_cmd53_buf_rw(struct sdio_func *func, unsigned int addr,
    void *buf, unsigned int len, unsigned char write)
{
    struct mmc_request mmc_req;
    struct mmc_command mmc_cmd;
    struct mmc_data mmc_dat;
    struct scatterlist sg;
    unsigned int blkcnt;
    int ret = 0;

    if (!func || !func->card || !func->card->host || !buf || len == 0)
        return -EINVAL;
    if (func->cur_blksize == 0 || (len % func->cur_blksize) != 0)
        return -EINVAL;

    blkcnt = len / func->cur_blksize;
    if (blkcnt == 0 || blkcnt > SDIO_MAX_BLK_CNT)
        return -EINVAL;

    sg_init_one(&sg, buf, len);
    memset(&mmc_req, 0, sizeof(struct mmc_request));
    memset(&mmc_cmd, 0, sizeof(struct mmc_command));
    memset(&mmc_dat, 0, sizeof(struct mmc_data));

    mmc_dat.sg = &sg;
    mmc_dat.sg_len = 1;
    mmc_dat.blksz = func->cur_blksize;
    mmc_dat.blocks = blkcnt;
    mmc_dat.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;

    mmc_cmd.opcode = SD_IO_RW_EXTENDED;
    mmc_cmd.arg = write ? 1 << 31 : 0;
    mmc_cmd.arg |= (func->num & 0x7) << 28;
    mmc_cmd.arg |= 1 << 27;
    mmc_cmd.arg |= SDIO_OPMODE_FIXED << 26;
    mmc_cmd.arg |= (addr & 0x1ffff) << 9;
    mmc_cmd.arg |= mmc_dat.blocks & 0x1ff;
    mmc_cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;

    mmc_req.cmd = &mmc_cmd;
    mmc_req.data = &mmc_dat;

    sdio_claim_host(func);
    mmc_set_data_timeout(&mmc_dat, func->card);
    mmc_wait_for_req(func->card->host, &mmc_req);
    sdio_release_host(func);

    if (mmc_cmd.error || mmc_dat.error) {
        ERROR_DEBUG_OUT("ERROR CMD53 %s cmd_error=%d data_error=%d\n",
            write ? "write" : "read", mmc_cmd.error, mmc_dat.error);
        ret = mmc_cmd.error ? mmc_cmd.error : mmc_dat.error;
    }

    return ret;
}

static int aml_sdio_validate_scat_req(struct amlw_hif_scatter_req *scat_req,
    struct sdio_func *func, unsigned int max_req_size, unsigned int bounce_size,
    unsigned int *total_len)
{
    unsigned int total = 0;
    int i;

    if (!scat_req || !func || func->cur_blksize == 0 || !total_len)
        return -EINVAL;
    if (scat_req->scat_count <= 0 || scat_req->scat_count > SDIO_MAX_SG_ENTRIES)
        return -EINVAL;

    for (i = 0; i < scat_req->scat_count; i++) {
        unsigned int pkt_len = scat_req->scat_list[i].len;
        unsigned int aligned;

        if (pkt_len == 0 || !scat_req->scat_list[i].packet)
            return -EINVAL;
        aligned = ALIGN(pkt_len, func->cur_blksize);
        if (aligned < pkt_len || aligned > max_req_size)
            return -EMSGSIZE;
        if (total > bounce_size || aligned > bounce_size - total)
            return -EMSGSIZE;
        total += aligned;
    }

    *total_len = total;
    return 0;
}
#endif

#ifndef SDIO_BUILD_IN
static int _aml_sdio_request_byte(unsigned char   func_num,
                                    unsigned char   write,
                                    unsigned int      reg_addr,
                                    unsigned char  *byte)
{
    int err_ret;
    struct sdio_func * func = aml_priv_to_func(func_num );

    ASSERT(func != NULL);
    ASSERT(byte != NULL);
    ASSERT(func->num == func_num);

    sdio_claim_host(func);

    if (write) {
        
        sdio_writeb(func, *byte, reg_addr, &err_ret);
    }
    else {
        
        *byte = sdio_readb(func, reg_addr, &err_ret);
    }

    sdio_release_host(func);

    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

static int _aml_sdio_request_buffer(unsigned char func_num,
                                    unsigned int    fix_incr,
                                    unsigned char write,
                                    unsigned int    addr,
                                    void	            *buf,
                                    unsigned int    nbytes)
{
    int err_ret;
    int align_nbytes = nbytes;
    struct sdio_func * func = aml_priv_to_func(func_num);
    bool fifo = (fix_incr == SDIO_OPMODE_FIXED);

    ASSERT(buf != NULL);
    ASSERT(func != NULL);
    ASSERT(fix_incr == SDIO_OPMODE_FIXED|| fix_incr == SDIO_OPMODE_INCREMENT);
    ASSERT(func->num == func_num);

    sdio_claim_host(func);

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

    sdio_release_host(func);

    return (err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL;
}

void aml_sdio_write_word(unsigned int addr, unsigned int data)
{
    
    static unsigned char sdio_word_buf[sizeof(unsigned int)];
    unsigned char *sdio_kmm = sdio_word_buf;
    memcpy(sdio_kmm, &data, sizeof(data));
    
    if ((addr & 0x00f00000) == 0x00f00000) {
        aml_aon_write_reg(addr, data);
    }
    else if(((addr & 0x00f00000) == 0x00b00000)||
        ((addr & 0x00f00000) == 0x00d00000)||
        ((addr & 0x00f00000) == 0x00900000))
    {
        aml_aon_write_reg(addr, data);
    }
    else if(((addr & 0x00f00000) == 0x00200000)||
        ((addr & 0x00f00000) == 0x00300000)||
        ((addr & 0x00f00000) == 0x00400000))
    {
        aml_aon_write_reg((addr), data);
    }
    else
    {
        aml_sdio_write_sram(sdio_kmm,
                                    (unsigned char*)(SYS_TYPE)(addr),
                                    sizeof(unsigned int));
    }
    
}

unsigned int aml_sdio_read_word(unsigned int addr)
{
    unsigned int regdata = 0;

    if ((addr & 0x00f00000) == 0x00f00000) {
        regdata = aml_aon_read_reg(addr);
    }
    else if(((addr & 0x00f00000) == 0x00b00000)||
        ((addr & 0x00f00000) == 0x00d00000)||
        ((addr & 0x00f00000) == 0x00900000))
    {
        regdata = aml_aon_read_reg(addr);
    }
    else if(((addr & 0x00f00000) == 0x00200000)||
        ((addr & 0x00f00000) == 0x00300000)||
        ((addr & 0x00f00000) == 0x00400000))
    {
        regdata = aml_aon_read_reg(addr);
    }
    else
    {
        aml_sdio_read_sram((unsigned char*)(SYS_TYPE)&regdata,
                                        (unsigned char*)(SYS_TYPE)(addr),
                                        sizeof(unsigned int));
    }
    return regdata;
}

void aml_sdio_write_cmd32(unsigned long sram_addr, unsigned long sramdata)
{
    struct  hw_interface* hif = hif_get_hw_interface();

}

int aml_sdio_suspend(unsigned int suspend_enable)
{
    mmc_pm_flag_t flags;
    struct sdio_func *func = NULL;
    int ret = 0, i;

    if (suspend_enable == 0)
    {
        
        return ret;
    }

#ifndef NOT_AMLOGIC_PLATFORM
    amlwifi_set_sdio_host_clk(0);
#endif

    for (i = SDIO_FUNC1; i <= FUNCNUM_SDIO_LAST; i++)
    {
        func = aml_priv_to_func(i);
        if (func == NULL)
            continue;
        flags = sdio_get_host_pm_caps(func);

        if ((flags & MMC_PM_KEEP_POWER) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);

        if (ret != 0)
            return -1;

        if ((flags & MMC_PM_WAKE_SDIO_IRQ) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_WAKE_SDIO_IRQ);

        if (ret != 0)
            return -1;
    }
    return ret;
}

unsigned long  aml_sdio_read_reg8(unsigned long sram_addr )
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned char regdata[8] = {0};

    ASSERT(hif != NULL);

    return regdata[0];
}

void aml_sdio_write_reg8(unsigned long sram_addr, unsigned long sramdata)
{
    struct  hw_interface* hif = hif_get_hw_interface();

}

unsigned long aml_sdio_read_reg32(unsigned long sram_addr)
{
    struct  hw_interface* hif = hif_get_hw_interface();

}

int aml_sdio_write_reg32(unsigned long sram_addr, unsigned long sramdata)
{
    struct  hw_interface* hif = hif_get_hw_interface();

}

void aml_sdio_write_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    struct hw_interface* hif = hif_get_hw_interface();

}

void aml_sdio_read_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    struct hw_interface* hif = hif_get_hw_interface();

}

void aml_sdio_recv_frame (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    struct hw_interface* hif = hif_get_hw_interface();

}

static int amlw_sdio_alloc_prep_scat_req(struct amlw_hwif_sdio *hif_sdio)
{
    struct amlw_hif_scatter_req * scat_req = NULL;

    ASSERT(hif_sdio != NULL);

    scat_req = ZMALLOC(sizeof(struct amlw_hif_scatter_req), "sdio_alloc_prep_scat_req", GFP_ATOMIC );
    if (scat_req == NULL)
    {
        ERROR_DEBUG_OUT("[sdio sg alloc_scat_req]: no mem\n");
        return 1;
    }

    scat_req->free = true;
    hif_sdio->scat_req = scat_req;
#ifdef NOT_AMLOGIC_PLATFORM
    hif_sdio->bounce_buf_size = AML_SDIO_SCAT_BOUNCE_SIZE;
    hif_sdio->bounce_buf = aml_sdio_dma_alloc(hif_sdio->bounce_buf_size);
    if (!hif_sdio->bounce_buf) {
        FREE(scat_req, "sdio_alloc_prep_scat_req");
        hif_sdio->scat_req = NULL;
        hif_sdio->bounce_buf_size = 0;
        ERROR_DEBUG_OUT("[sdio sg alloc_bounce]: no mem\n");
        return -ENOMEM;
    }
#else
    
    {
        size_t rx_need = READ_LEN_PER_ONCE + PAGE_LEN; 
        size_t tx_need = AML_SDIO_SCAT_BOUNCE_SIZE;
        hif_sdio->bounce_buf_size = (rx_need > tx_need) ? rx_need : tx_need;
    }
    hif_sdio->bounce_buf = aml_sdio_dma_alloc(hif_sdio->bounce_buf_size);
    if (!hif_sdio->bounce_buf) {
        pr_warn("[sdio sg alloc_bounce]: persistent FUNC6 bounce alloc failed, will use per-call fallback\n");
        hif_sdio->bounce_buf_size = 0;
    }
#endif

    return 0;
}

struct amlw_hif_scatter_req *aml_sdio_scatter_req_get(void)
{
    struct hw_interface * hif = hif_get_hw_interface();
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();

    struct amlw_hif_scatter_req *scat_req = NULL;

    ASSERT(hif != NULL);
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

int aml_sdio_enable_scatter(void)
{
    struct hw_interface * hif = hif_get_hw_interface();
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();
    int ret;

    ASSERT(hif != NULL);
    ASSERT(hif_sdio != NULL);

    if (hif_sdio == NULL)
        return -EINVAL;

    if (hif_sdio->scatter_enabled)
        return 0;

    ret = amlw_sdio_alloc_prep_scat_req(hif_sdio);
    if (ret == 0)
        hif_sdio->scatter_enabled = true;

    return ret;
}

int aml_sdio_scat_rw(struct scatterlist *sg_list, unsigned int sg_num, unsigned int blkcnt,
        unsigned char func_num, unsigned int addr, unsigned char write)
{
    struct mmc_request mmc_req;
    struct mmc_command mmc_cmd;
    struct mmc_data    mmc_dat;
    struct sdio_func *func = aml_priv_to_func(func_num);
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();
    
    bool use_bounce = false;
    unsigned int bounce_total = 0;
    struct scatterlist single_sg;
    int ret = 0;

    if (write && func_num == SDIO_FUNC4 && hif_sdio && hif_sdio->bounce_buf &&
        hif_sdio->bounce_buf_size) {
        unsigned char *dst = (unsigned char *)hif_sdio->bounce_buf;
        struct scatterlist *sg;
        unsigned int i;

        for_each_sg(sg_list, sg, sg_num, i) {
            unsigned int len = sg->length;
            unsigned char *src;

            if (bounce_total + len > hif_sdio->bounce_buf_size) {
                
                bounce_total = 0;
                break;
            }

            src = sg_virt(sg);
            if (unlikely(src == NULL)) {
                bounce_total = 0;
                break;
            }

            memcpy(dst + bounce_total, src, len);
            bounce_total += len;
        }

        if (bounce_total != 0) {
            sg_init_one(&single_sg, hif_sdio->bounce_buf, bounce_total);
            use_bounce = true;
        }
    }

    memset(&mmc_req, 0, sizeof(struct mmc_request));
    memset(&mmc_cmd, 0, sizeof(struct mmc_command));
    memset(&mmc_dat, 0, sizeof(struct mmc_data));

    if (use_bounce) {
        mmc_dat.sg     = &single_sg;
        mmc_dat.sg_len = 1;
    } else {
        mmc_dat.sg     = sg_list;
        mmc_dat.sg_len = sg_num;
    }
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
    sdio_release_host(func);

    if (mmc_cmd.error || mmc_dat.error) {
        pr_err("ERROR CMD53 %s cmd_error = %d data_error=%d (bounce=%d)\n",
            write ? "write" : "read", mmc_cmd.error, mmc_dat.error,
            use_bounce ? 1 : 0);
	    ret  = mmc_cmd.error;
    }

    return ret;
}

void aml_sdio_cleanup_scatter(void)
{
    struct hw_interface * hif = hif_get_hw_interface();
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();

    pr_debug("[sdio sg cleanup]: enter\n");

    ASSERT(hif != NULL);
    ASSERT(hif_sdio != NULL);

    if (hif_sdio == NULL)
        return;

    if (!hif_sdio->scatter_enabled)
        return;

    hif_sdio->scatter_enabled = false;

    if (hif_sdio->bounce_buf) {
        aml_sdio_dma_free(hif_sdio->bounce_buf, hif_sdio->bounce_buf_size);
        hif_sdio->bounce_buf = NULL;
        hif_sdio->bounce_buf_size = 0;
    }
    if (hif_sdio->scat_req) {
        FREE(hif_sdio->scat_req, "sdio_alloc_prep_scat_req");
        hif_sdio->scat_req = NULL;
    }

    pr_debug("[sdio sg cleanup]: exit\n");

    return;
}

void aml_bt_sdio_write_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    struct hw_interface* hif = hif_get_hw_interface();

}

void aml_bt_sdio_read_sram (unsigned char *buf, unsigned char *addr, SYS_TYPE len)
{
    struct hw_interface* hif = hif_get_hw_interface();

}

void aml_bt_hi_write_word(unsigned int addr,unsigned int data)
{
    unsigned int reg_tmp;
    struct hw_interface* hif = hif_get_hw_interface();

    unsigned char* sdio_kmm = (unsigned char*)ZMALLOC(sizeof(data), "bt_hi_write_word", GFP_ATOMIC );
    ASSERT(sdio_kmm);

    memcpy(sdio_kmm, &data, sizeof(data));
    
    reg_tmp = hif->hif_ops.hi_read_word(RG_SDIO_IF_MISC_CTRL);

    if ((reg_tmp & BIT(23)) != 1) {
        reg_tmp |= BIT(23);
        hif->hif_ops.hi_write_word(RG_SDIO_IF_MISC_CTRL , reg_tmp);
    }
    
    hif->hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);

    hif->hif_ops.bt_hi_write_sram(sdio_kmm,
        
        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));

    FREE(sdio_kmm, "bt_hi_write_word");
}

unsigned int aml_bt_hi_read_word(unsigned int addr)
{
    unsigned int regdata = 0;
    unsigned int reg_tmp;
    struct hw_interface* hif = hif_get_hw_interface();
    
    reg_tmp = hif->hif_ops.hi_read_word( RG_SDIO_IF_MISC_CTRL);

    if ((reg_tmp & BIT(23)) != 1) {
        reg_tmp |= BIT(23);
        hif->hif_ops.hi_write_word( RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }

    hif->hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A,addr & 0xfffe0000);
    hif->hif_ops.bt_hi_read_sram((unsigned char*)(SYS_TYPE)&regdata,
        
        (unsigned char*)(SYS_TYPE)(addr & 0x1ffff), sizeof(unsigned int));
    return regdata;
}

int aml_sdio_bottom_read(unsigned char  func_num, int addr, void *buf, size_t len,int incr_addr)
{
    void *kmalloc_buf = NULL;
    int result;
    int align_len = 0;
    unsigned int bounce_order = 0;
    int use_bounce = 0;

    ASSERT(func_num != SDIO_FUNC0);

    if (hal_wake_fw_req() == 0)
    {
        ERROR_DEBUG_OUT("wake up failed\n");
        return -1;
    }

    if (!aml_sdio_is_rx_bounce_func(func_num))
    {
        if (incr_addr == SDIO_OPMODE_INCREMENT)
        {
            struct sdio_func * func = aml_priv_to_func(func_num);

            align_len = sdio_align_size(func, len);
        }
        else
            align_len = len;

        kmalloc_buf = (unsigned char *)ZMALLOC(align_len, "sdio_bottom_read", GFP_ATOMIC );
    }
    else
    {
        struct amlw_hwif_sdio *hif_sdio_l = aml_sdio_priv();

        align_len = sdio_align_size(aml_priv_to_func(func_num), len);
        if (hif_sdio_l && hif_sdio_l->bounce_buf &&
            align_len <= hif_sdio_l->bounce_buf_size) {
            kmalloc_buf = hif_sdio_l->bounce_buf;
            use_bounce = 2;
        } else {
            bounce_order = get_order(align_len);
            kmalloc_buf = (unsigned char *)__get_free_pages(GFP_ATOMIC | __GFP_ZERO, bounce_order);
            if (kmalloc_buf != NULL)
                use_bounce = 1;
        }
    }
    if (kmalloc_buf == NULL) {
        ERROR_DEBUG_OUT("kmalloc buf fail\n");
        return SDIOH_API_RC_FAIL;
    }

    result = _aml_sdio_request_buffer(func_num, incr_addr, SDIO_READ, addr, kmalloc_buf,
                                      aml_sdio_is_rx_bounce_func(func_num) ? align_len : len);

    if (!aml_sdio_is_rx_bounce_func(func_num))
    {
        memcpy(buf, kmalloc_buf, len);
        FREE(kmalloc_buf, "sdio_bottom_read");
    }
    else if (use_bounce == 1)
    {
        
        memcpy(buf, kmalloc_buf, len);
        free_pages((unsigned long)kmalloc_buf, bounce_order);
    }
    else if (use_bounce == 2)
    {
        memcpy(buf, kmalloc_buf, len);
    }
    return result;
}

int aml_sdio_bottom_write(unsigned char  func_num,int addr, void *buf, size_t len,int incr_addr)
{
    void *kmalloc_buf;
    int result;

    ASSERT(func_num != SDIO_FUNC0);

    if (hal_wake_fw_req() == 0)
    {
        ERROR_DEBUG_OUT("wake up failed\n");
        return -1;
    }

    kmalloc_buf =  (unsigned char *)ZMALLOC(len, "sdio_bottom_write", GFP_ATOMIC );
    if (kmalloc_buf == NULL)
    {
        ERROR_DEBUG_OUT("kmalloc buf fail\n");
        return SDIOH_API_RC_FAIL;
    }
    memcpy(kmalloc_buf, buf, len);
    
    result = _aml_sdio_request_buffer(func_num, incr_addr, SDIO_WRITE, addr, kmalloc_buf, len);

    FREE(kmalloc_buf, "sdio_bottom_write");
    return result;
}

void aml_sdio_bottom_write8_func0(unsigned long sram_addr, unsigned char sramdata)
{
    struct  hw_interface* hif = hif_get_hw_interface();
    unsigned char * sdio_kmm = (unsigned char *)ZMALLOC(sizeof(unsigned char), "sdio_bottom_write8_func0", GFP_ATOMIC );
    ASSERT(sdio_kmm);
    ASSERT(hif != NULL);

    memcpy(sdio_kmm, &sramdata, sizeof(unsigned char));
    _aml_sdio_request_byte(SDIO_FUNC0, SDIO_WRITE, sram_addr, sdio_kmm);

    FREE(sdio_kmm, "sdio_bottom_write8_func0");
}

unsigned char aml_sdio_bottom_read8_func0(unsigned long sram_addr)
{
    struct  hw_interface* hif = hif_get_hw_interface();
    unsigned char sramdata;
    unsigned char* sdio_kmm = (unsigned char*)ZMALLOC(sizeof(unsigned char), "sdio_bottom_read8_func0", GFP_ATOMIC );
    ASSERT(sdio_kmm);
    ASSERT(hif != NULL);

    _aml_sdio_request_byte(SDIO_FUNC0, SDIO_READ, sram_addr, sdio_kmm);

    memcpy( &sramdata, sdio_kmm,sizeof(unsigned char));

    FREE(sdio_kmm, "sdio_bottom_read8_func0");
    return sramdata;
}

int aml_sdio_bottom_write8(unsigned char  func_num,int addr, unsigned char data)
{
    int ret = 0;
    unsigned char *sdio_kmm = (unsigned char*)ZMALLOC(sizeof(data), "sdio_bottom_write8", GFP_ATOMIC );

    ASSERT(sdio_kmm != NULL);
    ASSERT(func_num != SDIO_FUNC0);

    memcpy(sdio_kmm, &data, sizeof(char));
    ret =  _aml_sdio_request_byte(func_num, SDIO_WRITE, addr, sdio_kmm);
    FREE(sdio_kmm, "sdio_bottom_write8");

    return ret;
}

unsigned char aml_sdio_bottom_read8(unsigned char  func_num, int addr)
{
    unsigned char sramdata;
    unsigned char* sdio_kmm = (unsigned char*)ZMALLOC(sizeof(unsigned char), "sdio_bottom_read8", GFP_ATOMIC );
    ASSERT(sdio_kmm);

    _aml_sdio_request_byte(func_num, SDIO_READ, addr, sdio_kmm);

    memcpy(&sramdata, sdio_kmm, sizeof(unsigned char));
    FREE(sdio_kmm, "sdio_bottom_read8");

    return sramdata;
}
#endif

static void aml_sdio_scat_complete (struct amlw_hif_scatter_req * scat_req)
{
    int  i;
    struct hw_interface * hif = hif_get_hw_interface();
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();

    ASSERT(scat_req != NULL);
    ASSERT(hif != NULL);
    ASSERT(hif_sdio != NULL);

    if (scat_req == NULL || hif_sdio == NULL)
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

int aml_sdio_scat_req_rw(struct amlw_hif_scatter_req *scat_req)
{
    struct hw_interface *hif = hif_get_hw_interface();
    struct amlw_hwif_sdio *hif_sdio = aml_sdio_priv();
    struct sdio_func *func = NULL;
    struct mmc_host *host = NULL;

    unsigned int blk_size, blk_num;
    unsigned int max_blk_count, max_req_size;
    unsigned int func_num;

    struct scatterlist *sg;
    int sg_count, sgitem_count;
    int ttl_len, pkt_offset, ttl_page_num;

    struct mmc_request mmc_req;
    struct mmc_command mmc_cmd;
    struct mmc_data mmc_dat;

    int result = SDIOH_API_RC_FAIL;
    
    unsigned char *bounce_buf = NULL;
    unsigned int bounce_pos;

    ASSERT(scat_req != NULL);
    if (scat_req == NULL || hif_sdio == NULL) {
        ERROR_DEBUG_OUT("invalid scatter request context\n");
        return -EINVAL;
    }

    if (scat_req->req & HIF_WRITE)
        func_num = SDIO_FUNC4;
    else
        func_num = SDIO_FUNC6;

    func = hif_sdio->sdio_func_if[func_num];
    if (func == NULL || func->card == NULL || func->card->host == NULL) {
        ERROR_DEBUG_OUT("invalid sdio func/card/host for func %u\n", func_num);
        scat_req->result = -ENODEV;
        if (scat_req->req & HIF_ASYNCHRONOUS)
            aml_sdio_scat_complete(scat_req);
        return -ENODEV;
    }
    host = func->card->host;

    blk_size = func->cur_blksize;
    if (blk_size == 0) {
        ERROR_DEBUG_OUT("invalid sdio block size for func %u\n", func_num);
        scat_req->result = -EINVAL;
        if (scat_req->req & HIF_ASYNCHRONOUS)
            aml_sdio_scat_complete(scat_req);
        return -EINVAL;
    }
    max_blk_count = MIN(host->max_blk_count, SDIO_MAX_BLK_CNT);
    max_req_size = MIN(max_blk_count * blk_size, host->max_req_size);

    if (!hif_sdio->bounce_buf || hif_sdio->bounce_buf_size == 0) {
        result = -ENOMEM;
        goto armbian_scat_complete;
    }

    if (hif_sdio->bounce_buf_size < max_req_size)
        max_req_size = hif_sdio->bounce_buf_size;

    result = aml_sdio_validate_scat_req(scat_req, func, max_req_size,
        hif_sdio->bounce_buf_size, &ttl_len);
    if (result)
        goto armbian_scat_complete;

    bounce_buf = hif_sdio->bounce_buf;

    if (!(scat_req->req & HIF_WRITE)) {
        unsigned int rx_len = ttl_len;
        unsigned int copy_offset = 0;
        int ri;

        if (rx_len > max_req_size) {
            result = -EMSGSIZE;
            goto armbian_scat_complete;
        }

        memset(bounce_buf, 0, rx_len);
        result = aml_sdio_cmd53_buf_rw(func, scat_req->addr, bounce_buf,
            rx_len, SDIO_READ);
        if (result)
            goto armbian_scat_complete;

        for (ri = 0; ri < scat_req->scat_count; ri++) {
            unsigned int aligned = ALIGN(scat_req->scat_list[ri].len,
                func->cur_blksize);

            memcpy(scat_req->scat_list[ri].packet, bounce_buf + copy_offset,
                scat_req->scat_list[ri].len);
            copy_offset += aligned;
        }
        goto armbian_scat_complete;
    }

    pkt_offset = 0;
    sgitem_count = 0;
    while (sgitem_count < scat_req->scat_count)
    {
        ttl_len = 0;
        ttl_page_num = 0;
        bounce_pos = 0;

        while ((sgitem_count < scat_req->scat_count) && (ttl_len < max_req_size))
        {
            int packet_len = 0;
            int sg_data_size = 0;
            int copy_len = 0;
            unsigned char *pdata = NULL;

            packet_len = scat_req->scat_list[sgitem_count].len;
            pdata = scat_req->scat_list[sgitem_count].packet;

#if defined(HIF_SDIO_UNIT_MULTIBLKSZ)
            sg_data_size = ALIGN(packet_len, blk_size);
            if (sg_data_size > (max_req_size - ttl_len))
                break;
            copy_len = packet_len;
#else
            pdata += pkt_offset;
            sg_data_size = packet_len - pkt_offset;
            if (sg_data_size > max_req_size - ttl_len)
                sg_data_size = max_req_size - ttl_len;
            pkt_offset += sg_data_size;
            copy_len = sg_data_size;
#endif

            memcpy(bounce_buf + bounce_pos, pdata, copy_len);
            if (sg_data_size > copy_len)
                memset(bounce_buf + bounce_pos + copy_len, 0, sg_data_size - copy_len);
            bounce_pos += sg_data_size;
            ttl_len += sg_data_size;

#if defined(HIF_SDIO_UNIT_MULTIBLKSZ)
            ttl_page_num += scat_req->scat_list[sgitem_count].page_num;
            sgitem_count++;
#else
            if (pkt_offset == packet_len)
            {
                ttl_page_num += scat_req->scat_list[sgitem_count].page_num;
                sgitem_count++;
                pkt_offset = 0;

                if (sgitem_count == scat_req->scat_count)
                    ttl_len = ALIGN(ttl_len, blk_size);
            }
#endif
        }

        if ((ttl_len == 0) || (ttl_len % blk_size != 0)) {
            result = SDIOH_API_RC_FAIL;
            break;
        }

        result = aml_sdio_cmd53_buf_rw(func, scat_req->addr, bounce_buf,
            ttl_len, SDIO_WRITE);
        if (result)
            break;

#if (SDIO_UPDATE_HOST_WRPTR == 0)
        {
            struct hal_private *hal_priv = hal_get_priv();

            hal_priv->tx_page_offset = CIRCLE_Addition2(ttl_page_num,
                hal_priv->tx_page_offset, TX_ADDRESSTABLE_NUM);
            hif->hif_ops.hi_write_word(RG_WIFI_IF_MAC_TXTABLE_RD_ID,
                hal_priv->tx_page_offset);
        }
#endif
    }

armbian_scat_complete:
    scat_req->result = result;
    if (scat_req->result)
        ERROR_DEBUG_OUT("Bounce scatter request failed:%d\n", scat_req->result);

    if (scat_req->req & HIF_ASYNCHRONOUS)
        aml_sdio_scat_complete(scat_req);

    return result;

}

void aml_sdio_wake_up_int(void)
{
    hal_irq_top(0,0);
    return;
}

void aml_sdio_reset()
{
    unsigned char reg = 1;
    struct hw_interface* hif = hif_get_hw_interface();

    pr_debug("aml_sdio_reset \n");
    ASSERT(hif != NULL && hif->hif_ops.hi_bottom_write8 != NULL);

    hif->hif_ops.hi_bottom_write8(SDIO_FUNC0, SDIO_CCCR_IOABORT, reg);
}

void aml_sdio_irq_path(unsigned char b_gpio)
{
    unsigned int regdata = 0;
    unsigned int ck_reg = 0;
    struct hw_interface* hif = hif_get_hw_interface();

    pr_debug("%s: b_gpio=%d \n",__FUNCTION__, b_gpio);

    if (b_gpio)
    {
        regdata= hif->hif_ops.hi_read8_func0(SDIO_CCCR_IEN);

        pr_debug(" SDIO_CCCR_IEN=0x%x \n", regdata);
        regdata &= ~0x3;

        hif->hif_ops.hi_write8_func0(SDIO_CCCR_IEN,regdata);
        pr_debug(" SDIO CCCR IEN=0x%x \n", regdata);

        regdata = hif->hif_ops.hi_read_word(RG_WIFI_IF_FW2HST_CLR);

        regdata |= SDIO_FW2HOST_EN;
        hif->hif_ops.hi_write_word(RG_WIFI_IF_FW2HST_CLR, regdata);

        ck_reg = hif->hif_ops.hi_read_word(RG_WIFI_IF_FW2HST_CLR);
        pr_debug("%s(%d) ck_reg 0x%x\n", __FUNCTION__, __LINE__, ck_reg);
    }
    else
    {
        regdata= hif->hif_ops.hi_read8_func0(SDIO_CCCR_IEN);
        pr_debug(" SDIO_CCCR_IEN=0x%x \n", regdata);
        regdata |= 0x3;

        hif->hif_ops.hi_write8_func0(SDIO_CCCR_IEN,regdata);
        pr_debug(" SDIO_CCCR_IEN=0x%x \n", regdata);

        regdata = hif->hif_ops.hi_read_word(RG_WIFI_IF_FW2HST_CLR);
        regdata &= ~SDIO_FW2HOST_GPIO_EN;

        hif->hif_ops.hi_write_word(RG_WIFI_IF_FW2HST_CLR, regdata);

        regdata |= SDIO_GPIO_IRQ_TRIG_MODE_LEVEL;
        
    }
}

#if (USE_GPIO_IRQ==1)
int amlhal_gpio_open(struct hal_private * hal_priv)
{
    struct hw_interface* hif = hif_get_hw_interface();

    int ret = platform_wifi_request_gpio_irq(hal_priv);
    volatile unsigned int reg = 0;
    volatile unsigned int reg5c = 0;

    if (ret != 0)
    {
        ERROR_DEBUG_OUT("request_gpio ERR ret=%d!\n", ret);
        return ret;
    }
    else
    {
        
        reg5c = hif->hif_ops.hi_read_word(RG_WIFI_IF_FW2HST_CLR);

        pr_debug("%s(%d): -----------0x5C register=0x%x\n", __FUNCTION__, __LINE__, reg5c);
        reg5c |= BIT(31);

        hif->hif_ops.hi_write_word(RG_WIFI_IF_FW2HST_CLR, reg5c);

        switch (amlhal_gpio.gpio_irq_mode)
        {
            case WIFI_GPIO_IRQ_FALLING:
                reg &= (~SDIO_GPIO_IRQ_TRIG_MODE_LEVEL);
                reg &= (~SDIO_GPIO_IRQ_EDGE_TIMEUP_MASK);
                
                reg |= 0x3ff;
                break;
            case WIFI_GPIO_IRQ_LOW:
                reg &= (~SDIO_GPIO_IRQ_TRIG_MODE_LEVEL);
                reg |= (SDIO_GPIO_IRQ_EDGE_TIMEUP_MASK);
                break;
            default:
                ret = -1;
                goto err;
        }

        pr_debug("SDIO GPIO IRQ CONFIG REG=0x%x\n",reg);

        aml_sdio_irq_path(1); 
    }

    hal_priv->use_irq = 1;
    return ret;

err:
    platform_wifi_free_gpio_irq(hal_priv);
    return ret;
}

int amlhal_gpio_close(struct hal_private * hal_priv )
{
    if (hal_priv->use_irq)
        hal_priv->use_irq = 0;
    else
        return -1;
    platform_wifi_free_gpio_irq(hal_priv);

    return 0;
}

#endif 
static int aml_sdio_func1_irq_claimed;
static int aml_sdio_func4_irq_claimed;
static atomic_t aml_sdio_irq_pending = ATOMIC_INIT(0);

static void aml_sdio_interrupt(struct sdio_func * func)
{
    struct hal_private *hal_priv = hal_get_priv();

    if (!hal_priv->bhalOpen)
        return;

#if (USE_SDIO_IRQ == 1)
    
    hal_irq_top(0, hal_priv);
#else
    hal_irq_top(0, hal_priv);
#endif
}

void aml_sdio_set_card_irq(int enable)
{
    struct sdio_func *func = aml_priv_to_func(SDIO_FUNC1);
    struct mmc_host *host;

    if (!func || !func->card || !func->card->host)
        return;
    host = func->card->host;
    if (host->ops && host->ops->enable_sdio_irq)
        host->ops->enable_sdio_irq(host, enable ? 1 : 0);
}

void aml_sdio_unmask_irq(void)
{
    struct sdio_func *func;
    unsigned char ien;
    int err = 0;

    if (!atomic_xchg(&aml_sdio_irq_pending, 0))
        return;

    func = aml_priv_to_func(SDIO_FUNC1);
    if (!func)
        return;

    sdio_claim_host(func);
    ien = sdio_f0_readb(func, SDIO_CCCR_IEN, &err);
    if (!err) {
        ien |= BIT(SDIO_FUNC1);
        if (aml_sdio_func4_irq_claimed)
            ien |= BIT(SDIO_FUNC4);
        sdio_f0_writeb(func, ien, SDIO_CCCR_IEN, &err);
    }
    sdio_release_host(func);
}

static void aml_sdio_func4_interrupt(struct sdio_func *func)
{
    aml_sdio_interrupt(func);
}

extern unsigned char w1_wifi_irq_enable;
void aml_sdio_enable_irq(int func_n)
{
    struct hal_private *hal_priv = hal_get_priv();
    if (!hal_priv->hst_if_irq_en) {
#if (USE_SDIO_IRQ==1)
        struct sdio_func *func = aml_priv_to_func(func_n);
        struct sdio_func *func4 = aml_priv_to_func(SDIO_FUNC4);
        int ret = 0;
        int ret4 = 0;
        if (!func) {
            ERROR_DEBUG_OUT("SDIO func%d is NULL, can't claim IRQ\n", func_n);
            return;
        }
        sdio_claim_host(func);
        ret = sdio_claim_irq(func, aml_sdio_interrupt);
        sdio_release_host(func);
        if (ret == 0) {
            aml_sdio_func1_irq_claimed = 1;
        } else {
            ERROR_DEBUG_OUT("sdio_claim_irq func%d failed: %d\n", func_n, ret);
            return;
        }
        if (func4 && func4 != func) {
            sdio_claim_host(func4);
            ret4 = sdio_claim_irq(func4, aml_sdio_func4_interrupt);
            sdio_release_host(func4);
            if (ret4 == 0) {
                aml_sdio_func4_irq_claimed = 1;
            } else {
                unsigned char ien;
                ERROR_DEBUG_OUT("sdio_claim_irq func4 failed: %d\n", ret4);
                ien = hif_get_hw_interface()->hif_ops.hi_read8_func0(SDIO_CCCR_IEN);
                ien &= ~BIT(SDIO_FUNC4);
                hif_get_hw_interface()->hif_ops.hi_write8_func0(SDIO_CCCR_IEN, ien);
            }
        }
        aml_sdio_irq_path(0);
#elif (USE_GPIO_IRQ==1)
        int ret = amlhal_gpio_open(hal_priv);
        if (ret != 0) {
            ERROR_DEBUG_OUT("GPIO IRQ enable skipped/failed: %d\n", ret);
            return;
        }
#endif
        w1_wifi_irq_enable = 1;
        hal_priv->hst_if_irq_en = 1;
    }
}

void aml_sdio_disable_irq(int func_n)
{
    struct hal_private *hal_priv=NULL;
    hal_priv = hal_get_priv();

   if (hal_priv->hst_if_irq_en)
    {
#if (USE_SDIO_IRQ==1)
        struct sdio_func *func = aml_priv_to_func(func_n);
        struct sdio_func *func4 = aml_priv_to_func(SDIO_FUNC4);
        if (func4 && func4 != func && aml_sdio_func4_irq_claimed) {
            sdio_claim_host(func4);
            sdio_release_irq(func4);
            sdio_release_host(func4);
            aml_sdio_func4_irq_claimed = 0;
        }
        if (func && aml_sdio_func1_irq_claimed) {
            sdio_claim_host(func);
            sdio_release_irq(func);
            sdio_release_host(func);
            aml_sdio_func1_irq_claimed = 0;
        }
#elif (USE_GPIO_IRQ==1)
        amlhal_gpio_close(hal_priv);
#endif
        w1_wifi_irq_enable = 0;
        hal_priv->hst_if_irq_en = 0;
    }
}

static void aml_sdio_calibration(void)
{
    struct hw_interface *hif = hif_get_hw_interface();
    int err;
    unsigned char i, j, k, l;
    unsigned char step;

    step = 4;
    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c0, 0);
    for (i = 0; i < 32; i += step) {
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c2, i);

        for (j = 0; j < 32; j += step) {
            hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c3, j);

            for (k = 0; k < 32; k += step) {
                hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c4, k);

                for (l = 0; l < 32; l += step) {
                    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c5, l);

                    err = hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, l);

                    if (err) {
                        
                        hif->hif_ops.hi_write8_func0(SDIO_CCCR_IOABORT, 0x1);
                        pr_debug("%s error: i:%d, j:%d, k:%d, l:%d\n", __func__, i, j, k, l);

                    } else {
                        pr_debug("%s right, use this config: i:%d, j:%d, k:%d, l:%d\n", __func__, i, j, k, l);
                        return;
                    }
                }
            }
        }
    }

    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c2, 0);
    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c3, 0);
    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c4, 0);
    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, 0x2c5, 0);
}

void set_reg_fragment(unsigned int addr,unsigned int bit_end,
        unsigned int bit_start,unsigned int value)
{
    unsigned int tmp;
    unsigned int bitwidth = bit_end - bit_start + 1;
    int max_value = (bitwidth == 32)? -1 : (1 << (bitwidth)) - 1;
    struct hw_interface* hif = hif_get_hw_interface();

    ASSERT((bitwidth > 0)||(bit_start <= 31)||(bit_end <= 31));

    tmp = hif->hif_ops.hi_read_word(addr);
    tmp &= ~(max_value << bit_start); 
    tmp |= ((value & max_value) << bit_start);

    hif->hif_ops.hi_write_word(addr,tmp);
}

void aml_aon_write_reg(unsigned int addr,unsigned int data)
{
    struct hw_interface* hif = hif_get_hw_interface();

    hif->hif_ops.bt_hi_write_word((addr), data);
}

unsigned int aml_aon_read_reg(unsigned int addr)
{
    unsigned int regdata = 0;
    struct hw_interface* hif = hif_get_hw_interface();

    regdata = hif->hif_ops.bt_hi_read_word((addr));
    return regdata;
}

static void aml_customer_gpio_wlan_ctrl(int onoff)
{
    switch (onoff)
    {
        case WLAN_POWER_OFF:
            pr_debug("%s: call customer specific GPIO to turn off WL_REG_ON\n", __FUNCTION__);
            break;

        case WLAN_POWER_ON:
            platform_wifi_reset();
            pr_debug("=========== WLAN placed in POWER ON ========\n");
            break;
    }
}

#ifdef NOT_AMLOGIC_PLATFORM
static inline void set_usb_wifi_power(int is_on) {}
#else
extern void set_usb_wifi_power(int is_on);
#endif

#ifdef SDIO_BUILD_IN
static void config_pmu_reg(bool is_power_on)
{
    int value_pmu_A12 = 0;
    int value_pmu_A13 = 0;
    int value_pmu_A14 = 0;
    int value_pmu_A15 = 0;
    int value_pmu_A17 = 0;
    int value_pmu_A18 = 0;
    int value_pmu_A20 = 0;
    int value_pmu_A22 = 0;
    int value_pmu_A24 = 0;
    int value_aon30 = 0;

    unsigned char host_req_status;
    unsigned char wifi_pmu_status;
    RG_AON_A30_FIELD_T reg_aon30_data;
    RG_AON_A29_FIELD_T reg_aon29_data;
    RG_BG_A16_FIELD_T reg_bg16_data;
    RG_BG_A12_FIELD_T reg_bg12_data;

    struct hal_private * halpriv = hal_get_priv();
    struct hw_interface * hif = hif_get_hw_interface();

    wifi_pmu_status = halpriv->hal_ops.hal_get_fw_ps_status();
    pr_debug("%s wifi_pmu_status:0x%x\n", __func__, wifi_pmu_status);

    if (is_power_on) {
        host_req_status = 0;
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, host_req_status);

        msleep(10);

        value_pmu_A12 = hif->hif_ops.hi_read_word(RG_PMU_A12);
        value_pmu_A13 = hif->hif_ops.hi_read_word(RG_PMU_A13);
        value_pmu_A14 = hif->hif_ops.hi_read_word(RG_PMU_A14);
        value_pmu_A15 = hif->hif_ops.hi_read_word(RG_PMU_A15);
        value_pmu_A17 = hif->hif_ops.hi_read_word(RG_PMU_A17);
        value_pmu_A18 = hif->hif_ops.hi_read_word(RG_PMU_A18);
        value_pmu_A20 = hif->hif_ops.hi_read_word(RG_PMU_A20);
        value_pmu_A22 = hif->hif_ops.hi_read_word(RG_PMU_A22);
        value_pmu_A24 = hif->hif_ops.hi_read_word(RG_PMU_A24);

        pr_debug("%s power on: before write A12=0x%x, A13=0x%x, A14=0x%x, A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x\n",
            __func__, value_pmu_A12, value_pmu_A13, value_pmu_A14, value_pmu_A15,value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22,value_pmu_A24);

        reg_bg12_data.data = hif->hif_ops.hi_read_word(RG_BG_A12);
        reg_bg12_data.b.RG_WF_BG_EN = 1;
        hif->hif_ops.hi_write_word(RG_BG_A12, reg_bg12_data.data);

        reg_bg16_data.data = hif->hif_ops.hi_read_word(RG_BG_A16);
        reg_bg16_data.b.RG_WF_DVDD_LDO_EN = 1;
        hif->hif_ops.hi_write_word(RG_BG_A16, reg_bg16_data.data);

        msleep(20);

        reg_bg16_data.data = hif->hif_ops.hi_read_word(RG_BG_A16);
        reg_bg16_data.b.RG_WF_SLEEP_ENB = 1;
        hif->hif_ops.hi_write_word(RG_BG_A16, reg_bg16_data.data);

        msleep(2);

        if (w1_wifi_sdio_access == 0)
            hif->hif_ops.hi_write_word(RG_PMU_A12, 0x16a2c);
        
        else
            hif->hif_ops.hi_write_word(RG_PMU_A12, 0x2a2c);
        hif->hif_ops.hi_write_word(RG_PMU_A14, 0x1);
        hif->hif_ops.hi_write_word(RG_PMU_A17, 0x700);
        hif->hif_ops.hi_write_word(RG_PMU_A20, 0x0);
        hif->hif_ops.hi_write_word(RG_PMU_A18, 0x1700);
        hif->hif_ops.hi_write_word(RG_PMU_A22, 0x704);
        hif->hif_ops.hi_write_word(RG_PMU_A24, 0x0);

        host_req_status = (PMU_PWR_OFF << 1)| BIT(0);
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, host_req_status);

        host_req_status = 0;
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, host_req_status);
        msleep(20);

        wifi_pmu_status = halpriv->hal_ops.hal_get_fw_ps_status();
        pr_debug("%s wifi_pmu_status:0x%x\n", __func__, wifi_pmu_status);

        while ((wifi_pmu_status & 0xF) != PMU_ACT_MODE) {
            pr_debug("%s wifi_pmu_status:0x%x\n", __func__, wifi_pmu_status);
            wifi_pmu_status = halpriv->hal_ops.hal_get_fw_ps_status();
        }

        value_pmu_A15 = hif->hif_ops.hi_read_word(RG_PMU_A15);
        value_pmu_A17 = hif->hif_ops.hi_read_word(RG_PMU_A17);
        value_pmu_A18 = hif->hif_ops.hi_read_word(RG_PMU_A18);
        value_pmu_A20 = hif->hif_ops.hi_read_word(RG_PMU_A20);
        value_pmu_A22 = hif->hif_ops.hi_read_word(RG_PMU_A22);
        value_pmu_A24 = hif->hif_ops.hi_read_word(RG_PMU_A24);
        pr_debug("%s power on: after write A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x\n",
            __func__, value_pmu_A15, value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22,value_pmu_A24);

    } else {
        value_pmu_A12 = hif->hif_ops.hi_read_word(RG_PMU_A12);
        value_pmu_A15 = hif->hif_ops.hi_read_word(RG_PMU_A15);
        value_pmu_A17 = hif->hif_ops.hi_read_word(RG_PMU_A17);
        value_pmu_A18 = hif->hif_ops.hi_read_word(RG_PMU_A18);
        value_pmu_A20 = hif->hif_ops.hi_read_word(RG_PMU_A20);
        value_pmu_A22 = hif->hif_ops.hi_read_word(RG_PMU_A22);
        value_pmu_A24 = hif->hif_ops.hi_read_word(RG_PMU_A24);
        value_aon30 = hif->hif_ops.hi_read_word(RG_AON_A30);
        pr_debug("%s power off: before write A12=0x%x, A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x, AON30=0x%x\n",
            __func__, value_pmu_A12,value_pmu_A15,value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22,value_pmu_A24, value_aon30);

        reg_bg16_data.data = hif->hif_ops.hi_read_word(RG_BG_A16);
        reg_bg16_data.b.RG_WF_SLEEP_ENB = 0;
         hif->hif_ops.hi_write_word(RG_BG_A16, reg_bg16_data.data);

        reg_bg16_data.data = hif->hif_ops.hi_read_word(RG_BG_A16);
        reg_bg16_data.b.RG_WF_DVDD_LDO_EN = 0;
        hif->hif_ops.hi_write_word(RG_BG_A16, reg_bg16_data.data);

        msleep(2);
        
        reg_bg12_data.data = hif->hif_ops.hi_read_word(RG_BG_A12);
        reg_bg12_data.b.RG_WF_BG_EN = 0;
        hif->hif_ops.hi_write_word(RG_BG_A12, reg_bg12_data.data);

        reg_aon29_data.data = hif->hif_ops.hi_read_word(RG_AON_A29);
        reg_aon29_data.b.rg_ana_bpll_cfg |= BIT(1) | BIT(0);
        hif->hif_ops.hi_write_word(RG_AON_A29, reg_aon29_data.data);

        if (w1_wifi_sdio_access == 0)
            hif->hif_ops.hi_write_word(RG_PMU_A12, 0xbea2e);
        
        else
            hif->hif_ops.hi_write_word(RG_PMU_A12, 0x9ea2e); 
        hif->hif_ops.hi_write_word(RG_PMU_A14, 0x1);
        hif->hif_ops.hi_write_word(RG_PMU_A16, 0x0);
        msleep(2);

        hif->hif_ops.hi_write_word(RG_PMU_A18, 0x8700);
        hif->hif_ops.hi_write_word(RG_PMU_A20, 0x3ff01ff);
        
        hif->hif_ops.hi_write_word(RG_PMU_A17, 0x703);
        
        reg_aon30_data.data = hif->hif_ops.hi_read_word(RG_AON_A30);
        
        reg_aon30_data.b.rg_always_on_cfg4 |= BIT(12);
        hif->hif_ops.hi_write_word(RG_AON_A30, reg_aon30_data.data);

        value_pmu_A12 = hif->hif_ops.hi_read_word(RG_PMU_A12);
        value_pmu_A15 = hif->hif_ops.hi_read_word(RG_PMU_A15);
        value_pmu_A17 = hif->hif_ops.hi_read_word(RG_PMU_A17);
        value_pmu_A18 = hif->hif_ops.hi_read_word(RG_PMU_A18);
        value_pmu_A20 = hif->hif_ops.hi_read_word(RG_PMU_A20);
        value_pmu_A22 = hif->hif_ops.hi_read_word(RG_PMU_A22);
        value_pmu_A24 = hif->hif_ops.hi_read_word(RG_PMU_A24);
        value_aon30 = hif->hif_ops.hi_read_word(RG_AON_A30);

        pr_debug("%s power off: after write A12=0x%x, A15=0x%x, A17=0x%x, A18=0x%x, A20=0x%x, A22=0x%x, A24=0x%x, AON30=0x%x\n",
            __func__, value_pmu_A12,value_pmu_A15,value_pmu_A17,value_pmu_A18,value_pmu_A20,value_pmu_A22, value_pmu_A24, value_aon30);

        hif->hif_ops.hi_write_word(RG_PMU_A22, 0x707);
        hif->hif_ops.hi_write_word(RG_INTF_CPU_CLK, 0x4f070001);

        host_req_status = (PMU_SLEEP_MODE << 1)| BIT(0);
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, host_req_status);
    }
}

extern unsigned char w1_set_wifi_bt_sdio_driver_bit(bool is_register, int shift);
extern unsigned char w1_w1_sdio_driver_insmoded;
extern unsigned char w1_w1_sdio_after_porbe;
extern int  w1_aml_w1_sdio_init(void);
extern void  w1_aml_w1_sdio_exit(void);

int aml_sdio_init(void)
{
    int ret = 0;
    int wait_probe;
    struct hw_interface * hif = hif_get_hw_interface();
    struct hal_private * hal_priv = hal_get_priv();
    struct sdio_func *func;
    int power_on = 0;
    int hal_inited = 0;
    int parent_dev_set = 0;

    pr_info("aml_sdio_init: driver_insmoded=%u after_probe=%u\n",
            w1_w1_sdio_driver_insmoded, w1_w1_sdio_after_porbe);

    aml_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);
    set_usb_wifi_power(0);
    msleep(200);
    aml_customer_gpio_wlan_ctrl(WLAN_POWER_ON);
    set_usb_wifi_power(1);
    msleep(200);

    if (!w1_w1_sdio_driver_insmoded) {
        w1_aml_w1_sdio_init();
        msleep(200);
    }

    for (wait_probe = 0; !w1_w1_sdio_after_porbe && wait_probe < 20; wait_probe++) {
        pr_info("aml_sdio_init: waiting for aml_sdio probe (%d/20)\n", wait_probe + 1);
        msleep(100);
    }

    if (!w1_w1_sdio_after_porbe) {
        ERROR_DEBUG_OUT("can't probe sdio! driver_insmoded=%u after_probe=%u\n",
            w1_w1_sdio_driver_insmoded, w1_w1_sdio_after_porbe);
        
        return -ENODEV;
    }

    func = aml_priv_to_func(SDIO_FUNC7);
    if (func == NULL) {
        ERROR_DEBUG_OUT("can't get SDIO func7 after probe\n");
        return -ENODEV;
    }
    pr_info("aml_sdio_init: using SDIO func7=%p parent=%s\n",
            func, dev_name(&func->dev));
    w1_set_wifi_bt_sdio_driver_bit(AML_W1_WIFI_POWER_ON, WIFI_POWER_CHANGE_SHIFT);
    power_on = 1;

    tx_status_list_init(&(hif->tx_status_list), WIFI_MAX_TXFRAME*2);
    skb_queue_head_init(&hif->bcn_list_head);

    ret = hal_init_priv();
    if (ret != 0)
        goto create_thread_error;
    hal_inited = 1;

    ret = hal_create_thread();
    if (ret != 0)
        goto create_thread_error;

    vm_cfg80211_set_parent_dev(&func->dev);
    parent_dev_set = 1;
    if (!(hif->hif_ops.hi_bottom_write8)) {
        ERROR_DEBUG_OUT("not found w1 wifi\n");
        ret = -ENODEV;
        goto create_thread_error;
    }
    aml_sdio_calibration();
    config_pmu_reg(AML_W1_WIFI_POWER_ON);

    if ((hal_priv->hal_call_back != NULL)
        &&(hal_priv->hal_call_back->dev_probe != NULL))
    {
        pr_debug("hal_priv->hal_call_back->dev_probe\n");
        
        ret = hal_priv->hal_call_back->dev_probe();
        if (ret < 0)
        {
            goto create_thread_error;
        }
    }
    hal_priv->powersave_init_flag = 0;

    pr_debug("%s(%d): sg ops init\n", __func__, __LINE__);
    hif->hif_ops.hi_enable_scat();

    aml_sdio_enable_irq(SDIO_FUNC1);
    pr_debug("aml_sdio_probe-- ret %d\n", ret);

    if (aml_wifi_is_enable_rf_test()) {
        mib_init();
        driver_open();
    }

    return ret;

create_thread_error:
    aml_sdio_disable_irq(SDIO_FUNC1);
    if (parent_dev_set)
        vm_cfg80211_clear_parent_dev();
    hal_kill_thread();
    if (hal_inited)
        hal_free();
    config_pmu_reg(AML_W1_WIFI_POWER_OFF);
    if (power_on)
        w1_set_wifi_bt_sdio_driver_bit(AML_W1_WIFI_POWER_OFF, WIFI_POWER_CHANGE_SHIFT);
    return ret;
}

void aml_disable_wifi(void)
{
    w1_wifi_sdio_access = 0;
    pr_debug("aml_disable_wifi start sdio access %d\n", w1_wifi_sdio_access);
    aml_sdio_disable_irq(SDIO_FUNC1);
    config_pmu_reg(AML_W1_WIFI_POWER_OFF);
    msleep(50);
    aml_sdio_disable_irq(SDIO_FUNC1);
}

void aml_enable_wifi(void)
{
    struct hal_private * hal_priv = hal_get_priv();

    pr_debug("aml_enable_wifi start\n");
    aml_customer_gpio_wlan_ctrl(WLAN_POWER_ON);

    hal_recovery_init_priv();
    config_pmu_reg(AML_W1_WIFI_POWER_ON);
    aml_sdio_enable_irq(SDIO_FUNC1);
    w1_wifi_sdio_access = 1;
    pr_debug("aml_enable_wifi start sdio access %d\n", w1_wifi_sdio_access);
    if (hal_priv == NULL || hal_priv->txcompletestatus == NULL) {
        pr_err("aml_enable_wifi: hal private state is not ready\n");
        return;
    }

    if (hal_fw_repair() != 0) {
        pr_err("aml_enable_wifi: hal firmware repair/init failed\n");
        return;
    }

    hal_priv->txcompletestatus->txdoneframecounter = 0;
    hal_priv->HalTxFrameDoneCounter = 0;
    hal_priv->txcompletestatus->txpagecounter = 0;
    hal_priv->HalTxPageDoneCounter = 0;

    pr_debug("aml_enable_wifi end\n");
}

void aml_sdio_exit(void) {
    struct hal_private *hal_priv = hal_get_priv();

    config_pmu_reg(AML_W1_WIFI_POWER_OFF);
    hal_ops_detach();

    aml_sdio_disable_irq(SDIO_FUNC1);
    hal_priv->hst_if_irq_en = 0;
    hal_priv->powersave_init_flag = 1;
    hal_free();

    w1_set_wifi_bt_sdio_driver_bit(AML_W1_WIFI_POWER_OFF, WIFI_POWER_CHANGE_SHIFT);

    aml_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);
    set_usb_wifi_power(0);
    msleep(200);

    if (aml_wifi_is_enable_rf_test())
        b2b_tx_thread_remove();

    vm_cfg80211_clear_parent_dev();
}

int aml_sdio_pm_suspend(struct device *device)
{
    mmc_pm_flag_t flags;
    struct sdio_func *func = NULL;
    struct hal_private * hal_priv = hal_get_priv();
    int ret = 0, cnt = 0;

    func = dev_to_sdio_func(device);

    while (atomic_read(&hal_priv->drv_suspend_cnt) == 0)
    {
        msleep(50);
        cnt++;
        if (cnt > 20)
        {
            ERROR_DEBUG_OUT("wifi suspend fail \n");
            return -1;
        }
    }

    atomic_inc(&hal_priv->sdio_suspend_cnt);
    
    {
        flags = sdio_get_host_pm_caps(func);

        if ((flags & MMC_PM_KEEP_POWER) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);

        if (ret != 0)
        {
            ERROR_DEBUG_OUT("host can't keep power while suspend \n");
            return -1;
        }

        if ((flags & MMC_PM_WAKE_SDIO_IRQ) != 0)
            ret = sdio_set_host_pm_flags(func, MMC_PM_WAKE_SDIO_IRQ);

        if (ret != 0)
        {
            ERROR_DEBUG_OUT("host can't set irq while suspend \n");
            return -1;
        }
    }
    return ret;

    return 0;
}

int aml_sdio_pm_resume(struct device *device)
{
    struct sdio_func *func = NULL;
    struct hal_private * hal_priv = hal_get_priv();

    func = dev_to_sdio_func(device);

    atomic_dec(&hal_priv->sdio_suspend_cnt);

    if (func && func->num == SDIO_FUNC1) {
        pr_debug("%s: re-enabling SDIO IRQ after resume\n", __func__);
        
    }

    return 0;
}

#else

int aml_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
    int ret = 0;
    static struct sdio_func sdio_func_0;
    struct hw_interface * hif = hif_get_hw_interface();
    struct hal_private * hal_priv = hal_get_priv();
    int hal_inited = 0;
    int parent_dev_set = 0;

    struct amlw_hwif_sdio * hwif_sdio = aml_sdio_priv();

    sdio_claim_host(func);
    ret = sdio_enable_func(func);
    if (ret)
        goto sdio_enable_error;

    {
        int blksz_ret;
        if (func->num == 4)
            blksz_ret = sdio_set_block_size(func, FUNC4_BLKSIZE);
        else
            blksz_ret = sdio_set_block_size(func, BLKSIZE);
        if (blksz_ret) {
            pr_warn("aml_sdio: sdio_set_block_size failed on func%d ret=%d; throughput will be DEGRADED\n",
                func->num, blksz_ret);
        }
    }

    pr_debug("%s(%d): func->num %d sdio block size=%d, \n", __func__, __LINE__,
        func->num,  func->cur_blksize);

    if (func->num == 1)
    {
        sdio_func_0.num = 0;
        sdio_func_0.card = func->card;
        hwif_sdio->sdio_func_if[0] = &sdio_func_0;
    }
    hwif_sdio->sdio_func_if[func->num] = func;
    pr_debug("%s(%d): func->num %d sdio_func=%p, \n", __func__, __LINE__,
        func->num,  func);

    sdio_release_host(func);
    sdio_set_drvdata(func, hwif_sdio);
    if (func->num != FUNCNUM_SDIO_LAST)
    {
        pr_debug("%s(%d):func_num=%d, lastfuncnum=%d\n", __func__, __LINE__,
            func->num, FUNCNUM_SDIO_LAST);
        return 0;
    }

    tx_status_list_init(&(hif->tx_status_list), WIFI_MAX_TXFRAME*2);
    skb_queue_head_init(&hif->bcn_list_head);

    ret = hal_init_priv();
    if (ret != 0)
        goto create_thread_error;
    hal_inited = 1;

    ret = hal_create_thread();
    if (ret != 0)
        goto create_thread_error;

    vm_cfg80211_set_parent_dev(&func->dev);
    parent_dev_set = 1;

    aml_sdio_calibration();

    if ((hal_priv->hal_call_back != NULL)
        &&(hal_priv->hal_call_back->dev_probe != NULL))
    {
        pr_debug("hal_priv->hal_call_back->dev_probe\n");
        
        ret = hal_priv->hal_call_back->dev_probe();
        if (ret < 0)
        {
            goto create_thread_error;
        }
    }
    hal_priv->powersave_init_flag = 0;

    pr_debug("%s(%d): sg ops init\n", __func__, __LINE__);
    hif->hif_ops.hi_enable_scat();

    aml_sdio_enable_irq(SDIO_FUNC1);
    pr_debug("aml_sdio_probe-- ret %d\n", ret);

    if (aml_wifi_is_enable_rf_test()) {
        mib_init();
        driver_open();
    }

    return ret;

create_thread_error:
    aml_sdio_disable_irq(SDIO_FUNC1);
    if (parent_dev_set)
        vm_cfg80211_clear_parent_dev();
    hal_kill_thread();
    if (hal_inited)
        hal_free();
    sdio_claim_host(func);
    sdio_disable_func(func);

sdio_enable_error:
    ERROR_DEBUG_OUT("sdio_enable_error\n");
    sdio_release_host(func);
    return ret;
}

static void  aml_sdio_remove(struct sdio_func *func)
{
    struct hal_private * hal_priv = hal_get_priv();
    int ret = 0;
    int err = 0;
    u8 abort;

    if (func== NULL)
    {
        return ;
    }

    pr_info("aml_sdio_remove: enter func->num=%d FUNCNUM_SDIO_LAST=%d\n",
            func->num, FUNCNUM_SDIO_LAST);
    if (func->num == FUNCNUM_SDIO_LAST)
    {
        aml_sdio_disable_irq(SDIO_FUNC1);
        hal_priv->powersave_init_flag = 1;
        hal_kill_thread();
        if (aml_wifi_is_enable_rf_test())
            b2b_tx_thread_remove();

        vm_cfg80211_clear_parent_dev();
        hal_free();
    }
    sdio_claim_host(func);

    if (func->num == FUNCNUM_SDIO_LAST && func->card) {
        ret = mmc_hw_reset(func->card);
        if (ret == 0) {
            pr_info("aml_sdio_remove: mmc_hw_reset OK — chip ready for next modprobe\n");
        } else if (ret == 1) {
            pr_info("aml_sdio_remove: mmc_hw_reset asynchronous (ret=1)\n");
        } else {
            pr_warn("aml_sdio_remove: mmc_hw_reset failed: %d — falling back to CCCR RES\n",
                    ret);
            {
                u32 prev_quirks = func->card->quirks;

                func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

                abort = sdio_f0_readb(func, SDIO_CCCR_ABORT, &err);
                if (err)
                    abort = 0x00;
                abort |= 0x08; 
                sdio_f0_writeb(func, abort, SDIO_CCCR_ABORT, &err);
                if (err)
                    pr_warn("aml_sdio_remove: fallback CCCR RES write failed: %d\n", err);
                else
                    pr_info("aml_sdio_remove: fallback CCCR RES issued (bit 0x08 at offset 0x06)\n");

                func->card->quirks = prev_quirks;
            }
        }
        msleep(10);
    }

    sdio_disable_func(func);
    sdio_release_host(func);
}

static int aml_sdio_pm_suspend(struct device *device)
{
    return 0;
}

static int aml_sdio_pm_resume(struct device *device)
{
    return 0;
}

static SIMPLE_DEV_PM_OPS(aml_sdio_pm_ops, aml_sdio_pm_suspend,
                        aml_sdio_pm_resume);

static const struct sdio_device_id aml_devices[] =
{
    {SDIO_DEVICE(VENDOR_AMLOGIC,PRODUCT_AMLOGIC) },
    {SDIO_DEVICE(VENDOR_AMLOGIC_EFUSE,PRODUCT_AMLOGIC_EFUSE)},
    {}
};

MODULE_DEVICE_TABLE(sdio, aml_devices);

static struct sdio_driver aml_sdio_driver =
{
    .name = "aml_sdio",
    .id_table = aml_devices,
    .probe = aml_sdio_probe,
    .remove = aml_sdio_remove,
    .drv.pm = &aml_sdio_pm_ops,
};

int  aml_sdio_init(void)
{
    int err = 0;

    if (sdioclk > AML_SDIO_CLK_SAFE_MAX) {
        pr_warn("aml_sdio: requested sdioclk=%d exceeds safe max %d, clamping to 200MHz\n",
                sdioclk, AML_SDIO_CLK_SAFE_MAX);
        sdioclk = AML_SDIO_CLK_SAFE_MAX;
    }

    amlwifi_set_sdio_host_clk(sdioclk);

    set_usb_wifi_power(0);
    msleep(500);
    set_usb_wifi_power(1);
    msleep(500);

    aml_customer_gpio_wlan_ctrl(WLAN_POWER_ON);
    sdio_reinit();

    err = sdio_register_driver(&aml_sdio_driver);
    if (err)
        ERROR_DEBUG_OUT("failed to register sdio driver: %d \n", err);

    return err;
}

void  aml_sdio_exit(void)
{
    struct hw_interface * hif = hif_get_hw_interface();

    hif->hif_ops.hi_write_word(RG_PMU_A22, 0x704);
    hif->hif_ops.hi_write_word(RG_PMU_A16, 0x0);

    PRINT("aml_sdio_exit++ \n");
    sdio_unregister_driver(&aml_sdio_driver);
    PRINT("*****************amlwifi unloaded ok********************\n");

    hal_ops_detach();
    set_usb_wifi_power(0);
    aml_customer_gpio_wlan_ctrl(WLAN_POWER_OFF);
}
#endif
