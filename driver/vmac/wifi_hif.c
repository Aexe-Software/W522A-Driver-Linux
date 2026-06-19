
#include "wifi_hal.h"
#include "wifi_hif.h"

#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/mmc/sdio.h>
#include "wifi_common.h"

static struct hw_interface g_hw_interface;
static int w522a_irq_poll_max_iters = 512;
module_param_named(irq_poll_max_iters, w522a_irq_poll_max_iters, int, 0644);
MODULE_PARM_DESC(irq_poll_max_iters,
    "W522A max hi_irq_task RX/TX poll iterations while SDIO IRQ is masked");

static int w522a_irq_idle_polls = 8;
module_param_named(irq_idle_polls, w522a_irq_idle_polls, int, 0644);
MODULE_PARM_DESC(irq_idle_polls,
    "W522A idle poll grace count before unmasking SDIO IRQ");

static int w522a_rx_drain_iter_max = 8;
module_param_named(rx_drain_iter_max, w522a_rx_drain_iter_max, int, 0644);
MODULE_PARM_DESC(rx_drain_iter_max,
    "W522A max RX FIFO drain passes inside one hi_soft_rx_irq call");

static int w522a_irq_idle_sleep_min_us = 80;
module_param_named(irq_idle_sleep_min_us, w522a_irq_idle_sleep_min_us, int, 0644);
MODULE_PARM_DESC(irq_idle_sleep_min_us,
    "W522A minimum idle sleep usec between SDIO IRQ poll probes");

static int w522a_irq_idle_sleep_max_us = 160;
module_param_named(irq_idle_sleep_max_us, w522a_irq_idle_sleep_max_us, int, 0644);
MODULE_PARM_DESC(irq_idle_sleep_max_us,
    "W522A maximum idle sleep usec between SDIO IRQ poll probes");

struct hw_interface* hif_get_hw_interface(void)
{
    return &g_hw_interface;
}

void hif_init_ops(void)
{
    struct amlw_hif_ops* ops = &g_hw_interface.hif_ops;
#ifdef SDIO_BUILD_IN
    memcpy(&g_hw_interface.hif_ops, &w1_g_w1_hif_ops,
           sizeof(struct amlw_hif_ops));
#endif

#ifndef SDIO_BUILD_IN
    ops->hi_bottom_write8 = aml_sdio_bottom_write8;
    ops->hi_bottom_read8 = aml_sdio_bottom_read8;
    ops->hi_bottom_read = aml_sdio_bottom_read;
    ops->hi_bottom_write = aml_sdio_bottom_write;

    ops->hi_write8_func0 = aml_sdio_bottom_write8_func0;
    ops->hi_read8_func0 = aml_sdio_bottom_read8_func0;

    ops->hi_enable_scat = aml_sdio_enable_scatter;
    ops->hi_cleanup_scat = aml_sdio_cleanup_scatter;
    ops->hi_get_scatreq = aml_sdio_scatter_req_get;
    ops->hi_scat_rw = aml_sdio_scat_rw;
    ops->hi_rcv_frame = aml_sdio_recv_frame;

    ops->hi_read_reg8 = aml_sdio_read_reg8;
    ops->hi_write_reg8 = aml_sdio_write_reg8;
    ops->hi_read_reg32 = aml_sdio_read_reg32;
    ops->hi_write_reg32 = aml_sdio_write_reg32;
    ops->hi_write_cmd = aml_sdio_write_cmd32;
    ops->hi_write_sram = aml_sdio_write_sram;
    ops->hi_read_sram = aml_sdio_read_sram;

    ops->hi_write_word = aml_sdio_write_word;
    ops->hi_read_word = aml_sdio_read_word;

    ops->bt_hi_write_sram = aml_bt_sdio_write_sram;
    ops->bt_hi_read_sram = aml_bt_sdio_read_sram;
    ops->bt_hi_write_word = aml_bt_hi_write_word;
    ops->bt_hi_read_word = aml_bt_hi_read_word;
    ops->hif_suspend = aml_sdio_suspend;
#endif
    ops->hi_send_frame = aml_sdio_scat_req_rw;
    ops->hif_get_sts = hif_get_sts;
    ops->hif_pt_rx_start = hif_pt_rx_start;
    ops->hif_pt_rx_stop = hif_pt_rx_stop;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0))
extern void do_gettimeofday(struct timeval *tv);
#endif
void b2b_rx_throughput_calc(HW_RxDescripter_bit *RxPrivHdr)
{
    static int rx_count = 0;
    static unsigned long length_start = 0;
    static unsigned long length_end = 0;
    static long start = 0;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 20, 0))
    struct timeval t_start;
    struct timeval t_end;
#endif

    if (RxPrivHdr->RxLength > DATA_PACKET_LENGTH_MIN)
    {
        rx_count++;
        if (rx_count == 1)
        {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0))
            start = ktime_to_ms(ktime_get_boottime());
#else
            do_gettimeofday(&t_start);
            start = ((long)t_start.tv_sec) * 1000 + (long)t_start.tv_usec / 1000;
#endif
            pr_debug("Rx side:Got the first packet and the start time is :%ld ms.\n",start);
            length_start += RxPrivHdr->RxLength;
            length_end += RxPrivHdr->RxLength;
        }
        else
        {
            long time_cost;
            long end;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0))
            end = ktime_to_ms(ktime_get_boottime());
#else
            do_gettimeofday(&t_end);
            end = ((long)t_end.tv_sec) * 1000+(long)t_end.tv_usec / 1000;
#endif
            time_cost = end - start;
            length_end += RxPrivHdr->RxLength;
            if (time_cost >= THROUGHPUT_GET_INTERVAL)
            {
                unsigned int throughput;

                throughput = (length_end - length_start) * 8 / time_cost;
                start = end;
                length_start = length_end;
                pr_debug("---B2B debug---Rx side: Current throughput: %d kbps.\n\n",throughput);
            }
        }
    }
}

unsigned int hi_get_device_id(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int regdata;

    regdata = hif->hif_ops.hi_read_word(PRODUCT_AMLOGIC_ADDR);

    return regdata;
}

unsigned int hi_get_vendor_id(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int regdata;

    regdata = hif->hif_ops.hi_read_word(VENDOR_AMLOGIC_ADDR);

    return regdata;
}

unsigned int hi_get_fw_version(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int regdata;

    regdata =  hif->hif_ops.hi_read_word(FIRMWARE_VERSION_ADDR);

    return regdata;
}

unsigned int hi_get_irq_status(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int regdata;

    regdata = hif->hif_ops.hi_read_word(RG_WIFI_IF_HOST_IRQ_ST);

    return regdata;
}

extern void aml_sdio_unmask_irq(void);

void hi_clear_irq_status(unsigned int data)
{
    struct hw_interface* hif = hif_get_hw_interface();

    hif->hif_ops.hi_write_word(RG_WIFI_IF_FW2HST_CLR, data);
    aml_sdio_unmask_irq();

#ifdef PROJECT_W1
    
    if (0)
    {
        struct hal_private * halpriv = hal_get_priv();

        if (halpriv->hal_ops.hal_check_fw_wake() == 0)
            return;

        POWER_BEGIN_LOCK();
        if (halpriv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
        {
            unsigned int tmpreg;

            POWER_END_LOCK();
            tmpreg = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ);
            
            tmpreg |= HOST_SLEEP_REQ;
            hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, tmpreg);
            POWER_BEGIN_LOCK();
        }
        POWER_END_LOCK();
    }
#endif
}

static SYS_TYPE hi_get_next_up_fifo(FIFO_SHARE_CTRL *pFifiShare)
{
    return (pFifiShare->FDB + pFifiShare->FDT * pFifiShare->FDL);
}

static unsigned char bFifoFull(FIFO_SHARE_CTRL *pFifiShare)
{
    unsigned int FDH = pFifiShare->FDH;

    CIRCLE_Add_One(FDH,pFifiShare->FDN);

    return (FDH == pFifiShare->FDT);
}

static unsigned char bDownFifo_Up(FIFO_SHARE_CTRL *pFifiShare)
{
    unsigned int FDH = pFifiShare->FDH;

    return (FDH == pFifiShare->FDT);
}

static unsigned char hi_down_fifo_full_up(FIFO_SHARE_CTRL *pFifiShare)
{
    
    struct hw_interface* hif = hif_get_hw_interface();

    pFifiShare->FDT = hif->hif_ops.hi_read_word(pFifiShare->FCB + 12);

    return bDownFifo_Up(pFifiShare);
}

static unsigned char hi_down_fifo_pull(FIFO_SHARE_CTRL *pFifiShare)
{
    
    struct hw_interface* hif = hif_get_hw_interface();

    pFifiShare->FDT =  hif->hif_ops.hi_read_word(pFifiShare->FCB + 12);

    return bFifoFull(pFifiShare);
}

static SYS_TYPE hi_get_next_down_fifo(FIFO_SHARE_CTRL *pFifiShare)
{
    return (pFifiShare->FDB + pFifiShare->FDH*pFifiShare->FDL);
}

static unsigned char hi_push_down_fifo(FIFO_SHARE_CTRL *pDownFifo,
                                       unsigned char *pdata, unsigned int len)
{
    struct hw_interface* hif = hif_get_hw_interface();

    if (!hi_down_fifo_pull(pDownFifo))
    {
        hif->hif_ops.hi_write_sram((unsigned char *)pdata,
                                   (unsigned char *)hi_get_next_down_fifo(pDownFifo),
                                   len);
        CIRCLE_Add_One( pDownFifo->FDH, pDownFifo->FDN);
        
        hif->hif_ops.hi_write_sram((unsigned char *)(&(pDownFifo->FDH)),
                                   (unsigned char *)(SYS_TYPE)((pDownFifo)->FCB + 8),
                                   4);
        return true;
    }

    return false;
}

static unsigned char hi_push_down_fifo_up(FIFO_SHARE_CTRL *pUpFifo,
                                          unsigned char *pdata, unsigned int len)
{
    
    struct hw_interface* hif = hif_get_hw_interface();

    if (hi_down_fifo_full_up(pUpFifo)) 
    {
        CIRCLE_Sub_One(pUpFifo->FDT ,pUpFifo->FDN); 
        hif->hif_ops.hi_read_sram((unsigned char *)pdata,
                                 (unsigned char *)hi_get_next_up_fifo(pUpFifo),
                                  len);
        
        CIRCLE_Add_One(pUpFifo->FDT, pUpFifo->FDN);
        return true;
    }

    return false;
}

static struct tx_status_node tx_status[WIFI_MAX_TXFRAME * 2];
int tx_status_list_init(struct tx_status_list *tx_status_list, int num)
{
    int i = 0;
    int node_num = num;

    INIT_LIST_HEAD(&(tx_status_list->status_list));
    INIT_LIST_HEAD(&(tx_status_list->free_list));
    spin_lock_init(&(tx_status_list->status_list_lock));

    if (node_num > WIFI_MAX_TXFRAME * 2)
    {
        node_num = WIFI_MAX_TXFRAME * 2;
    }

    while (i < node_num)
    {
        INIT_LIST_HEAD(&(tx_status[i].list_node));
        list_add_tail(&(tx_status[i].list_node), &(tx_status_list->free_list));
        i++;
    }

    return node_num;
}

struct tx_status_node *tx_status_node_alloc(struct tx_status_list *tx_status_list)
{
    struct tx_status_node *txok_status_node = NULL;

    if (tx_status_list == NULL)
        return NULL;

    spin_lock_bh(&tx_status_list->status_list_lock);

    if (!list_empty(&tx_status_list->free_list))
    {
        txok_status_node = list_first_entry(&tx_status_list->free_list,
                                            struct tx_status_node,
                                            list_node);
        list_del(&txok_status_node->list_node);
    }
    spin_unlock_bh(&tx_status_list->status_list_lock);

    return txok_status_node;
}

void tx_status_node_enqueue(struct tx_status_node *txok_status_node,
                            struct tx_status_list *tx_status_list)
{
    if (txok_status_node == NULL || tx_status_list == NULL)
        return;

    spin_lock_bh(&tx_status_list->status_list_lock);
    list_add_tail(&txok_status_node->list_node, &tx_status_list->status_list);
    spin_unlock_bh(&tx_status_list->status_list_lock);
}

void tx_status_node_free(struct tx_status_node *txok_status_node,
                         struct tx_status_list *tx_status_list)
{
    if (txok_status_node == NULL || tx_status_list == NULL)
        return;

    spin_lock_bh(&tx_status_list->status_list_lock);
    list_add_tail(&txok_status_node->list_node, &tx_status_list->free_list);
    spin_unlock_bh(&tx_status_list->status_list_lock);
}

struct tx_status_node * tx_status_node_dequeue(struct tx_status_list *tx_status_list)
{
    struct tx_status_node *txok_status_node = NULL;

    if (tx_status_list == NULL)
        return NULL;

    spin_lock_bh(&(tx_status_list->status_list_lock));
    if (!list_empty(&(tx_status_list->status_list)))
    {
        txok_status_node = list_first_entry(&(tx_status_list->status_list),
                                            struct tx_status_node,
                                            list_node);
        list_del(&(txok_status_node->list_node));
    }
    spin_unlock_bh(&(tx_status_list->status_list_lock));

    return txok_status_node;
}

unsigned char *g_rx_fifo = NULL;
void hi_rx_fifo_init(void)
{
    struct hw_interface* hif = hif_get_hw_interface();

#ifdef CONFIG_AML_USE_STATIC_BUF
    g_rx_fifo = wifi_mem_prealloc(AML_RX_FIFO, RX_FIFO_SIZE + 2 * FUNC6_BLKSIZE);
#else
    g_rx_fifo = (unsigned char *)ZMALLOC(RX_FIFO_SIZE + 2 * FUNC6_BLKSIZE,
                                         "hi_rx_fifo_init", GFP_KERNEL);
#endif
    if (g_rx_fifo == NULL)
        ERROR_DEBUG_OUT("rx fifo alloc failed\n");

    hif->rx_fifo.FDH = 0;
    hif->rx_fifo.FDT = 0;
    hif->rx_fifo.FDN = RX_FIFO_SIZE;
    hif->rx_fifo.FDB = g_rx_fifo;
}

static unsigned int remain_mpdu_num = 0;

static unsigned int send_page_num = 0;
int hi_sdio_setup_scat_data(struct scatterlist *sg_list, struct hi_tx_desc **pTxDPape,
                            unsigned int sg_num, unsigned char func_num)
{
    int i;
    struct sdio_func *func = aml_priv_to_func(func_num);
    struct mmc_host *host = func->card->host;
    unsigned int blkcnt, sg_total_len;

    unsigned int max_sg_size = min(func->cur_blksize * SDIO_MAX_BLK_CNT, host->max_req_size);

    if (sg_num > SG_NUM_MAX || sg_num == 0)
    {
        ERROR_DEBUG_OUT("ERROR: sg_num=%d\n", sg_num);
        return -1;
    }

    sg_init_table(sg_list, sg_num);
    remain_mpdu_num = sg_num;
    blkcnt = 0;
    sg_total_len = 0;

    for (i = 0; i < sg_num; i++)
    {
        unsigned int sg_data_size, page_tmp;

        sg_data_size = HW_MPDU_LEN_GET(pTxDPape[i]->MPDUBufFlag) + HI_TXDESC_DATAOFFSET;
        page_tmp = pTxDPape[i]->TxPriv.PageNum;
        
        sg_data_size = ALIGN(sg_data_size, FUNC4_BLKSIZE);
        if (sg_data_size > (max_sg_size - sg_total_len))
        {
            
            if (sg_total_len == 0)
            {
                ERROR_DEBUG_OUT("mpdu length too large:%d\n", sg_data_size);
            }
            break;
        }

        blkcnt += (sg_data_size / FUNC4_BLKSIZE);
        sg_total_len += sg_data_size;
        send_page_num += page_tmp;

        sg_set_buf(&sg_list[i], pTxDPape[i], sg_data_size);
        remain_mpdu_num--;
    }

    return blkcnt;
}

int hi_tx_frame(struct hi_tx_desc **pTxDPape, unsigned int mpdu_num,
                unsigned int page_num)
{
    struct scatterlist sg_list[SG_NUM_MAX] = {{0}};
    unsigned int sg_remain = 0, sg_num = 0, offset = 0, blkcnt = 0;
    int loop = 0, ret = 0;
    struct hw_interface* hif = hif_get_hw_interface();
    static unsigned int cnt = 0;
    cnt++;
    sg_num = mpdu_num;
    
    do
    {
        
        if (sg_num > SG_NUM_MAX)
        {
            sg_remain = sg_num - SG_NUM_MAX;
            sg_num = SG_NUM_MAX;
        }
        else
        {
            sg_remain = 0;
        }

        blkcnt = hi_sdio_setup_scat_data(sg_list, &pTxDPape[offset], sg_num,
                                         SDIO_FUNC4);
        if (remain_mpdu_num > 0)
        {
            sg_remain += remain_mpdu_num;
            sg_num    -= remain_mpdu_num;
        }
        if (blkcnt <= 0)
        {
            ERROR_DEBUG_OUT("sg init fail.\n\n");
            ret = -1;
            break;
        }
        
        while ((ret = hif->hif_ops.hi_scat_rw(sg_list, sg_num, blkcnt,
                                              SDIO_FUNC4, 0x0, SG_WRITE)) != 0)
        {
            if (loop > 4)
            {
                ERROR_DEBUG_OUT("sg process fail.\n\n");
                break;
            }
            loop++;
        }

        if (ret == 0)
        {
#if (SDIO_UPDATE_HOST_WRPTR == 0)
            
            unsigned int page_tmp = 0;
            unsigned int reg_tmp;
            struct hal_private * hal_priv = hal_get_priv();

            page_tmp += send_page_num;
            if (page_tmp > page_num)
            {
                pr_err("%s:%d, update host wr ptr failed. page_tmp :%d, all page num%d\n",
                       __func__, __LINE__,page_tmp, page_num);
            }
            hal_priv->tx_page_offset = CIRCLE_Addition2(send_page_num,
                                                        hal_priv->tx_page_offset,
                                                        TX_ADDRESSTABLE_NUM);
            send_page_num = 0;
            
            reg_tmp = hal_priv->tx_page_offset;

            hif->hif_ops.hi_write_word(RG_WIFI_IF_MAC_TXTABLE_RD_ID, reg_tmp);
#endif
        }
        else
        {
            
            pr_err("%s(%d):SDIO try re-transmit %d times, but fail\n",__func__, __LINE__, loop);
        }
        
        offset += sg_num;
        sg_num = sg_remain;
    }
    while(sg_remain > 0);

    return ret;
}

void hi_cfg_firmware(void)
{
    struct hw_interface *hif = hif_get_hw_interface();

    hif->hw_config.txpagenum = DEFAULT_TXPAGENUM;
    hif->hw_config.rxpagenum = DEFAULT_RXPAGENUM;
    hif->hw_config.bcn_page_num = DEFAULT_BCN_NUM;

    if (aml_wifi_is_enable_rf_test())
        hif->hw_config.flags = PT_MODE;

    hif->hif_ops.hi_write_sram((unsigned char *)&hif->hw_config,
                               (unsigned char *)HW_CONFIG_ADDR,
                               sizeof(HW_CONFIG));
}

extern unsigned char w1_wifi_sdio_access;
unsigned char hi_set_cmd(unsigned char *pdata, unsigned int len)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    FIFO_SHARE_CTRL *pCmdDownFifo = NULL;
    unsigned int loop = 0;
    PowerSaveCmd pscmd;
    struct SuspendCmd suspend_cmd;

    if (pdata == NULL || len == 0 || hif == NULL || hal_priv == NULL)
        return false;

    pCmdDownFifo = &hif->CmdDownFifo;
    memset(&pscmd, 0, sizeof(pscmd));
    memset(&suspend_cmd, 0, sizeof(suspend_cmd));
    if (len >= sizeof(pscmd))
        memcpy(&pscmd, pdata, sizeof(pscmd));
    if (len >= sizeof(suspend_cmd))
        memcpy(&suspend_cmd, pdata, sizeof(suspend_cmd));

    if (!w1_wifi_sdio_access) {
        AML_OUTPUT("recovering downloading firmware\n");
        return false;
    }

    POWER_BEGIN_LOCK();
    if (((hal_priv->hal_drv_ps_status & HAL_DRV_IN_SLEEPING) != 0) &&
        (pdata[0] != Power_Save_Cmd) && (pdata[0] != WoW_Enable_Cmd))
    {
        POWER_END_LOCK();
        return false;
    }
    
    if (atomic_read(&hal_priv->drv_suspend_cnt) != 0)
    {
        POWER_END_LOCK();
        return false;
    }

    hal_priv->hal_drv_ps_status |= HAL_DRV_IN_ACTIVE;
    POWER_END_LOCK();

    while (!hi_push_down_fifo(pCmdDownFifo,pdata,len))
    {
        if (loop++>10000)
        {
            pr_err("err-> f_fdh: %d, f_fdt: %d, d_fdh %d \n",
                   hif->hif_ops.hi_read_word(0x00a10038),
            hif->hif_ops.hi_read_word(0x00a1003c), pCmdDownFifo->FDH);
            PRINT("hi_set_cmd err, cmd=0x%x, vid=0x%x\n", pdata[0], pdata[3]);

            POWER_BEGIN_LOCK();
            hal_priv->hal_drv_ps_status &= ~HAL_DRV_IN_ACTIVE;
            POWER_END_LOCK();
            return false;
        }

        usleep_range(20, 40);
    }
    w1_aml_wifi_sdio_power_lock();
    POWER_BEGIN_LOCK();
    if (((hal_priv->powersave_init_flag == 0) && (pscmd.Cmd == Power_Save_Cmd) &&
        (pscmd.psmode == PS_DOZE)) ||
        ((hal_priv->powersave_init_flag == 0) &&
        (suspend_cmd.Cmd == WoW_Enable_Cmd) && (suspend_cmd.enable == 1)))
    {
        unsigned int tmpreg;

        tmpreg = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ);
        
        tmpreg |= HOST_SLEEP_REQ;
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, tmpreg);
    }

    hal_priv->hal_drv_ps_status &= ~HAL_DRV_IN_ACTIVE;
    POWER_END_LOCK();
    w1_aml_wifi_sdio_power_unlock();

    return true;
}

unsigned char hi_get_cmd(unsigned char *pdata,unsigned int len)
{
    struct hw_interface *hif = hif_get_hw_interface();
    FIFO_SHARE_CTRL *pCmdDownFifo = NULL;
    unsigned int Cmd;
    unsigned int loop = 0;

    if (pdata == NULL || len == 0 || hif == NULL)
        return false;

    pCmdDownFifo = &hif->CmdDownFifo;
    Cmd = pdata[0];

    if (!hi_set_cmd(pdata,len))
    {
        hif->HiStatus.PushDownCmd_Err_num++;
        return false;
    }
    
    while (!hi_push_down_fifo_up(pCmdDownFifo, pdata, len))
    {
        if (loop++ > 10000) {
            hif->HiStatus.PushDownCmd_Err_num++;
            return false;
        }
        
        usleep_range(20, 40);
    }
    if (Cmd != pdata[0])
    {
        return false;
    }
    return true;
}

unsigned int hi_soft_tx_irq(void)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    struct txdonestatus *txstatus = NULL;
    unsigned int hw_txc_addr;
    struct tx_complete_status *txcompletestatus;
    struct tx_nulldata_status *tx_null_status = NULL;
    
    unsigned int reaped = 0;

    struct tx_status_node *txok_status_node = NULL;
    struct tx_status_list *txok_status_list = &hif->tx_status_list;

    hw_txc_addr = hif->hw_config.txcompleteaddress;
    txcompletestatus = hal_priv->txcompletestatus;

    hif->hif_ops.hi_read_sram((unsigned char *)txcompletestatus,
                              (unsigned char *)(SYS_TYPE)hw_txc_addr,
                              (SYS_TYPE)sizeof(struct tx_complete_status));

    ASSERT(((hal_priv->txcompletestatus->txdoneframecounter - hal_priv->HalTxFrameDoneCounter) & 0xff)
            <= WIFI_MAX_TXFRAME);

    AML_TXLOCK_LOCK();
    hal_priv->txPageFreeNum +=
        (hal_priv->txcompletestatus->txpagecounter - hal_priv->HalTxPageDoneCounter) & 0xff;
    AML_TXLOCK_UNLOCK();

    hal_priv->HalTxPageDoneCounter = hal_priv->txcompletestatus->txpagecounter;

    if (hal_priv->bhaltxdrop) {
        pr_debug("page:%d, txdoneframecounter:%d, HalTxFrameDoneCounter:%d\n",
               hal_priv->txPageFreeNum, hal_priv->txcompletestatus->txdoneframecounter,
               hal_priv->HalTxFrameDoneCounter);
    }

    AML_PRINT(AML_DBG_MODULES_TX, "txPageFreeNum:%d, HalTxPageDoneCounter:%d\n",
              hal_priv->txPageFreeNum, hal_priv->HalTxPageDoneCounter);

    while (hal_priv->txcompletestatus->txdoneframecounter != hal_priv->HalTxFrameDoneCounter)
    {
        txstatus = &hal_priv->txcompletestatus->tx_status[hal_priv->HalTxFrameDoneCounter % WIFI_MAX_TXFRAME].txstatus;
        hal_priv->HalTxFrameDoneCounter++;
        reaped++;

        txok_status_node = tx_status_node_alloc(txok_status_list);
        if (txok_status_node != NULL)
        {
            memcpy(&(txok_status_node->tx_status.txstatus), txstatus, sizeof(struct txdonestatus));
            
            tx_status_node_enqueue(txok_status_node, txok_status_list);

            tx_null_status = &(txok_status_node->tx_status.tx_null_status);

            if (!((tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NULL_DATA_OK) ||
                  (tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NULL_DATA_FAIL) ||
                  (tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NEW)))
            {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
                __sync_fetch_and_add(&hif->HiStatus.Tx_Free_num, 1);
#else
                atomic_add(1, &hif->HiStatus.Tx_Free_num);
#endif
            }

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
            if ((tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NEW) &&
                (hif->HiStatus.Tx_Free_num < hif->HiStatus.Tx_Done_num)) {
                __sync_fetch_and_add(&hif->HiStatus.Tx_Free_num,1);
#else
            if ((tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NEW) &&
                (atomic_read(&hif->HiStatus.Tx_Free_num) < atomic_read(&hif->HiStatus.Tx_Done_num))) {
                atomic_add(1, &hif->HiStatus.Tx_Free_num);
#endif
            }
        }
    }

    up(&hal_priv->txok_thread_sem);
    return reaped;
}

static void hi_soft_fw_event(void)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    struct fw_event_to_driver *event;
    unsigned int hw_event_addr;

    hw_event_addr = hif->hw_config.fweventaddress;
    event = hal_priv->fw_event;

    hif->hif_ops.hi_read_sram((unsigned char *)event, (unsigned char *)(SYS_TYPE)hw_event_addr,
                              (SYS_TYPE)sizeof(struct fw_event_to_driver));

    if (aml_wifi_is_enable_rf_test())
        return;

    while (hal_priv->fw_event->event_counter != hal_priv->hal_fw_event_counter)
    {
        struct fw_event_no_data *normal_event;

        normal_event = &hal_priv->fw_event->event_data[hal_priv->hal_fw_event_counter % WIFI_MAX_FW_EVENT].normal_event;
        hal_priv->hal_call_back->intr_fw_event(hal_priv->drv_priv, normal_event);
        event->event_data[0].data[28] = hal_priv->hal_fw_event_counter;
        hif->hif_ops.hi_write_sram((unsigned char *)&event->event_data[0].data[28],
                                   (unsigned char *)(SYS_TYPE)(hw_event_addr + 32),
                                   (SYS_TYPE)(sizeof(unsigned char) * 4));
        hal_priv->hal_fw_event_counter++;
    }

    return;
}

void hi_soft_rx_bcn(struct hal_private *hal_priv, unsigned char vid)
{
    struct sk_buff *skb;
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned char bcnpage = hif->hw_config.bcn_page_num;
    
    unsigned int bcnaddr = hif->hw_config.beaconframeaddress +
                           (vid * (bcnpage / WIFI_MAX_VID) * PAGE_LEN);
    unsigned int buflen = bcnpage / WIFI_MAX_VID * PAGE_LEN;
    unsigned long bcnctl_reg;
    unsigned int rx_bcn_page;
    HW_RxDescripter_bit *RxDesc;

    bcnctl_reg = hif->hif_ops.hi_read_word(RG_MAC_BCN_CTRL_REG);

    if ((bcnctl_reg & BCN_READABLE) == 0)
    {
        PRINT("RecvBcn: bcnctrl_reg = %lx\n", (SYS_TYPE)bcnctl_reg);
        return;
    }

    skb = os_skb_alloc(buflen);
    if (skb == NULL)
    {
        PRINT("RecvBcn: alloc skb failed\n");
        return;
    }
    
    rx_bcn_page = (bcnctl_reg & BCN_LEN_MASK) >> 17;
    if (rx_bcn_page != 0)
        buflen = rx_bcn_page * PAGE_LEN;

    hif->hif_ops.hi_read_sram((unsigned char *)os_skb_data(skb),
                              (unsigned char *)(SYS_TYPE)bcnaddr,
                              (SYS_TYPE)buflen);

    RxDesc = (HW_RxDescripter_bit *)skb->data;

    if ((RxDesc->RxLength + RX_PRIV_HDR_LEN) >= buflen)
    {
        PRINT("RecvBcn: RxLength =%lu\n", (SYS_TYPE)RxDesc->RxLength);
        os_skb_free(skb);
        return;
    }
    os_skb_put(skb, (RxDesc->RxLength + RX_PRIV_HDR_LEN)); 

    skb_queue_tail(&hif->bcn_list_head, skb);
    up(&hal_priv->rx_thread_sem);
}

void hi_soft_rx_irq(struct hal_private *hal_priv, unsigned int rx_fw_ptr)
{
    unsigned int rx_total_len, rxmax_offset;
    struct hw_interface* hif;
    unsigned char hal_open;
    unsigned int rx_fifo_fw;
    int remain_len, read_len;
    int host_remain_len = 0;
    unsigned int fifo_empty_size;
    unsigned int rx_fifo_fdt;
    unsigned int rx_fifo_fdh;
#if (SDIO_UPDATE_HOST_RDPTR != 0)
    int host_wrapper_len;
#endif
    static unsigned char print_cnt = 0;
    
    unsigned int drain_iter = 0;
    unsigned int drain_iter_max =
        clamp_t(unsigned int, READ_ONCE(w522a_rx_drain_iter_max), 1, 64);

    hif = hif_get_hw_interface();
    hal_open = hal_priv->bhalOpen;
    rxmax_offset = hif->hw_config.rxpagenum * PAGE_LEN;
    rx_fifo_fw = rx_fw_ptr * 4;

    if ((hal_priv->rx_host_offset == rx_fifo_fw) || !hal_open)
    {
        if (print_cnt++ == 200) {
            AML_OUTPUT("rx_host_offset:%d, rx_fifo_fw:%d, hal_open:%d\n",
                       hal_priv->rx_host_offset, rx_fifo_fw, hal_open);
        }
        return;
    }

rx_drain_again:
    rx_fifo_fdt = hif->rx_fifo.FDT;
    rx_fifo_fdh = hif->rx_fifo.FDH;

    fifo_empty_size = CIRCLE_Subtract2(rx_fifo_fdh, rx_fifo_fdt,
                                       hif->rx_fifo.FDN);
    rx_total_len = CIRCLE_Subtract2(rx_fifo_fw, hal_priv->rx_host_offset,
                                    rxmax_offset);
    
    if (fifo_empty_size <= rx_total_len + FUNC6_BLKSIZE)
    {
        
        up(&hal_priv->rx_thread_sem);
        return ;
    }
    remain_len = rx_total_len;
    
    do
    {
        read_len = (remain_len > (READ_LEN_PER_ONCE + READ_BUFFERABLE_LEN)) ?
                    READ_LEN_PER_ONCE : remain_len;
        
        host_remain_len = hif->rx_fifo.FDN - rx_fifo_fdt;
        if (read_len >= host_remain_len)
        {
#if (SDIO_UPDATE_HOST_RDPTR != 0)
            struct sdio_func * func = aml_priv_to_func(SDIO_FUNC6);

            read_len = host_remain_len;
            host_wrapper_len = sdio_align_size(func, read_len) - read_len;
#else
            read_len = host_remain_len;
#endif
        }
        
        hif->hif_ops.hi_rcv_frame((hif->rx_fifo.FDB + rx_fifo_fdt),
                                  (unsigned char *)(SYS_TYPE)hal_priv->rx_host_offset,
                                  read_len);

#if (SDIO_UPDATE_HOST_RDPTR != 0)
        if (host_wrapper_len > 0)
        {
            
            if (host_wrapper_len > remain_len - read_len)
                host_wrapper_len = remain_len - read_len;

            memcpy(hif->rx_fifo.FDB, (hif->rx_fifo.FDB + hif->rx_fifo.FDN),
                   host_wrapper_len);
            read_len += host_wrapper_len;

            host_wrapper_len = 0;
        }
#endif

        rx_fifo_fdt = CIRCLE_Addition2(rx_fifo_fdt, read_len, hif->rx_fifo.FDN);

        hal_priv->rx_host_offset = CIRCLE_Addition2(hal_priv->rx_host_offset,
                                                    read_len, rxmax_offset);

        remain_len -= read_len;
    }
    while (remain_len > 0);

#if (SDIO_UPDATE_HOST_RDPTR == 0)
    
    {
        unsigned int write_data;

        write_data = BIT(31) | (hal_priv->rx_host_offset >> 2);
        hif->hif_ops.hi_write_word(RG_WIFI_IF_RXPAGE_BUF_RDPTR, write_data);
    }
#endif
    
    if (atomic_read(&hal_priv->drv_suspend_cnt) == 0)
    {
        
        WRITE_ONCE(hif->rx_fifo.FDT, CIRCLE_Addition2(hif->rx_fifo.FDT, rx_total_len,
                                            hif->rx_fifo.FDN));
        smp_wmb();
    }

    if (drain_iter++ < drain_iter_max) {
        unsigned int new_status;
        unsigned int new_rx_fw_ptr;

        new_status = hi_get_irq_status();
        new_rx_fw_ptr = (new_status & FW_RX_PTR_MASK) >> FW_RX_PTR_OFFSET;
        if (new_rx_fw_ptr * 4 != hal_priv->rx_host_offset) {
            rx_fw_ptr = new_rx_fw_ptr;
            rx_fifo_fw = rx_fw_ptr * 4;
            goto rx_drain_again;
        }
    }

    up(&hal_priv->rx_thread_sem);
}

void hi_irq_task(struct hal_private *hal_priv)
{
    unsigned int intr_status;
    unsigned char loop_count = 0;
    unsigned char int_loop_num = 0;
    
#if 1
    if (hal_priv->ps_host_state == 3)
    {
        aml_sdio_unmask_irq();
        return ;
    }
    if (w1_wifi_sdio_access == 0)
    {
        aml_sdio_unmask_irq();
        return ;
    }
#endif
    if (atomic_read(&hal_priv->drv_suspend_cnt) != 0)
    {
        aml_sdio_unmask_irq();
        return;
    }

    intr_status = hi_get_irq_status();
    hal_priv->int_status_copy = intr_status;

    loop_count += 1;
    if (intr_status & GOTO_WAKEUP_VID1)
    {
        hal_priv->sts_hirq[hirq_goto_wkp_vid1_idx]++;
        POWER_BEGIN_LOCK();
        if (hal_priv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
            hal_priv->hal_fw_ps_status = HAL_FW_IN_AWAKE;
        POWER_END_LOCK();
        hal_priv->hal_call_back->intr_gotowakeup(hal_priv->drv_priv, 1);
    }

    if (intr_status & GOTO_WAKEUP_VID0)
    {
        hal_priv->sts_hirq[hirq_goto_wkp_vid0_idx]++;
        POWER_BEGIN_LOCK();
        if (hal_priv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
            hal_priv->hal_fw_ps_status = HAL_FW_IN_AWAKE;
        POWER_END_LOCK();
        hal_priv->hal_call_back->intr_gotowakeup(hal_priv->drv_priv, 0);
    }

    if (intr_status & RX_OK)
    {
        unsigned int rx_fw_ptr = (intr_status & FW_RX_PTR_MASK) >>
                                 FW_RX_PTR_OFFSET;

        hal_priv->sts_hirq[hirq_rx_ok_idx]++;
        hi_soft_rx_irq(hal_priv, rx_fw_ptr);
        hal_priv->need_scheduling_tx = 1;
    }

    if (intr_status & TX_OK)
    {
        hal_priv->sts_hirq[hirq_tx_ok_idx]++;
        hi_soft_tx_irq();
        hal_priv->need_scheduling_tx = 1;
    }

    AML_TXLOCK_LOCK();
    if (hal_priv->need_scheduling_tx) {
        hal_tx_frame();
    }
    AML_TXLOCK_UNLOCK();

    if (intr_status & BEACON_SEND_OK_VID1)
    {
        hal_priv->sts_hirq[hirq_bcn_send_ok_vid1_idx]++;
        if (hal_priv->hal_call_back != NULL && hal_priv->hal_call_back->intr_bcn_send != NULL)
            hal_priv->hal_call_back->intr_bcn_send(hal_priv->drv_priv, 1);
    }
    else if (intr_status & BEACON_SEND_OK_VID0)
    {
        hal_priv->sts_hirq[hirq_bcn_send_ok_vid0_idx]++;
        if (hal_priv->hal_call_back != NULL && hal_priv->hal_call_back->intr_bcn_send != NULL)
            hal_priv->hal_call_back->intr_bcn_send(hal_priv->drv_priv, 0);
    }

    if (intr_status & P2P_OPPPS_CWEND_VID1)
    {
        hal_priv->sts_hirq[hirq_p2p_oppps_cwend_vid1_idx]++;
        hal_priv->hal_call_back->intr_p2p_opps_cwend(hal_priv->drv_priv, 1);
    }

    if (intr_status & P2P_NoA_START_VID1)
    {
        hal_priv->sts_hirq[hirq_p2p_noa_start_vid1_idx]++;
        hal_priv->hal_call_back->intr_p2p_noa_start(hal_priv->drv_priv, 1);
    }

    if (intr_status & FW_EVENT)
    {
        hi_soft_fw_event();
    }

    if (intr_status & BEACON_MISS_VID0)
    {
        POWER_BEGIN_LOCK();
        if (hal_priv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
            hal_priv->hal_fw_ps_status = HAL_FW_IN_AWAKE;
        POWER_END_LOCK();
        hal_priv->sts_hirq[hirq_bcn_miss_vid0_idx]++;
        hal_priv->hal_call_back->intr_beacon_miss_handle(hal_priv->drv_priv, 0);
    }

    if (intr_status & BEACON_MISS_VID1)
    {
        POWER_BEGIN_LOCK();
        if (hal_priv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
            hal_priv->hal_fw_ps_status = HAL_FW_IN_AWAKE;
        POWER_END_LOCK();
        hal_priv->sts_hirq[hirq_bcn_miss_vid1_idx]++;
        hal_priv->hal_call_back->intr_beacon_miss_handle(hal_priv->drv_priv, 1);
    }

    if (intr_status & COEX_BT_EXIST_INF0)
    {
           PRINT("--->BT work status change\n");
           hal_priv->hal_call_back->drv_intr_bt_info_change(hal_priv->drv_priv, 0, 0);
    }

     if (intr_status & COEX_INFO_BT_LK_INFO)
    {
        PRINT("--->BT link status change\n");
        hal_priv->hal_call_back->drv_intr_bt_info_change(hal_priv->drv_priv, 0, 1);
    }

    if (intr_status & TX_ERROR_IRQ)
    {
         hal_priv->sts_hirq[hirq_tx_err_idx]++;
        PRINT("--->TX_ERROR_IRQ\n");
        hi_clear_irq_status(SDIO_FW2HOST_EN);
        return;
    }

    if (intr_status & RX_ERROR_IRQ)
    {
        hal_priv->sts_hirq[hirq_rx_err_idx]++;
        PRINT("--->RX__ERROR_IRQ\n");
        hi_clear_irq_status(SDIO_FW2HOST_EN);
        return;
    }

    if (loop_count > int_loop_num)
    {
        unsigned int gpio_en;

        {
            unsigned int re_status, re_rx_fw_ptr;
            struct hal_private *p = hal_priv;
            int idle_polls = 0;
            
            int work_since_flush = 0;
            const int TX_FLUSH_EVERY = 4;
            int max_idle_polls =
                clamp_t(int, READ_ONCE(w522a_irq_idle_polls), 1, 64);
            int idle_sleep_min_us =
                clamp_t(int, READ_ONCE(w522a_irq_idle_sleep_min_us), 0, 5000);
            int idle_sleep_max_us =
                clamp_t(int, READ_ONCE(w522a_irq_idle_sleep_max_us),
                        idle_sleep_min_us, 10000);
            unsigned int iters = 0;
            unsigned int max_iters =
                clamp_t(unsigned int, READ_ONCE(w522a_irq_poll_max_iters),
                        1, 4096);

            while (iters++ < max_iters) {
                int did_work = 0;
                int tx_reaped = 0;
                re_status = hi_get_irq_status();
                re_rx_fw_ptr = (re_status & FW_RX_PTR_MASK) >>
                               FW_RX_PTR_OFFSET;
                if ((re_status & RX_OK) &&
                    re_rx_fw_ptr * 4 != p->rx_host_offset) {
                    
                    p->sts_hirq[hirq_rx_ok_idx]++;
                    hi_soft_rx_irq(p, re_rx_fw_ptr);
                    did_work = 1;
                }
                
                if (re_status & TX_OK) {
                    unsigned int reaped = hi_soft_tx_irq();
                    if (reaped) {
                        p->sts_hirq[hirq_tx_ok_idx]++;
                        did_work = 1;
                        tx_reaped = 1;
                    }
                }
                if (did_work) {
                    p->need_scheduling_tx = 1;
                    
                    if (tx_reaped || ++work_since_flush >= TX_FLUSH_EVERY) {
                        AML_TXLOCK_LOCK();
                        hal_tx_frame();
                        AML_TXLOCK_UNLOCK();
                        work_since_flush = 0;
                    }
                    idle_polls = 0;
                    continue;
                }
                
                if (work_since_flush) {
                    AML_TXLOCK_LOCK();
                    hal_tx_frame();
                    AML_TXLOCK_UNLOCK();
                    work_since_flush = 0;
                }
                if (++idle_polls >= max_idle_polls)
                    break;
                if (idle_sleep_max_us > 0)
                    usleep_range(idle_sleep_min_us, idle_sleep_max_us);
                else
                    cond_resched();
            }
            
            if (work_since_flush) {
                AML_TXLOCK_LOCK();
                hal_tx_frame();
                AML_TXLOCK_UNLOCK();
            }
        }

        gpio_en = SDIO_FW2HOST_GPIO_EN;
        hi_clear_irq_status(SDIO_FW2HOST_EN);
        return;
    }
    
}

extern void aml_sdio_set_card_irq(int enable);

void hi_top_task( unsigned long data)
{
    struct hal_private *hal_priv = (struct hal_private *)data;
    struct  hw_interface* hif = hif_get_hw_interface();

    hif->HiStatus.HiTask_enter_num++;
    hal_priv->need_scheduling_tx = 1;
    
    aml_sdio_set_card_irq(0);
    hi_irq_task(hal_priv);
    aml_sdio_set_card_irq(1);
    hif->HiStatus.HiTask_exit_num++;
}

static void hif_get_mac_sts(void)
{
    int i;
    struct hw_interface* hif = hif_get_hw_interface();
    struct hif_reg_info rd_info[HIF_RD_MAC_REG_MAX];

    rd_info[0].addr = RG_MAC_IRQ_STATUS_CNT0;
    rd_info[1].addr = RG_MAC_IRQ_STATUS_CNT1;
    rd_info[2].addr = RG_MAC_IRQ_STATUS_CNT2;
    rd_info[3].addr = RG_MAC_IRQ_STATUS_CNT3;

    rd_info[4].addr = RG_MAC_FRM_TYPE_CNT0;
    rd_info[5].addr = RG_MAC_FRM_TYPE_CNT1;
    rd_info[6].addr = RG_MAC_FRM_TYPE_CNT2;
    rd_info[7].addr = RG_MAC_FRM_TYPE_CNT3;

    rd_info[8].addr = RG_MAC_TX_STT_CHK;
    rd_info[9].addr = RG_MAC_TX_END_CHK;
    rd_info[10].addr = RG_MAC_TX_EN_CHK;
    rd_info[11].addr = RG_MAC_TX_ENWR_CHK;

    rd_info[12].addr = RG_MAC_RX_DATA_LOCAL_CNT;
    rd_info[13].addr = RG_MAC_RX_DATA_OTHER_CNT;
    rd_info[14].addr = RG_MAC_RX_DATA_MUTIL_CNT;

    rd_info[0].prt = "irq_cnt_0";
    rd_info[1].prt = "irq_cnt_1";
    rd_info[2].prt = "irq_cnt_2";
    rd_info[3].prt = "irq_cnt_3";

    rd_info[4].prt = "rx_rts";
    rd_info[5].prt = "rx_cts";
    rd_info[6].prt = "rx_ack";
    rd_info[7].prt = "rx_qos_data";

    rd_info[8].prt = "tx_stt_chk";
    rd_info[9].prt = "tx_end_chk";
    rd_info[10].prt = "tx_en_chk";
    rd_info[11].prt = "tx_enwr_chk";

    rd_info[12].prt = "rx_local";
    rd_info[13].prt = "rx_other";
    rd_info[14].prt = "rx_mult";

    for (i = 0; i < HIF_RD_MAC_REG_MAX; i++)
    {
        unsigned int rd_data;

        rd_data = hif->hif_ops.hi_read_word(rd_info[i].addr);

        if(rd_info[i].addr == RG_MAC_RX_DATA_LOCAL_CNT)
        {
            rd_data = (rd_data << 3 ) >> 3;
        }
        else if(rd_info[i].addr == RG_MAC_RX_DATA_OTHER_CNT ||
                rd_info[i].addr == RG_MAC_RX_DATA_MUTIL_CNT )
        {
            rd_data = (rd_data << 2 ) >> 2;
        }
        else if (rd_info[i].addr == RG_MAC_TX_STT_CHK ||
                 rd_info[i].addr == RG_MAC_TX_END_CHK ||
                 rd_info[i].addr == RG_MAC_TX_EN_CHK ||
                 rd_info[i].addr == RG_MAC_TX_ENWR_CHK)
        {
            rd_data = (rd_data << 6 ) >> 6;
        }

        pr_debug("%s, %d\n", rd_info[i].prt, rd_data);
    }
}

static void hif_get_phy_sts(void)
{
    struct hif_reg_info rd_info[HIF_RD_PHY_REG_MAX];
    int i;
    struct hw_interface *hif = hif_get_hw_interface();

    rd_info[0].addr = RG_AGC_STATE;
    rd_info[1].addr = RG_AGC_OB_IC_GAIN;
    rd_info[2].addr = RG_AGC_WATCH1;
    rd_info[3].addr = RG_AGC_WATCH2;

    rd_info[4].addr = RG_AGC_WATCH3;
    rd_info[5].addr = RG_AGC_WATCH4;
    rd_info[6].addr = RG_AGC_OB_ANT_NFLOOR;
    rd_info[7].addr = RG_AGC_OB_CCA_RES;

    rd_info[8].addr = RG_AGC_OB_CCA_COND01;
    rd_info[9].addr = RG_AGC_OB_CCA_COND23;

    for (i = 0; i < HIF_RD_PHY_REG_MAX; i++)
    {
        unsigned int rd_data;

        rd_data = hif->hif_ops.hi_read_word(rd_info[i].addr);
        if (rd_info[i].addr == RG_AGC_STATE)
        {
            struct sts_agc_state *agc_state = (struct sts_agc_state *)&rd_data;
            pr_debug("agc-state=0x%x,\n", agc_state->state);
        }

        if (rd_info[i].addr == RG_AGC_OB_IC_GAIN)
        {
            struct sts_agc_ob_ic_gain *agc_ob_ic_gain = (struct sts_agc_ob_ic_gain *)&rd_data;
            pr_debug("r_agc_fine_db=0x%x, r_rx_ic_gain=0x%x,\n",
                agc_ob_ic_gain->r_agc_fine_db, agc_ob_ic_gain->r_rx_ic_gain);
        }

        if (rd_info[i].addr == RG_AGC_WATCH1)
        {
            struct sts_agc_watch1 *agc_watch1 = (struct sts_agc_watch1 *)&rd_data;
            pr_debug("r_pow_sat_db=0x%x, r_primary20_db=0x%x,\n",
                   agc_watch1->r_pow_sat_db, agc_watch1->r_primary20_db);
        }

        if (rd_info[i].addr == RG_AGC_WATCH2)
        {
            struct sts_agc_watch2 *agc_watch2 = (struct sts_agc_watch2 *)&rd_data;
            pr_debug("r_primary40_db=0x%x, r_primary80_db=0x%x,\n",
                    agc_watch2->r_primary40_db, agc_watch2->r_primary80_db);
        }

        if (rd_info[i].addr == RG_AGC_WATCH3)
        {
            struct sts_agc_watch3 *agc_watch3 = (struct sts_agc_watch3 *)&rd_data;
            pr_debug("r_rssi0_db=0x%x, r_rssi1_db=0x%x,\n",
                    agc_watch3->r_rssi0_db, agc_watch3->r_rssi1_db);
        }

        if (rd_info[i].addr == RG_AGC_WATCH4)
        {
            struct sts_agc_watch4 *agc_watch4 = (struct sts_agc_watch4 *)&rd_data;

            pr_debug("r_rssi2_db=0x%x, r_rssi3_db=0x%x,\n",
                   agc_watch4->r_rssi2_db, agc_watch4->r_rssi3_db);
        }

        if (rd_info[i].addr == RG_AGC_OB_ANT_NFLOOR)
        {
            struct sts_ob_ant_nfloor *ob_ant_nfloor = (struct sts_ob_ant_nfloor *)&rd_data;
            pr_debug("r_ant_nfloor_qdb=0x%x, r_snr_qdb=0x%x,\n",
                   ob_ant_nfloor->r_ant_nfloor_qdb, ob_ant_nfloor->r_snr_qdb);
        }

        if (rd_info[i].addr == RG_AGC_OB_CCA_RES)
        {
            struct sts_ob_cca_res *ob_cca_res = (struct sts_ob_cca_res *)&rd_data;
            pr_debug("cca_cond8=0x%x, cca_cond_rdy=0x%x,\n",
                   ob_cca_res->cca_cond8, ob_cca_res->cca_cond_rdy);
        }

        if (rd_info[i].addr == RG_AGC_OB_CCA_COND01)
        {
            struct sts_ob_cca_cond01 *ob_cca_cond01 =  (struct sts_ob_cca_cond01 *)&rd_data;
            pr_debug("cca_cond0=0x%x, cca_cond1=0x%x,\n",
                        ob_cca_cond01->cca_cond0, ob_cca_cond01->cca_cond1);
        }

        if (rd_info[i].addr == RG_AGC_OB_CCA_COND23)
        {
            struct sts_ob_cca_cond23 *ob_cca_cond23 = (struct sts_ob_cca_cond23 *)&rd_data;
            pr_debug("cca_cond2=0x%x, cca_cond3=0x%x,\n",
                   ob_cca_cond23->cca_cond2, ob_cca_cond23->cca_cond3);
        }
    }
}

#define RG_MAC_RX_WPTR (WIFI_MAC + 0x1fc)
#define RG_MAC_RX_FRPTR (WIFI_MAC + 0x200)
#define RG_MAC_RX_HRPTR (WIFI_MAC + 0x204)

static void hif_get_hst_int_status(void)
{
       struct hal_private *hal_prv = hal_get_priv();

       pr_debug("fw2hst_int_status 0x%x  rx_dbuf_fw_rptr 0x%x\n",
              (hal_prv->int_status_copy>> 16) & 0xffff,
              hal_prv->int_status_copy & 0xffff);

}

void hif_get_sts(unsigned int op_code, unsigned int ctrl_code)
{

    if ((ctrl_code & STS_MOD_HIF) == STS_MOD_HIF)
    {
        struct  hw_interface *hif = hif_get_hw_interface();
        unsigned int buf_rd;
        unsigned int mac_wt;
        unsigned int fw_rd;
        unsigned int sdio_rd;

        pr_debug("\n--------hif statistic--------\n");
        buf_rd = hif->hif_ops.hi_read_word(RG_WIFI_IF_RXPAGE_BUF_RDPTR) ;
        mac_wt = hif->hif_ops.hi_read_word(RG_MAC_RX_WPTR);
        fw_rd = hif->hif_ops.hi_read_word(RG_MAC_RX_FRPTR);
        sdio_rd = hif->hif_ops.hi_read_word(RG_MAC_RX_HRPTR);

        if ((ctrl_code & STS_TYP_RX) == STS_TYP_RX)
        {
            pr_debug("rx_fifo: head %ld, tail %ld, total %ld\n",
                   hif->rx_fifo.FDH, hif->rx_fifo.FDT, hif->rx_fifo.FDN);

            pr_debug("rx by word: free %d, fw_to_do %d, sdio_to_mv %d, buf_rd_ptr 0x%x, mac_wt_ptr 0x%x, fw_rd_ptr 0x%x, sdio_rd_ptr 0x%x \n",
            CIRCLE_Subtract2(sdio_rd, mac_wt, DEFAULT_RXPAGENUM * PAGE_LEN / 4),
            CIRCLE_Subtract2(mac_wt, fw_rd, DEFAULT_RXPAGENUM * PAGE_LEN / 4),
            CIRCLE_Subtract2( fw_rd, sdio_rd,DEFAULT_RXPAGENUM * PAGE_LEN / 4),
            buf_rd, mac_wt, fw_rd, sdio_rd);

        }

        hif_get_hst_int_status();
        hif_get_mac_sts();
        hif_get_phy_sts();
    }
}

void hif_pt_rx_start(unsigned int qos)
{
    unsigned int chg_data = 0;
    struct data_rx_local_cnt_bits*  data_rx_local_cnt =NULL;
    struct data_rx_cnt_bits* data_rx_cnt =NULL;
    struct  cnt_ctrl_bits* cnt_irq_ctrl = NULL;

    struct hw_interface* hif = hif_get_hw_interface();

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_LOCAL_CNT);

    data_rx_local_cnt = ( struct data_rx_local_cnt_bits*)&chg_data;
    data_rx_local_cnt->data_local_cnt_qos = qos;
    data_rx_local_cnt->data_local_cnt_en = 1;
    data_rx_local_cnt->data_local_cnt_clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_LOCAL_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_OTHER_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 1;
    data_rx_cnt->clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_OTHER_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_MUTIL_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 1;
    data_rx_cnt->clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_MUTIL_CNT, chg_data);

   chg_data = hif->hif_ops.hi_read_word(RG_MAC_CNT_CTRL_FIQ);
   cnt_irq_ctrl = (struct  cnt_ctrl_bits* )&chg_data;
   cnt_irq_ctrl->type_idx3 = 32+23; 
   cnt_irq_ctrl->en3 = 1;
   cnt_irq_ctrl->clr3 = 0;
   hif->hif_ops.hi_write_word(RG_MAC_CNT_CTRL_FIQ, chg_data);
    mdelay(5);  
    hif_get_phy_sts();

}

void hif_pt_rx_stop(void)
{
     unsigned int chg_data = 0;
     unsigned int rx_ucast = 0;
     unsigned int rx_other = 0;
     unsigned int rx_mcast= 0;
     unsigned int rx_fcserr = 0;

    struct data_rx_local_cnt_bits*  data_rx_local_cnt =NULL;
    struct data_rx_cnt_bits* data_rx_cnt =NULL;
    struct  cnt_ctrl_bits* cnt_irq_ctrl = NULL;

    struct hw_interface* hif = hif_get_hw_interface();

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_LOCAL_CNT);
    data_rx_local_cnt = ( struct data_rx_local_cnt_bits*)&chg_data;
    data_rx_local_cnt->data_local_cnt_en = 0;
    data_rx_local_cnt->data_local_cnt_clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_LOCAL_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_OTHER_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 0;
    data_rx_cnt->clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_OTHER_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_MUTIL_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 0;
    data_rx_cnt->clr = 0;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_MUTIL_CNT, chg_data);

    rx_ucast = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_LOCAL_CNT);
    rx_other = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_OTHER_CNT);
    rx_mcast = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_MUTIL_CNT);
    rx_fcserr = hif->hif_ops.hi_read_word(RG_MAC_IRQ_STATUS_CNT3);

    pr_debug("rx_ucast=0x%x,rx_other=0x%x,rx_mcast=0x%x,rx_fcserr=0x%x,\n",
                    (rx_ucast<<3)>>3, (rx_other<<2)>>2, (rx_mcast<<2)>>2,rx_fcserr);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_LOCAL_CNT);
    data_rx_local_cnt = ( struct data_rx_local_cnt_bits*)&chg_data;
    data_rx_local_cnt->data_local_cnt_en = 1;
    data_rx_local_cnt->data_local_cnt_clr = 1;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_LOCAL_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_OTHER_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 1;
    data_rx_cnt->clr = 1;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_OTHER_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_RX_DATA_MUTIL_CNT);
    data_rx_cnt = (struct data_rx_cnt_bits*)&chg_data;
    data_rx_cnt->en = 1;
    data_rx_cnt->clr = 1;
    hif->hif_ops.hi_write_word(RG_MAC_RX_DATA_MUTIL_CNT, chg_data);

    chg_data = hif->hif_ops.hi_read_word(RG_MAC_CNT_CTRL_FIQ);
    cnt_irq_ctrl = (struct  cnt_ctrl_bits* )&chg_data;
    cnt_irq_ctrl->type_idx3 = 32 +23; 
    cnt_irq_ctrl->en3 = 1;
    cnt_irq_ctrl->clr3 = 1;
    hif->hif_ops.hi_write_word(RG_MAC_CNT_CTRL_FIQ, chg_data);
}
