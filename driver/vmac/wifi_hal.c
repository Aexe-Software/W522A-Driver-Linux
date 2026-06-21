
#include "wifi_hal_com.h"
#include "wifi_hal.h"
#include "wifi_hif.h"
#include "wifi_hal_cmd.h"
#include "wifi_hal_txdesc.h"
#include "wifi_hal_platform.h"
#if LINUX_VERSION_CODE > KERNEL_VERSION(4,12,0)
#include <uapi/linux/sched/types.h>
#endif
#include "wifi_debug.h"
#include "wifi_mac_com.h"
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include "rf_d_top_reg.h"
#include "dpd_memory_packet.h"
#include "wifi_common.h"
#include <linux/inetdevice.h>
#include <linux/sched.h>

/* Exported by the w1_sdio bus driver: serialize the shared FUNC5 banked-window
 * (RG_SCFG_FUNC5_BADDR_A) sequence so BT and the WiFi fwlog/DPD paths can't
 * clobber the bank base mid-access. */
extern void w1_func5_bank_lock(void);
extern void w1_func5_bank_unlock(void);

#define CHIP_ID_F "CHIP_ID=%04x%08x\n"
#define CHIP_ID_EFUASE_L 0x8
#define CHIP_ID_EFUASE_H 0x9

unsigned int   host_wake_w1_fail_cnt = 0;
unsigned int   tx_up_required = 0;

static int w522a_tx_page_reserve = 6;
module_param_named(tx_page_reserve, w522a_tx_page_reserve, int, 0644);
MODULE_PARM_DESC(tx_page_reserve,
    "W522A HAL TX pages reserved for small/control traffic under load");

static int w522a_tx_small_page_bypass = 4;
module_param_named(tx_small_page_bypass, w522a_tx_small_page_bypass, int, 0644);
MODULE_PARM_DESC(tx_small_page_bypass,
    "W522A max page count allowed to bypass the TX page reserve");

static int w522a_hi_irq_nice = 0;
module_param_named(hi_irq_nice, w522a_hi_irq_nice, int, 0644);
MODULE_PARM_DESC(hi_irq_nice,
    "W522A hi_irq kthread nice value (-10..10, default 0)");

extern struct aml_hal_call_backs * get_hal_call_back_table(void);
extern unsigned char *g_rx_fifo;
static int hal_dev_unreg(void);
static int hal_dev_reg(void * drv_priv);
struct hal_private g_hal_priv;
DEFINE_PER_CPU(unsigned long, aml_common_lock_flags);
struct hal_private *  hal_get_priv(void)
{
    return &g_hal_priv;
}

unsigned char hal_set_pn_win(unsigned long long *pn_win)
{
    unsigned char i, offset = 0;
    unsigned long long win;
    unsigned long long tmp;

    for (i = 0; i < 2; i++) {
        win = *(pn_win + i);

        while(win > 0) {
            if ((win & 1) == 0) {
                break;
            }
            win >>= 1;
            offset++;
        }

        if (offset < MAX_PN_WINDOW / 2) {
            break;
        }
    }

    if (offset == 0) {
        offset = 1;
    }

    if (offset > 0 && offset < MAX_PN_WINDOW / 2) {
        pn_win[0] >>= offset;
        tmp = pn_win[1] & ((1 << offset) - 1);
        pn_win[0] |= (tmp << (MAX_PN_WINDOW / 2 - offset));
        pn_win[1] >>= offset;
    } else if (offset >= MAX_PN_WINDOW / 2 && offset < MAX_PN_WINDOW) {
        pn_win[0] = pn_win[1] >> (offset - MAX_PN_WINDOW / 2);
        pn_win[1] = 0;
    } else {
        pn_win[0] = 0;
        pn_win[1] = 0;
    }

    return offset;
}

unsigned long long hal_check_dup_pn(unsigned long long *pn_win, unsigned int repcnt_diff)
{
    if (repcnt_diff > 0 && repcnt_diff <= MAX_PN_WINDOW / 2) {
        return pn_win[0] & (1 << (repcnt_diff - 1));
    } else if (repcnt_diff > MAX_PN_WINDOW / 2 && repcnt_diff <= MAX_PN_WINDOW) {
        return pn_win[1] & (1 << (repcnt_diff - MAX_PN_WINDOW / 2 - 1));
    } else {
        return 0;
    }
}

unsigned char hal_chk_replay_cnt(struct hal_private *hal_priv, HW_RxDescripter_bit *RxPrivHdr)
{
    int staid = 0;
    unsigned char wnet_vif_id = 0;
    unsigned char *PN = NULL;
    unsigned long long *repcnt_my[2];
    unsigned long long *repcnt_rx ;
    int ret = 0;
    struct aml_hal_call_backs *AmlmS_opt = hal_get_drv_func();
    HW_RxDescripter_bit *RxPrivHdr_bit = (HW_RxDescripter_bit *)RxPrivHdr;
    struct drv_private *drv_priv = (struct drv_private *)hal_priv->drv_priv;
    struct wlan_net_vif *wnet_vif = NULL;
    unsigned char offset = 0;
    unsigned char is_group = AML_UCAST_TYPE;
    unsigned int repcnt_diff = 0;

    if (WIFI_ADDR_ISGROUP(RxPrivHdr->data))
    {
        is_group = AML_MCAST_TYPE;
        wnet_vif_id =RxPrivHdr_bit->bssidmatch_id;
        PN = &hal_priv->mRepCnt[wnet_vif_id].rxPN[0];
        wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    }
    else
    {
        if (AmlmS_opt->get_stationid == NULL)
            return 1;

        ASSERT(AmlmS_opt->get_stationid != NULL);
        wnet_vif_id = RxPrivHdr_bit->RxA1match_id; 
        ret = AmlmS_opt->get_stationid(hal_priv->drv_priv, &RxPrivHdr->data[10], wnet_vif_id, &staid);

        if (ret < 0)
        {
            
            return false;
        }
        wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
        PN = &hal_priv->uRepCnt[wnet_vif_id][staid].rxPN[0][0];
    }

    switch (RxPrivHdr->RxDecryptType)
    {
        case RX_PHY_TKIP:
        case RX_PHY_CCMP:
            repcnt_my[is_group] = (unsigned long long *)PN;
            repcnt_rx = (unsigned long long *)RxPrivHdr->PN;
            
            if (*repcnt_my[is_group] < *repcnt_rx) {
                if (*repcnt_my[is_group] == 0 && *repcnt_rx > 0) {
                    *repcnt_my[is_group] = *repcnt_rx - 1;
                }

                if (*repcnt_my[is_group] == *repcnt_rx - 1
                    && wnet_vif->pn_window[is_group][0] == 0
                    && wnet_vif->pn_window[is_group][1] == 0) {
                    memcpy(PN, RxPrivHdr->PN, MAX_PN_LEN);

                } else {
                    repcnt_diff = (int)(*repcnt_rx - *repcnt_my[is_group]);
                    if (hal_check_dup_pn(wnet_vif->pn_window[is_group], repcnt_diff)) {
                        return false;
                    }

                    if (repcnt_diff > MAX_PN_WINDOW) {
                        offset = hal_set_pn_win(wnet_vif->pn_window[is_group]);
                        *repcnt_my[is_group] += offset;
                        repcnt_diff = (int)(*repcnt_rx - *repcnt_my[is_group]);
                    }

                    if (repcnt_diff > 0 && repcnt_diff <= MAX_PN_WINDOW / 2) {
                        wnet_vif->pn_window[is_group][0] |= (1 << (repcnt_diff - 1));
                        if (repcnt_diff == 1) {
                            offset = hal_set_pn_win(wnet_vif->pn_window[is_group]);
                            *repcnt_my[is_group] += offset;
                        }
                    } else if (repcnt_diff > MAX_PN_WINDOW / 2 && repcnt_diff <= MAX_PN_WINDOW){
                        wnet_vif->pn_window[is_group][1] |= (1 << (repcnt_diff - MAX_PN_WINDOW / 2 - 1));
                    }
                }

                return true;
            }
            else {
                return false;
            }
            break;
        case RX_PHY_WAPI:
            if (hal_wpi_chk_pn_increase(RxPrivHdr->PN, PN) == true) {
                memcpy(PN, RxPrivHdr->PN, MAX_PN_LEN);
                return true;
            }

            return false;
            break;
        case RX_PHY_WEP64:
        case RX_PHY_WEP128:
        default:
            break;
    }

    return true;
}

void hal_soft_rx_cs(struct hal_private *hal_priv, struct sk_buff *skb)
{
    HW_RxDescripter_bit *RxPrivHdr_bit;
    unsigned char wnet_vif_id;
    static unsigned char print_count = 0;
    unsigned char pkt_drop = 0;

    if (skb == NULL || hal_priv == NULL || hal_priv->hal_call_back == NULL)
    {
        if (skb != NULL)
            os_skb_free(skb);
        return;
    }

    if (os_skb_get_pktlen(skb) < sizeof(HW_RxDescripter_bit) ||
        os_skb_get_pktlen(skb) < sizeof(HW_RxDescripter_bit) + WIFINET_ADDR_LEN)
    {
        os_skb_free(skb);
        return;
    }

    RxPrivHdr_bit = (HW_RxDescripter_bit *)os_skb_data(skb);
    if (RxPrivHdr_bit->RxLength + sizeof(HW_RxDescripter_bit) > os_skb_get_pktlen(skb) ||
        os_skb_pull(skb, sizeof(HW_RxDescripter_bit)) == NULL)
    {
        os_skb_free(skb);
        return;
    }

    if (WIFI_ADDR_ISGROUP(RxPrivHdr_bit->data))
        wnet_vif_id = RxPrivHdr_bit->bssidmatch_id;
    else
        wnet_vif_id = RxPrivHdr_bit->RxA1match_id;

    if (wnet_vif_id >= WIFI_MAX_VID)
    {
        PRINT("hal_soft_rx_cs wnet_vif_id %d \n", wnet_vif_id);
        
        os_skb_free(skb);
        return;
    }

    if ((RxPrivHdr_bit->mic_err) || (RxPrivHdr_bit->keymiss_err) ||
        (RxPrivHdr_bit->icv_err) || (hal_priv->hal_call_back->intr_rx_handle == NULL))
    {
        if (!(print_count++ % 100))
            PRINT("mic_err = %d, keymiss_err %d wnet_vif_id %d\n",
                  RxPrivHdr_bit->mic_err, RxPrivHdr_bit->keymiss_err,
                  wnet_vif_id);

        if (RxPrivHdr_bit->mic_err) {
            if (hal_priv->hal_call_back->pmf_encrypt_pkt_handle != NULL) {
                pkt_drop = hal_priv->hal_call_back->pmf_encrypt_pkt_handle(hal_priv->drv_priv,
                                                                           skb, RxPrivHdr_bit->RxRSSI_ant0,
                                                                           RxPrivHdr_bit->RxRate,
                                                                           RxPrivHdr_bit->RxChannel,
                                                                           RxPrivHdr_bit->aggregation,
                                                                           wnet_vif_id,RxPrivHdr_bit->key_id);
            } else {
                pkt_drop = 1;
            }

            if (!hal_priv->rx_thread_quit && hal_priv->hal_call_back->mic_error_event != NULL) {
                hal_priv->hal_call_back->mic_error_event(hal_priv->drv_priv,
                                                         RxPrivHdr_bit->data,
                                                         WIFI_ADDR2(RxPrivHdr_bit->data),
                                                         wnet_vif_id);
            }
        } else {
            pkt_drop = 1;
        }

        if (pkt_drop) {
            os_skb_free(skb);
        }
        return;
    }
    else
    {
        if (RxPrivHdr_bit->RxDecryptType != RX_PHY_NOWEP)
        {
        }

        if (RxPrivHdr_bit->RxTcpCSUM_err && RxPrivHdr_bit->RxTcpCSUMCalculated &&
            RxPrivHdr_bit->RxIPCSUM_err &&  RxPrivHdr_bit->RxIPCSUMCalculated)
        {
            skb->ip_summed = CHECKSUM_UNNECESSARY;
        }
        else
        {
            skb->ip_summed = CHECKSUM_NONE;
        }

        hal_priv->hal_call_back->intr_rx_handle(hal_priv->drv_priv, skb,
                                                RxPrivHdr_bit->RxRSSI_ant0,
                                                RxPrivHdr_bit->RxRate,
                                                RxPrivHdr_bit->Channel_BW,
                                                RxPrivHdr_bit->RxChannel,
                                                RxPrivHdr_bit->aggregation,
                                                wnet_vif_id,RxPrivHdr_bit->key_id);
    }
}

static void hal_write_word(unsigned int addr,unsigned int data)
{
    struct  hw_interface *hif = hif_get_hw_interface();

#if 1
    
    if ((addr & 0xff000000) == 0x11000000)
        hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, (addr & ~0xff000000),
                                      (unsigned char)data);
    else
#endif
        hif->hif_ops.hi_write_word(addr, data);
}

static unsigned int hal_read_word(unsigned int addr)
{
    struct  hw_interface *hif = hif_get_hw_interface();
    unsigned int regdata;

#if 1
    
    if ((addr & 0xff000000) == 0x11000000)
        regdata = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, (addr & ~0xff000000));
    else
#endif
        regdata = hif->hif_ops.hi_read_word(addr);

    return regdata;
}

static void hal_bt_write_word(unsigned int addr,unsigned int data)
{
    struct hw_interface *hif = hif_get_hw_interface();

    hif->hif_ops.bt_hi_write_word(addr, data);
}

static unsigned int hal_bt_read_word(unsigned int addr)
{
    struct hw_interface *hif = hif_get_hw_interface();

    return hif->hif_ops.bt_hi_read_word(addr);
}

static void hal_pt_rx_start(unsigned int qos)
{
    struct hw_interface *hif = hif_get_hw_interface();

    hif->hif_ops.hif_pt_rx_start(qos);
}

static void hal_pt_rx_stop(void)
{
     struct  hw_interface *hif = hif_get_hw_interface();

     hif->hif_ops.hif_pt_rx_stop();
}

unsigned char hal_wake_fw_req(void)
{
    struct hal_private * halpriv = hal_get_priv();
    unsigned char host_sleep_req = 0;
    unsigned int loop = 0, wake_flag = 0;
#ifdef PROJECT_W1
    struct hw_interface* hif = hif_get_hw_interface();
#endif

    POWER_BEGIN_LOCK();
    if (halpriv->hal_fw_ps_status == HAL_FW_IN_ACTIVE)
    {
        
        host_wake_w1_fail_cnt = 0;
        POWER_END_LOCK();
        return 1;
    }

    if (atomic_read(&halpriv->drv_suspend_cnt) != 0)
    {
        POWER_END_LOCK();
        pr_debug("%s:%d, suspending, does not wake, fw st %d\n", __func__, __LINE__,
            halpriv->hal_fw_ps_status);
        return 0;
    }

    while (halpriv->hal_fw_ps_status != HAL_FW_IN_ACTIVE)
    {
        unsigned char host_req_status;
        unsigned char host_sleep_req;
        unsigned char fw_ps_st;
        unsigned char fw_sleep;

        fw_ps_st = halpriv->hal_ops.hal_get_fw_ps_status();
        fw_sleep = ((fw_ps_st & FW_SLEEP) != 0) ? 1 : 0;
        host_req_status = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ);
        host_sleep_req = ((host_req_status & HOST_SLEEP_REQ) != 0) ? 1 : 0;

        fw_ps_st = fw_ps_st & 0xF;
        if (fw_ps_st != PMU_ACT_MODE)
        {
            if (wake_flag == 0)
            {
                wake_flag = 1;
                
                OS_MDELAY(10);

                continue;
            }
            else if ((fw_ps_st == PMU_SLEEP_MODE) && (wake_flag == 1))
            {
                wake_flag = 2;
                hal_clear_fw_wake();
                hal_set_fw_wake();
                
                usleep_range(20, 40);
            }
        }

        if (host_sleep_req == 1)
        {
            host_req_status &= ~HOST_SLEEP_REQ;
            hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_HOST_REQ, host_req_status);
        }

        if ((fw_ps_st == PMU_ACT_MODE) && (fw_sleep == 0) && (host_sleep_req == 0))
        {
            if (wake_flag == 2)
            {
                hal_clear_fw_wake();
            }
            if (halpriv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
                halpriv->hal_fw_ps_status = HAL_FW_IN_AWAKE;
            
            host_wake_w1_fail_cnt = 0;
            POWER_END_LOCK();
            return 1;
        }

        loop++;
        if (loop < 1000)
        {
            
            usleep_range(200, 240);
        }
        else
        {
            POWER_END_LOCK();
            host_wake_w1_fail_cnt++;
            
            pr_warn("hal_wake_fw_req: timeout (fw_ps_st=0x%x fw_sleep=0x%x host_sleep_req=0x%x fail_cnt=%u)\n",
                    fw_ps_st, fw_sleep, host_sleep_req, host_wake_w1_fail_cnt);
            return 0;
        }
    }
    
    host_wake_w1_fail_cnt = 0;
    POWER_END_LOCK();
    return 1;
}

unsigned char hal_check_fw_wake(void)
{
    struct hal_private * halpriv = hal_get_priv();
    int loop_count = 0;

    POWER_BEGIN_LOCK();

    while (halpriv->hal_fw_ps_status == HAL_FW_IN_SLEEP)
    {
        unsigned int fw_ps_st;
        unsigned int fw_sleep;
        unsigned int host_sleep_req;

        POWER_END_LOCK();
        fw_ps_st =  halpriv->hal_ops.hal_get_fw_ps_status();
        fw_sleep = ((fw_ps_st & FW_SLEEP) != 0) ? 1 : 0;
        host_sleep_req = ((fw_ps_st & HOST_SLEEP_REQ) != 0) ? 1 : 0;

        fw_ps_st = fw_ps_st & 0xF;
        loop_count++;
        if (((fw_ps_st & 0xF) != PMU_ACT_MODE)
            || (fw_sleep == 1)
            )
        {
            
        }
        else
        {
            return 1;
        }
        if (loop_count > 2)
        {
            return 0;
        }
        POWER_BEGIN_LOCK();
    }

    POWER_END_LOCK();
    return 1;
}

unsigned char hal_get_fw_ps_status(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned char fw_ps_st;

    fw_ps_st = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, RG_SDIO_PMU_STATUS) & 0xFF;
    return fw_ps_st;
}

unsigned char hal_set_fw_wake(void)
{
    struct hw_interface* hif = hif_get_hw_interface();

    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_WAKE, BIT(0));
    return 0;
}

unsigned char hal_clear_fw_wake(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned char tmp = 0;

    tmp = hif->hif_ops.hi_bottom_read8(SDIO_FUNC1, RG_SDIO_PMU_WAKE);
    hif->hif_ops.hi_bottom_write8(SDIO_FUNC1, RG_SDIO_PMU_WAKE, (tmp & ~BIT(0)));
    return 0;
}

void hal_ops_attach(void)
{
    struct hal_private *hal_priv = hal_get_priv();

    DBG_ENTER();

    hal_priv->hal_ops.hal_fill_agg_start = hal_fill_agg_start;
    hal_priv->hal_ops.hal_fill_priv = hal_fill_priv;

    hal_priv->hal_ops.hal_get_agg_pend_cnt = hal_get_agg_pend_cnt;
    hal_priv->hal_ops.hal_calc_mpdu_page = hal_calc_mpdu_page;
    hal_priv->hal_ops.hal_get_agg_cnt = hal_get_agg_cnt;
    hal_priv->hal_ops.hal_get_priv_cnt = hal_get_priv_cnt;

    hal_priv->hal_ops.hal_init = hal_dev_reg;
    hal_priv->hal_ops.hal_exit =hal_dev_unreg;
    hal_priv->hal_ops.hal_probe = hal_probe;
    hal_priv->hal_ops.hal_open = hal_open;
    hal_priv->hal_ops.hal_close = hal_close;
    hal_priv->hal_ops.hal_tx_empty = hal_tx_empty;

    hal_priv->hal_ops.hal_get_sts = hal_get_sts;
    hal_priv->hal_ops.hal_call_task= hal_call_task;
    hal_priv->hal_ops.hal_reg_task= hal_reg_task;
    hal_priv->hal_ops.hal_tx_flush = hal_tx_flush;
    hal_priv->hal_ops.hal_txframe_pause = hal_txframe_pause;
    
    hal_priv->hal_ops.hal_set_suspend = hal_set_suspend;

    hal_priv->hal_ops.hal_get_config = hal_get_config;

    hal_priv->hal_ops.phy_set_chan_support_type = phy_set_chan_support_type;
    hal_priv->hal_ops.phy_set_chan_phy_type = phy_set_chan_phy_type;
    hal_priv->hal_ops.phy_set_cam_mode = phy_set_cam_mode;
    hal_priv->hal_ops.phy_set_dhcp = phy_set_dhcp;
    hal_priv->hal_ops.phy_switch_chan = phy_switch_chan;
    hal_priv->hal_ops.phy_set_rf_chan = phy_set_rf_chan;
    hal_priv->hal_ops.phy_set_mac_bssid = phy_set_mac_bssid;
    hal_priv->hal_ops.phy_set_mac_addr = phy_set_mac_addr;
    hal_priv->hal_ops.phy_vmac_disconnect = phy_vmac_disconnect;
    
    hal_priv->hal_ops.phy_enable_bcn = phy_enable_bcn;
    hal_priv->hal_ops.phy_set_bcn_buf = phy_set_bcn_buf;
    hal_priv->hal_ops.phy_update_bcn_intvl = phy_update_bcn_intvl;
    hal_priv->hal_ops.phy_set_bcn_intvl = phy_set_bcn_intvl;
    
    hal_priv->hal_ops.phy_rst_mcast_key = phy_rst_mcast_key;
    hal_priv->hal_ops.phy_rst_ucast_key = phy_rst_ucast_key;
    hal_priv->hal_ops.phy_rst_all_key = phy_rst_all_key;
    hal_priv->hal_ops.phy_clr_key = phy_clr_key;
    hal_priv->hal_ops.phy_set_ucast_key = phy_set_ucast_key;
    hal_priv->hal_ops.phy_set_mcast_key = phy_set_mcast_key;
    hal_priv->hal_ops.phy_set_rekey_data = phy_set_rekey_data;

    hal_priv->hal_ops.phy_set_preamble_type = phy_set_preamble_type;
    hal_priv->hal_ops.phy_register_sta_id = phy_register_sta_id;
    hal_priv->hal_ops.phy_unregister_sta_id = phy_unregister_sta_id;
    hal_priv->hal_ops.phy_unregister_all_sta_id = phy_unregister_all_sta_id;
    hal_priv->hal_ops.phy_addba_ok = phy_addba_ok;
    hal_priv->hal_ops.phy_delt_ba_ok = phy_delt_ba_ok;
    hal_priv->hal_ops.phy_get_extern_chan_status = phy_get_extern_chan_status;
    hal_priv->hal_ops.phy_set_lretry_limit = phy_set_lretry_limit;
    hal_priv->hal_ops.phy_set_sretry_limit = phy_set_sretry_limit;

    hal_priv->hal_ops.phy_scan_cmd = phy_scan_cmd;
    hal_priv->hal_ops.phy_set_tx_power_accord_rssi = phy_set_tx_power_accord_rssi;
    hal_priv->hal_ops.phy_set_channel_rssi = phy_set_channel_rssi;
    hal_priv->hal_ops.phy_pwr_save_mode = phy_pwr_save_mode;
    hal_priv->hal_ops.phy_set_rd_support = phy_set_rd_support;
    hal_priv->hal_ops.phy_set_txlive_time = phy_set_txlive_time;
    hal_priv->hal_ops.phy_set_slot_time = phy_set_slot_time;
    hal_priv->hal_ops.phy_set_ac_param = phy_set_ac_param;

    hal_priv->hal_ops.phy_set_p2p_opps_cwend_enable = phy_set_p2p_opps_cwend_enable;
    hal_priv->hal_ops.phy_set_p2p_noa_enable = phy_set_p2p_noa_enable;

    hal_priv->hal_ops.phy_send_null_data = phy_send_null_data;
    hal_priv->hal_ops.phy_keep_alive = phy_keep_alive;
    hal_priv->hal_ops.phy_set_beacon_miss = phy_set_beacon_miss;
    hal_priv->hal_ops.phy_set_vsdb = phy_set_vsdb;
    hal_priv->hal_ops.phy_set_arp_agent = phy_set_arp_agent;
    hal_priv->hal_ops.phy_set_pattern = phy_set_pattern;
    hal_priv->hal_ops.phy_set_suspend = phy_set_suspend;
    hal_priv->hal_ops.phy_get_rw_ptr = phy_get_rw_ptr;
    hal_priv->hal_ops.phy_get_tsf = phy_get_tsf;

    hal_priv->hal_ops.phy_set_bmfm_info = phy_set_bmfm_info;
    
    hal_priv->hal_ops.phy_set_coexist_en = phy_set_coexist_en;

    hal_priv->hal_ops.phy_set_coexist_max_miss_bcn = phy_set_coexist_max_miss_bcn;
    hal_priv->hal_ops.phy_set_coexist_req_timeslice_timeout_value = phy_set_coexist_req_timeslice_timeout_value;
    hal_priv->hal_ops.phy_set_coexist_not_grant_weight = phy_set_coexist_not_grant_weight;
    hal_priv->hal_ops.phy_set_coexist_max_not_grant_cnt = phy_set_coexist_max_not_grant_cnt;
    hal_priv->hal_ops.phy_set_coexist_scan_priority_range = phy_set_coexist_scan_priority_range;
    hal_priv->hal_ops.phy_set_coexist_be_bk_noqos_priority_range = phy_set_coexist_be_bk_noqos_priority_range;
    hal_priv->hal_ops.phy_coexist_config = phy_coexist_config;

    hal_priv->hal_ops.phy_send_ndp_announcement = phy_send_ndp_announcement;
    hal_priv->hal_ops.phy_interface_enable = phy_interface_enable;

    hal_priv->hal_ops.hal_write_word = hal_write_word;
    hal_priv->hal_ops.hal_read_word = hal_read_word;
    hal_priv->hal_ops.hal_bt_read_word = hal_bt_read_word;
    hal_priv->hal_ops.hal_bt_write_word = hal_bt_write_word;
    hal_priv->hal_ops.hal_pt_rx_start = hal_pt_rx_start;
    hal_priv->hal_ops.hal_pt_rx_stop = hal_pt_rx_stop;

    hal_priv->hal_ops.hal_get_fw_ps_status = hal_get_fw_ps_status;
    hal_priv->hal_ops.hal_check_fw_wake = hal_check_fw_wake;
    hal_priv->hal_ops.hal_set_fwlog_cmd = hal_set_fwlog_cmd;
    hal_priv->hal_ops.hal_cfg_cali_param = hal_cfg_cali_param;
    hal_priv->hal_ops.hal_cfg_txpwr_cffc_param = hal_cfg_txpwr_cffc_param;

#ifdef SDIO_BUILD_IN
    w1_host_wake_w1_req = hal_wake_fw_req;
    w1_host_suspend_req = aml_sdio_pm_suspend;
    w1_host_resume_req = aml_sdio_pm_resume;
#endif
    DBG_EXIT();
}

void hal_ops_detach(void)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_fill_agg_start = NULL;
    hal_priv->hal_ops.hal_fill_priv = NULL;
    hal_priv->hal_ops.hal_get_agg_cnt = NULL;
    hal_priv->hal_ops.hal_get_priv_cnt = NULL;
    hal_priv->hal_ops.hal_init = NULL;
    hal_priv->hal_ops.hal_exit = NULL;
    hal_priv->hal_ops.hal_probe = NULL;
    hal_priv->hal_ops.hal_open = NULL;
    hal_priv->hal_ops.hal_close = NULL;

    hal_priv->hal_ops.phy_enable_bcn = NULL;
    hal_priv->hal_ops.phy_set_bcn_buf = NULL;
    hal_priv->hal_ops.phy_switch_chan = NULL;
    hal_priv->hal_ops.phy_set_rf_chan = NULL;
    hal_priv->hal_ops.phy_set_mac_bssid = NULL;
    hal_priv->hal_ops.phy_set_mac_addr = NULL;
    hal_priv->hal_ops.phy_update_bcn_intvl = NULL;
    hal_priv->hal_ops.phy_rst_mcast_key = NULL;
    hal_priv->hal_ops.phy_rst_ucast_key = NULL;
    hal_priv->hal_ops.phy_rst_all_key = NULL;
    hal_priv->hal_ops.phy_clr_key = NULL;
    hal_priv->hal_ops.phy_set_ucast_key = NULL;
    hal_priv->hal_ops.phy_set_mcast_key = NULL;
    hal_priv->hal_ops.phy_set_rekey_data = NULL;
    hal_priv->hal_ops.phy_set_preamble_type = NULL;
    hal_priv->hal_ops.phy_register_sta_id = NULL;
    hal_priv->hal_ops.phy_unregister_sta_id = NULL;
    hal_priv->hal_ops.phy_unregister_all_sta_id = NULL;
    hal_priv->hal_ops.phy_addba_ok = NULL;
    hal_priv->hal_ops.phy_delt_ba_ok = NULL;
    hal_priv->hal_ops.phy_set_lretry_limit = NULL;
    hal_priv->hal_ops.phy_set_sretry_limit = NULL;
    hal_priv->hal_ops.phy_pwr_save_mode = NULL;
    hal_priv->hal_ops.phy_set_txlive_time = NULL;
    hal_priv->hal_ops.phy_send_null_data = NULL;
    hal_priv->hal_ops.phy_keep_alive = NULL;
    hal_priv->hal_ops.phy_set_beacon_miss = NULL;
    hal_priv->hal_ops.phy_set_vsdb = NULL;
    hal_priv->hal_ops.phy_set_arp_agent = NULL;
    hal_priv->hal_ops.phy_set_pattern = NULL;
    hal_priv->hal_ops.phy_set_suspend = NULL;
    hal_priv->hal_ops.hal_get_sts = NULL;
    hal_priv->hal_ops.phy_set_bmfm_info = NULL;
    hal_priv->hal_ops.phy_send_ndp_announcement = NULL;
    hal_priv->hal_ops.phy_interface_enable = NULL;
    hal_priv->hal_ops.hal_set_suspend = NULL;
    hal_priv->hal_call_back = NULL;

    hal_priv->hal_ops.hal_get_fw_ps_status = NULL;
    hal_priv->hal_ops.hal_check_fw_wake = NULL;
    hal_priv->hal_ops.hal_set_fwlog_cmd = NULL;
    hal_priv->hal_ops.hal_cfg_cali_param = NULL;

#ifdef SDIO_BUILD_IN
    w1_host_wake_w1_req = NULL;
    w1_host_suspend_req = NULL;
    w1_host_resume_req = NULL;
#endif
}

int hal_set_suspend(unsigned char enable)
{
    struct hw_interface *hif = hif_get_hw_interface();

    if (hif->hif_ops.hif_suspend != NULL)
        return hif->hif_ops.hif_suspend(enable);
    else
        return -1;
}

struct aml_hal_call_backs *hal_get_drv_func(void)
{
    struct hal_private *hal_priv = hal_get_priv();

    return hal_priv->hal_call_back;
}

enum irqreturn  hal_irq_top(int irq, void *dev_id)
{
    struct hal_private *hal_priv = hal_get_priv();
    if (hal_priv->bhalOpen == 0)
    {
        goto exit;
    }
    
    hal_priv->oob_irq = irq;
    if (!hal_priv->oob_irq_masked) {
        disable_irq_nosync(irq);
        hal_priv->oob_irq_masked = 1;
    }
    
    if (atomic_inc_return(&hal_priv->hi_irq_thread_sem_pending) <= MAX_SEM_CNT)
        up(&hal_priv->hi_irq_thread_sem);
    hal_priv->gpio_irq_cnt++;

exit:
    return IRQ_HANDLED;
}

void hal_pn_win_init(enum aml_key_type type, unsigned char wnet_vif_id)
{
    unsigned int LoopCnt;
    struct hal_private *hal_priv = hal_get_priv();
    struct drv_private *drv_priv = (struct drv_private *)hal_priv->drv_priv;
    struct wlan_net_vif *wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];

    for (LoopCnt = 0; LoopCnt < 2; LoopCnt++) {
        wnet_vif->pn_window[type][LoopCnt] = 0;
    }
}

void hal_urep_cnt_init(struct unicastReplayCnt *RepCnt, unsigned char encryType)
{
    unsigned int LoopCnt;

    if (encryType == WIFI_WPI)
    {
        unsigned char AP_upn[MAX_PN_LEN] = {0x37, 0x5c, 0x36, 0x5c, 0x36,
                                            0x5c, 0x36, 0x5c, 0x36, 0x5c,
                                            0x36, 0x5c, 0x36, 0x5c, 0x36,
                                            0x5c};
        unsigned char STA_upn[MAX_PN_LEN] = {0x36, 0x5c, 0x36, 0x5c, 0x36,
                                             0x5c, 0x36, 0x5c, 0x36, 0x5c,
                                             0x36, 0x5c, 0x36, 0x5c, 0x36,
                                             0x5c};

        if (wifi_conf_mib.dot11CamMode == MODE_STA)
        {
            memcpy(&RepCnt->txPN[TX_UNICAST_REPCNT_ID][0], STA_upn, MAX_PN_LEN);
        }
        else
        {
            memcpy(&RepCnt->txPN[TX_UNICAST_REPCNT_ID][0], AP_upn, MAX_PN_LEN);
        }
    }
    else
    {
        
        for (LoopCnt = 0; LoopCnt < MAX_TX_QUEUE; LoopCnt++)
        {
            memset(&RepCnt->txPN[LoopCnt][0], 0, MAX_PN_LEN);
            RepCnt->txPN[LoopCnt][0] = 1;
        }
    }
    for (LoopCnt = 0; LoopCnt < MAX_RX_QUEUE; LoopCnt++)
    {
        
        memset(&RepCnt->rxPN[LoopCnt][0], 0, MAX_PN_LEN);
    }

}

void hal_mrep_cnt_init(struct hal_private *halpriv, unsigned char wnet_vif_id,
                       unsigned char encryType)
{
    struct multicastReplayCnt *mRepCnt = &halpriv->mRepCnt[wnet_vif_id];

    if (encryType == WIFI_WPI)
    {
        unsigned char gpn[16]= {0x36, 0x5c, 0x36, 0x5c, 0x36, 0x5c,
                                0x36, 0x5c, 0x36, 0x5c, 0x36, 0x5c,
                                0x36, 0x5c, 0x36, 0x5c};
        
        memcpy(&mRepCnt->txPN[0], gpn, MAX_PN_LEN);
    }
    else
    {
        
        memset(&mRepCnt->txPN[0], 0, MAX_PN_LEN);
        mRepCnt->txPN[0] = 1;
    }
    
    memset(&mRepCnt->rxPN[0], 0, MAX_PN_LEN);
}

void hal_wpi_pn_self_plus(unsigned long long *WpiPN)
{
    if (WpiPN[0] == 0xFFFFFFFFFFFFFFFFull)
    {
        WpiPN[0] = 0x0ull;
        WpiPN[1] += 0x1ull;
    }
    else
    {
        WpiPN[0] += 0x1ull;
    }
}

void hal_wpi_pn_self_plus_plus(unsigned long long *WpiPN)
{

    if (WpiPN[0] == 0xFFFFFFFFFFFFFFFFull)
    {
        WpiPN[0] = 0x1ull;
        WpiPN[1] += 0x1ull;
    }
    else if (WpiPN[0] == 0xFFFFFFFFFFFFFFFEull)
    {
        WpiPN[0] = 0x0ull;
        WpiPN[1] += 0x1ull;
    }
    else
    {
        WpiPN[0] +=0x2ull;
    }
}

unsigned char hal_wpi_chk_pn_increase(unsigned char *RcvPN, unsigned char *SavedPn)
{
    int i;

    for (i= (MAX_PN_LEN - 1); i >= 0; i--)
    {
        if (RcvPN[i]>SavedPn[i])
        {
            return true;
        }
        else if (RcvPN[i]<SavedPn[i])
        {
            return false;
        }
    }

    return false;
}

void hal_free_txcmp_buf(struct hal_private *hal_priv)
{
    FREE(hal_priv->txcompletestatus, "hal_priv->txcompletestatus");
}

unsigned char hal_alloc_txcmp_buf( struct hal_private *hal_priv)
{
    
    hal_priv->txcompletestatus = (struct tx_complete_status *)ZMALLOC(sizeof(struct tx_complete_status),
                                                                      "hal_priv->txcompletestatus",
                                                                      GFP_KERNEL);
    if (hal_priv->txcompletestatus == NULL)
    {
        ERROR_DEBUG_OUT("alloc_err\n");
        return false;
    }
    return true;
}

void hal_free_fw_event_buf(struct hal_private *hal_priv)
{

    FREE(hal_priv->fw_event,"hal_priv->fw_event");
}

unsigned char hal_alloc_fw_event_buf( struct hal_private *hal_priv)
{
    
    hal_priv->fw_event = (struct fw_event_to_driver *)ZMALLOC(sizeof(struct fw_event_to_driver), "hal_priv->fw_event", GFP_KERNEL);
    if (hal_priv->fw_event == NULL)
    {
        ERROR_DEBUG_OUT("alloc_err\n");
        return false;
    }
    return true;
}

unsigned char hal_tx_empty()
{
    struct hw_interface* hif = hif_get_hw_interface();
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
    if ((hif->HiStatus.Tx_Send_num == hif->HiStatus.Tx_Free_num)
        &&(hif->HiStatus.Tx_Send_num ==hif->HiStatus.Tx_Done_num))
#else
    if ((atomic_read(&hif->HiStatus.Tx_Send_num) == atomic_read(&hif->HiStatus.Tx_Free_num))
        &&(atomic_read(&hif->HiStatus.Tx_Send_num) == atomic_read(&hif->HiStatus.Tx_Done_num)))
#endif
    {
        return true;
    }
    return  false;
}

int hal_is_empty_tx_id(struct hal_private * hal_priv)
{
    unsigned char id =0;

    while(id < WIFI_MAX_TXFRAME)
    {
        if(test_bit(id, hal_priv->tx_frames_map))
        {
            return -1;
        }
        id++;
    }
    return 0;
}

static unsigned char hal_get_free_tx_id_num(struct hal_private * hal_priv)
{
    unsigned char bit_num = 0;
    unsigned char i;
    unsigned long * map;

    if (hal_priv == NULL)
        return 0;

    COMMON_LOCK();
    map = hal_priv->tx_frames_map;
    for (i=0; i<(sizeof(unsigned long) *BITS_PER_BYTE); i++) {
        if (((*map >> i) & 0x01) == 0)
            bit_num++;
    }

    for (i=0; i<(sizeof(unsigned long) *BITS_PER_BYTE); i++) {
        if (((*(map+1) >> i) & 0x01) == 0)
            bit_num++;
    }
    COMMON_UNLOCK();
    return bit_num;
}

int hal_free_tx_id(struct hal_private * hal_priv, struct txdonestatus *txstatus,
    unsigned long *callback, unsigned char *queue_id)
{
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned char id;
    struct Tx_FrameDesc *pTxFrameDesc;
    int ret = 0;

    if (callback != NULL)
        *callback = 0;
    if (queue_id != NULL)
        *queue_id = 0;

    if (hal_priv == NULL || txstatus == NULL || callback == NULL ||
        queue_id == NULL || hif == NULL)
        return -1;

    id = txstatus->callbackid;
    *callback = 0;

    if (id >= WIFI_MAX_TXFRAME)
        return -1;

    COMMON_LOCK();
    if (test_and_clear_bit(id, hal_priv->tx_frames_map))
    {
        pTxFrameDesc = &hal_priv->tx_frames[id];
        hif->HiStatus.TX_SEND_OK_EVENT_num[pTxFrameDesc->txqueueid]++;
        *callback = pTxFrameDesc->callback;
        *queue_id = pTxFrameDesc->txqueueid;

        if (hal_priv->bhaltxdrop) {
            if (hal_is_empty_tx_id(hal_priv) == 0) {
                PRINT("%s id:%d, set bhaltxdrop = 0!\n", __func__, id);
                hal_priv->bhaltxdrop = 0;

            } else {
                PRINT("%s keep bhaltxdrop = 1, id:%d, %lx, %lx\n", __func__, id, hal_priv->tx_frames_map[0], hal_priv->tx_frames_map[1]);
            }
        }
    }
    else
    {
        pr_debug("free id:%d, %lx, %lx\n", id, hal_priv->tx_frames_map[0], hal_priv->tx_frames_map[1]);
        ret = -1;
    }
    COMMON_UNLOCK();

    return ret;
}

static int hal_alloc_tx_id(struct hal_private * hal_priv,struct fw_txdesc_fifo *pTxDescFiFo)
{
    int id;
    struct  hw_interface* hif = hif_get_hw_interface();

    COMMON_LOCK();
    id = find_first_zero_bit(hal_priv->tx_frames_map, WIFI_MAX_TXFRAME);

    if (id >= WIFI_MAX_TXFRAME)
    {
        id= TXID_INVALID;
        goto __alloc_fail;
    }

    set_bit(id, hal_priv->tx_frames_map);
    hal_priv->tx_frames[id].txqueueid= pTxDescFiFo->txqueueid;
    hal_priv->tx_frames[id].callback= pTxDescFiFo->callback;
    hal_priv->tx_frames[id].skb = NULL;
    hal_priv->tx_frames[id].valid = 1;
    hif->HiStatus.TX_REQ_EVENT_num[pTxDescFiFo->txqueueid]++;

__alloc_fail:
    COMMON_UNLOCK();

    return id;
}

void hal_txframe_pre(void)
{
    unsigned short STATUS;
    unsigned char *EltPtr = NULL;
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    struct _CO_SHARED_FIFO *pTxShareFifo =NULL;
    struct fw_txdesc_fifo *pTxDescFiFo = NULL;
    int txqueueid = 0;
    int id = 0;
    int loop = 2;
    int i = 0;

    txqueueid = hal_priv->tx_queueid_downloadok;

    do
    {
        for (i = HAL_NUM_TX_QUEUES-1; i >= 0; i--)
        {
            txqueueid--;
            if (txqueueid<0)
                txqueueid = HAL_NUM_TX_QUEUES-1;

            if ((hif->HiStatus.TX_DOWNLOAD_OK_EVENT_num[txqueueid]
                 - hif->HiStatus.TX_SEND_OK_EVENT_num[txqueueid]) > 16)
            {
                continue;
            }

            pTxShareFifo = &hal_priv->txds_trista_fifo[txqueueid];
            
            if (CO_SharedFifoEmpty(pTxShareFifo, CO_TX_BUFFER_MAKE))
            {
                if ((txqueueid == HAL_WME_NOQOS)
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
                    && (hif->HiStatus.Tx_Done_num - hif->HiStatus.Tx_Free_num > 32)) {
#else
                && (atomic_read(&hif->HiStatus.Tx_Done_num) - atomic_read(&hif->HiStatus.Tx_Free_num) > 32)) {
#endif
                    return;
                }
                EltPtr = CO_SharedFifoPick(pTxShareFifo, CO_TX_BUFFER_MAKE);
                pTxDescFiFo = (struct fw_txdesc_fifo *)EltPtr;

                id = hal_alloc_tx_id(hal_priv,pTxDescFiFo);
                if (id == TXID_INVALID)
                {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
                    if (hif->HiStatus.Tx_Done_num == hif->HiStatus.Tx_Free_num) {
#else
                    if (atomic_read(&hif->HiStatus.Tx_Done_num) == atomic_read(&hif->HiStatus.Tx_Free_num)) {
#endif
                        tx_up_required = 1;
                    }
                    return ;
                }
                pTxDescFiFo->pTxDPape->TxPriv.hostcallbackid= (unsigned char)id;

                STATUS = CO_SharedFifoGet(pTxShareFifo, CO_TX_BUFFER_MAKE, 1, &EltPtr);
                STATUS = CO_SharedFifoPut(pTxShareFifo, CO_TX_BUFFER_MAKE, 1);
            }
        }
    } while (--loop > 0);

    for (txqueueid = HAL_NUM_TX_QUEUES-1; txqueueid>=0; txqueueid--)
    {
        pTxShareFifo = &hal_priv->txds_trista_fifo[txqueueid];
        while (CO_SharedFifoEmpty(pTxShareFifo, CO_TX_BUFFER_MAKE))
        {
            EltPtr = CO_SharedFifoPick(pTxShareFifo,CO_TX_BUFFER_MAKE);
            pTxDescFiFo = ( struct fw_txdesc_fifo * )EltPtr;

            id = hal_alloc_tx_id(hal_priv,pTxDescFiFo);
            if (id == TXID_INVALID)
            {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
                if (hif->HiStatus.Tx_Done_num == hif->HiStatus.Tx_Free_num) {
#else
                if (atomic_read(&hif->HiStatus.Tx_Done_num) == atomic_read(&hif->HiStatus.Tx_Free_num)) {
#endif
                    tx_up_required = 1;
                }
                return ;
            }
            pTxDescFiFo->pTxDPape->TxPriv.hostcallbackid = id;
            STATUS = CO_SharedFifoGet(pTxShareFifo, CO_TX_BUFFER_MAKE, 1, &EltPtr);
            if (STATUS != CO_STATUS_OK)
                return;
            STATUS = CO_SharedFifoPut(pTxShareFifo, CO_TX_BUFFER_MAKE, 1);
            if (STATUS != CO_STATUS_OK)
                return;
        }
    }
}

static void hal_tx_complete(struct sk_buff * skb_buf)
{
    if (aml_wifi_is_enable_rf_test() && (skb_buf != NULL)) {
        hi_irq_task(hal_get_priv());
    }
}

void  hal_tx_frame(void)
{
    unsigned short STATUS;
    unsigned char *EltPtr = NULL;
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    struct _CO_SHARED_FIFO *pTxShareFifo = NULL;
    struct fw_txdesc_fifo *pTxDescFiFo = NULL;
    static unsigned char print_cnt = 0;

    int txqueueid = 0;
    struct hi_tx_desc *pTxDPape = NULL;
    unsigned char AggrNum = 0;
    unsigned int q_fifo_set_cnt = 0;
    unsigned int tx_page_need = 0;
    unsigned int tx_page_free = 0;
    unsigned int tx_page_reserve = 0;
    int tx_page_small = 0;
    int tx_page_priority = 0;
    int qid = -1;

    if (hal_priv->bhaltxdrop || hal_priv->bhalPowerSave) {
        if (print_cnt++ == 200) {
            pr_debug("%s bhaltxdrop:%d, bhalPowerSave:%d\n", __func__, hal_priv->bhaltxdrop, hal_priv->bhalPowerSave);
        }
        return;
    }
    AML_TXLOCK_UNLOCK();

    POWER_BEGIN_LOCK();
    if ((hal_priv->hal_drv_ps_status & HAL_DRV_IN_SLEEPING) != 0)
    {
        POWER_END_LOCK();
        AML_TXLOCK_LOCK();
        if (print_cnt++ == 200) {
            pr_debug("%s %d hal_drv_ps_status:%02x\n", __func__, __LINE__, hal_priv->hal_drv_ps_status);
        }
        return;
    }

    hal_priv->hal_drv_ps_status |= HAL_DRV_IN_ACTIVE;
    if (hal_priv->hal_fw_ps_status != HAL_FW_IN_ACTIVE)
    {
        hal_priv->hal_drv_ps_status &= ~HAL_DRV_IN_ACTIVE;
        POWER_END_LOCK();
        if (!hal_tx_empty())
            hal_priv->hal_call_back->drv_pwrsave_wake_req(hal_priv->drv_priv, 1);
        AML_TXLOCK_LOCK();

        if (print_cnt++ == 200) {
            pr_debug("%s %d hal_drv_ps_status:%02x\n", __func__, __LINE__, hal_priv->hal_drv_ps_status);
        }
        return;
    }
    POWER_END_LOCK();

    AML_TXLOCK_LOCK();
    hal_txframe_pre();
    for (txqueueid = HAL_NUM_TX_QUEUES-1; txqueueid>=0; txqueueid--)
    {
        unsigned int mpdu_num       = 0;
        unsigned int aggr_page_num  = 0;

        struct amlw_hif_scatter_req * scat_req = NULL;

        pTxShareFifo = &hal_priv->txds_trista_fifo[txqueueid];
        if (CO_SharedFifoEmpty( pTxShareFifo, CO_TX_BUFFER_SET))
        {
            scat_req = hif->hif_ops.hi_get_scatreq();
            if (scat_req != NULL)
            {
                scat_req->req = HIF_WRITE | HIF_ASYNCHRONOUS;
                scat_req->complete = hal_tx_complete;
                scat_req->addr = 0x0;
            }
            else
            {
                continue;
            }
        } else {
            continue;
        }

        pTxShareFifo = &hal_priv->txds_trista_fifo[txqueueid];
        while (CO_SharedFifoEmpty( pTxShareFifo, CO_TX_BUFFER_SET))
        {
            EltPtr      = CO_SharedFifoPick(pTxShareFifo,CO_TX_BUFFER_SET);
            pTxDescFiFo = (struct fw_txdesc_fifo *) EltPtr;
            pTxDPape    = pTxDescFiFo->pTxDPape;
            AggrNum     = pTxDPape->TxPriv.AggrNum;

            q_fifo_set_cnt = CO_SharedFifoNbEltCont(pTxShareFifo,CO_TX_BUFFER_SET);

            if (!pTxDPape->TxPriv.AggrNum || !pTxDPape->TxPriv.aggr_page_num || pTxDPape->TxPriv.TID > QUEUE_BEACON)
            {
                STATUS =CO_SharedFifoGet(pTxShareFifo,CO_TX_BUFFER_SET,1,&EltPtr);
                STATUS =CO_SharedFifoPut(pTxShareFifo,CO_TX_BUFFER_SET,1);
                PRINT("!!!error:set_cnt:%d, aggrnum:%d, aggr_page_num:%d, sn:%d, skb:%p, tid:%d, fc:0x%x\n", q_fifo_set_cnt, pTxDPape->TxPriv.AggrNum, pTxDPape->TxPriv.aggr_page_num,
                       pTxDPape->TxPriv.SN, pTxDescFiFo->ampduskb, pTxDPape->TxPriv.TID, pTxDPape->TxVector.tv_FrameControl);
                break;
            }
            tx_page_need = pTxDPape->TxPriv.aggr_page_num;
            tx_page_free = hal_priv->txPageFreeNum;
            tx_page_reserve = (w522a_tx_page_reserve > 0) ?
                (unsigned int)w522a_tx_page_reserve : 0;
            tx_page_small = (w522a_tx_small_page_bypass > 0) &&
                (tx_page_need <= (unsigned int)w522a_tx_small_page_bypass);
            tx_page_priority = (txqueueid == HAL_WME_MGT) ||
                (txqueueid == HAL_WME_NOQOS) ||
                (txqueueid == HAL_WME_MCAST) ||
                (txqueueid == HAL_WME_UAPSD) ||
                (txqueueid == HAL_WME_AC_VO);

            if ((tx_page_need <= tx_page_free)
                    && (tx_page_priority || tx_page_small ||
                        (tx_page_free >= tx_page_need + tx_page_reserve))
                    && (pTxDPape->TxPriv.AggrNum <= q_fifo_set_cnt)
                    && ((AggrNum + mpdu_num) < SDIO_MAXSG_SIZE)
                    && ((aggr_page_num + pTxDPape->TxPriv.aggr_page_num <= SG_PAGE_MAX) || (aggr_page_num == 0)))
            {
                aggr_page_num += pTxDPape->TxPriv.aggr_page_num;
                AML_PRINT(AML_DBG_MODULES_TX, "TxPriv.aggr_page_num:%d <= txPageFreeNum:%d, TxPriv.AggrNum:%d <= q_fifo_set_cnt:%d, "\
                    "AggrNum:%d + mpdu_num:%d, aggr_page_num:%d, len:%d, sn:%d(0x%04x)\n",
                    pTxDPape->TxPriv.aggr_page_num, hal_priv->txPageFreeNum, pTxDPape->TxPriv.AggrNum, q_fifo_set_cnt,
                    AggrNum, mpdu_num, aggr_page_num, pTxDPape->TxPriv.AggrLen, pTxDPape->TxPriv.SN, pTxDPape->TxPriv.SN);
                do
                {
                    hal_priv->txPageFreeNum -= pTxDPape->TxPriv.PageNum;
                    hif->HiStatus.TX_DOWNLOAD_OK_EVENT_num[txqueueid]++;
                    hal_priv->tx_queueid_downloadok =  txqueueid;

                    STATUS =CO_SharedFifoGet(pTxShareFifo,CO_TX_BUFFER_SET,1,&EltPtr);
                    if (STATUS != CO_STATUS_OK)
                    {
                        PRINT("!!!error:TxPriv.aggr_page_num:%d, txPageFreeNum:%d, TxPriv.AggrNum:%d, set_cnt:%d, "\
                        "AggrNum:%d, mpdu_num:%d, aggr_page_num:%d, aggrlen:%d, sn:%d, queueid:%d, sn:%d, skb:%p, tid:%d, fc:0x%x \n",
                        pTxDPape->TxPriv.aggr_page_num, hal_priv->txPageFreeNum, pTxDPape->TxPriv.AggrNum, q_fifo_set_cnt,
                        AggrNum, mpdu_num, aggr_page_num, pTxDPape->TxPriv.AggrLen, pTxDPape->TxPriv.SN, txqueueid,
                        pTxDPape->TxPriv.SN, pTxDescFiFo->ampduskb, pTxDPape->TxPriv.TID, pTxDPape->TxVector.tv_FrameControl);
                        break;
                    }

                    STATUS =CO_SharedFifoPut(pTxShareFifo,CO_TX_BUFFER_SET,1);
                    if (STATUS != CO_STATUS_OK)
                        break;

                   assign_tx_desc_pn(pTxDPape->TxOption.is_bc, pTxDPape->TxPriv.vid,
                        pTxDPape->TxPriv.StaId, pTxDPape, pTxDPape->TxOption.key_type);
                    ASSERT(scat_req->scat_count == mpdu_num);
                    scat_req->scat_list[mpdu_num].skbbuf = pTxDescFiFo->ampduskb; 
                    scat_req->scat_list[mpdu_num].packet = pTxDPape;
                    scat_req->scat_list[mpdu_num].page_num = pTxDPape->TxPriv.PageNum;
                    scat_req->scat_list[mpdu_num].len = HW_MPDU_LEN_GET(pTxDPape->MPDUBufFlag) + HI_TXDESC_DATAOFFSET;
                    scat_req->scat_count++;
                    scat_req->len += scat_req->scat_list[mpdu_num].len;
                    pTxDPape->TxOption.pkt_position = AML_PKT_NOT_IN_HAL;
                    pTxDescFiFo->ampduskb = NULL;
                    mpdu_num++;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
                    __sync_fetch_and_add(&hif->HiStatus.Tx_Done_num,1);
#else
                    atomic_add(1, &hif->HiStatus.Tx_Done_num);
#endif
                    EltPtr = CO_SharedFifoPick(pTxShareFifo,CO_TX_BUFFER_SET);
                    pTxDescFiFo = ( struct fw_txdesc_fifo *)EltPtr ;
                    pTxDPape = pTxDescFiFo->pTxDPape;
                }
                while (--AggrNum > 0);

            }
            else
            {
                AML_PRINT(AML_DBG_MODULES_TX, "not TxPriv.aggr_page_num:%d <= txPageFreeNum:%d, TxPriv.AggrNum:%d <= q_fifo_set_cnt:%d, "\
                    "AggrNum:%d + mpdu_num:%d, aggr_page_num:%d\n",
                    pTxDPape->TxPriv.aggr_page_num, hal_priv->txPageFreeNum,
                    pTxDPape->TxPriv.AggrNum, q_fifo_set_cnt, AggrNum, mpdu_num, aggr_page_num);
                if ((g_dbg_modules & AML_DBG_MODULES_HAL_TX) &&
                    !tx_page_priority && !tx_page_small &&
                    tx_page_need <= tx_page_free &&
                    tx_page_free < tx_page_need + tx_page_reserve) {
                    printk_ratelimited(KERN_INFO
                        "W522A: tx-reserve hold q=%d need=%u free=%u reserve=%u aggr=%u set=%u mpdu=%u\n",
                        txqueueid, tx_page_need, tx_page_free,
                        tx_page_reserve, AggrNum, q_fifo_set_cnt, mpdu_num);
                }
                break;
            }
        }
        if (mpdu_num > 0)
        {
            /* TX is the immediate signal that replies need low latency. */
            aml_sdio_poll_activity();
            AML_TXLOCK_UNLOCK();
            hif->hif_ops.hi_send_frame(scat_req);
            hal_priv->need_scheduling_tx = 0;
            AML_TXLOCK_LOCK();
            qid = txqueueid;
        }
    }

    AML_TXLOCK_UNLOCK();

    POWER_BEGIN_LOCK();
    hal_priv->hal_drv_ps_status &= ~HAL_DRV_IN_ACTIVE;
    POWER_END_LOCK();
    AML_TXLOCK_LOCK();

    hal_priv->need_scheduling_tx = 0;
}

int  hal_txframe_pause(int pause)
{
    struct hal_private * hal_priv = hal_get_priv();

    AML_TXLOCK_LOCK();
    if (pause)
    {
        hal_priv->bhalPowerSave = 1;
    }
    else
    {
        if (hal_priv->bhalPowerSave)
        {
            hal_priv->bhalPowerSave = 0;
            DPRINTF(AML_DEBUG_HAL, "%s %d continue\n", __func__,__LINE__);
        }
    }
    AML_TXLOCK_UNLOCK();
    return 0;
}

int hal_calc_mpdu_page (int mpdulen)
{
    return howmanypage(mpdulen+FW_TXDESC_DATAOFFSET, PAGE_LEN);
}

static int hal_calc_block_in_mpdu (int mpdulen)
{
    return (mpdulen + (PAGE_LEN - 1)) / PAGE_LEN;
}

struct sk_buff *hal_fill_agg_start(struct hi_agg_tx_desc *HI_AGG,struct hi_tx_priv_hdr *HI_TxPriv)
{
    unsigned short STATUS;
    unsigned char *EltPtr = NULL;
    struct sk_buff *skb = (struct sk_buff *)(HI_TxPriv->DMAADDR);
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();

    struct hi_tx_desc *pTxDPape = NULL;
    struct fw_txdesc_fifo *pTxDescFiFo = NULL;
    unsigned char txqueueid = 0;
    unsigned short frame_control;

    DBG_ENTER();
    ASSERT(HI_AGG->TID<WIFI_MAX_TXQUEUE_ID);
    ASSERT(HI_TxPriv->TX_STATUS == TX_DESCRIPTOR_STATUS_NEW);
    ASSERT(HI_TxPriv->Flag & WIFI_FIRST_BUFFER);
    ASSERT(HI_AGG->TxPrivNum <=16);
    ASSERT(HI_AGG->AGGR_len >= 18);
    ASSERT((((HI_AGG->FLAG >> WIFI_CHANNEL_BW_OFFSET) & 0x3)  == CHAN_BW_20M) ||
                 (((HI_AGG->FLAG >> WIFI_CHANNEL_BW_OFFSET) & 0x3) == CHAN_BW_40M)||
                 (((HI_AGG->FLAG >> WIFI_CHANNEL_BW_OFFSET) & 0x3) == CHAN_BW_80M));

    if (skb == NULL)
    {
        ERROR_DEBUG_OUT("alloc skb fail\n");
        return NULL;
    }

    txqueueid = HI_AGG->queue_id;
    
    STATUS = CO_SharedFifoGet(&hal_priv->txds_trista_fifo[txqueueid],CO_TX_BUFFER_GET,1,&EltPtr);
    if (STATUS != CO_STATUS_OK || EltPtr == NULL)
        return NULL;

    os_skb_push(skb, ALIGN(HI_TXDESC_DATAOFFSET, 8));
    pTxDPape = (struct hi_tx_desc *)os_skb_data(skb);
    memset(pTxDPape, 0, HI_TXDESC_DATAOFFSET);
    
    pTxDescFiFo = (struct fw_txdesc_fifo *)EltPtr;
    pTxDescFiFo->ampduskb = skb;

    pTxDescFiFo->callback = HI_TxPriv->hostreserve;
    pTxDescFiFo->txqueueid = txqueueid;
    pTxDescFiFo->pTxDPape = pTxDPape;
    pTxDPape->TxPriv.SN = HI_TxPriv->Seqnum;

    pTxDPape->TxPriv.PageNum = hal_calc_mpdu_page(HI_TxPriv->MPDULEN);
    pTxDPape->MPDUBufFlag = HW_FIRST_MPDUBUF_FLAG|HW_FIRST_AGG_FLAG|HW_LAST_MPDUBUF_FLAG;
    pTxDPape->MPDUBufFlag |= HW_MPDU_LEN_SET(HI_TxPriv->MPDULEN);
    pTxDPape->MPDUBufFlag |= HW_BUFFER_LEN_SET(HI_TxPriv->Delimiter);
    if ((HI_TxPriv->Flag & WIFI_MORE_AGG) == 0)
    {
        pTxDPape->MPDUBufFlag |= HW_LAST_AGG_FLAG;
    }

    hal_tx_desc_build(HI_AGG,HI_TxPriv,pTxDPape);
    frame_control = *(unsigned short *)(&pTxDPape->txdata[0]);
    if (g_dbg_modules & AML_DBG_MODULES_HAL_TX)
    {
        
        hal_show_txframe(pTxDPape);
    }

    pTxDPape->TxOption.pkt_position = AML_PKT_IN_HAL;
    hal_priv->Hi_TxAgg[txqueueid] = pTxDPape;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
    __sync_fetch_and_add(&hif->HiStatus.Tx_Send_num,1);
#else
    atomic_add(1, &hif->HiStatus.Tx_Send_num);
#endif
    STATUS = CO_SharedFifoPut(&hal_priv->txds_trista_fifo[txqueueid],CO_TX_BUFFER_GET,1);
    if (STATUS != CO_STATUS_OK)
        return NULL;

    DBG_EXIT();
    return skb;
}

struct sk_buff *hal_fill_priv(struct hi_tx_priv_hdr* HI_TxPriv,unsigned char queue_id)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned char *EltPtr = NULL;
    unsigned short STATUS;
    unsigned char txqueueid = 0;
    struct fw_txdesc_fifo *pTxDescFiFo = NULL;
    struct hi_tx_desc *pTxDPape = NULL;
    struct hi_tx_desc *pFirstTxDPape = NULL;
    struct sk_buff *skb = (struct sk_buff *)(HI_TxPriv->DMAADDR);

    DBG_ENTER();

    if (!skb)
    {
        ERROR_DEBUG_OUT("alloc skb fail\n");
        return NULL;
    }

    txqueueid = queue_id;
    pFirstTxDPape = (struct hi_tx_desc *)hal_priv->Hi_TxAgg[txqueueid];
    
    STATUS = CO_SharedFifoGet(&hal_priv->txds_trista_fifo[txqueueid],CO_TX_BUFFER_GET,1,&EltPtr);
    if (STATUS != CO_STATUS_OK || EltPtr == NULL)
        return NULL;
    os_skb_push(skb, ALIGN(HI_TXDESC_DATAOFFSET, 8));
    pTxDPape = (struct hi_tx_desc *)os_skb_data(skb);

    memset(pTxDPape, 0, HI_TXDESC_DATAOFFSET);
    
    pTxDescFiFo = ( struct fw_txdesc_fifo *)EltPtr ;
    pTxDescFiFo->ampduskb = skb;

    pTxDescFiFo->callback = HI_TxPriv->hostreserve;
    pTxDescFiFo->txqueueid = txqueueid;
    pTxDescFiFo->pTxDPape =pTxDPape;

    hal_tx_desc_build_sub(HI_TxPriv,pTxDPape, pFirstTxDPape);
    pTxDPape->TxOption.pkt_position = AML_PKT_IN_HAL;
    pTxDPape->TxPriv.SN = HI_TxPriv->Seqnum;
    if (g_dbg_modules & AML_DBG_MODULES_HAL_TX)
    {
        hal_show_txframe(pTxDPape);
    }
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
    __sync_fetch_and_add(&hif->HiStatus.Tx_Send_num,1);
#else
    atomic_add(1, &hif->HiStatus.Tx_Send_num);
#endif
    STATUS = CO_SharedFifoPut(&hal_priv->txds_trista_fifo[txqueueid],CO_TX_BUFFER_GET,1);
    if (STATUS != CO_STATUS_OK)
        return NULL;

    DBG_EXIT();
    return skb;
}

int hal_get_agg_cnt(unsigned char queue_id)
{
    
    return 3;
}

int hal_get_priv_cnt(unsigned char queue_id)
{
    struct hal_private* hal_priv = hal_get_priv();
    
    return CO_SharedFifoNbElt(&hal_priv->txds_trista_fifo[queue_id],CO_TX_BUFFER_GET);
}

int hal_get_agg_pend_cnt(void)
{
    struct hw_interface* hif = hif_get_hw_interface();
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
    return hif->HiStatus.Tx_Send_num - hif->HiStatus.Tx_Free_num;
#else
    return atomic_read(&hif->HiStatus.Tx_Send_num) - atomic_read(&hif->HiStatus.Tx_Free_num);
#endif
}

 int hal_tx_flush(unsigned char vid)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    struct txdonestatus txstatus;
    int id = 0;
    unsigned char queue_id = 0;
    int queueid = 0;
    struct _CO_SHARED_FIFO *pTxShareFifo = NULL;
    unsigned short STATUS;
    struct fw_txdesc_fifo *pTxDescFiFo = NULL;
    unsigned char *EltPtr = NULL;
    unsigned char *remain_eltptr[HI_AGG_TXD_NUM_PER_QUEUE];
    unsigned char remain_count = 0;
    unsigned char i = 0;
    unsigned long callback;

    memset(&txstatus, 0, sizeof(struct txdonestatus));
    hal_priv->bhaltxdrop = 0;

    COMMON_LOCK();
    if (hal_is_empty_tx_id(hal_priv) < 0) {
        pr_debug("set bhaltxdrop to 1, tx_frames_map:%lx, %lx, page:%d\n",
            hal_priv->tx_frames_map[0], hal_priv->tx_frames_map[1], hal_priv->txPageFreeNum);
        hal_priv->bhaltxdrop = 1;
    }
    COMMON_UNLOCK();

    AML_TXLOCK_LOCK();
    for (queueid = 0; queueid < HAL_NUM_TX_QUEUES; queueid++) {
        pTxShareFifo = &hal_priv->txds_trista_fifo[queueid];
        while (CO_SharedFifoEmpty(pTxShareFifo, CO_TX_BUFFER_MAKE)) {
            STATUS = CO_SharedFifoGet(pTxShareFifo, CO_TX_BUFFER_MAKE, 1, &EltPtr);
            pTxDescFiFo = (struct fw_txdesc_fifo *)EltPtr;

            if (!((pTxDescFiFo->pTxDPape->TxPriv.vid == vid) || (vid == 3))) {
                id = hal_alloc_tx_id(hal_priv,pTxDescFiFo);
                if (id == TXID_INVALID) {
                    pr_warn("%s warning, please handle this situation!!!\n", __func__);
                    break;
                }
                pTxDescFiFo->pTxDPape->TxPriv.hostcallbackid= (unsigned char)id;
            }

            STATUS = CO_SharedFifoPut(pTxShareFifo, CO_TX_BUFFER_MAKE, 1);
        }

        while (CO_SharedFifoEmpty(pTxShareFifo, CO_TX_BUFFER_SET)) {
            STATUS = CO_SharedFifoGet(pTxShareFifo, CO_TX_BUFFER_SET, 1, &EltPtr);
            pTxDescFiFo = (struct fw_txdesc_fifo *)EltPtr;

            if ((pTxDescFiFo->pTxDPape->TxPriv.vid == vid) || (vid == 3)) {
                txstatus.callbackid = pTxDescFiFo->pTxDPape->TxPriv.hostcallbackid;
                txstatus.txstatus = TX_DESCRIPTOR_STATUS_SUCCESS;

                hal_free_tx_id(hal_priv, &txstatus, &callback, &queue_id);
                hal_priv->hal_call_back->intr_tx_handle(hal_priv->drv_priv, &txstatus, pTxDescFiFo->callback, queue_id);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)) && !defined (LINUX_PLATFORM)
                __sync_fetch_and_add(&hif->HiStatus.Tx_Done_num,1);
                __sync_fetch_and_add(&hif->HiStatus.Tx_Free_num,1);
#else
                atomic_add(1,&hif->HiStatus.Tx_Done_num);
                atomic_add(1,&hif->HiStatus.Tx_Free_num);
#endif

            } else {
                remain_eltptr[remain_count++] = EltPtr;
            }
            STATUS = CO_SharedFifoPut(pTxShareFifo, CO_TX_BUFFER_SET, 1);
        }

        for (i = 0; i < remain_count; ++i) {
            STATUS = CO_SharedFifoGet(&hal_priv->txds_trista_fifo[queueid],CO_TX_BUFFER_GET, 1, &EltPtr);
            memcpy(EltPtr, remain_eltptr[i], sizeof(struct fw_txdesc_fifo));
            STATUS = CO_SharedFifoPut(&hal_priv->txds_trista_fifo[queueid],CO_TX_BUFFER_GET, 1);
        }

        remain_count = 0;
    }
    AML_TXLOCK_UNLOCK();

    if (hal_priv->bhaltxdrop == 1) {
        hal_priv->bhaltxdrop = 0;
        pr_debug("recover bhaltxdrop to 0, tx_frames_map:%lx, %lx, page:%d, vid:%d\n",
            hal_priv->tx_frames_map[0], hal_priv->tx_frames_map[1], hal_priv->txPageFreeNum, vid);
    }
    return 0;
}

int hal_open(void *drv_priv)
{
    struct hal_private *hal_priv = hal_get_priv();
    unsigned int to_sdio = 0;

    PRINT("hal_open++ \n");

#ifndef FW_RF_CALIBRATION
#ifdef RF_T9026
    if(!hal_priv->bRfInit){
        t9026_top();
        hal_priv->bRfInit = 1;
    }
#endif
#else
    hal_priv->bRfInit = 1;
#endif

    hal_priv->bhalOpen =1;
    PRINT("%s(%d) bhalOpen 0x%x\n",__func__,__LINE__, hal_priv->bhalOpen);

    to_sdio = 0xffffffff;

    hi_clear_irq_status(to_sdio);

    hal_reinit_priv();
    PRINT("hal_open-- && clear irq 0x60 \n");
    return 1;
}

void hal_get_sts(unsigned int op_code, unsigned int ctrl_code)
{
    char * hirq_prt_info[] = {
                "hirq_tx_err",
                "hirq_rx_err",
                "hirq_rx_ok",
                "hirq_tx_ok",
                "hirq_bcn_send_ok_vid0"	,
                "hirq_bcn_send_ok_vid1"	,
                "hirq_goto_wkp_vid0"		,
                "hirq_goto_wkp_vid1"		,
                "hirq_p2p_oppps_cwend_vid0",
                "hirq_p2p_oppps_cwend_vid1",
                "hirq_p2p_noa_start_vid0"	,
                "hirq_p2p_noa_start_vid1"	,
                "hirq_bcn_recv_ok_vid0"	,
                "hirq_bcn_recv_ok_vid1"	,
                "hirq_bcn_miss_vid0"		,
                "hirq_bcn_miss_vid1"		,
                "NULL"
    };

    int i ;
    struct hal_private* hal_priv = hal_get_priv();
    struct hw_interface* hif = hif_get_hw_interface();
    struct _CO_SHARED_FIFO * tx_share_fifo = NULL;

    pr_debug("\n--------hal statistic--------\n");

    if((ctrl_code & STS_MOD_HAL) == STS_MOD_HAL)
    {
        if ((ctrl_code & STS_TYP_TX) == STS_TYP_TX)
        {
            pr_debug("en_beacon[0]=%d\nen_beacon[1]=%d\n", hal_priv->sts_en_bcn[0], hal_priv->sts_en_bcn[1]);
            pr_debug("dis_beacon[0]=%d\ndis_beacon[1]=%d\n", hal_priv->sts_dis_bcn[0], hal_priv->sts_dis_bcn[1]);

            pr_debug("hal:tx_free_page %d \n", hal_priv->txPageFreeNum);
            pr_debug("hal:tx_ok_num:%d, tx_fail_num:%d\n", hif->HiStatus.tx_ok_num, hif->HiStatus.tx_fail_num);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
            pr_debug("hal:send_frm:%d, done_frm:%d, free_frm:%d\n",  hif->HiStatus.Tx_Send_num, hif->HiStatus.Tx_Done_num, hif->HiStatus.Tx_Free_num);
#else
            pr_debug("hal:send_frm:%d, done_frm:%d, free_frm:%d\n",
                   atomic_read(&hif->HiStatus.Tx_Send_num),
                   atomic_read(&hif->HiStatus.Tx_Done_num),
                   atomic_read(&hif->HiStatus.Tx_Free_num));
#endif
            pr_debug("hal:gpio irq cnt %d \n",  hal_priv->gpio_irq_cnt);
            pr_debug("tx_cmp: tx_done_frm %d, tx_mng_frm %d, tx_page %d\n",
                hal_priv->txcompletestatus->txdoneframecounter,
                hal_priv->txcompletestatus->txmanageframecounter,
                hal_priv->txcompletestatus->txpagecounter);

            for(i = 0; i < HAL_NUM_TX_QUEUES; i++)
            {
                tx_share_fifo = &hal_priv->txds_trista_fifo[i];
                pr_debug("txds_trista_fifo[%d]: make %d, set %d, get %d\n",
                    i,
                    CO_SharedFifoNbElt(tx_share_fifo, CO_TX_BUFFER_MAKE),
                    CO_SharedFifoNbElt(tx_share_fifo, CO_TX_BUFFER_SET),
                    CO_SharedFifoNbElt(tx_share_fifo, CO_TX_BUFFER_GET));
            }

            for(i = 0; i < hirq_max_idx; i++)
            {
                pr_debug("%s, %d\n", hirq_prt_info[i], hal_priv->sts_hirq[i]);
            }
        }
    }

    hif->hif_ops.hif_get_sts(op_code, ctrl_code);
}

int hal_close(void *drv_priv)
{
    struct hal_private *hal_priv = hal_get_priv();
    unsigned int to_sdio = 0;
    hal_priv->bhalOpen = 0;

    to_sdio = 0x00000000;
    hi_clear_irq_status(to_sdio);
    pr_debug("%s(%d) hal_priv->bhalOpen 0x%x\n", __func__, __LINE__, hal_priv->bhalOpen);
    return 1;
}

extern int drv_add_wnet_vif(struct drv_private *drv_priv, int wnet_vif_id, void * if_data, enum hal_op_mode vm_opmode, unsigned char *myaddr, unsigned int ip);
extern int wifi_mac_set_tx_power_coefficient(struct drv_private *drv_priv, struct wifi_channel *chan, int tx_power_plan);
extern int drv_rate_setup(struct drv_private *drv_priv, enum wifi_mac_macmode mode);
extern int drv_channel_init(struct drv_private *drv_priv, unsigned int cc);
extern void aml_set_mac_control_register(void);

int cmp_vid(struct drv_private *drv_priv)
{
    struct wlan_net_vif *wnet_vif = NULL;
    unsigned char myaddr[WIFINET_ADDR_LEN] = { mac_addr0, mac_addr1, mac_addr2, mac_addr3, mac_addr4, mac_addr5 };
    unsigned int ipv4 = 0;

    if (drv_priv == NULL)
        return -EINVAL;

    wnet_vif = drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];
    if (wnet_vif == NULL) {
        ERROR_DEBUG_OUT("main vmac is NULL, skip fw repair vif restore\n");
        return -ENODEV;
    }
    myaddr[2] += NET80211_MAIN_VMAC<<4;
    drv_add_wnet_vif(drv_priv, NET80211_MAIN_VMAC, wnet_vif,
        wifi_mac_opmode_2_halmode(wnet_vif->vm_opmode), myaddr, ipv4);

    wnet_vif = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];
    if (wnet_vif == NULL)
        return 0;

    myaddr[2] += NET80211_P2P_VMAC<<4;
    if (wnet_vif->vm_opmode == WIFINET_M_P2P_DEV) {
        drv_add_wnet_vif(drv_priv, NET80211_P2P_VMAC, wnet_vif, wifi_mac_opmode_2_halmode(WIFINET_M_STA),  myaddr, ipv4);
    } else if (wnet_vif->vm_opmode == WIFINET_M_HOSTAP) {
        drv_add_wnet_vif(drv_priv, NET80211_P2P_VMAC, wnet_vif, wifi_mac_opmode_2_halmode(WIFINET_M_HOSTAP),  myaddr, ipv4);
    } else {
        
        ERROR_DEBUG_OUT("error opmode %d\n", wnet_vif->vm_opmode);
    }

    return 0;
}

int hal_fw_repair(void)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct drv_private *drv_priv = drv_get_drv_priv();

    if (hal_priv == NULL || drv_priv == NULL || drv_priv->net_ops == NULL)
        return -ENODEV;

    hal_tx_flush(0);
    hal_download_wifi_fw_img();
    hal_host_init(hal_priv);

    drv_set_config((void *)drv_priv, CHIP_PARAM_RETRY_LIMIT, 100 << 8 | 100);
    wifi_mac_set_tx_power_coefficient(drv_priv, NULL, drv_priv->drv_config.cfg_txpoweplan);

    if (drv_priv->net_ops->wifi_mac_rate_ratmod_detach != NULL)
        drv_priv->net_ops->wifi_mac_rate_ratmod_detach(drv_priv);
    if (drv_priv->net_ops->wifi_mac_rate_ratmod_attach != NULL)
        drv_priv->net_ops->wifi_mac_rate_ratmod_attach(drv_priv);

    drv_rate_setup(drv_priv, WIFINET_MODE_11GNAC);
    aml_set_mac_control_register();
    cmp_vid(drv_priv);
    if (drv_priv->drv_ops.drv_set_bmfm_info != NULL)
        drv_priv->drv_ops.drv_set_bmfm_info(drv_priv, 0, 0, 0, 0);
    drv_hal_enable_coexist(drv_priv->drv_config.cfg_wifi_bt_coexist_support);

    return 0;
}

int hal_probe(void)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->bWpaMicMeasureEnable =1;
    OS_INIT_TQUEUE(&hal_priv->hi_tasktq, hi_top_task, (unsigned long)hal_priv);
    hal_priv->use_irq = 0;
    hal_priv->tx_queueid_downloadok =0;

    while(hal_priv->hst_if_init_ok == 0)
    {
        ERROR_DEBUG_OUT("------------->host interface init failed! \n");
        goto __exit_err;
    }

    if(hal_download_wifi_fw_img() != true)
    {
        goto __exit_err;
    }

    if(hal_host_init(hal_priv) != true)
    {
        goto __exit_err;
    }

    hal_tx_desc_init();

    hal_priv->bhalOpen =0;
    hal_priv->bhalProbelok = 1;
    hal_priv->beaconaddr[0]=0;
    hal_priv->beaconaddr[1]=0;
    pr_debug("%s(%d) hal_priv->bhalOpen 0x%x\n", __func__, __LINE__, hal_priv->bhalOpen);
    return true;

__exit_err:
    return false;
}

static int hal_get_did(struct hw_interface* hif)
{
    unsigned int Wifi_DeviceID = 0;
    int delay_ms = 0;
    int ret = false;

    PRINT("Wifi_DeviceID = %x\n",Wifi_DeviceID);
    while ((Wifi_DeviceID != PRODUCT_AMLOGIC) && (delay_ms < HI_FI_SYNC_DELAY_MS)) {
        Wifi_DeviceID = hi_get_device_id();
        OS_MDELAY( HI_FI_SYNC_DELAY_MS_STEP);
        delay_ms += HI_FI_SYNC_DELAY_MS_STEP;
    }
    if (Wifi_DeviceID == PRODUCT_AMLOGIC) {
        ret = true;
    }
    hif->Wifi_DeviceID = Wifi_DeviceID;
    return ret;
}

static int hal_get_vid(struct hw_interface* hif)
{
    unsigned int Wifi_VendorID = 0;
    int delay_ms = 0;
    int ret = false;
    PRINT("Wifi_DeviceID = %x\n",Wifi_VendorID);
    while ((Wifi_VendorID != VENDOR_AMLOGIC) && (delay_ms < HI_FI_SYNC_DELAY_MS)) {
        Wifi_VendorID = hi_get_vendor_id();
        OS_MDELAY( HI_FI_SYNC_DELAY_MS_STEP);
        delay_ms += HI_FI_SYNC_DELAY_MS_STEP;
        PRINT("Wifi_DeviceID = %x  already delayed=%dms\n",Wifi_VendorID, delay_ms);
    }
    if (Wifi_VendorID == PRODUCT_AMLOGIC) {
        ret = true;
    }
    hif->Wifi_VendorID = Wifi_VendorID;
    return ret;
}

extern unsigned int efuse_manual_read(unsigned int addr);
static int hal_get_chip_id(void)
{
    int ret = false;
#ifdef CONFIG_MAC_SUPPORT
    unsigned int chip_id_l = 0;
    unsigned int chip_id_h = 0;
    unsigned char chip_id_buf[23];

    chip_id_l = efuse_manual_read(CHIP_ID_EFUASE_L);
    pr_debug("efuse addr:%08x, chip_id is :%08x\n", CHIP_ID_EFUASE_L, chip_id_l);
    chip_id_h = efuse_manual_read(CHIP_ID_EFUASE_H);
    pr_debug("efuse addr:%08x, chip_id is :%08x\n", CHIP_ID_EFUASE_H, chip_id_h);

    sprintf(chip_id_buf, CHIP_ID_F, chip_id_h & 0xffff, chip_id_l);
    if (aml_store_to_file(WIFIMAC_PATH, chip_id_buf, strlen(chip_id_buf)) > 0) {
        pr_debug("write the chip_id to wifimac.txt \n");
        ret = true;
    }
#endif
    return ret;
}

int hal_host_init(struct hal_private *hal_priv)
{
    int queueid;
    struct hw_interface* hif;
    Irq_Time Irq_Time;
    int reg_tmp;
    int ret;
    unsigned int to_sdio = 0;
    int rfmode = HOST_VERSION;

    hif = hif_get_hw_interface();

    ret = hal_get_did(hif);
    if (ret == false) {
        goto exit;
    }

    hif->CmdDownFifo.FDH = 0;
    hif->CmdDownFifo.FDT = 0;
    hif->CmdDownFifo.FDN = hif->hif_ops.hi_read_word(CMD_DOWN_FIFO_FDN_ADDR);
    hif->CmdDownFifo.FDB = hif->hif_ops.hi_read_word(CMD_DOWN_FIFO_FDB_ADDR);
    hif->CmdDownFifo.FCB = CMD_DOWN_FIFO_CTRL_ADDR;
    hif->CmdDownFifo.FDL = APP_CMD_PERFIFO_LEN;

    hi_rx_fifo_init();

    for (queueid=0; queueid < HAL_NUM_TX_QUEUES; queueid++) {
        CO_SharedFifoInit(&(hal_priv->txds_trista_fifo[queueid]),
            (SYS_TYPE)&hal_priv->txds_trista_fifo_buf[HI_AGG_TXD_NUM_PER_QUEUE *queueid],
            (void *)&hal_priv->txds_trista_fifo_buf[HI_AGG_TXD_NUM_PER_QUEUE *queueid],
            HI_AGG_TXD_NUM_PER_QUEUE, sizeof(struct fw_txdesc_fifo), CO_SF_BLOCK_TX_NBR);
    }

    memset((unsigned char *)&hif->HiStatus,0,sizeof(struct hi_status));

    hif->hif_ops.hi_write_sram((unsigned char*)&rfmode,
        (unsigned char*)HOST_VERSION_ADDR, sizeof(rfmode));

    Irq_Time.MaxTimerOut = 200;  
    Irq_Time.MinInterval = 1;  

    hif->hif_ops.hi_write_sram((unsigned char*)&Irq_Time,
        (unsigned char*)FW_IRQ_TIME_ADDR, sizeof(Irq_Time));

    hal_get_vid(hif);
    hal_get_chip_id();

    set_wifi_baudrate(UART_WORK_CLK);

    hif->hif_ops.hi_read_sram((unsigned char *)&hif->hw_config,
        (unsigned char *)HW_CONFIG_ADDR,sizeof(HW_CONFIG));
    hal_priv->txPageFreeNum = hif->hw_config.txpagenum;

    PRINT("hal_priv->beaconframeaddress  0x%x \n",hif->hw_config.beaconframeaddress);
    PRINT("hal_priv->rxframeaddress     0x%x \n",hif->hw_config.rxframeaddress);
    PRINT("hal_priv->txcompleteaddress  0x%x \n",hif->hw_config.txcompleteaddress);
    PRINT("hal_priv->txPageFreeNum 0x%x \n",hif->hw_config.txpagenum );

    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC6, (SYS_TYPE)hif->hw_config.rxframeaddress);

    reg_tmp = hif->hif_ops.hi_read_word(RG_SDIO_IF_MISC_CTRL);

    reg_tmp |= BIT(8);
#if (SDIO_UPDATE_HOST_WRPTR == 0)
    reg_tmp |= BIT(9);
#endif
#if RTL_WRAPPER_AROUND_ADDR
    reg_tmp |= BIT(16);
#endif

    reg_tmp |= BIT(23) | BIT(24);
    hif->hif_ops.hi_write_word(RG_SDIO_IF_MISC_CTRL, reg_tmp);

    reg_tmp = hif->hif_ops.hi_read_word(RG_SDIO_IF_MISC_CTRL2);

#if SDIO_UPDATE_HOST_RDPTR
    reg_tmp |= BIT(6);
#endif

    hif->hif_ops.hi_write_word(RG_SDIO_IF_MISC_CTRL2, reg_tmp);

    to_sdio = 0xffffffff;
    
    hif->hif_ops.hi_write_word(RG_WIFI_IF_FW2HST_MASK, to_sdio);

    hal_cfg_cali_param();

DBG_EXIT();

exit:
    return ret;
}

int hal_free(void)
{
    struct hal_private *hal_priv=NULL;
    hal_priv = hal_get_priv();

    if (hal_priv->bhalProbelok)
    {
        OS_FREE_TQUEUE(&hal_priv->hi_tasktq);
        hal_priv->drv_priv= NULL;
        hal_priv->bWpaMicMeasureEnable =0;
        hal_priv->bhalProbelok = 0;
    }
    return 1;
}

unsigned char * hal_get_config(void)
{
    return NULL;
}

static int hal_dev_reg(void * drv_priv)
{
    struct hal_private *hal_priv=NULL;
    int loop  = 0;
    hal_priv = hal_get_priv();
   while (hal_priv->hst_if_init_ok ==0)
    {
        PRINT("-------------> host interface init failed!\n");
        msleep(100);
        if (loop++>10)
        {
            platform_wifi_reset_cpu();
            return -1;
        }
    }
    hal_priv->drv_priv = drv_priv;

    return 0;
}

static int hal_dev_unreg(void)
{
    struct hal_private *hal_priv=NULL;

    hal_priv = hal_get_priv();
    hal_priv->drv_priv= NULL;
    return 0;
}

int hal_recovery_init_priv(void)
{
    struct hal_private * hal_priv = hal_get_priv();
    memset(hal_priv->fw_event, 0, sizeof(struct fw_event_to_driver));
    hal_priv->hal_fw_event_counter = 0;

    hal_priv->tx_page_offset = 0;
    hal_priv->rx_host_offset = 0;
    
    hal_priv->bhalPowerSave  = 0;
    hal_priv->powersave_init_flag = 1;

    POWER_BEGIN_LOCK();
    hal_priv->hal_fw_ps_status = HAL_FW_IN_ACTIVE;
    POWER_END_LOCK();

    atomic_set(&hal_priv->sdio_suspend_cnt, 0);
    atomic_set(&hal_priv->drv_suspend_cnt, 0);
    hal_priv->hal_suspend_mode = HIF_SUSPEND_STATE_NONE;

    hal_priv->gpio_irq_cnt = 0;
    hal_priv->hst_if_init_ok = 1;

    hal_priv->tx_frames_map[0] = 0;
    hal_priv->tx_frames_map[1] = 0;

    return 0;
}

int hal_init_priv(void)
{
    struct hal_private * hal_priv = hal_get_priv();

    DBG_ENTER();
    hif_init_ops();

    memset(hal_priv , 0, sizeof( struct hal_private));
    hal_priv->hif = hif_get_hw_interface();

    HAL_LOCK_INIT();
    POWER_LOCK_INIT();
    COMMON_LOCK_INIT();
    AML_TXLOCK_INIT();
    PN_LOCK_INIT();

    hal_alloc_txcmp_buf(hal_priv);

    hal_alloc_fw_event_buf(hal_priv);
    
    hal_ops_attach();
    
    hal_priv->hal_call_back = get_hal_call_back_table();
    
    hal_priv->tx_page_offset = 0;
    hal_priv->rx_host_offset = 0;
    
    hal_priv->bhalPowerSave  = 0;
    hal_priv->powersave_init_flag = 1;

    POWER_BEGIN_LOCK();
    hal_priv->hal_fw_ps_status = HAL_FW_IN_ACTIVE;
    POWER_END_LOCK();

    atomic_set(&hal_priv->sdio_suspend_cnt, 0);
    atomic_set(&hal_priv->drv_suspend_cnt, 0);
    hal_priv->hal_suspend_mode = HIF_SUSPEND_STATE_NONE;

    hal_priv->gpio_irq_cnt = 0;
    hal_init_task();
    hal_priv->hst_if_init_ok = 1;

    DBG_EXIT();
    return 0;
}

int hal_reinit_priv(void) {
    struct hal_private * hal_priv = hal_get_priv();

    hal_priv->bhaltxdrop = 0;
    hal_priv->bhalPowerSave  = 0;
    hal_priv->tx_frames_map[0] = 0;
    hal_priv->tx_frames_map[1] = 0;
    
    return 0;
}

extern unsigned int rf_read_register(unsigned int addr);
extern void rf_write_register(unsigned int addr,unsigned int data);
void hal_exit_priv(void)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned int reg_val = 0;

    if (!hal_priv)
    {
        PRINT("%s %d\n", __func__, __LINE__);
        return;
    }

    reg_val = rf_read_register(RG_TOP_A2);
    reg_val = reg_val &(~0x1f);
    
    reg_val |= 0x15;
    rf_write_register(RG_TOP_A2, reg_val);

    reg_val  = hif->hif_ops.hi_read_word(RG_PMU_A16);
    reg_val &= (~BIT(31));
    hif->hif_ops.hi_write_word(RG_PMU_A16, reg_val);

    reg_val  = hif->hif_ops.hi_read_word(RG_COEX_WF_OWNER_CTRL);
    reg_val &= (~BIT(28) );
    hif->hif_ops.hi_write_word(RG_COEX_WF_OWNER_CTRL, reg_val);

    hal_kill_thread();

    hal_priv->hal_call_back->dev_remove();

    hal_free_txcmp_buf(hal_priv);
    hal_free_fw_event_buf(hal_priv);

     if (hal_priv->hst_if_init_ok && hal_priv->bhalProbelok)
    {
        amlhal_resetsdio();
    }

    aml_sdio_exit();
#ifndef CONFIG_AML_USE_STATIC_BUF
    FREE(g_rx_fifo, "hi_rx_fifo_init");
#endif

    HAL_LOCK_DESTROY();
    POWER_LOCK_DESTROY();
    AML_TXLOCK_DESTROY();
    COMMON_LOCK_DESTROY();
    PN_LOCK_DESTROY();

#if defined(CONFIG_GXL) && defined(CONFIG_GPIO_RESET)
    platform_wifi_reset();
#endif

}

int hal_init_task(void)
{
    int i =0;
    struct hal_private * hal_priv = hal_get_priv();

    hal_priv->ctrl_wait_flag =0;
    hal_priv->ctrl_register_flag=0;
    DBG_ENTER();
    for (i = 0; i < MAX_WORK_TASK; i++)
    {
        hal_priv->ctrl_task[i].task = NULL;
    }
    return 0;
}

int hal_reg_task(TaskHandler task)
{
    int i =0;
    struct hal_private * hal_priv = hal_get_priv();

    COMMON_LOCK();
    for (i=0; i<MAX_WORK_TASK; i++)
    {
        if ((hal_priv->ctrl_register_flag&BIT(i)) ==0)
        {
            hal_priv->ctrl_register_flag |= BIT(i);
            hal_priv->ctrl_task[i].task= task;
            break;
        }
    }
    COMMON_UNLOCK();
    PRINT("hal_reg_task taskid %d\n",i);
    if (i<MAX_WORK_TASK)
        return i;
    else
        return -1;
}

void hal_unreg_task(int taskid)
{
    int i =0;
    struct hal_private * hal_priv = hal_get_priv();

    if (taskid < 0 || taskid >= MAX_WORK_TASK)
        return;

    DBG_ENTER();
    COMMON_LOCK();
    hal_priv->ctrl_register_flag &= ~BIT(taskid);
    hal_priv->ctrl_wait_flag &= ~BIT(taskid);
    hal_priv->ctrl_task[taskid].task = NULL;
    COMMON_UNLOCK();
    PRINT("hal_unreg_task taskid %d\n",i);
}

int hal_call_task(SYS_TYPE taskid,SYS_TYPE param1)
{
    struct hal_private * hal_priv = hal_get_priv();

    if (taskid >= MAX_WORK_TASK)
        return -EINVAL;

    COMMON_LOCK();
     hal_priv->ctrl_wait_flag |= BIT(taskid);
     hal_priv->ctrl_task[taskid].param1= param1;
    COMMON_UNLOCK();
    up(&hal_priv->work_thread_sem);
    return 0;
}

int hal_work_thread(void *param)
{
    struct hal_private * hal_priv = (struct hal_private *)param;
    int  i = 0;

    PRINT("%s(%d)  =====creat thread hal_world_thread<=====\n",__func__,__LINE__);

    set_user_nice(current, -10);

    while (!hal_priv->work_thread_quit)
    {
        
        if (down_interruptible(&hal_priv->work_thread_sem) != 0)
        {
            
            PRINT_ERR("hal_work_thread down_interruptible interrupted\n");
            break;
        }
        if (0)
        {
            
        }
        if (hal_priv->work_thread_quit)
        {
            PRINT_ERR("hal_work_thread quit\n");
            break;
        }

        for (i=0; i<MAX_WORK_TASK; i++)
        {
            if (hal_priv->ctrl_wait_flag & BIT(i))
            {
                COMMON_LOCK();
                hal_priv->ctrl_wait_flag  &= ~BIT(i);
                COMMON_UNLOCK();
                if (hal_priv->ctrl_task[i].task)
                {
                    hal_priv->ctrl_task[i].task(hal_priv->ctrl_task[i].param1);
                }
            }
        }
    }

    PRINT("############# Exit work Thread ###############\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
    complete_and_exit(&hal_priv->work_thread_completion, 0);
#else
    kthread_complete_and_exit(&hal_priv->work_thread_completion, 0);
#endif

    return 0;
}

int hal_txok_thread(void *param)
{
    struct hal_private * hal_priv = (struct hal_private *)param;
    struct tx_status_node* txok_status_node = NULL;
    struct tx_status_list* txok_status_list = &hal_priv->hif->tx_status_list;
    struct txdonestatus* txstatus = NULL;
    struct tx_nulldata_status* tx_null_status = NULL;
    unsigned long callback = 0;
    unsigned char queue_id = 0;

    set_user_nice(current, -10);
    while (!hal_priv->txok_thread_quit)
    {
        if (down_interruptible(&hal_priv->txok_thread_sem) != 0)
        {
            PRINT_ERR("hal_txok_thread down_interruptible interrupted\n");
            break;
        }

        if (hal_priv->hal_call_back != NULL) {
            if (hal_priv->hal_call_back->intr_tx_ok_timeout != NULL) {
                hal_priv->hal_call_back->intr_tx_ok_timeout(hal_priv->drv_priv);
            }

            if ((hal_priv->hal_call_back->intr_tx_pkt_clear != NULL)
                && (hal_priv->txPageFreeNum == DEFAULT_TXPAGENUM))
                hal_priv->hal_call_back->intr_tx_pkt_clear(hal_priv->drv_priv);
        }

        while ((txok_status_node = tx_status_node_dequeue(txok_status_list)) != NULL)
        {
            tx_null_status = &(txok_status_node->tx_status.tx_null_status);
            if (((tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NULL_DATA_OK)
                || (tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NULL_DATA_FAIL))
                && (hal_priv->hal_call_back != NULL))
            {
                hal_priv->hal_call_back->intr_tx_nulldata_handle(hal_priv->drv_priv, tx_null_status);
            }
            else
            {
                txstatus = &txok_status_node->tx_status.txstatus;
                
                if (hal_free_tx_id(hal_priv, txstatus, &callback, &queue_id) < 0)
                {
                    
                    pr_debug("W522A: stale tx_id completion ignored id=%u\n",
                             txstatus->callbackid);
                    tx_status_node_free(txok_status_node,txok_status_list);
                    txok_status_node = NULL;
                    continue;
                }
                
                if (hal_priv->hal_call_back != NULL) {
                    hal_priv->hal_call_back->intr_tx_handle(hal_priv->drv_priv, txstatus, callback, queue_id);
                }
            }
            tx_status_node_free(txok_status_node,txok_status_list);
            txok_status_node = NULL;
       }
        if (tx_up_required && hal_get_free_tx_id_num(hal_priv) >= 32) {
            tx_up_required = 0;
            up(&hal_priv->hi_irq_thread_sem);
        }
    }

    pr_debug("%s(%d)  =====> exit TXOK Thread <=====\n",__func__,__LINE__);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
    complete_and_exit(&hal_priv->txok_thread_completion, 0);
#else
    kthread_complete_and_exit(&hal_priv->txok_thread_completion, 0);
#endif

    return 0;
}

int hal_rx_thread(void *param)
{
    struct hal_private * hal_priv = (struct hal_private *)param;
    struct hw_interface* hif = hal_priv->hif;
    struct sk_buff *skb = NULL;
    unsigned int remain_num = 0;
    unsigned char rxtmpbuffer[RX_TMP_MAX_LEN] = {0};
    V_HW_RxDescripter_bit* pVRxDesc = NULL;
    unsigned short mpdu_len = 0;
    unsigned long rx_fifo_fdh;
    unsigned long rx_fifo_fdt;
    unsigned int rx_fifo_total_len = 0;
#if 1
    unsigned short frame_control;
    unsigned short sn;
#endif

    set_user_nice(current, 5);

    pr_debug("%s(%d)  =====creat thread hal_rx_thread<=====\n",__func__,__LINE__);

    while (!hal_priv->rx_thread_quit)
    {
        if (down_interruptible(&hal_priv->rx_thread_sem) != 0)
        {
            PRINT_ERR("hal_rx_thread down_interruptible interrupted\n");
            break;
        }

        if (0)
        {
            
        }

        smp_rmb();
        rx_fifo_fdh = READ_ONCE(hif->rx_fifo.FDH);
        rx_fifo_fdt = READ_ONCE(hif->rx_fifo.FDT);

        while( rx_fifo_fdh != rx_fifo_fdt)
        {
            remain_num = hif->rx_fifo.FDN - rx_fifo_fdh;
            if( remain_num >= RX_PRIV_HDR_LEN )
            {
                pVRxDesc = (V_HW_RxDescripter_bit*)(hif->rx_fifo.FDB +rx_fifo_fdh);
            }
            else
            {
                memset(rxtmpbuffer, 0, RX_TMP_MAX_LEN);
                
                memcpy(rxtmpbuffer ,hif->rx_fifo.FDB +rx_fifo_fdh, remain_num);
                memcpy(&rxtmpbuffer[remain_num],hif->rx_fifo.FDB,  RX_PRIV_HDR_LEN -remain_num);
                pVRxDesc = (V_HW_RxDescripter_bit*)rxtmpbuffer;
            }
            rx_fifo_total_len = CIRCLE_Subtract2(rx_fifo_fdt, rx_fifo_fdh, hif->rx_fifo.FDN);
            if ((pVRxDesc->RxLength == 0) ||
                (pVRxDesc->RxLength + RX_PRIV_HDR_LEN > rx_fifo_total_len) ||
                (pVRxDesc->RxLength + RX_PRIV_HDR_LEN > hif->rx_fifo.FDN))
            {
                
                rx_fifo_fdh = rx_fifo_fdt;
                pr_debug("%s(%d)(RxLength:%d,rx_fifo_total_len%d)\n", __func__,__LINE__,
                pVRxDesc->RxLength,rx_fifo_total_len);
                continue;
            }

            if ( !pVRxDesc->upload2host_flag )
            {
                mpdu_len = RX_PRIV_HDR_LEN + pVRxDesc->RxLength;
                mpdu_len = ALIGN(mpdu_len, 4);
                rx_fifo_fdh = CIRCLE_Addition2(rx_fifo_fdh,mpdu_len , hif->rx_fifo.FDN);
                continue;
            }

            skb = os_skb_alloc(pVRxDesc->RxLength + RX_PRIV_HDR_LEN);
            if (skb == NULL)
            {
                pr_err("Couldn't allocate RX frame");
                break;
            }

            if( (pVRxDesc->RxLength+ RX_PRIV_HDR_LEN )<= remain_num )
            {
                memcpy(skb->data, (unsigned char *)(hif->rx_fifo.FDB + rx_fifo_fdh),
                    (pVRxDesc->RxLength+ RX_PRIV_HDR_LEN ) );
            }
            else
            {
                
                memcpy(skb->data, (unsigned char *)(hif->rx_fifo.FDB +rx_fifo_fdh), remain_num);
                memcpy(skb->data +remain_num, (unsigned char *)(hif->rx_fifo.FDB),
                    (pVRxDesc->RxLength+ RX_PRIV_HDR_LEN )-remain_num);
            }

            mpdu_len = RX_PRIV_HDR_LEN + pVRxDesc->RxLength;
            os_skb_put(skb, mpdu_len);
            mpdu_len = ALIGN(mpdu_len, 4);
            rx_fifo_fdh = CIRCLE_Addition2(rx_fifo_fdh,  mpdu_len, hif->rx_fifo.FDN);

        #if 1
            if (pVRxDesc->RxLength >= 34) {
                frame_control = *(unsigned short *)pVRxDesc->data;
                sn = *(pVRxDesc->data + 23);
                sn =  (sn << 4) | ((*(pVRxDesc->data + 22) & 0xf0) >> 4);

                if (frame_control != 0x80) {
                    AML_PRINT(AML_DBG_MODULES_TX, "frame type:0x%x, seq:%d, %04x:%04x\n", frame_control, sn,
                        *((unsigned short *)(pVRxDesc->data) + 15), *((unsigned short *)(pVRxDesc->data) + 16));
                } else {
                    
                }
            }
        #endif
            hal_soft_rx_cs(hal_priv, skb);
            cond_resched();
        }

        WRITE_ONCE(hif->rx_fifo.FDH, rx_fifo_fdh); 
    }

    pr_debug("%s(%d)  =====> exit RX Thread <=====\n",__func__,__LINE__);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
    kthread_complete_and_exit(&hal_priv->rx_thread_completion, 0);
#else
    complete_and_exit(&hal_priv->rx_thread_completion, 0);
#endif
    return 0;
}

int hi_irq_thread(void *param)
{
    struct hal_private * hal_priv = (struct hal_private *)param;

    set_user_nice(current, clamp_t(int, READ_ONCE(w522a_hi_irq_nice), -10, 10));
    hal_priv->hi_task_stop = 0;
    while (!hal_priv->hi_irq_thread_quit)
    {
        
        if (down_interruptible(&hal_priv->hi_irq_thread_sem) != 0)
        {
            
            PRINT_ERR("hal_work_thread down_interruptible interrupted\n");
            break;
        }

        atomic_set(&hal_priv->hi_irq_thread_sem_pending, 0);

        if (hal_priv->hi_irq_thread_quit)
        {
            PRINT_ERR("hi_irq_thread quit\n");
            break;
        }

        if(!hal_priv->hi_task_stop)
        {
            hi_top_task((unsigned long)hal_priv);
            
            if (hal_priv->oob_irq_masked) {
                hal_priv->oob_irq_masked = 0;
                enable_irq(hal_priv->oob_irq);
            }
            cond_resched();
        }
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
    kthread_complete_and_exit(&hal_priv->hi_irq_thread_completion, 0);
#else
    complete_and_exit(&hal_priv->hi_irq_thread_completion, 0);
#endif

    return 0;
}

int hal_kill_thread(void)
{
    struct hal_private * hal_priv = hal_get_priv();
    unsigned long timeout = msecs_to_jiffies(3000);

    if (hal_priv == NULL)
        return -ENODEV;

    if (hal_priv->txok_thread)
    {
        init_completion(&hal_priv->txok_thread_completion);
        hal_priv->txok_thread_quit = 1;
        up(&hal_priv->txok_thread_sem);
        if (!wait_for_completion_timeout(&hal_priv->txok_thread_completion, timeout))
            pr_err("txok_thread did not stop cleanly\n");
        hal_priv->txok_thread =0;
    }

    if (hal_priv->rx_thread)
    {
        init_completion(&hal_priv->rx_thread_completion);
        hal_priv->rx_thread_quit = 1;
        up(&hal_priv->rx_thread_sem);
        if (!wait_for_completion_timeout(&hal_priv->rx_thread_completion, timeout))
            pr_err("rx_thread did not stop cleanly\n");
        hal_priv->rx_thread =0;
    }

    if (hal_priv->work_thread)
    {
        init_completion(&hal_priv->work_thread_completion);
        hal_priv->work_thread_quit = 1;
        up(&hal_priv->work_thread_sem);
        if (!wait_for_completion_timeout(&hal_priv->work_thread_completion, timeout))
            pr_err("work_thread did not stop cleanly\n");
        hal_priv->work_thread =0;
    }

    if (hal_priv->hi_irq_thread)
    {
        init_completion(&hal_priv->hi_irq_thread_completion);
        hal_priv->hi_irq_thread_quit = 1;
        up(&hal_priv->hi_irq_thread_sem);
        if (!wait_for_completion_timeout(&hal_priv->hi_irq_thread_completion, timeout))
            pr_err("hi_irq_thread did not stop cleanly\n");
        hal_priv->hi_irq_thread =0;
    }
    return 0;
}

int hal_create_thread(void)
{
    struct hal_private * hal_priv = hal_get_priv();

    DBG_ENTER();

    sema_init(&hal_priv->work_thread_sem, 0);
    sema_init(&hal_priv->rx_thread_sem, 0);
    sema_init(&hal_priv->hi_irq_thread_sem, 0);
    sema_init(&hal_priv->txok_thread_sem, 0);

    hal_priv->work_thread_quit = 0;
    hal_priv->work_thread = kthread_run(hal_work_thread, hal_priv, "work_thread amlwifi");

    if (IS_ERR(hal_priv->work_thread))
    {
        ERROR_DEBUG_OUT("aml wifi to create work_thread thread error!!!!\n");
        hal_priv->work_thread = NULL;
        goto create_thread_error;
    }
    PRINT("aml wifi to create txok_thread thread !!!!\n");

    hal_priv->rx_thread_quit = 0;
    hal_priv->rx_thread = kthread_run(hal_rx_thread, hal_priv, "rx_proc amlwifi");

    if (IS_ERR(hal_priv->rx_thread))
    {
        ERROR_DEBUG_OUT("aml wifi to create rx_thread thread error!!!!\n");
        hal_priv->rx_thread = NULL;
        goto create_thread_error;
    }

    hal_priv->txok_thread_quit = 0;
    hal_priv->txok_thread = kthread_run(hal_txok_thread, hal_priv, "txok amlwifi");
    if (IS_ERR(hal_priv->txok_thread))
    {
        ERROR_DEBUG_OUT("aml wifi to create txok_thread thread error!!!!\n");
        hal_priv->txok_thread = NULL;
        goto create_thread_error;
    }

    hal_priv->hi_irq_thread_quit = 0;
    hal_priv->hi_irq_thread = kthread_run(hi_irq_thread, hal_priv, "hi_irq amlwifi");
    if(IS_ERR(hal_priv->hi_irq_thread))
    {
        ERROR_DEBUG_OUT("aml wifi to create irq_thread thread error!!!!\n");
        hal_priv->hi_irq_thread = NULL;
        goto create_thread_error;
    }

    return 0;

create_thread_error:
    hal_kill_thread();
    return -1;
}

void show_rxvector (HW_RxDescripter_bit *RxPrivHdr)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("%s dump rx PN\n", __FUNCTION__);
    dump_memory_internal((unsigned char *)RxPrivHdr->PN, 16);
    PRINT("RxRSSI_ant0=%d RxRSSI_ant1=%d\n",
          RxPrivHdr->RxRSSI_ant0, RxPrivHdr->RxRSSI_ant1);
    PRINT("RxChannel=%d\n", RxPrivHdr->RxChannel);
    PRINT("RxRate=0x%x\n", RxPrivHdr->RxRate);
    PRINT("RxAntenna=%d\n", RxPrivHdr->RxAntenna);
    PRINT("ch_bwd_innonht=%d dyn_bwd_innonht=%d\n",
          RxPrivHdr->ch_bwd_innonht, RxPrivHdr->dyn_bwd_innonht);
    PRINT("RxA1match=%d RxA1match_id=%d\n",
          RxPrivHdr->RxA1match, RxPrivHdr->RxA1match_id);
    PRINT("RxShortGI=%d\n", RxPrivHdr->RxShortGI);
    PRINT("Channel_BW=%d\n", RxPrivHdr->Channel_BW);
    PRINT("RxPreambleType=%d\n", RxPrivHdr->RxPreambleType);
    PRINT("aggregation=%d\n", RxPrivHdr->aggregation);
    PRINT("mic_err=%d, keymiss_err=%d icv_err=%d \n",
          RxPrivHdr->mic_err, RxPrivHdr->keymiss_err, RxPrivHdr->icv_err);
    PRINT("RxLength=%d(0x%x)\n", RxPrivHdr->RxLength, RxPrivHdr->RxLength);
    PRINT("RxTime=%d(0x%x)\n", RxPrivHdr->RxTime, RxPrivHdr->RxTime);
    PRINT("bssidmatch_id=%d\n", RxPrivHdr->bssidmatch_id);
    PRINT("partial_aid=%d\n", RxPrivHdr->partial_aid);
    PRINT("Channel_OFFSET=%d\n", RxPrivHdr->Channel_OFFSET);
    PRINT("Next_RxPage=%d\n", RxPrivHdr->Next_RxPage);
    PRINT("RxDecryptType=%d\n", RxPrivHdr->RxDecryptType);
    PRINT("upload2host_flag=%d\n", RxPrivHdr->upload2host_flag);
    PRINT("key_id=%d\n", RxPrivHdr->key_id);
    PRINT("RxTcpCSUM_err=%d, RxIPCSUM_err=%d RxTcpCSUMCalculated=%d, RxIPCSUMCalculated=%d\n",
          RxPrivHdr->RxTcpCSUM_err, RxPrivHdr->RxIPCSUM_err,
          RxPrivHdr->RxTcpCSUMCalculated, RxPrivHdr->RxIPCSUMCalculated);
    PRINT("channel_bw_rts=%d\n", RxPrivHdr->channel_bw_rts);
    PRINT("%s---\n", __FUNCTION__);
}

 void hal_show_rxframe (HW_RxDescripter_bit *RxPrivHdr)
{
    PRINT("%s dump HW_RxDescripter_bit, len=%zd\n", __FUNCTION__, RX_PRIV_HDR_LEN);
    dump_memory_internal((unsigned char*)RxPrivHdr, RX_PRIV_HDR_LEN);
    show_rxvector(RxPrivHdr);

    PRINT("%s dump body, len=%d(0x%x)\n", __FUNCTION__, RxPrivHdr->RxLength, RxPrivHdr->RxLength);
    dump_memory_internal((unsigned char*)RxPrivHdr->data, RxPrivHdr->RxLength);
    show_macframe((unsigned char *)RxPrivHdr->data, RxPrivHdr->RxLength);
}

 void show_mpdu_buf_flag (unsigned int MPDUBufFlag)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("mpdu first buffer=%d, mpdu last buffer=%d, ampdu first packet=%d, ampdu last packet=%d\n",
          !!(MPDUBufFlag&HW_FIRST_MPDUBUF_FLAG), !!(MPDUBufFlag&HW_LAST_MPDUBUF_FLAG),
          !!(MPDUBufFlag&HW_FIRST_AGG_FLAG), !!(MPDUBufFlag&HW_LAST_AGG_FLAG));
    PRINT("mpdu length=%d(0x%x)\n", HW_MPDU_LEN_GET(MPDUBufFlag), HW_MPDU_LEN_GET(MPDUBufFlag));
    if (MPDUBufFlag&HW_LAST_MPDUBUF_FLAG)
    {
        PRINT("tx mpdu delimiter number=%d(0x%x)\n", HW_BUFFER_LEN_GET(MPDUBufFlag), HW_BUFFER_LEN_GET(MPDUBufFlag));
    }
    else
    {
        PRINT("tx buffer length=%d(0x%x)\n", HW_BUFFER_LEN_GET(MPDUBufFlag), HW_BUFFER_LEN_GET(MPDUBufFlag));
    }
    PRINT("%s---\n", __FUNCTION__);
}

void show_tx_vector (struct hw_tx_vector_bits *TxVector)
{
    PRINT("%s+++\n", __FUNCTION__);

    PRINT("LLen_hw_calc=%d\n",   TxVector->tv_llength_hw_calc );

    PRINT("tv_rty_flag=%d\n",   TxVector->tv_rty_flag );
    PRINT("tv_fw_duration_valid=%d\n",TxVector->tv_fw_duration_valid);

    PRINT("tv_txcsum_enbale=%d tv_encrypted_disable=%d\n",
        TxVector->tv_txcsum_enbale, TxVector->tv_encrypted_disable);
    PRINT("tv_htc_modify=%d \n", TxVector->tv_htc_modify);
    PRINT("tv_sq_valid=%d tv_fc_valid=%d tv_du_valid=%d\n",
        TxVector->tv_sq_valid, TxVector->tv_fc_valid, TxVector->tv_du_valid);
    PRINT("tv_beacon_flag=%d \n", TxVector->tv_beacon_flag);
    PRINT("tv_tkip_mic_enable=%d \n", TxVector->tv_tkip_mic_enable);

    PRINT("tv_ack_policy=%d\n", TxVector->tv_ack_policy );
    PRINT("tv_single_ampdu=%d\n", TxVector->tv_single_ampdu );
    PRINT("tv_control_wrapper=%d \n", TxVector->tv_control_wrapper);
    PRINT("tv_tid_num=%d \n", TxVector->tv_tid_num);
    PRINT("tv_wnet_vif_id=%d \n", TxVector->tv_wnet_vif_id);
    PRINT("tv_ack_to_en=%d tv_ack_timeout=%d\n",
        TxVector->tv_ack_to_en, TxVector->tv_ack_timeout);
    PRINT("tv_L_length=%d(0x%x) \n", TxVector->tv_L_length, TxVector->tv_L_length);
    PRINT("tv_length=%d(0x%x) \n", TxVector->tv_length, TxVector->tv_length);
    PRINT("tv_FrameControl=0x%x \n", TxVector->tv_FrameControl);
    PRINT("tv_txop_time=%d\n", TxVector->tv_txop_time);
    PRINT("tv_burst_en=%d\n", TxVector->tv_burst_en );

    PRINT("tv_SequenceNum=%d(0x%x) \n", TxVector->tv_SequenceNum, TxVector->tv_SequenceNum);
    PRINT("tv_ampdu_flag=%d \n", TxVector->tv_ampdu_flag);
    PRINT("tv_no_sig_extn=%d \n", TxVector->tv_no_sig_extn);
    PRINT("tv_STBC=%d \n", TxVector->tv_STBC);
    PRINT("tv_dyn_bw_nonht=%d \n", TxVector->tv_dyn_bw_nonht);
    PRINT("tv_txop_ps_not=%d \n", TxVector->tv_txop_ps_not);
    PRINT("tv_usr_postion=%d \n", TxVector->tv_usr_postion);
    PRINT("tv_group_id=%d \n", TxVector->tv_group_id);
    PRINT("tv_partial_id=%d \n", TxVector->tv_partial_id);
    PRINT("tv_num_users=%d \n", TxVector->tv_num_users);
    PRINT("tv_sounding=%d \n", TxVector->tv_sounding);
    PRINT("tv_FEC_coding=%d \n", TxVector->tv_FEC_coding);
    PRINT("tv_num_exten_ss=%d \n", TxVector->tv_num_exten_ss);
    PRINT("tv_num_tx=%d \n", TxVector->tv_num_tx);
    PRINT("tv_num_sts=%d \n", TxVector->tv_num_sts);
    PRINT("tv_ch_bw_nonht=%d \n", TxVector->tv_ch_bw_nonht);
    PRINT("tv_expansion_mat_type=%d \n", TxVector->tv_expansion_mat_type);
    PRINT("tv_format=%d \n", TxVector->tv_ht.htbit.tv_format);
    PRINT("tv_nonht_mod=%d \n", TxVector->tv_ht.htbit.tv_nonht_mod);
    PRINT("tv_tx_pwr=%d \n", TxVector->tv_ht.htbit.tv_tx_pwr);
    PRINT("tv_tx_rate=0x%x \n", TxVector->tv_ht.htbit.tv_tx_rate);
    PRINT("tv_Channel_BW=%d\n",TxVector->tv_ht.htbit.tv_Channel_BW);
    PRINT("tv_preamble_type=%d \n", TxVector->tv_ht.htbit.tv_preamble_type);
    PRINT("tv_GI_type=%d \n", TxVector->tv_ht.htbit.tv_GI_type);
    PRINT("tv_beamformed=%d \n", TxVector->tv_ht.htbit.tv_beamformed);
    PRINT("tv_antenna_set=%d \n", TxVector->tv_ht.htbit.tv_antenna_set);
    PRINT("tv_bar_flag=%d\n",   TxVector->tv_ht.htbit.tv_bar_flag );
    PRINT("tv_cfend_flag=%d\n",   TxVector->tv_ht.htbit.tv_cfend_flag );
    PRINT("tv_rts_flag=%d \n", TxVector->tv_ht.htbit.tv_rts_flag);
    PRINT("tv_cts_flag=%d \n", TxVector->tv_ht.htbit.tv_cts_flag);

    PRINT("%s---\n", __FUNCTION__);
}

void show_tx_priv (struct Fw_TxPriv *TxPriv)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("FrameExchDur=%d(0x%x) \n", TxPriv->FrameExchDur, TxPriv->FrameExchDur);
    PRINT("TxLiveTime=%d(0x%x) \n", TxPriv->TxLiveTime, TxPriv->TxLiveTime);
    PRINT("Flag=0x%x Flag2=0x%x\n", TxPriv->Flag, TxPriv->Flag2);
    PRINT("HiP2pNoaCountNow=%d \n", TxPriv->HiP2pNoaCountNow);
    PRINT("AggrLen=%d(0x%x) \n", TxPriv->AggrLen, TxPriv->AggrLen);
    PRINT("SN=%d(0x%x) \n", TxPriv->SN, TxPriv->SN);
    PRINT("TxCurrentRate=0x%x\n", TxPriv->TxCurrentRate);
    PRINT("AggrNum=%d \n", TxPriv->AggrNum);
    PRINT("txretrynum=%d txsretrynum=%d\n", TxPriv->txretrynum, TxPriv->txsretrynum);
    PRINT("TID=%d \n", TxPriv->TID);
    PRINT("StaId=%d \n", TxPriv->StaId);
    PRINT("PageNum=%d \n", TxPriv->PageNum);
    PRINT("txstatus=%d(0x%x) \n", TxPriv->txstatus, TxPriv->txstatus);
    PRINT("ackrssi=%d(0x%x) \n", TxPriv->ackrssi, TxPriv->ackrssi);
    PRINT("vid=%d \n", TxPriv->vid);
    PRINT("hostcallbackid=0x%x \n", TxPriv->hostcallbackid);
    PRINT("TxRate[0]=0x%x TxRate[1]=0x%x TxRate[2]=0x%x TxRate[3]=0x%x\n",
          TxPriv->TxRate[0], TxPriv->TxRate[1], TxPriv->TxRate[2], TxPriv->TxRate[3]);
    PRINT("TxRetry[0]=%d TxRetry[1]=%d TxRetry[2]=%d TxRetry[3]=%d\n",
          TxPriv->TxRetry[0], TxPriv->TxRetry[1], TxPriv->TxRetry[2], TxPriv->TxRetry[3]);
    PRINT("%s---\n", __FUNCTION__);
}

 void show_frame_control (unsigned short frame_control)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("ProtocolVersion=%d Type=%d Subtype=%d ToDS=%d FromDS=%d\n",
          frame_control&MAC_FCTRL_PROTOCOLVERSION_MASK,
          (frame_control&MAC_FCTRL_TYPE_MASK)>>2,
          (frame_control&MAC_FCTRL_SUBT_MASK)>>4,
          !!(frame_control&FRAME_CONTROL_TO_DS),
          !!(frame_control&FRAME_CONTROL_FROM_DS));
    PRINT("MoreFrag=%d Retry=%d PS=%d MoreData=%d encrypted=%d Order=%d\n",
          !!(frame_control&FRAME_CONTROL_MORE_FRAGMENT),
          !!(frame_control&FRAME_CONTROL_RETRY),
          !!(frame_control&FRAME_CONTROL_POWER_MANAGEMENT),
          !!(frame_control&FRAME_CONTROL_MORE_DATA),
          !!(frame_control&FRAME_CONTROL_PROTECTED),
          !!(frame_control&FRAME_CONTROL_ORDERED));
    PRINT("%s---\n", __FUNCTION__);
}

static void show_tx_option (struct HW_TxOption *TxOption)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("KeyIdex=%d\n", TxOption->KeyIdex);
    PRINT("%s dump tx PN\n", __FUNCTION__);
    dump_memory_internal((unsigned char *)TxOption->PN, 16);
    PRINT("%s---\n", __FUNCTION__);
}

 void show_qos_control (unsigned short qos_control)
{
    PRINT("%s+++\n", __FUNCTION__);
    PRINT("tid=%d eosp=%d ackPolicy=%d amsdu=%d txop=%d\n",
          qos_control&QOS_CONTROL_TID_MASK,
          !!(qos_control&QOS_CONTROL_EOSP_MASK),
          (qos_control&QOS_CONTROL_ACK_POLICY_MASK)>>QOS_CONTROL_ACK_POLICY_SHIFT,
          !!(qos_control&QOS_CONTROL_AMSDU_MASK),
          (qos_control&QOS_CONTROL_TXOP_QUEUE_MASK)>>QOS_CONTROL_TXOP_QUEUE_SHIFT);
    PRINT("%s---\n", __FUNCTION__);
}

void show_macframe(unsigned char *start, unsigned char len)
{
    unsigned char *cursor = (unsigned char *)start;
    unsigned short frame_control;
    unsigned short duration;
    unsigned char ra_mac_address[6], ta_mac_address[6], bssid_mac_address[6];
    unsigned short sequence;
    unsigned short qos_control;
    unsigned char llc_type_ip = 0, udp = 0, tcp = 0;

    PRINT("%s+++\n", __FUNCTION__);

    frame_control = READ_16L(cursor);
    cursor += 2;
    show_frame_control(frame_control);

    duration = READ_16L(cursor);
    cursor += 2;
    PRINT("duration=%d(0x%x)\n", duration, duration);

    address_read(cursor, ra_mac_address);
    cursor += 6;
    address_read(cursor, ta_mac_address);
    cursor += 6;
    address_read(cursor, bssid_mac_address);
    cursor += 6;
    PRINT("ra_mac_address\n");
    address_print(ra_mac_address);
    PRINT("ta_mac_address\n");
    address_print(ta_mac_address);
    PRINT("bssid_mac_address\n");
    address_print(bssid_mac_address);

    sequence = READ_16L(cursor);
    cursor += 2;
    PRINT("fragment number=%d, sequence number=%d(0x%x)\n",
          sequence & SEQUENCE_CONTROL_FRAGMENT_MASK,
          (sequence & SEQUENCE_CONTROL_SEQUENCE_MASK) >> SEQUENCE_CONTROL_SEQUENCE_SHIFT,
          (sequence & SEQUENCE_CONTROL_SEQUENCE_MASK) >> SEQUENCE_CONTROL_SEQUENCE_SHIFT);

    if ((frame_control & MAC_FCTRL_QOS_DATA) == MAC_FCTRL_QOS_DATA)
    {
        qos_control = READ_16L(cursor);
        cursor += 2;
        show_qos_control(qos_control);
    }

    if ((len - (cursor - start)) >= 8)
    {
        if ((cursor[0] == 0xaa) && (cursor[1]== 0xaa) &&
            (cursor[2] == 0x03))
        {
            PRINT("LLC SNAP\n");
        }
        PRINT("LLC type =0x%x\n", READ_16B(cursor + 6));
        if ((cursor[6] == 0x08) && (cursor[7] == 0x06))
        {
            PRINT("ARP\n");
        }
        if ((cursor[6] == 0x08) && (cursor[7] == 0x00))
        {
            PRINT("IP\n");
            llc_type_ip = 1;
        }
        cursor += 8;
    }

    if ((llc_type_ip==1) && ((len - (cursor - start)) >= 20))
    {
        unsigned char ipv4 = 0;
        unsigned char ip_header_len;
        unsigned short fragmentation;

        if (((cursor[0] & 0xf0)>>4) == 4)
        {
            ipv4 = 1;
        }

        PRINT("IPv%d\n", ((cursor[0] & 0xf0)>>4));
        ip_header_len = (cursor[0] & 0x0f)*4;
        PRINT("IP Header Length=%dB\n", (cursor[0] & 0x0f) * 4);
        PRINT("IP type of service =0x%x\n", cursor[1]);
        PRINT("IP Total Length=%dB\n", READ_16B(cursor + 2));
        PRINT("IP ID=%d\n", READ_16B(cursor + 4));
        fragmentation = READ_16B(cursor + 6);
        PRINT("IP fragmentation flags=0x%x offset=%d\n", ((fragmentation & 0xe000) >> 13),
              fragmentation & 0x1fff);
        PRINT("IP Time to live =%d\n", cursor[8]);
        PRINT("IP transfer protocol type=%d\n", cursor[9]);
        if (cursor[9] == 6)
        {
            tcp = 1;
            PRINT("TCP\n");
        }
        if (cursor[9] == 17)
        {
            udp = 1;
            PRINT("UDP\n");
        }
        PRINT("IP Header checksum=0x%x\n", READ_16B(cursor + 10));
        if (ipv4 == 1)
        {
            PRINT("Source IP address\n");
            IPv4_address_print(cursor + 12);
            PRINT("Dest IP address\n");
            IPv4_address_print(cursor + 16);
        }
        if ((len - (cursor - start)) >= ip_header_len)
        {
            cursor += ip_header_len;
        }
        else
        {
            PRINT("IP header not ended!!!\n");
            cursor += 20;
        }
    }

    if ((tcp == 1) && ((len - (cursor - start)) >= 20))
    {
        unsigned char tcp_header_len;
        unsigned short tcp_flags;

        PRINT("TCP source port=%d\n", READ_16B(cursor));
        PRINT("TCP dest port=%d\n", READ_16B(cursor + 2));
        PRINT("TCP sequence number=%u\n", READ_32B(cursor + 4));
        PRINT("TCP ack number=%u\n", READ_32B(cursor + 8));
        tcp_flags = READ_16B(cursor + 12);
        tcp_header_len = ((tcp_flags & 0xf000) >> 12) * 4;
        PRINT("TCP Header Length=%d\n", tcp_header_len);
        PRINT("TCP flags URG=%d ACK=%d PSH=%d RST=%d SYN=%d FIN=%d\n",
              !!(tcp_flags & 0x20), !!(tcp_flags & 0x10), !!(tcp_flags & 0x08),
              !!(tcp_flags & 0x04), !!(tcp_flags & 0x02), !!(tcp_flags & 0x01));
        PRINT("TCP window size=%d\n", READ_16B(cursor + 14));
        PRINT("TCP checksum of header and body=0x%x\n", READ_16B(cursor + 16));
        PRINT("TCP urgent pointer=0x%x\n", READ_16B(cursor + 18));
        if ((len - (cursor - start)) >= tcp_header_len)
        {
            cursor += tcp_header_len;
        }
        else
        {
            cursor += 20;
            PRINT("TCP header not ended!!!\n");
        }
    }

    if ((udp==1) && ((len - (cursor - start)) >= 8))
    {
        PRINT("UDP source port=%d\n", READ_16B(cursor));
        PRINT("UDP dest port=%d\n", READ_16B(cursor+2));
        PRINT("UDP len=%d\n", READ_16B(cursor+4));
        PRINT("UDP checksum=0x%x\n", READ_16B(cursor+6));
        cursor += 8;
    }
    PRINT("%s---\n", __FUNCTION__);
}

void hal_show_txframe (struct hi_tx_desc *pTxDPape)
{
    show_tx_vector(&pTxDPape->TxVector);
    show_tx_option(&pTxDPape->TxOption);
    show_tx_priv(&pTxDPape->TxPriv);

}

void hal_show_txagg_desc(struct hi_agg_tx_desc *HiTxDesc)
{
    PRINT("HiTxDesc->RateTableMode %x\n", HiTxDesc->RateTableMode);
    PRINT("HiTxDesc->TxPower %x\n", HiTxDesc->TxPower);
    PRINT("HiTxDesc->StaId %x\n",   HiTxDesc->StaId);
    PRINT("HiTxDesc->KeyId %x\n", HiTxDesc->KeyId);
    PRINT("HiTxDesc->EncryptType %x\n", HiTxDesc->EncryptType);
    PRINT("HiTxDesc->TID %x\n", HiTxDesc->TID);
    PRINT("HiTxDesc->CurrentRate %x\n", HiTxDesc->CurrentRate);
    PRINT("HiTxDesc->TxTryRate1 %x\n", HiTxDesc->TxTryRate1);

    PRINT("HiTxDesc->TxTryNum0 %x\n", HiTxDesc->TxTryNum0);

    PRINT("HiTxDesc->FLAG %x\n", HiTxDesc->FLAG);
    PRINT("HiTxDesc->FLAG2 %x\n", HiTxDesc->FLAG2);
    PRINT("HiTxDesc->AGGR_len %x\n", HiTxDesc->AGGR_len);
    PRINT("HiTxDesc->TxPrivNum %x\n", HiTxDesc->TxPrivNum);

    PRINT("HiTxDesc->MpduNum       %x\n", HiTxDesc->MpduNum);
}

void hal_tx_priv_desc_show(const struct hi_tx_priv_hdr *const HiTxPrivDesc)
{
    PRINT("HiTxPrivDesc->DMALEN %x\n", HiTxPrivDesc->DMALEN);
    PRINT("HiTxPrivDesc->MPDULEN %x\n", HiTxPrivDesc->MPDULEN);

    PRINT("HiTxPrivDesc->Delimiter %x\n", HiTxPrivDesc->Delimiter);
    PRINT("HiTxPrivDesc->Flag %x\n",  HiTxPrivDesc->Flag);
    PRINT("HiTxPrivDesc->Seqnum %x\n", HiTxPrivDesc->Seqnum);

    PRINT("HiTxPrivDesc->FrameControl %x\n", HiTxPrivDesc->FrameControl);
}

void hal_txinfo_show()
{
    struct hal_private *hal_priv = hal_get_priv();
    struct hw_interface *hif = hif_get_hw_interface();
    int i;

    DBG_ENTER();

    for (i = 0; i < HAL_NUM_TX_QUEUES; i++)
    {
        struct _CO_SHARED_FIFO* pTxShareFifo;

        pTxShareFifo = &hal_priv->txds_trista_fifo[i];
        PRINT("TID = %d,hal_get_priv_cnt %d\n", i, hal_get_priv_cnt(i));
        PRINT("TID = %d,CO_TX_BUFFER_MAKE= %d\n", i,
              CO_SharedFifoNbElt(&hal_priv->txds_trista_fifo[i], CO_TX_BUFFER_MAKE)) ;
        PRINT("TID = %d,CO_TX_BUFFER_SET= %d\n", i,
              CO_SharedFifoNbElt(&hal_priv->txds_trista_fifo[i], CO_TX_BUFFER_SET)) ;
        PRINT("pTxShareFifo->IdxTab[0] %d,%d,\n", pTxShareFifo->IdxTab[0].In,
              pTxShareFifo->IdxTab[0].Out);
        PRINT("pTxShareFifo->IdxTab[1] %d,%d,\n", pTxShareFifo->IdxTab[1].In,
              pTxShareFifo->IdxTab[1].Out);
        PRINT("pTxShareFifo->IdxTab[2] %d,%d,\n", pTxShareFifo->IdxTab[2].In,
              pTxShareFifo->IdxTab[2].Out);
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,15,0) && !defined (LINUX_PLATFORM)
    PRINT("HiStatus.Tx_Send_num %d \n", hif->HiStatus.Tx_Send_num);
    PRINT("HiStatus.Tx_Free_num %d \n", hif->HiStatus.Tx_Free_num);
    PRINT("HiStatus.Tx_Done_num %d \n", hif->HiStatus.Tx_Done_num);
#else
    PRINT("HiStatus.Tx_Send_num %d \n", atomic_read(&hif->HiStatus.Tx_Send_num));
    PRINT("HiStatus.Tx_Free_num %d \n", atomic_read(&hif->HiStatus.Tx_Free_num));
    PRINT("HiStatus.Tx_Done_num %d \n", atomic_read(&hif->HiStatus.Tx_Done_num));
#endif
    PRINT(" hal_priv->txPageFreeNum%d,\n", hal_priv->txPageFreeNum);
    PRINT(" hal_priv->bitmap[0]=%lx,\n", hal_priv->tx_frames_map[0]);
    PRINT(" hal_priv->bitmap[1]=%lx,\n", hal_priv->tx_frames_map[1]);
    PRINT(" hal_priv->HalTxPageDoneCounter=%d,\n", hal_priv->HalTxPageDoneCounter );
    PRINT(" hal_priv->HalTxFrameDoneCounter=%d,\n", hal_priv->HalTxFrameDoneCounter );
    PRINT(" hal_priv->txcompletestatus->txpagecounter=%d,\n", hal_priv->txcompletestatus->txpagecounter );

    PRINT("HiStatus.Tx_Done_num-- %d \n", hif->HiStatus.Tx_Done_num);
}

void hal_dpd_memory_download(void)
{
    unsigned int reg_tmp;
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned char *d_ptr;
    unsigned int addr = DPD_MEMORY_ADDR;
    int len, offset = 0;

    d_ptr = (unsigned char *)MEMDATA;
    
    len = sizeof(MEMDATA);

    ASSERT(len > 0);
    ASSERT(len <= DPD_MEMORY_LEN);

    hif->hif_ops.hi_write_word(0x00a0d0e4, 0x8000007f);

    w1_func5_bank_lock();

    reg_tmp = hif->hif_ops.hi_read_word(RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23)))
    {
        reg_tmp |= BIT(23);
        hif->hif_ops.hi_write_word(RG_SDIO_IF_MISC_CTRL, reg_tmp);
    }

    hif->hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A, addr & 0xfffe0000);

    do
    {
        unsigned char *sdio_kmm;
        int databyte;

        databyte = (len > SRAM_MAX_LEN) ? SRAM_MAX_LEN : len;

        sdio_kmm = (unsigned char*)ZMALLOC(databyte, "dpd memory", GFP_DMA | GFP_ATOMIC);
        ASSERT(sdio_kmm != NULL);

        memcpy(sdio_kmm, d_ptr + offset, databyte);

        hif->hif_ops.bt_hi_write_sram(sdio_kmm, (unsigned char*)(SYS_TYPE)((addr & 0x1ffff) + offset),
                                      databyte);

        len -= databyte;
        offset += databyte;

        FREE(sdio_kmm, "dpd memory");
    } while(len > 0);

    w1_func5_bank_unlock();
}

void hal_dpd_calibration(void)
{
    hal_dpd_memory_download();
    hal_dpd_memory_download_cmd();
}

unsigned char fwlog_buf[SRAM_FWLOG_BUFFER_LEN] = { 0 };
void hal_get_fwlog(void)
{
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned int addr = SRAM_FWLOG_BUFFER;
    int databyte = SRAM_FWLOG_BUFFER_LEN;
    unsigned int reg_tmp;
    struct drv_private *drv_priv = drv_get_drv_priv();

    memset(fwlog_buf, 0, sizeof(fwlog_buf));

    w1_func5_bank_lock();

    reg_tmp = hif->hif_ops.hi_read_word(RG_SDIO_IF_MISC_CTRL);

    if (!(reg_tmp & BIT(23)))
    {
        reg_tmp |= BIT(23);
        hif->hif_ops.hi_write_word(RG_SDIO_IF_MISC_CTRL , reg_tmp);
    }

    hif->hif_ops.hi_write_reg32(RG_SCFG_FUNC5_BADDR_A, addr & 0xfffe0000);

    hif->hif_ops.bt_hi_read_sram((unsigned char*)fwlog_buf,
                                 (unsigned char*)(SYS_TYPE)(addr & 0x1ffff),
                                 databyte);

    w1_func5_bank_unlock();

    drv_priv->drv_ops.drv_print_fwlog(fwlog_buf, databyte);
}
