
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <net/addrconf.h>

#include "wifi_drv_power.h"
#include "wifi_drv_uapsd.h"
#include "wifi_drv_xmit.h"
#include "chip_bt_pmu_reg.h"
#include "wifi_drv_recv.h"
#include "wifi_mac_p2p.h"
#include "wifi_cfg80211.h"
#include "wifi_mac_concurrent.h"
#include "wifi_mac_if.h"
#include "wifi_mac_var.h"
#include "patch_fi_cmd.h"
#include "wifi_mac_chan.h"
#include "wifi_hal_platform.h"
#include "wifi_mac_action.h"
#include "wifi_mac_main_reg.h"
#include "wifi_mac_tx_reg.h"
#include "wifi_mac_xmit.h"

const unsigned char drv_bcast_mac[WIFINET_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static unsigned int drv_phy_reg_sta_id(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                                       unsigned short StaAid, unsigned char *pMac, unsigned char encrypt);

static struct drv_private drv_priv;

extern void hal_dpd_calibration(void);
extern struct notifier_block aml_inetaddr_cb;
extern struct notifier_block aml_inet6addr_cb;
extern int hal_tx_flush(unsigned char vid);
extern int hal_fw_repair(void);
extern void aml_sdio_disable_irq(int func_n);
extern void aml_sdio_enable_irq(int func_n);
extern volatile unsigned char w1_wifi_hal_probe_done;

static DEFINE_MUTEX(aml_w1_fw_recovery_lock);
static unsigned long aml_w1_fw_recovery_last_jiffies;

struct drv_private *drv_get_drv_priv(void)
{
    return &drv_priv;
}

int drv_rate_setup(struct drv_private *drv_priv, enum wifi_mac_macmode mode)
{
    struct drv_rate_table *rt = drv_hal_get_rate_tbl(mode);

    if (rt == NULL)
    {
        ERROR_DEBUG_OUT("<running> error \n");
        return 0;
    }

    drv_hal_setupratetable(rt);
    DRV_MINSTREL_LOCK_INIT(drv_priv);
    drv_priv->drv_currratetable = rt;
    drv_priv->drv_protect_rateindex = 0;

    if (drv_priv->net_ops->wifi_mac_rate_init)
    {
        drv_priv->net_ops->wifi_mac_rate_init(drv_priv->wmac, mode, rt);
    }
    return 1;
}

static void drv_set_cfg_tx_power_limit(struct drv_private *drv_priv,
                                       unsigned short cfg_txpowlimit)
{
    drv_priv->drv_config.cfg_txpowlimit = clamp_t(unsigned short, cfg_txpowlimit, 1, 20);
}

void
drv_update_tx_pwr(struct drv_private *drv_priv, unsigned short txpowerdb)
{
    unsigned int txpow, cfg_txpowlimit;

    cfg_txpowlimit = clamp_t(unsigned int, drv_priv->drv_config.cfg_txpowlimit, 1, 20);
    txpow = txpowerdb ? txpowerdb : cfg_txpowlimit;

    if (drv_priv->drv_curtxpower != cfg_txpowlimit)
    {
        drv_priv->drv_curtxpower = cfg_txpowlimit;
    }

    if (drv_priv->net_ops->wifi_mac_update_txpower)
        drv_priv->net_ops->wifi_mac_update_txpower(drv_priv->wmac,
            drv_priv->drv_curtxpower, txpow);
}

static void drv_assoc_proc(struct drv_private *drv_priv, void *nsta,
                           int isnew, int b_uapsd)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);

    if (!isnew)
    {
        int tid_index;

        for (tid_index = 0; tid_index < WME_NUM_TID; tid_index++)
        {
            if (drv_priv->drv_config.cfg_txaggr)
            {
                drv_txampdu_del(drv_priv,drv_sta,tid_index);
            }
            if (drv_priv->drv_config.cfg_rxaggr)
            {
                drv_rxampdu_del(drv_priv,drv_sta,tid_index);
            }
        }
    }

    drv_sta->sta_cleanup = 0;
    drv_sta->sta_powesave = 0;
    drv_sta->sta_isuapsd = 0;
    drv_sta->sta_isuapsd = b_uapsd ;
}

int drv_channel_init(struct drv_private *drv_priv, unsigned int cc)
{
    pr_debug("%s %d \n",__func__,__LINE__);
    if (drv_priv->net_ops->wifi_mac_setup_channel_list)
    {
        unsigned int wMode;

        wMode = drv_priv->drv_config.cfg_modesupport;
        drv_priv->net_ops->wifi_mac_setup_channel_list(drv_priv->wmac, wMode, cc);
    }

    return 0;
}

static void
drv_update_slot( struct drv_private *drv_priv, int slottime)
{
    driv_ps_wakeup(drv_priv);
    drv_hal_setslottime( slottime);
    driv_ps_sleep(drv_priv);
}

void aml_w1_fw_recovery(void)
{
    unsigned long now = jiffies;
    const unsigned long min_interval = msecs_to_jiffies(5000);
    int repair_ret;

    if (!mutex_trylock(&aml_w1_fw_recovery_lock)) {
        pr_warn("W155S1 fw_recovery: already in progress, skipping concurrent call\n");
        return;
    }

    if (aml_w1_fw_recovery_last_jiffies != 0 &&
        time_before(now, aml_w1_fw_recovery_last_jiffies + min_interval)) {
        pr_warn("W155S1 fw_recovery: rate-limited (last attempt %u ms ago, min %u ms)\n",
                jiffies_to_msecs(now - aml_w1_fw_recovery_last_jiffies),
                jiffies_to_msecs(min_interval));
        mutex_unlock(&aml_w1_fw_recovery_lock);
        return;
    }

    pr_warn("W155S1 fw_recovery: starting firmware re-download\n");

    aml_sdio_disable_irq(1); 

    hal_tx_flush(0);

    msleep(50);

    repair_ret = hal_fw_repair();

    aml_sdio_enable_irq(1);

    aml_w1_fw_recovery_last_jiffies = jiffies;

    if (repair_ret != 0) {
        pr_err("W155S1 fw_recovery: hal_fw_repair() returned %d; SDIO state may be corrupt\n",
               repair_ret);
    } else {
        pr_warn("W155S1 fw_recovery: success (HiStatus counters reset)\n");
    }

    mutex_unlock(&aml_w1_fw_recovery_lock);
}

static void drv_scan_start_ex(SYS_TYPE param1, SYS_TYPE param2,
                              SYS_TYPE param3, SYS_TYPE param4,
                              SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;
    unsigned int data;

    if (drv_priv->drv_scanning == 0)
    {
        driv_ps_wakeup(drv_priv);
    }
    drv_priv->drv_scanning = 1;
    data = drv_priv->stop_noa_flag << 8 | drv_priv->drv_scanning;
    drv_hal_scancmd(data);
    
}

static void drv_scan_start(struct drv_private *drv_priv)
{
    drv_hal_add_workitem((WorkHandler)drv_scan_start_ex, NULL,
                         (SYS_TYPE)drv_priv, 0, 0, 0, 0);
}

static void drv_fw_recovery(struct drv_private *drv_priv)
{
    if (drv_priv == NULL)
        return;

    aml_w1_fw_recovery();
}

static void
drv_scan_end_ex(SYS_TYPE param1, SYS_TYPE param2, SYS_TYPE param3,
                SYS_TYPE param4, SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;
    unsigned char prescanning = drv_priv->drv_scanning;
    unsigned int data;

    drv_priv->drv_scanning = 0;
    data = drv_priv->stop_noa_flag << 8 | drv_priv->drv_scanning;
    drv_hal_scancmd(data);

    if (prescanning == 1)
    {
        driv_ps_sleep(drv_priv);
    }
}

static void drv_scan_end(struct drv_private *drv_priv)
{
    drv_hal_add_workitem((WorkHandler)drv_scan_end_ex,
                         NULL, (SYS_TYPE)drv_priv, 0, 0, 0, 0);
}

unsigned char print_ctl = 0;
extern unsigned char print_type;

static void drv_print_fwlog_ex(SYS_TYPE param1, SYS_TYPE param2, SYS_TYPE param3,
                               SYS_TYPE param4, SYS_TYPE param5)
{
    unsigned char *logbuf_ptr = (unsigned char *)param1;
    int databyte = (int)param2;
    unsigned char new_string[64] = { 0 };
    int i = 0;

    if (print_type == 1) 
    {
        
        print_ctl = print_ctl ^ 1;
        databyte = databyte >> 1;
        if (print_ctl == 0)
        {
            logbuf_ptr += databyte;
        }
    }

    AML_OUTPUT("logbuf_ptr 0x%x\n", logbuf_ptr);
    AML_OUTPUT("fw_log:\n");

    while (databyte--)
    {
        if ((*logbuf_ptr == '\n') || (i == 63)) {
            new_string[i] = *logbuf_ptr;
            AML_OUTPUT("%s", new_string);
            memset(new_string, 0, 64);
            i = 0;
        } else {
            new_string[i] = *logbuf_ptr;
            i++;
        }

        logbuf_ptr++;
    }

    AML_OUTPUT("%s", new_string);

    AML_OUTPUT("\nfwlog_print end\n");
}

static void drv_print_fwlog(unsigned char *logbuf_ptr, int databyte)
{
    drv_hal_add_workitem((WorkHandler)drv_print_fwlog_ex,
                         NULL, (SYS_TYPE)logbuf_ptr, (SYS_TYPE)databyte,
                         0, 0, 0);
}

static void
drv_connect_start_ex(SYS_TYPE param1, SYS_TYPE param2,
                     SYS_TYPE param3, SYS_TYPE param4, SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;

    if(drv_priv->drv_connetting == 0)
    {
        driv_ps_wakeup(drv_priv);
    }
    drv_priv->drv_connetting = 1;
}

static void
drv_connect_end_ex(SYS_TYPE param1, SYS_TYPE param2,
                   SYS_TYPE param3, SYS_TYPE param4, SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;

    unsigned char preconnetting = drv_priv->drv_connetting;

    drv_priv->drv_connetting = 0;

    if (preconnetting == 1)
    {
        driv_ps_sleep(drv_priv);

    }
}

static void
drv_connect_start(struct drv_private *drv_priv)
{
    drv_hal_add_workitem((WorkHandler)drv_connect_start_ex,
                         NULL, (SYS_TYPE)drv_priv, 0, 0, 0, 0);
}

static void
drv_connect_end(struct drv_private *drv_priv)
{
    drv_hal_add_workitem((WorkHandler)drv_connect_end_ex,
                         NULL, (SYS_TYPE)drv_priv, 0, 0, 0, 0);
}

static void drv_set_channel_rssi_ex(SYS_TYPE param1, SYS_TYPE param2,
                                    SYS_TYPE param3, SYS_TYPE param4,
                                    SYS_TYPE param5)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.phy_set_channel_rssi(param1);
}

static void drv_set_channel_rssi(struct drv_private *drv_priv, unsigned char rssi)
{
    static unsigned char pre_rssi = 0;

    if (pre_rssi != rssi) {
        pre_rssi = rssi;
        drv_hal_add_workitem((WorkHandler)drv_set_channel_rssi_ex, NULL,
                             (SYS_TYPE)rssi, 0, 0, 0, 0);
    }
}

static void drv_set_tx_power_accord_rssi_ex(SYS_TYPE param1, SYS_TYPE param2,
                                            SYS_TYPE param3, SYS_TYPE param4,
                                            SYS_TYPE param5)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.phy_set_tx_power_accord_rssi(param1, param2, param3, param4);
}

static void drv_set_tx_power_accord_rssi(struct drv_private *drv_priv,
                                         struct hal_channel *hchan, unsigned char rssi,
                                         unsigned char power_mode)
{
    drv_hal_add_workitem((WorkHandler)drv_set_tx_power_accord_rssi_ex, NULL,
                         (SYS_TYPE)hchan->chan_bw, (SYS_TYPE)hchan->channel,
                         (SYS_TYPE)rssi, (SYS_TYPE)power_mode, 0);
}

static void drv_cfg_txpwr_cffc_param_ex(SYS_TYPE param1, SYS_TYPE param2,
                                        SYS_TYPE param3, SYS_TYPE param4,
                                        SYS_TYPE param5)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_cfg_txpwr_cffc_param((void*)param1, (void*)param2);
}

static void drv_cfg_txpwr_cffc_param(struct wifi_channel *chan,
                                     struct tx_power_plan *txpwr_plan)
{
    drv_hal_add_workitem((WorkHandler)drv_cfg_txpwr_cffc_param_ex, NULL,
                         (SYS_TYPE)chan, (SYS_TYPE)txpwr_plan, 0, 0, 0);
}

static void drv_set_channel(struct drv_private *drv_priv, struct hal_channel *hchan,
                            unsigned char flag, unsigned char vid, unsigned char opmode)
{
#ifdef RF_T9026
    struct hal_private* hal_priv = hal_get_priv();
#endif
    DPRINTF(AML_DEBUG_BWC,"%s %d ieee_chan=%d, pn:%d, cn:%d, bw=%d, drv_chan:%d, drv_pn:%d, drv_cn:%d, drv_bw:%d \n",
            __func__, __LINE__, hchan->pchan_num, hchan->pchan_num, hchan->cchan_num, hchan->chan_bw,
            drv_priv->drv_curchannel.channel, drv_priv->drv_curchannel.pchan_num,
            drv_priv->drv_curchannel.cchan_num, drv_priv->drv_curchannel.chan_bw);

#ifdef RF_T9026
    if (hal_priv->bRfInit) {
#endif
        driv_ps_wakeup(drv_priv);

        if (hchan != NULL) {
            drv_hal_setchannel(hchan, flag, vid, opmode);
            drv_hal_set_chan_support(hchan);
            drv_priv->drv_curchannel = *hchan;

            DPRINTF(AML_DEBUG_SCAN, "%s (%d): channel_freq %d pchan_num %d cchan_num %d chan_bw %d\n",
                    __func__, __LINE__, hchan->channel, hchan->pchan_num, hchan->cchan_num, hchan->chan_bw);
        }
        driv_ps_sleep(drv_priv);
#ifdef RF_T9026
    }
#endif
}

static int drv_set_bssid(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                         unsigned char *bssid)
{
    int err;
    struct hal_private* hal_priv = hal_get_priv();

    memcpy((void *)(drv_priv->drv_bssid), bssid, WIFINET_ADDR_LEN);
    driv_ps_wakeup(drv_priv);
    err = hal_priv->hal_ops.phy_set_mac_bssid(wnet_vif_id,bssid);
    driv_ps_sleep(drv_priv);

    return err;
}

static void drv_set_mac_addr(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                             unsigned char *macaddr)
{
    driv_ps_wakeup(drv_priv);
    drv_hal_setmac(wnet_vif_id, macaddr);
    driv_ps_sleep(drv_priv);
}

static void
drv_bcn_init( struct drv_private *drv_priv, const  unsigned char wnet_vif_id,
    unsigned int _bperiod)
{
    pr_debug("<running> %s %d \n",__func__,__LINE__);
    driv_ps_wakeup(drv_priv);
    drv_hal_beaconinit( wnet_vif_id, _bperiod);
    driv_ps_sleep(drv_priv);
}
static void
drv_set_bcn_start( struct drv_private *drv_priv, unsigned char wnet_vif_id,
    unsigned short intval, unsigned char dtim_count,unsigned short bsstype)
{
    pr_debug("<running> %s %d \n",__func__,__LINE__);
    driv_ps_wakeup(drv_priv);
    drv_hal_set_bcn_start( wnet_vif_id, intval,  dtim_count, bsstype);
    driv_ps_sleep(drv_priv);
}

static void
drv_put_bcn_buf(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                unsigned char *pBeacon, unsigned short len,
                unsigned char Rate, unsigned short Flag)
{
    
    driv_ps_wakeup(drv_priv);
    drv_hal_put_bcn_buf(wnet_vif_id, pBeacon, len, Rate, Flag);
    driv_ps_sleep(drv_priv);
}

static int
drv_hal_set_p2p_opps_cwend_enable(struct drv_private *drv_priv,
                                  unsigned char vid, unsigned char p2p_oppps_cw)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d p2p_oppps_cw=%xTU\n",
            __func__,__LINE__,vid, p2p_oppps_cw);
    return drv_priv->hal_priv->hal_ops.phy_set_p2p_opps_cwend_enable(vid, p2p_oppps_cw);
}

static int
drv_hal_set_p2p_noa_enable(struct drv_private *drv_priv, unsigned char vid,
                           unsigned int duration, unsigned int interval,
                           unsigned int starttime, unsigned char count,
                           unsigned char flag)
{
    unsigned char lflag = 0;

    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d count=%d\n", __func__,__LINE__, vid, count);
    if (flag & P2P_NOA_START_MATCH_BEACON_HI)
    {
        lflag |= P2P_NOA_START_MATCH_BEACON;
    }
    else if (flag & P2P_NOA_END_MATCH_BEACON_HI)
    {
        lflag |= P2P_NOA_END_MATCH_BEACON;
    }
    return drv_priv->hal_priv->hal_ops.phy_set_p2p_noa_enable(vid, duration,
                                                              interval, starttime,
                                                              count, lflag);
}
static unsigned long long
drv_hal_get_tsf(struct drv_private *drv_priv, unsigned char vid)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d\n", __func__,__LINE__, vid);
    return drv_priv->hal_priv->hal_ops.phy_get_tsf(vid);
}

static int drv_send_null_data(struct drv_private *drv_priv,
                              struct NullDataCmd null_data, int len)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, ps=%d\n", __func__,__LINE__,
            null_data.vid, null_data.pwr_save);

    return drv_priv->hal_priv->hal_ops.phy_send_null_data(null_data, len);
}

static int drv_keep_alive(struct drv_private *drv_priv,
                          struct NullDataCmd null_data, int len,
                          int enable, int period)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d enable=%d, vid=%d, period=%d\n",
            __func__, __LINE__, enable, null_data.vid, period);

    return drv_priv->hal_priv->hal_ops.phy_keep_alive(null_data, len, enable, period);
}

static int drv_set_beacon_miss(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                               unsigned char enable, int period)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, enable=%d, period=%d\n",
            __func__,__LINE__, wnet_vif_id, enable, period);
    return drv_priv->hal_priv->hal_ops.phy_set_beacon_miss(wnet_vif_id, enable, period);
}

static int drv_set_vsdb(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                        unsigned char enable)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, enable=%d\n",
            __func__,__LINE__, wnet_vif_id, enable);
    return drv_priv->hal_priv->hal_ops.phy_set_vsdb(wnet_vif_id, enable);
}

static int
drv_set_arp_agent(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                  unsigned char enable, unsigned int ipv4, unsigned char *ipv6)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, enable=%d, ipv4=0x%x\n",
            __func__,__LINE__, wnet_vif_id, enable, ipv4);
    return drv_priv->hal_priv->hal_ops.phy_set_arp_agent(wnet_vif_id, enable, ipv4, ipv6);
}

static int
drv_set_pattern(struct drv_private *drv_priv, unsigned char vid,
                unsigned char offset,unsigned char len, unsigned char id,
                unsigned char *mask, unsigned char *pattern)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, pattern len=%d mask=0x%x\n",
            __func__,__LINE__, vid, len, *(unsigned int*)mask);
    return drv_priv->hal_priv->hal_ops.phy_set_pattern(vid, offset, len, id, mask, pattern);
}

static int drv_set_suspend(struct drv_private *drv_priv, unsigned char vid,
                           unsigned char enable, unsigned char mode, unsigned int filters)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d vid=%d, enable=%d filters=0x%x\n",
            __func__,__LINE__, vid, enable, filters);

    if (enable == 1)
        drv_priv->dot11VmacPsState[vid] = DRV_PWRSAVE_FULL_SLEEP;
    else
        drv_priv->dot11VmacPsState[vid] = DRV_PWRSAVE_AWAKE;

    return drv_priv->hal_priv->hal_ops.phy_set_suspend(vid, enable, mode, filters);
}

int drv_hal_tx_frm_pause(struct drv_private *drv_priv, int pause)
{
    DPRINTF(AML_DEBUG_HAL, "%s %d pause=%d\n", __func__,__LINE__, pause);
    return drv_priv->hal_priv->hal_ops.hal_txframe_pause(pause);
}

static void
clear_staid_and_bssid_ex(SYS_TYPE param1, SYS_TYPE param2, SYS_TYPE param3,
                         SYS_TYPE param4,SYS_TYPE param5)
{
    struct drv_private *drv_priv = (void *)param1;
    unsigned char wnet_vif_id = param2;
    unsigned short sta_id = param3;
    struct wlan_net_vif *wnet_vif;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL)
    {
        return;
    }

    driv_ps_wakeup(drv_priv);
    hal_phy_unregister_sta_id(wnet_vif_id,sta_id);

    if (wnet_vif->vm_hal_opmode == WIFI_M_STA) {
        unsigned char bssid[6] = { 0 };

        drv_set_bssid(drv_priv, wnet_vif_id, bssid);
    }

    driv_ps_sleep(drv_priv);
    return;
}

static int
drv_clear_staid_and_bssid(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                          unsigned short sta_id)
{
    drv_hal_add_workitem((WorkHandler)clear_staid_and_bssid_ex, NULL,
                         (SYS_TYPE)drv_priv,(SYS_TYPE)wnet_vif_id,
                         (SYS_TYPE)sta_id,(SYS_TYPE)0, (SYS_TYPE)0);
    return 0;
}

static void
drv_wnet_vif_disconnect_ex(SYS_TYPE param1, SYS_TYPE param2, SYS_TYPE param3,
                           SYS_TYPE param4, SYS_TYPE param5)
{
    struct drv_private *drv_priv = (void *)param1;
    int wnet_vif_id = param2;

    driv_ps_wakeup(drv_priv);
    drv_hal_wnet_vifdisconnect(wnet_vif_id);
    driv_ps_sleep(drv_priv);
    return;
}

static int
drv_wnet_vif_disconnect(struct drv_private *drv_priv, int wnet_vif_id)
{
    drv_hal_add_workitem((WorkHandler)drv_wnet_vif_disconnect_ex, NULL,
                         (SYS_TYPE)drv_priv, (SYS_TYPE)wnet_vif_id, (SYS_TYPE)0,
                         (SYS_TYPE)0,  (SYS_TYPE)0);
    return 0;
}

static int drv_open(struct drv_private *drv_priv)
{
    struct wifi_mac *wifimac = NULL;
    int error = 0;
    wifimac = (struct wifi_mac *)(drv_priv->wmac);

    driv_ps_wakeup(drv_priv);
    drv_txlist_flushfree(drv_priv, 3);

    if (!drv_hal_reset())
    {
        ERROR_DEBUG_OUT("unable to reset chip\n");
        error = -EIO;
        goto done;
    }

    drv_hal_enable();
    memset((void *)(&drv_priv->drv_curchannel), 0, sizeof(struct hal_channel));
    drv_priv->drv_not_init_flag = 0;

done:
    driv_ps_sleep(drv_priv);
    return error;
}

static void drv_get_sts(struct drv_private *drv_priv, unsigned int op_code,
                        unsigned int ctrl_code)
{
    int i;
    struct wifi_mac *wifimac = wifi_mac_get_mac_handle();
    struct wlan_net_vif *connect_wnet = wifi_mac_running_wnet_vif(wifimac);

    if (connect_wnet != NULL) {
        struct wifi_station *sta;
        struct aml_driver_nsta *drv_sta;

        sta = connect_wnet->vm_mainsta;
        drv_sta = sta->drv_sta;
        pr_debug("\n--------drv statistic--------\n");

        for (i = 0; i < WME_NUM_AC; i++)
        {
            struct drv_txlist *txlist;

            txlist = drv_txlist_initial(drv_priv, i);
            pr_debug("queen %d ac_queue_buffer_in_drv %d\n", i, txlist->txds_pending_cnt);

        }

        for (i = 0; i < WME_NUM_TID; i++)
        {
            struct drv_tx_scoreboard *tid;

            tid = &drv_sta->tx_agg_st.tid[i];
            pr_debug("tid %d tid_queue_buffer_in_drv %d, msdu_count %d\n", i,
                   WIFINET_SAVEQ_QLEN(&tid->tid_tx_buffer_queue), wifimac->msdu_cnt[i]);

        }
        pr_debug("tx_buffer_queue_in_drv %d\n",
               WIFINET_SAVEQ_QLEN(&connect_wnet->vm_tx_buffer_queue));
    }

    if ((ctrl_code & STS_MOD_DRV) == STS_MOD_DRV)
    {
        if ((ctrl_code & STS_TYP_TX) == STS_TYP_TX)
        {
            for(i = 0; i < HAL_NUM_TX_QUEUES; i++)
            {
                pr_debug("txlist_idx %d,  mpdu_to_hal %d,mpdu_pend %d, "
                       "mpdu_bkup %d,last_time %ld\n",
                       drv_priv->drv_txlist_table[i].txlist_qnum,
                       drv_priv->drv_txlist_table[i].txlist_qcnt,
                       drv_priv->drv_txlist_table[i].txds_pending_cnt,
                       drv_priv->drv_txlist_table[i].txlist_backup_qcnt,
                       drv_priv->drv_txlist_table[i].txlist_lasttime);
            }

            pr_debug("--**--tx_retry_cnt %d\n", drv_priv->drv_stats.tx_retry_cnt);
            pr_debug("--**--tx_short_retry_cnt %d\n", drv_priv->drv_stats.tx_short_retry_cnt);
            pr_debug("tx_ampdu_mutil %d\n", drv_priv->drv_stats.tx_pkts_cnt);
            pr_debug("tx_ampdu_uniq %d\n", drv_priv->drv_stats.tx_ampdu_one_frame_cnt);
            
            pr_debug("tx_normal mpdu %d\n", drv_priv->drv_stats.tx_normal_cnt);

            pr_debug("tx_ampdu_fail %d\n", drv_priv->drv_stats.tx_end_fail_cnt);
            
            pr_debug("tx_end_normal %d\n", drv_priv->drv_stats.tx_end_normal_cnt);
            
        }

        if ((ctrl_code & STS_TYP_RX) == STS_TYP_RX)
        {
            pr_debug("rx_indicate %d\n", drv_priv->drv_stats.rx_indicate_cnt);
            pr_debug("rx_free_skb %d\n", drv_priv->drv_stats.rx_free_skb_cnt);
            pr_debug("rx_ampdu %d\n", drv_priv->drv_stats.rx_ampdu_cnt);
            pr_debug("rx_bar %d\n", drv_priv->drv_stats.rx_bar_cnt);
            pr_debug("rx_nonqos %d\n", drv_priv->drv_stats.rx_nonqos_cnt);
            pr_debug("rx_dup %d\n", drv_priv->drv_stats.rx_dup_cnt);
            pr_debug("rx_ok %d\n", drv_priv->drv_stats.rx_ok_cnt);
            pr_debug("rx_bardrop %d\n", drv_priv->drv_stats.rx_bardrop_cnt);
            pr_debug("rx_skipped %d\n", drv_priv->drv_stats.rx_skipped_cnt);
            pr_debug("--**--rx_ack_rssi %d\n", drv_priv->drv_stats.rx_ack_rssi);
        }

    }

    drv_priv->hal_priv->hal_ops.hal_get_sts(op_code, ctrl_code);
}

static int drv_stop(struct drv_private *drv_priv)
{
    driv_ps_wakeup(drv_priv);
    drv_priv->drv_config.cfg_rtcenable = 0;
    driv_ps_sleep(drv_priv);
    return 0;
}

int drv_reset(void * dev)
{
    struct drv_private *drv_priv = (struct drv_private *)dev;
    int error = 0;

    driv_ps_wakeup(drv_priv);

    if (!drv_hal_reset())
    {
        ERROR_DEBUG_OUT("unable to reset chip\n");
        error = -EIO;
    }

    driv_ps_sleep(drv_priv);
    return error;
}

static int drv_suspend(void *dev)
{
    struct drv_private *drv_priv = (struct drv_private *)dev;
    driv_ps_wakeup(drv_priv);

    drv_hal_disable();
    driv_ps_sleep(drv_priv);
    msleep(10);

    if (drv_priv->drv_not_init_flag)
        return -EIO;

    drv_stop(drv_priv);

    drv_priv->drv_not_init_flag = 1;

    AML_OUTPUT("<running>\n");

    return 0;
}

static void
drv_set_chan_bw_mode_orig(SYS_TYPE param1, SYS_TYPE param2,
                          SYS_TYPE param3, SYS_TYPE param4,
                          SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;
    int chanbw = (int)param2;

    driv_ps_wakeup(drv_priv);
    drv_hal_set11nmac2040( chanbw);
    driv_ps_sleep(drv_priv);
}

static void
drv_set_chan_bw_mode(struct drv_private *drv_priv, enum wifi_mac_chanbw bw)
{
    drv_hal_add_workitem((WorkHandler)drv_set_chan_bw_mode_orig, NULL,
                         (SYS_TYPE)drv_priv, (SYS_TYPE)bw, 0, 0, 0);
}

static int
drv_get_scn_chan_busy(struct drv_private *drv_priv)
{
    int ret;

    driv_ps_wakeup(drv_priv);
    ret = drv_hal_get11nextbusy();
    driv_ps_sleep(drv_priv);

    return ret;
}

static unsigned int
drv_low_reg_behind_task(struct drv_private *drv_priv, void *func)
{
    return drv_low_register_behind_task(func);
}

static unsigned int
drv_low_call_task(struct drv_private *drv_priv, SYS_TYPE taskid,
                  SYS_TYPE param1)
{
    return drv_low_call_register_task(taskid, param1);
}

unsigned int drv_low_add_worktask(struct drv_private *drv_priv, void *func,
                                  void *func_cb, SYS_TYPE param1, SYS_TYPE param2,
                                  SYS_TYPE param3, SYS_TYPE param4, SYS_TYPE param5)
{
    return drv_hal_add_workitem(func, func_cb, param1, param2, param3, param4, param5);
}

static unsigned int
drv_phy_reg_sta_id(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                   unsigned short StaAid, unsigned char *pMac, unsigned char encrypt)
{
    unsigned int ret;

    driv_ps_wakeup(drv_priv);
    ret = hal_phy_register_sta_id(wnet_vif_id, StaAid, pMac, encrypt);
    driv_ps_sleep(drv_priv);

    return ret;
}

static unsigned int drv_phy_unreg_all_sta_id(struct drv_private *drv_priv,
                                             unsigned char wnet_vif_id)
{
    unsigned int ret;

    driv_ps_wakeup(drv_priv);
    ret =  hal_phy_unregister_all_sta_id(wnet_vif_id);
    driv_ps_sleep(drv_priv);

    return ret;
}

int drv_add_wnet_vif(struct drv_private *drv_priv, int wnet_vif_id,
                     void *if_data, enum hal_op_mode vm_opmode,
                     unsigned char *myaddr, unsigned int ip)
{
    struct wlan_net_vif *wnet_vif;

    if (wnet_vif_id >= DEFAULT_MAX_VMAC) {
        return -EINVAL;
    }

    if ((drv_priv->drv_wnet_vif_table[wnet_vif_id] != NULL) &&
        (drv_priv->drv_wnet_vif_table[wnet_vif_id]->vm_wmac->recovery_stat == WIFINET_RECOVERY_END)) {
        ERROR_DEBUG_OUT("Invalid interface id = %u, recovery %d \n", wnet_vif_id,
                        drv_priv->drv_wnet_vif_table[wnet_vif_id]->vm_wmac->recovery_stat);
        return -EINVAL;
    }

    driv_ps_wakeup(drv_priv);
    pr_debug("%s(%d) macaddr "MAC_FMT"\n", __func__, __LINE__, MAC_ARG(myaddr));

    if (vm_opmode == WIFI_M_HOSTAP) {
        
        drv_set_bssid(drv_priv, wnet_vif_id, myaddr);
    }

    wnet_vif = (struct wlan_net_vif *)if_data;
    
    wnet_vif->vm_hal_opmode = vm_opmode;
    AML_OUTPUT("<%s> hal_opmode=%d \n", VMAC_DEV_NAME(wnet_vif), wnet_vif->vm_hal_opmode);

    drv_priv->drv_wnet_vif_table[wnet_vif_id] = wnet_vif;
    drv_priv->drv_wnet_vif_num++;

#ifdef  AML_MCAST_QUEUE
    drv_tx_mcastq_init(drv_priv, wnet_vif_id);
#endif

    drv_hal_setmac( wnet_vif_id, myaddr);
    drv_hal_setopmode( wnet_vif_id, vm_opmode);
    drv_hal_setdhcp(wnet_vif_id, ip);
    driv_ps_sleep(drv_priv);

    return 0;
}

static int drv_delete_wnet_vif(struct drv_private *drv_priv, int wnet_vif_id)
{
    int flags;

    driv_ps_wakeup(drv_priv);

    flags = drv_priv->net_ops->wifi_mac_get_netif_cfg(drv_priv->wmac);

#ifdef  AML_MCAST_QUEUE
    drv_tx_mcastq_cleanup(drv_priv, wnet_vif_id);
#endif

    drv_priv->drv_wnet_vif_table[wnet_vif_id] = NULL;
    drv_priv->drv_wnet_vif_num--;

    if ((flags & NETCOM_NETIF_RUNNING) && drv_priv->drv_wnet_vif_num)
    {
        drv_hal_interset(1);
    }

    drv_hal_wnet_vifStop(wnet_vif_id);
    driv_ps_sleep(drv_priv);

    return 0;
}

static int
drv_change_wnet_vif(struct drv_private *drv_priv, int wnet_vif_id, void *if_data,
                    enum hal_op_mode vm_opmode, unsigned char *myaddr, unsigned int ip)
{
    struct wlan_net_vif *wnet_vif;

    if (wnet_vif_id >= DEFAULT_MAX_VMAC ||
        drv_priv->drv_wnet_vif_table[wnet_vif_id] == NULL)
    {
        ERROR_DEBUG_OUT("Invalid interface id = %u\n", wnet_vif_id);
        return -EINVAL;
    }

    driv_ps_wakeup(drv_priv);
    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];

    if (vm_opmode == WIFI_M_HOSTAP)
        drv_set_bssid(drv_priv, wnet_vif_id, myaddr);

    wnet_vif->vm_hal_opmode = vm_opmode;

    drv_hal_setmac(wnet_vif_id, myaddr);
    drv_hal_setopmode(wnet_vif_id, vm_opmode);
    drv_hal_setdhcp(wnet_vif_id, ip);
    driv_ps_sleep(drv_priv);

    return 0;
}

static void *
drv_nsta_attach( struct drv_private *drv_priv, int wnet_vif_id, void *sta)
{
    struct aml_driver_nsta *drv_sta;

    drv_sta = (struct aml_driver_nsta *)NET_MALLOC(sizeof(struct aml_driver_nsta),
                                                   GFP_ATOMIC, "drv_nsta_attach.drv_sta");
    if (drv_sta == NULL)
    {
        ERROR_DEBUG_OUT("<running> drv_sta is NULL \n");
        return NULL;
    }

    drv_sta->net_nsta = sta;
    drv_sta->sta_drv_priv = drv_priv;

    if (drv_priv->drv_ratectrl_size <= 0)
        drv_priv->drv_ratectrl_size = 1;
    drv_sta->sta_rc_nsta = ZMALLOC(drv_priv->drv_ratectrl_size,
                                   "drv_sta->sta_rc_nsta", GFP_ATOMIC);

    if (drv_sta->sta_rc_nsta == NULL)
    {
        ERROR_DEBUG_OUT("<running> drv_sta->sta_rc_nsta == NULL\n");
        FREE(drv_sta, "drv_nsta_attach.drv_sta");
        return NULL;
    }

    pr_debug("%s %d drv_ratectrl_size = %d sta_rc_nsta = %p\n",
            __func__, __LINE__, drv_priv->drv_ratectrl_size,
            drv_sta->sta_rc_nsta);

    drv_txlist_init_for_sta(drv_priv, drv_sta, wnet_vif_id);
    drv_rx_nsta_init(drv_priv, drv_sta);
    drv_tx_uapsd_nsta_init(drv_priv, drv_sta);

    DPRINTF(AML_DEBUG_INFO, "<running> %s %d  \n",__func__,__LINE__);

    return drv_sta;
}

static void
drv_nsta_detach(struct drv_private *drv_priv, void *nsta)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);

    drv_txlist_free_for_sta(drv_priv, drv_sta);
    drv_rx_nsta_free(drv_priv, drv_sta);

    smp_store_release(&drv_sta->net_nsta, NULL);
    pr_debug("%s drv_sta:%p\n", __func__, drv_sta);
    FREE(drv_sta->sta_rc_nsta, "drv_sta->sta_rc_nsta");
    FREE(drv_sta, "drv_nsta_attach.drv_sta");
}

static void
drv_nsta_cleanup( struct drv_private *drv_priv, void *nsta)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);

    drv_sta->sta_cleanup = 1;

    if (drv_sta->sta_isuapsd)
    {
        drv_sta->sta_isuapsd = 0;
        drv_tx_uapsd_nsta_cleanup(drv_priv, drv_sta);
    }

    drv_txlist_cleanup_for_sta(drv_priv, drv_sta);
    drv_rx_nsta_clean(drv_priv, drv_sta);
}

static void
drv_nsta_update_pwrsave(struct drv_private *drv_priv,
                        void *nsta, int pwrsave)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);

    if (pwrsave)
    {
        drv_sta->sta_powesave = 1;
        drv_txlist_pause_for_sta(drv_priv, drv_sta);
    }
    else
    {
        drv_sta->sta_powesave = 0;
        drv_txlist_resume_for_sta(drv_priv, drv_sta);
    }
}

static void
drv_key_rst_ex(SYS_TYPE param1, SYS_TYPE param2, SYS_TYPE param3,
               SYS_TYPE param4, SYS_TYPE param5)
{
    struct drv_private *drv_priv = (struct drv_private *)param1;
    unsigned char wnet_vif_id = (unsigned char)param2;
    unsigned short key_index = (unsigned short)param3;
    int staid = (int)param4;
    driv_ps_wakeup(drv_priv);

    pr_debug("<running> %s %d wnet_vif_id=%d\n",__func__,__LINE__,wnet_vif_id);
    if (staid == 0) {
        drv_hal_keyreset(wnet_vif_id, key_index);
    } else {
        drv_hal_keyclear(wnet_vif_id, staid);
    }

    driv_ps_sleep(drv_priv);
}

static void
drv_key_rst(struct drv_private *drv_priv, unsigned char wnet_vif_id,
            unsigned short key_index, int staid)
{
    drv_hal_add_workitem((WorkHandler)drv_key_rst_ex, NULL,
                         (SYS_TYPE)drv_priv, (SYS_TYPE)wnet_vif_id,
                         (SYS_TYPE)key_index, (SYS_TYPE)staid,
                         (SYS_TYPE)0);
}

static int
drv_key_set(struct drv_private *drv_priv, unsigned char wnet_vif_id,
            unsigned short key_index, struct hal_key_val *hk,
            const unsigned char mac[WIFINET_ADDR_LEN])
{
    int status = 1;
    struct wlan_net_vif *wnet_vif;
    struct wifi_station *sta;

    if (drv_priv == NULL || hk == NULL || mac == NULL || wnet_vif_id >= DEFAULT_MAX_VMAC)
        return 1;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL)
        return 1;

    driv_ps_wakeup(drv_priv);
    if ((mac[0] & 0x01) == 0x01)
    {
        status = drv_hal_keyset(wnet_vif_id, key_index, hk, (unsigned char *)mac, 0);
    }
    else
    {
        if (hk->kv_pad == WIFINET_M_HOSTAP) 
        {
            if (drv_priv->net_ops == NULL || drv_priv->net_ops->wifi_mac_get_sta == NULL) {
                driv_ps_sleep(drv_priv);
                return 1;
            }
            sta = drv_priv->net_ops->wifi_mac_get_sta(&wnet_vif->vm_sta_tbl, mac,wnet_vif_id);
            if (sta == NULL) {
                driv_ps_sleep(drv_priv);
                return 1;
            }
            status = drv_hal_keyset(wnet_vif_id, key_index, hk,
                                    (unsigned char *)mac,sta->sta_associd);
        }
        else
        {
            status = drv_hal_keyset(wnet_vif_id,key_index, hk,
                                    (unsigned char *)mac,1);
        }
    }

    driv_ps_sleep(drv_priv);
    return (status != 0);
}

static int
drv_rekey_data_set(struct drv_private *drv_priv, unsigned char wnet_vif_id,
                   void  *rekey_data)
{
    int status;

    driv_ps_wakeup(drv_priv);
    status = drv_hal_rekey_data_set(wnet_vif_id, rekey_data, 1);
    driv_ps_sleep(drv_priv);

    return (status != 0);
}

static int
drv_set_country(struct drv_private *drv_priv, char *isoName)
{
    unsigned int wMode;
    int tmpi;

    tmpi = find_country_code((unsigned char *)isoName);
    if (tmpi == 0xff) {
        
        tmpi = 0;
    }

    drv_priv->drv_config.cfg_countrycode = tmpi;
    drv_priv->drv_config.cfg_txpoweplan = wifimac_get_tx_pwr_plan(tmpi);

    pr_debug("<running> %s %d %d\n",__func__,__LINE__, drv_priv->drv_config.cfg_countrycode );
    wMode = drv_priv->drv_config.cfg_modesupport;

    if (drv_priv->net_ops->wifi_mac_setup_channel_list)
    {
        drv_priv->net_ops->wifi_mac_setup_channel_list(drv_priv->wmac, wMode,
                                                       drv_priv->drv_config.cfg_countrycode);
    }

    return 0;
}

extern struct country_chan_mapping  country_chan_mapping_list[];
static void
drv_get_curr_cntry(struct drv_private *drv_priv, unsigned char *iso_name)
{
    if (drv_priv->drv_config.cfg_countrycode != 0xff)
    {
        iso_name[0] = country_chan_mapping_list[drv_priv->drv_config.cfg_countrycode].country[0];
        iso_name[1] = country_chan_mapping_list[drv_priv->drv_config.cfg_countrycode].country[1];
        iso_name[2] = 0;
    }
    else
    {
        iso_name[0] = 'W';
        iso_name[1] = 'W';
        iso_name[2] = 0;
    }
}

static int
drv_set_quiet(struct drv_private *drv_priv, unsigned short period,
              unsigned short duration, unsigned short nextStart,
              unsigned short enabled)
{
    
    return 0;
}

void
drv_set_tx_pwr_limit(struct drv_private *drv_priv, unsigned int limit,
                     unsigned short txpowerdb)
{
    limit = clamp_t(unsigned int, limit, 1, 20);
    drv_priv->drv_config.cfg_txpowlimit = (unsigned short)limit;

    drv_update_tx_pwr(drv_priv, txpowerdb);
}

static short
drv_get_noise_floor(struct drv_private *drv_priv,
                    unsigned short freq, unsigned int chan_flags)
{
    return -85;
}

static void drv_write_word(unsigned int addr, unsigned int data)
{
    struct hal_private* hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_write_word(addr, data);
}

static unsigned int drv_read_word(unsigned int addr)
{
    struct hal_private* hal_priv = hal_get_priv();

    return hal_priv->hal_ops.hal_read_word(addr);
}

static void drv_bt_write_word(unsigned int addr, unsigned int data)
{
    struct hal_private* hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_bt_write_word(addr, data);
}

static unsigned int drv_bt_read_word(unsigned int addr)
{
    struct hal_private* hal_priv = hal_get_priv();

    return hal_priv->hal_ops.hal_bt_read_word(addr);
}

static void drv_pt_rx_start(unsigned int qos)
{
    struct hal_private* hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_pt_rx_start(qos);
}

static void drv_pt_rx_stop(void)
{
    struct hal_private* hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_pt_rx_stop();
}

static void drv_set_bmfm_info(struct drv_private *drv_priv, int wnet_vif_id,
                              unsigned char *group_id, unsigned char *user_position,
                              unsigned char feedback_type)
{
    drv_hal_set_bmfm_info(wnet_vif_id, group_id, user_position, feedback_type);
}

static void drv_interface_enable(unsigned char enable, unsigned char vid)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.phy_interface_enable(enable, vid);
}

static void drv_cfg_cali_param(void)
{
    struct hal_private *hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_cfg_cali_param();
}

static void drv_init_ops(struct drv_private *drv_priv)
{
    drv_priv->drv_ops.drv_open = drv_open;				
    drv_priv->drv_ops.drv_stop = drv_suspend;				
    drv_priv->drv_ops.add_interface = drv_add_wnet_vif;			
    drv_priv->drv_ops.remove_interface = drv_delete_wnet_vif;		
    drv_priv->drv_ops.change_interface = drv_change_wnet_vif;		
    drv_priv->drv_ops.down_interface = drv_wnet_vif_disconnect;		
    drv_priv->drv_ops.alloc_nsta = drv_nsta_attach;			
    drv_priv->drv_ops.free_nsta = drv_nsta_detach;			
    drv_priv->drv_ops.cleanup_nsta = drv_nsta_cleanup;			
    drv_priv->drv_ops.update_nsta_pwrsave = drv_nsta_update_pwrsave;	
    drv_priv->drv_ops.new_assoc = drv_assoc_proc;			
    drv_priv->drv_ops.reset = drv_reset;				
    drv_priv->drv_ops.set_channel = drv_set_channel;			
    drv_priv->drv_ops.scan_start = drv_scan_start;			
    drv_priv->drv_ops.fw_repair = drv_fw_recovery;			
    drv_priv->drv_ops.scan_end = drv_scan_end;				
    drv_priv->drv_ops.connect_start = drv_connect_start;		
    drv_priv->drv_ops.connect_end = drv_connect_end;			
    drv_priv->drv_ops.set_channel_rssi = drv_set_channel_rssi;		
    drv_priv->drv_ops.set_tx_power_accord_rssi = drv_set_tx_power_accord_rssi;
    drv_priv->drv_ops.tx_init = drv_tx_init;				
    drv_priv->drv_ops.tx_cleanup = drv_txlist_cleanup;			
    drv_priv->drv_ops.tx_wmm_queue_update = drv_update_wmmq_param;	
    drv_priv->drv_ops.tx_start = drv_tx_start;				
    drv_priv->drv_ops.txlist_qcnt_handle = drv_txlist_qcnt;		
    drv_priv->drv_ops.txlist_all_qcnt = drv_txlist_all_qcnt;		
    drv_priv->drv_ops.txlist_isfull = drv_txlist_isfull;		
    drv_priv->drv_ops.rx_init = drv_rx_init;				
    drv_priv->drv_ops.rx_proc_frame = drv_rx_input;			
    drv_priv->drv_ops.check_aggr = drv_aggr_check;			
    drv_priv->drv_ops.aggr_tid_query = drv_aggr_tid;			
    drv_priv->drv_ops.check_aggr_allow_to_send = drv_aggr_allow_to_send;
    drv_priv->drv_ops.set_ampdu_params = drv_set_ampduparams;		
    drv_priv->drv_ops.addba_request_setup = drv_addba_req_setup;	
    drv_priv->drv_ops.addba_response_setup = drv_rx_addbarsp;		
    drv_priv->drv_ops.addba_request_process = drv_rx_addbareq;		
    drv_priv->drv_ops.addba_response_process = drv_addba_rsp_process;	
    drv_priv->drv_ops.addba_clear = drv_addba_clear;			
    drv_priv->drv_ops.delba_process = drv_rx_delba;			
    drv_priv->drv_ops.addba_status = drv_addba_status;			
    drv_priv->drv_ops.drv_txrxampdu_del = drv_txrxampdu_del;		
    drv_priv->drv_ops.set_addbaresponse = drv_set_addba_rsp;		
    drv_priv->drv_ops.clear_addbaresponsestatus = drv_clr_addba_rsp_status;
    drv_priv->drv_ops.set_slottime = drv_update_slot;			
    drv_priv->drv_ops.set_protmode = drv_set_protmode;			
    drv_priv->drv_ops.set_cfg_txpowlimit = drv_set_cfg_tx_power_limit;	
    drv_priv->drv_ops.key_delete = drv_key_rst;				
    drv_priv->drv_ops.key_set = drv_key_set;				
    drv_priv->drv_ops.rekey_data_set = drv_rekey_data_set;		
    drv_priv->drv_ops.awake = drv_pwrsave_awake;			
    drv_priv->drv_ops.netsleep = drv_pwrsave_netsleep;			
    drv_priv->drv_ops.fullsleep = drv_pwrsave_fullsleep;		
    drv_priv->drv_ops.set_country = drv_set_country;			
    drv_priv->drv_ops.get_current_country = drv_get_curr_cntry;		
    drv_priv->drv_ops.set_quiet = drv_set_quiet;			
    drv_priv->drv_ops.set_bwmode = drv_set_chan_bw_mode;		
    drv_priv->drv_ops.GetSecondChanBusy = drv_get_scn_chan_busy;	
    drv_priv->drv_ops.drv_get_config_param = drv_get_config;		
    drv_priv->drv_ops.drv_set_config_param = drv_set_config;		
    drv_priv->drv_ops.get_noisefloor = drv_get_noise_floor;		
    drv_priv->drv_ops.drv_set_txPwrLimit = drv_set_tx_pwr_limit;	
    drv_priv->drv_ops.get_amsdu_supported = drv_get_amsdu_supported;	
    drv_priv->drv_ops.process_uapsd_trigger = drv_process_uapsd_nsta_trigger;
    drv_priv->drv_ops.uapsd_qcnt = drv_tx_uapsd_nsta_qcnt;		
    drv_priv->drv_ops.set_macaddr = drv_set_mac_addr;			
    drv_priv->drv_ops.Low_register_behindTask = drv_low_reg_behind_task;
    drv_priv->drv_ops.Low_callRegisteredTask = drv_low_call_task;	
    drv_priv->drv_ops.Low_addDHWorkTask = drv_low_add_worktask;		
    drv_priv->drv_ops.RegisterStationID = drv_phy_reg_sta_id;		
    drv_priv->drv_ops.clear_staid_and_bssid = drv_clear_staid_and_bssid;
    drv_priv->drv_ops.UnRegisterAllStationID = drv_phy_unreg_all_sta_id;
    drv_priv->drv_ops.phy_setchannelsupport = NULL;			
    drv_priv->drv_ops.Phy_beaconinit = drv_bcn_init;			
    drv_priv->drv_ops.Phy_SetBeaconStart = drv_set_bcn_start;		
    drv_priv->drv_ops.Phy_PutBeaconBuf = drv_put_bcn_buf;		
    
    drv_priv->drv_ops.drv_hal_set_p2p_opps_cwend_enable = drv_hal_set_p2p_opps_cwend_enable;
    
    drv_priv->drv_ops.drv_hal_set_p2p_noa_enable = drv_hal_set_p2p_noa_enable;
    drv_priv->drv_ops.drv_hal_get_tsf = drv_hal_get_tsf;		
    drv_priv->drv_ops.drv_hal_tx_frm_pause = drv_hal_tx_frm_pause;	
    drv_priv->drv_ops.drv_p2p_client_opps_cwend_may_sleep = NULL;
    drv_priv->drv_ops.drv_txq_backup_send = drv_txq_backup_send;
    drv_priv->drv_ops.phy_stc = phy_stc;
    drv_priv->drv_ops.get_snr = get_snr;
    drv_priv->drv_ops.cca_busy_check = cca_busy_check;
    drv_priv->drv_ops.drv_send_null_data = drv_send_null_data;
    drv_priv->drv_ops.drv_keep_alive = drv_keep_alive;
    drv_priv->drv_ops.drv_set_beacon_miss = drv_set_beacon_miss;
    drv_priv->drv_ops.drv_set_vsdb = drv_set_vsdb;
    drv_priv->drv_ops.drv_set_arp_agent = drv_set_arp_agent;
    drv_priv->drv_ops.drv_set_pattern = drv_set_pattern;
    drv_priv->drv_ops.drv_set_suspend = drv_set_suspend;
    drv_priv->drv_ops.drv_set_pkt_drop = drv_set_pkt_drop;
    drv_priv->drv_ops.drv_set_is_mother_channel = drv_set_is_mother_channel;
    drv_priv->drv_ops.drv_flush_normal_buffer_queue = drv_flush_normal_buffer_queue;
    drv_priv->drv_ops.drv_free_normal_buffer_queue = drv_free_normal_buffer_queue;
    drv_priv->drv_ops.drv_flush_txdata = drv_txlist_flushfree;
    drv_priv->drv_ops.drv_get_sts = drv_get_sts;
    drv_priv->drv_ops.drv_tx_pending_pkt = drv_tx_pending_pkt;
    drv_priv->drv_ops.drv_write_word = drv_write_word;
    drv_priv->drv_ops.drv_read_word =  drv_read_word;
    drv_priv->drv_ops.drv_bt_write_word = drv_bt_write_word;
    drv_priv->drv_ops.drv_bt_read_word =  drv_bt_read_word;
    drv_priv->drv_ops.drv_pt_rx_start = drv_pt_rx_start;
    drv_priv->drv_ops.drv_pt_rx_stop = drv_pt_rx_stop;

    drv_priv->drv_ops.drv_set_bmfm_info = drv_set_bmfm_info;
    drv_priv->drv_ops.drv_interface_enable = drv_interface_enable;
    drv_priv->drv_ops.drv_cfg_cali_param = drv_cfg_cali_param;
    drv_priv->drv_ops.drv_cfg_txpwr_cffc_param = drv_cfg_txpwr_cffc_param;
    drv_priv->drv_ops.drv_print_fwlog = drv_print_fwlog;
    drv_priv->drv_ops.drv_set_bssid = drv_set_bssid;
    drv_priv->drv_ops.drv_set_tx_livetime = drv_hal_txlivetime;
}

void aml_set_mac_control_register(void)
{
    unsigned int read_tmp;

    read_tmp = drv_read_word(MAC_CONTROL);
    read_tmp |= BIT(20); 
    drv_write_word(MAC_CONTROL, read_tmp);
}

int aml_driv_attach( struct drv_private *drv_priv, struct wifi_mac *wmac)
{
    int i;
    int error;
    char *country_code = NULL;
    int country_index = DEFAULT_CONTRY;

    AML_OUTPUT("enter Here\n");

    drv_init_ops(drv_priv);

    drv_priv->wmac = wmac;
    drv_priv->net_ops = &wmac->wmac_ops;
    drv_priv->drv_not_init_flag = 1;
    drv_priv->drv_connetting = 0;
    drv_priv->drv_scanning = 0;
    drv_priv->is_mother_channel[0] = 1;
    drv_priv->is_mother_channel[1] = 1;
    drv_priv->drv_config.cfg_cachelsz = DEFAULT_CACHESIZE; 
    drv_priv->wait_mpdu_timeout = 1;
    drv_priv->add_wakeup_work = 0;
    drv_priv->stop_noa_flag = 0;

    drv_hal_attach(drv_priv, (void *)get_hal_call_back_table()); 

    drv_priv->hal_priv = hal_get_priv();

    drv_pwrsave_init(drv_priv);
    driv_ps_wakeup(drv_priv);

    country_code = aml_wifi_get_country_code();

    pr_debug("<running> %s %d insmod country: %s\n", __func__, __LINE__,
           country_code);

    country_index = find_country_code((unsigned char *)country_code);
    if (country_index != 0xff) {
        drv_priv->drv_config.cfg_countrycode = country_index;
    } else {
        drv_priv->drv_config.cfg_countrycode = 0;
    }

    drv_priv->drv_config.cfg_txpoweplan = wifimac_get_tx_pwr_plan(drv_priv->drv_config.cfg_countrycode);
    drv_priv->drv_config.cfg_ampduackpolicy = DEFAULT_AMPDUACKPOLICY;
    drv_priv->drv_config.cfg_htsupport = DEFAULT_HT_ENABLE;
    drv_priv->drv_config.cfg_vhtsupport = DEFAULT_VHT_ENABLE;
    drv_priv->drv_config.cfg_burst_ack = DEFAULT_BURST_ENABLE;
    drv_priv->drv_config.cfg_txpowlimit = DEFAULT_TXPOWER;
    drv_priv->drv_config.cfg_aggr_prot = DEFAULT_TXAGG_PROT;
    drv_priv->drv_config.cfg_disratecontrol = DEFAULT_RATECONTROL_DIS;
    drv_priv->drv_config.cfg_haswme = DEFAULT_WMMSUPPORT;
    
    drv_priv->drv_config.cfg_dynamic_bw = DEFAULT_SUPPORT_DYNAMIC_BW;
    drv_priv->drv_config.cfg_wifi_bt_coexist_support = DEFAULT_SUPPORT_WIFI_BT_COEXIST;

    if (drv_priv->drv_config.cfg_htsupport)
    {
        drv_priv->drv_config.cfg_ampdu_limit = DEFAULT_TXAMPDU_LEN_MAX;
        drv_priv->drv_config.cfg_txaggr = DEFAULT_TXAMPDU_EN;
        drv_priv->drv_config.cfg_rxaggr = DEFAULT_RXAMPDU_EN;
        drv_priv->drv_config.cfg_rx_ba_window = DEFAULT_RX_BA_WINDOW;
        drv_priv->drv_config.cfg_rx_reorder_timeout = DEFAULT_RX_REORDER_TIMEOUT;
        drv_priv->drv_config.cfg_ap_txaggr = DEFAULT_AP_TXAMPDU_EN;
        drv_priv->drv_config.cfg_ap_rxaggr = DEFAULT_AP_RXAMPDU_EN;
        drv_priv->drv_config.cfg_monitor_txaggr = DEFAULT_MONITOR_TXAMPDU_EN;
        drv_priv->drv_config.cfg_monitor_rxaggr = DEFAULT_MONITOR_RXAMPDU_EN;
        drv_priv->drv_config.cfg_ap_ampdu_wait_target = DEFAULT_AP_AMPDU_WAIT_TARGET;
        drv_priv->drv_config.cfg_ap_ampdu_ht20_limit = DEFAULT_AP_AMPDU_HT20_LIMIT;
        drv_priv->drv_config.cfg_ap_ampdu_wide_limit = DEFAULT_AP_AMPDU_WIDE_LIMIT;
        drv_priv->drv_config.cfg_ap_vht80_txaggr = DEFAULT_AP_VHT80_TXAGGR;
        drv_priv->drv_config.cfg_txamsdu = DEFAULT_TXAMSDU_EN;
        drv_priv->drv_config.cfg_ampdu_subframes = DEFAULT_TXAMPDU_SUB_MAX;
        drv_priv->drv_config.cfg_40Msupport = DEFAULT_11N40M_SUPPORT;
        drv_priv->drv_config.cfg_ampdu_oneframe = DEFAULT_TXAMPDU_ONEFRAME;
        drv_priv->drv_config.cfg_ampdu_livetime = DEFAULT_TXAMPDU_ONEFRAME;
    }

    drv_set_config((void *)drv_priv, CHIP_PARAM_RETRY_LIMIT, 100 << 8 | 100);

    drv_priv->drv_config.cfg_txchainmask = DEFAULT_TXCHAINMASK;
    drv_priv->drv_config.cfg_rxchainmask = DEFAULT_RXCHAINMASK;
    drv_priv->drv_config.cfg_txchainmasklegacy = DEFAULT_TXCHAINMASKLEGACY;
    drv_priv->drv_config.cfg_rxchainmasklegacy = DEFAULT_RXCHAINMASKLEGACY;
    drv_priv->drv_config.cfg_bw_ctrl = DEFAULT_BWC_ENABLE;
    drv_priv->drv_config.cfg_aessupport  = 1;
    drv_priv->drv_config.cfg_tkipsupport = 1;
    drv_priv->drv_config.cfg_wepsupport = 1;
    drv_priv->drv_config.cfg_wapisupport = 1;
    drv_priv->drv_config.cfg_tkipmicsupport = DEFAULT_HW_TKIP_MIC; 
    drv_priv->drv_config.cfg_rtcenable = DEFAULT_RTC_ENABLE;
    drv_priv->drv_config.cfg_retrynum = DEFAULT_TXRETRY_MAX;
    drv_priv->drv_config.cfg_uapsdsupported = DEFAULT_UAPSU_EN;
    drv_priv->drv_config.cfg_modesupport = DEFAULT_SELECT_MODE;
    drv_priv->drv_config.cfg_checksumoffload = DEFAULT_HW_CSUM;
    drv_priv->drv_config.cfg_eap_lowest_rate = DEFAULT_MCAST_EAPOL_NULLDATA_RATE_11G;
    drv_priv->drv_config.cfg_dssupport = DEFAULT_DSSUPPORT;
    drv_priv->drv_config.cfg_eat_count_max = DEFAULT_EAT_COUNT_MAX;
    drv_priv->drv_config.cfg_aggr_thresh = DEFAULT_AGGR_THRESH;
    drv_priv->drv_config.cfg_no_aggr_thresh = DEFAULT_NO_AGGR_THRESH;
    drv_priv->drv_config.cfg_hrtimer_interval = DEFAULT_HRTIMER_INTERVAL;
    drv_priv->drv_config.cfg_listen_interval = DEFAULT_LISTEN_INTERVAL;
    drv_priv->drv_config.cfg_mfp = DEFAULT_MFP_EN; 
    drv_priv->drv_config.cfg_mac_mode = DEFAULT_AUTO;
    drv_priv->drv_config.cfg_band = DEFAULT_BAND_ALL;

    for (i = 0; i < DEFAULT_MAX_VMAC; i++)
    {
        drv_priv->dot11VmacPsState[i] = DRV_PWRSAVE_FULL_SLEEP;
    }
    drv_priv->dot11ComPsState = DRV_PWRSAVE_AWAKE;
    drv_priv->drv_PhyPsState = DRV_PWRSAVE_AWAKE;
    
    drv_priv->drv_ratectrl_mrr = 1;
    error = drv_channel_init(drv_priv, drv_priv->drv_config.cfg_countrycode);
    if (error != 0)
    {
        driv_ps_sleep(drv_priv);
        ERROR_DEBUG_OUT("<running> error!!!\n");
        goto bad;
    }

    wifi_mac_set_tx_power_coefficient(drv_priv, NULL,
                                      drv_priv->drv_config.cfg_txpoweplan);

    drv_priv->net_ops->wifi_mac_rate_ratmod_attach(drv_priv);
    pr_debug("<running> %s %d  drv_priv->drv_ratectrl_size = %d\n",
           __func__,__LINE__,drv_priv->drv_ratectrl_size);

    drv_rate_setup(drv_priv, WIFINET_MODE_11GNAC);
    drv_txlist_setup(drv_priv);
    driv_ps_sleep(drv_priv);
    aml_set_mac_control_register();

    drv_cfg_load_from_file();
    drv_priv->drv_curtxpower = clamp_t(unsigned short, drv_priv->drv_config.cfg_txpowlimit, 1, 20);
    return 0;

bad:
    return -ENODEV;
}

void aml_driv_detach( struct drv_private *drv_priv)
{
    int i;

    drv_stop(drv_priv);
    if (drv_priv->net_ops) {
        drv_priv->net_ops->wifi_mac_rate_ratmod_detach(drv_priv);
    }

    for (i = 0; i < HAL_NUM_TX_QUEUES; i++)
        if (DRV_TXQUEUE_VALUE(drv_priv, i))
            drv_txlist_destroy(drv_priv, &drv_priv->drv_txlist_table[i]);

    AML_OUTPUT("<running>\n");
    drv_hal_detach();
}

static int drv_read_manu_cfg(void *drv_priv, void *cfg)
{
    pr_debug("maybe involve nand/nor flash operating function,to get manufactory cfg\n");
    return 0;
}

static int drv_get_default_cfg(void *drv_priv, void *cfg)
{
    return drv_read_manu_cfg(drv_priv, cfg);
}

static void
drv_mic_error_event(void *dpriv, const void *frm, unsigned char *sa,
                    unsigned char wnet_vif_id)
{
#if 1
    struct wifi_frame *wh = (struct wifi_frame *)frm;
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || drv_priv->net_ops == NULL ||
        wh == NULL || sa == NULL || wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL || wnet_vif->vm_wmac == NULL ||
        wnet_vif->vm_state < WIFINET_S_AUTH ||
        (wnet_vif->vm_phase_flags & PHASE_DISCONNECTING))
        return;

    if (wnet_vif->vm_opmode != WIFINET_M_STA &&
        wnet_vif->vm_opmode != WIFINET_M_HOSTAP)
        return;

    if (wnet_vif->vm_opmode == WIFINET_M_STA &&
        (wnet_vif->vm_mainsta == NULL ||
         wnet_vif->vm_mainsta->sta_wnet_vif != wnet_vif))
        return;
#endif
}

extern atomic_t g_hr_lock_timer_valid;
static void drv_tx_ok_timeout(void *drv_priv)
{
    struct drv_private *drv_private = (struct drv_private *)(drv_priv);

    if (drv_private != NULL) {
        DRV_TX_TIMEOUT_LOCK(drv_private);

        if (!atomic_read(&g_hr_lock_timer_valid)) {
            drv_private->wait_mpdu_timeout = 1;
        }

        AML_PRINT(AML_DBG_MODULES_TX, "waiting_pkt_timeout:%d\n", drv_private->wait_mpdu_timeout);
        DRV_TX_TIMEOUT_UNLOCK(drv_private);
    }
}

static void drv_tx_pkt_clear(void *drv_priv)
{
    struct drv_private *drv_private = (struct drv_private *)(drv_priv);

    if (drv_private != NULL) {
        wifi_mac_notify_pkt_clear(drv_private->wmac);
    }
}

static void
drv_intr_rx_ok(void *dpriv, struct sk_buff *skb, unsigned char Rssi, unsigned char vendor_rate_code,
               unsigned char bw, unsigned char channel, unsigned char aggr, unsigned char wnet_vif_id,
               unsigned char keyid)
{
    struct wifi_mac *wifimac;
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct sk_buff *skbbuf = (struct sk_buff *)skb;
    struct wifi_frame *wh;
    struct wifi_mac_rx_status rxstatus;
    int rate_index;

    if (drv_priv == NULL || skb == NULL || drv_priv->wmac == NULL ||
        drv_priv->drv_currratetable == NULL ||
        drv_priv->net_ops == NULL ||
        drv_priv->net_ops->wifi_mac_rx_complete == NULL) {
        if (skb != NULL)
            os_skb_free(skb);
        return;
    }

    wifimac = drv_priv->wmac;

    rate_index = drv_rate_findindex_from_ratecode(drv_priv->drv_currratetable, vendor_rate_code);
    if (rate_index < 0 || rate_index >= drv_priv->drv_currratetable->rateCount)
        rate_index = 0;

    rxstatus.rs_flags = aggr;
    rxstatus.rs_channel = channel;
    rxstatus.rs_rssi = Rssi;
    rxstatus.rs_datarate = drv_priv->drv_currratetable->info[rate_index].rateKbps;
    rxstatus.rs_vendor_rate = vendor_rate_code;
    rxstatus.rs_bw = bw;
    rxstatus.rs_tstamp.tsf = jiffies;
    rxstatus.rs_wnet_vif_id = wnet_vif_id;
    rxstatus.rs_keyid = keyid ;

    wh = (struct wifi_frame *)os_skb_data(skb);
    if (!list_empty(&wifimac->wm_wnet_vifs))
    {
        drv_priv->drv_stats.rx_indicate_cnt++;
        drv_priv->net_ops->wifi_mac_rx_complete(wifimac, skbbuf, &rxstatus);
    }
    else
    {
        drv_priv->drv_stats.rx_free_skb_cnt++;
        os_skb_free(skb);
    }
}

static int pmf_encrypt_pkt_handle(void *dpriv, struct sk_buff *skb, unsigned char rssi,
                                  unsigned char vendor_rate_code, unsigned char channel,
                                  unsigned char aggr, unsigned char wnet_vif_id,
                                  unsigned char keyid)
{
    struct wifi_mac *wifimac;
    struct wifi_frame *wh;
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct sk_buff *skbbuf = (struct sk_buff *)skb;
    struct wifi_mac_rx_status rxstatus;
    int rate_index;
    unsigned char pkt_type;
    unsigned char is_protect;

    wh = (struct wifi_frame *)os_skb_data(skb);
    pkt_type = wh->i_fc[0] & WIFINET_FC0_TYPE_MASK;
    is_protect = wh->i_fc[1] & WIFINET_FC1_WEP;
    if (!((pkt_type == WIFINET_FC0_TYPE_MGT) && is_protect)) {
        return 1;

    } else {
    }

    wifimac = drv_priv->wmac;
    rate_index = drv_rate_findindex_from_ratecode(drv_priv->drv_currratetable, vendor_rate_code);

    rxstatus.rs_flags = aggr;
    rxstatus.rs_channel = channel;
    rxstatus.rs_rssi = rssi;
    rxstatus.rs_datarate = drv_priv->drv_currratetable->info[rate_index].rateKbps;
    rxstatus.rs_tstamp.tsf = jiffies;
    rxstatus.rs_wnet_vif_id = wnet_vif_id;
    rxstatus.rs_keyid = keyid ;

    if (!list_empty(&wifimac->wm_wnet_vifs)) {
        drv_priv->drv_stats.rx_indicate_cnt++;
        drv_priv->net_ops->wifi_mac_rx_complete(wifimac, skbbuf, &rxstatus);
    } else {
        drv_priv->drv_stats.rx_free_skb_cnt++;
        os_skb_free(skb);
    }

    return 0;
}

static void drv_intr_bcn_send_ok(void * dpriv,unsigned char vma_id)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;
    struct wifi_mac *wifimac;
    struct sk_buff *skb;
    unsigned short flag;

    if (drv_priv == NULL || vma_id >= WIFI_MAX_VID)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[vma_id];
    if ((wnet_vif == NULL) || (wnet_vif->vm_state != WIFINET_S_CONNECTED))
        return;

    wifimac = wnet_vif->vm_wmac;
    if (wifimac == NULL || drv_priv->net_ops == NULL ||
        drv_priv->net_ops->wifi_mac_update_beacon == NULL)
        return;

    if ((wnet_vif->vm_opmode == WIFINET_M_HOSTAP)||
        (wnet_vif->vm_opmode == WIFINET_M_IBSS))
    {
        unsigned char rate = 0;

        drv_tx_get_mgmt_frm_rate(drv_priv, wnet_vif,
                                 WIFINET_FC0_TYPE_MGT | WIFINET_FC0_SUBTYPE_BEACON,
                                 &rate, &flag);

        WIFINET_BEACONBUF_LOCK(wifimac);

        skb = wnet_vif->vm_beaconbuf;
        if(skb)
        {
            drv_priv->net_ops->wifi_mac_update_beacon(drv_priv->wmac, vma_id, skb, 0);
            drv_hal_put_bcn_buf(vma_id, os_skb_data(skb), os_skb_get_pktlen(skb),
                                rate, wnet_vif->vm_bandwidth);
        }
        else
        {
            pr_err("Enter %s Function ERROR \n",__func__);
        }
        WIFINET_BEACONBUF_UNLOCK(wifimac);

    }
}

static void drv_intr_dtim_send_ok(void *drv_priv, unsigned char vma_id)
{

}

static void drv_intr_ba_recv_ok(void *drv_priv, unsigned char vma_id)
{
    pr_debug("Enter %s Function \n",__func__);
}

static int
drv_get_sta_id(void *dpriv, unsigned char *mac, unsigned char wnet_vif_id,
               int *staid)
{
    struct wifi_station *sta;
    struct wifi_station_tbl *nt;
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || mac == NULL || staid == NULL ||
        drv_priv->net_ops == NULL || drv_priv->net_ops->wifi_mac_get_sta == NULL ||
        wnet_vif_id >= DEFAULT_MAX_VMAC) {
        return -1;
    }

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL) {
        return -1;
    }
    nt = &wnet_vif->vm_sta_tbl;

    sta = drv_priv->net_ops->wifi_mac_get_sta(nt, mac, wnet_vif_id);
    if (sta == NULL || sta->sta_wnet_vif == NULL) {
        return -1;
    }

    if (sta->sta_wnet_vif->vm_opmode == WIFINET_M_STA) {
        *staid = 1;
    } else {
        *staid = sta->sta_associd & 0xff;
    }

    return 0;
}

static void drv_goto_deep_sleep(void * dpriv)
{

}

static void drv_goto_wakeup(void *dpriv, unsigned char wnet_vif_id)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || drv_priv->net_ops == NULL ||
        drv_priv->net_ops->wifi_mac_pwrsave_is_sta_fullsleep == NULL ||
        drv_priv->net_ops->wifi_mac_pwrsave_networksleep == NULL ||
        wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];

    if (wnet_vif)
    {
        struct wifi_mac_pwrsave_t *ps = &wnet_vif->vm_pwrsave;

        if (drv_priv->net_ops->wifi_mac_pwrsave_is_sta_fullsleep(wnet_vif) == 0)
        {
            WIFINET_PWRSAVE_LOCK(wnet_vif);
            
            ps->ips_sleep_wait_reason = SLEEP_AFTER_WAIT_BEACON_TIMEOUT;
            os_timer_ex_start_period(&ps->ips_timer_sleep_wait, ps->ips_ps_waitbeacon_timeout);
            ps->ips_flag_waitbeacon_timer_start = 1;
            WIFINET_PWRSAVE_UNLOCK(wnet_vif);

            drv_priv->net_ops->wifi_mac_pwrsave_networksleep(wnet_vif, NETSLEEP_AFTER_WAKEUP);
        }
    }
}

static void
drv_p2p_opps_cwend_sleep(struct wlan_net_vif *wnet_vif, struct drv_private *drv_priv)
{
    wnet_vif->vm_pstxqueue_flags |= WIFINET_PSQUEUE_OPPS;
    drv_priv->net_ops->wifi_mac_pwrsave_fullsleep(wnet_vif, SLEEP_AFTER_OPPS_END);
}

int
drv_p2p_go_opps_cwend_may_sleep(struct wlan_net_vif *wnet_vif)
{
    struct wifi_mac *wifimac;
    struct drv_private *drv_priv;
    int ret = -1;

    if (wnet_vif == NULL || wnet_vif->vm_wmac == NULL)
        return ret;
    wifimac = wnet_vif->vm_wmac;
    drv_priv = wifimac->drv_priv;
    if (drv_priv == NULL || drv_priv->net_ops == NULL)
        return ret;

    DPRINTF(AML_DEBUG_PWR_SAVE,"%s(%d) opmode %d vm_ps_sta %d "
            "vm_sta_assoc %d lgcy/mcast/uapsd %d/%d/%d\n",
            __func__,__LINE__,wnet_vif->vm_opmode,
            wnet_vif->vm_ps_sta,wnet_vif->vm_sta_assoc,
            !wnet_vif->vm_legacyps_txframes,
            !drv_txlist_all_qcnt(drv_priv, HAL_WME_MCAST),
            !drv_txlist_all_qcnt(drv_priv, HAL_WME_UAPSD));
    
    if ((wifi_mac_pwrsave_if_ap_can_opps (wnet_vif) == 0) &&
        !drv_txlist_all_qcnt(drv_priv, HAL_WME_MCAST) &&
        !wnet_vif->vm_legacyps_txframes &&
        !drv_txlist_all_qcnt(drv_priv, HAL_WME_UAPSD))
    {
        drv_p2p_opps_cwend_sleep(wnet_vif, drv_priv);
        ret = 0;
    }
    return ret;
}

int drv_p2p_client_opps_cwend_may_sleep (struct wlan_net_vif *wnet_vif)
{
    struct wifi_mac *wifimac;
    struct drv_private *drv_priv;
    int ret = -1;

    if (wnet_vif == NULL || wnet_vif->vm_wmac == NULL)
        return ret;
    wifimac = wnet_vif->vm_wmac;
    drv_priv = wifimac->drv_priv;
    if (drv_priv == NULL)
        return ret;

    if ((wnet_vif->vm_opmode == WIFINET_M_STA) &&
        !wnet_vif->vm_pwrsave.ips_flag_send_ps_trigger)
    {
        drv_p2p_opps_cwend_sleep(wnet_vif, drv_priv);
        ret = 0;
    }
    return ret;
}

static void drv_intr_p2p_opps_cwend(void *dpriv,unsigned char wnet_vif_id)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif)
    {
    }
}

static void
drv_intr_p2p_noa_start (void *dpriv,unsigned char wnet_vif_id)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif)
    {
    }
}

static void drv_intr_fw_event(void *dpriv, void *event)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;
    struct fw_event_no_data *fw_event = (struct fw_event_no_data *)event;
    struct wifi_mac *wifimac;

    if (drv_priv == NULL || fw_event == NULL || fw_event->basic_info.vid >= DEFAULT_MAX_VMAC ||
        drv_priv->net_ops == NULL)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[fw_event->basic_info.vid];
    if (wnet_vif == NULL) {
        return;
    }
    wifimac = wnet_vif->vm_wmac;
    if (wifimac == NULL)
        return;

    switch (fw_event->basic_info.event) {
        case DHCP_OFFLOAD_EVENT:
            drv_priv->net_ops->wifi_mac_device_ip_config(wnet_vif, event);
            break;

        case TBTT_EVENT:
            drv_priv->net_ops->wifi_mac_tbtt_handle(wnet_vif);
            break;

        case CHANNEL_SWITCH_EVENT:
            drv_priv->net_ops->wifi_mac_channel_switch_complete(wnet_vif);
            break;

        case DPD_CALIBRATION_EVENT:
            hal_dpd_calibration();
            break;

        case TX_ERROR_EVENT:
        {
            struct tx_error_event *error_event = (struct tx_error_event *)fw_event;

            pr_debug("%s:%d, frame type %x, error type %x \n", __func__, __LINE__,
                   error_event->frame_type, error_event->error_type);

            drv_priv->net_ops->wifi_mac_process_tx_error(wnet_vif);
            break;
        }

        case COEX_EVENT:
        {
            struct coex_info_event *coex_event = (struct coex_info_event *)fw_event;
            AML_OUTPUT("coex state = %d\n", coex_event->coexist_state);

            if (coex_event->coexist_state == TRUE_RSSI) {
                wifimac->bt_lk = 1;
                wifi_mac_set_channel_rssi(wifimac, (unsigned char)(wnet_vif->vm_mainsta->sta_avg_bcn_rssi));
            } else if (coex_event->coexist_state == OPTIMIZE_RSSI) {
                wifimac->bt_lk = 0;
                wifi_mac_set_channel_rssi(wifimac, (unsigned char)(wnet_vif->vm_mainsta->sta_avg_bcn_rssi));
            }
            break;
        }

        case FWLOG_PRINT_EVENT:
            drv_priv->hal_priv->hal_ops.hal_set_fwlog_cmd(3); 
            break;

        default:
            break;
    }
}

static void drv_intr_tx_null_data(void *dpriv, struct tx_nulldata_status *tx_null_status)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || tx_null_status == NULL ||
        tx_null_status->wnet_vif_id >= DEFAULT_MAX_VMAC || drv_priv->net_ops == NULL)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[tx_null_status->wnet_vif_id];
    if (wnet_vif == NULL || wnet_vif->vm_wmac == NULL)
        return;

    if (tx_null_status->pwr_flag == 0)
        return;

    if ((tx_null_status->qos == 0) && (tx_null_status->ps == 1)) {
        if (tx_null_status->txstatus== TX_DESCRIPTOR_STATUS_NULL_DATA_OK) {
            if (!(wnet_vif->vm_wmac->wm_flags & WIFINET_F_SCAN)) {
                drv_priv->net_ops->wifi_mac_pwrsave_fullsleep(wnet_vif,
                                                              SLEEP_AFTER_TX_NULL_WITH_PS);
            }
            drv_priv->net_ops->wifi_mac_notify_ap_success(wnet_vif);
        } else if (tx_null_status->txstatus == TX_DESCRIPTOR_STATUS_NULL_DATA_FAIL) {
            if (!(wnet_vif->vm_wmac->wm_flags & WIFINET_F_SCAN)) {
                drv_priv->net_ops->wifi_mac_pwrsave_wkup_and_NtfyAp(wnet_vif,
                                                                    WKUP_FROM_PSNULL_FAIL);
            }
            drv_priv->net_ops->wifi_mac_notify_ap_success(wnet_vif);
        }
    }
}

static void drv_intr_beacon_miss(void *dpriv, unsigned char wnet_vif_id)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;

    if (drv_priv == NULL || drv_priv->net_ops == NULL ||
        drv_priv->net_ops->wifi_mac_beacon_miss == NULL ||
        wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL)
        return;

    drv_priv->net_ops->wifi_mac_beacon_miss(wnet_vif);
}

static void drv_trigger_send_delba(SYS_TYPE param1, SYS_TYPE param2,
                                   SYS_TYPE param3, SYS_TYPE param4,
                                   SYS_TYPE param5)
{
    struct drv_private *drv_priv = drv_get_drv_priv();
    unsigned int vid = 0;
    struct wlan_net_vif *wnet_vif;
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int reg_val;
    struct wifi_mac* wifi_mac = wifi_mac_get_mac_handle();
    unsigned int j;

    if (drv_priv == NULL || hif == NULL || wifi_mac == NULL)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[vid]; 
    if (wnet_vif == NULL)
        return;

    reg_val = hif->hif_ops.hi_read_word(RG_COEX_BT_LOGIC_INFO);
    if (reg_val & COEX_EN_ESCO) {
        drv_priv->drv_config.cfg_ampdu_subframes = DEFAULT_TXAMPDU_SUB_MAX_COEX_ESCO;
        wifi_mac->wm_esco_en =1;

    } else {
        drv_priv->drv_config.cfg_ampdu_subframes = DEFAULT_TXAMPDU_SUB_MAX_COEX;
        wifi_mac->wm_esco_en = 0;
    }

    for (j = 0; j < 2; ++j) {
        struct wifi_station_tbl *vm_sta_tbl;
        unsigned int i;

        if (drv_priv->drv_wnet_vif_table[j] == NULL)
            continue;
        vm_sta_tbl = &drv_priv->drv_wnet_vif_table[j]->vm_sta_tbl;

        for (i = 0; i < WIFINET_NODE_HASHSIZE; i++) {
            struct wifi_station* sta = NULL;
            struct wifi_station* sta_next = NULL;

            WIFINET_NODE_LOCK(vm_sta_tbl);
            VLSI_FOR_EACH_ENTRY_SAFE(sta, sta_next, &vm_sta_tbl->nt_nsta, sta_list) {
                struct aml_driver_nsta *drv_sta;
                unsigned int tid_index;

                drv_sta = DRIVER_NODE(sta->drv_sta);

                for (tid_index = 0; tid_index < WME_NUM_TID; tid_index++) {
                    struct drv_rx_scoreboard *RxTidState;

                    RxTidState = &drv_sta->rx_scb[tid_index];

                    if (RxTidState->rx_addba_exchangecomplete) {
                        struct wifi_mac_action_mgt_args actionargs;

                        memset(&actionargs, 0, sizeof(struct wifi_mac_action_mgt_args));

                        actionargs.category = AML_CATEGORY_BACK;
                        actionargs.action = WIFINET_ACTION_BA_DELBA;
                        actionargs.arg1 = tid_index;
                        
                        actionargs.arg2 = BA_INITIATOR;

                        actionargs.arg3 = 1;
                        wifi_mac_send_action(sta, (void *)&actionargs);
                        pr_debug("%s:%d, tid_index %d ->sta_macaddr=%s\n",
                               __func__, __LINE__, tid_index, ether_sprintf(sta->sta_macaddr));
                    }
                }
            }
            WIFINET_NODE_UNLOCK(vm_sta_tbl);
        }
    }
}

static void drv_intr_bt_info_change(void *dpriv, unsigned char wnet_vif_id, unsigned char bt_lk_change)
{
    struct drv_private *drv_priv = (struct drv_private *)dpriv;
    struct wlan_net_vif *wnet_vif;
    struct hw_interface* hif = hif_get_hw_interface();

    if (drv_priv == NULL || wnet_vif_id >= DEFAULT_MAX_VMAC)
        return;

    wnet_vif = drv_priv->drv_wnet_vif_table[wnet_vif_id];
    if (wnet_vif == NULL)
        return;

    pr_debug("%s:%d, vid %d \n", __func__, __LINE__, wnet_vif_id);

    if (drv_priv->drv_config.cfg_wifi_bt_coexist_support == 0) {
        return;
    }

    if ((wnet_vif->vm_opmode != WIFINET_M_STA) || (drv_priv->drv_config.cfg_txaggr == 0)) {
        return;
    }

    if (bt_lk_change == 1) { 
        drv_trigger_send_delba(0, 0, 0, 0, 0);
    } else {
         struct wifi_mac *p_wifi_mac = wifi_mac_get_mac_handle();
         unsigned int reg_val;
         unsigned int reg_val2;

         reg_val = hif->hif_ops.hi_read_word(RG_BT_PMU_A16);
         reg_val2 = hif->hif_ops.hi_read_word(RG_PMU_A16);
         pr_debug("drv_intr_bt_info_change reg addr=0x%x,value=0x%x reg addr=0x%x,value=0x%x ",
                RG_BT_PMU_A16, reg_val, RG_PMU_A16, reg_val2);

         if ((reg_val & BIT(31)) && (reg_val2 & BIT(31))) { 
            drv_priv->drv_config.cfg_ampdu_subframes = DEFAULT_TXAMPDU_SUB_MAX_COEX_ESCO;
            p_wifi_mac->wm_bt_en = 1;
            pr_debug("coex change to work\n");
         } else { 
            drv_priv->drv_config.cfg_ampdu_subframes = DEFAULT_TXAMPDU_SUB_MAX;
            p_wifi_mac->wm_bt_en = 0;
            pr_debug("coex change to not work\n");
         }
    }
}

static int
drv_dev_probe(void)
{
    struct vm_wlan_net_vif_params vm_param = { 0 };
    struct hal_private *hal_priv = hal_get_priv();
    struct drv_private *drv_priv = drv_get_drv_priv();
    struct wifi_mac *wm_mac = wifi_mac_get_mac_handle();
    struct net_device *ndev;
    int ret;
    int vif0opmode;
    int vif1opmode;
    char *vmac0;
    char *vmac1;
    int single_vif;
    
    int inet_notifier_registered = 0;
    int inet6_notifier_registered = 0;
    struct net_device *primary_ndev_registered = NULL;

    if ((hal_priv->hal_ops.hal_probe != NULL) && (!hal_priv->hal_ops.hal_probe())) {
        w1_wifi_hal_probe_done = 0;
        ERROR_DEBUG_OUT("init hal error\n");
        goto err_ret;
    }
    w1_wifi_hal_probe_done = 1;

    memset(drv_priv, 0, sizeof(struct drv_private));
    memset(wm_mac, 0, sizeof(struct wifi_mac));

    wifi_mac_init_ops(wm_mac);

    aml_wifi_set_mac_addr();
    pr_debug("%s(%d) mac_addr set done."MAC_FMT"", __func__,__LINE__,
           mac_addr0, mac_addr1, mac_addr2, mac_addr3, mac_addr4, mac_addr5);

    if(aml_driv_attach(drv_priv, wm_mac)) {
        ERROR_DEBUG_OUT("init driver error\n");
        goto err_ret;
    }

    if (wifi_mac_entry(wm_mac, drv_priv) != 0) {
        ERROR_DEBUG_OUT( "init wifimac error\n");
        goto err_ret;
    }

    single_vif = aml_wifi_get_single_vif();
    vmac0 = aml_wifi_get_vif0_name();
    strscpy(vm_param.vm_param_name, vmac0, sizeof(vm_param.vm_param_name));

    vif0opmode = aml_wifi_get_vif0_opmode();
    if ((vif0opmode >= WIFINET_M_IBSS) && (vif0opmode <= WIFINET_M_P2P_DEV)) {
        vm_param.vm_param_opmode = vif0opmode;
    } else {
        vm_param.vm_param_opmode = WIFINET_M_STA;
    }
    ret = drv_priv->net_ops->wifi_mac_create_vmac(wm_mac, &vm_param, 0);
    if (ret) {
        ERROR_DEBUG_OUT("create %s vmac failed\n", vm_param.vm_param_name);
        goto err_ret;
    }

    if (!single_vif) {
        vmac1 = aml_wifi_get_vif1_name();
        strscpy(vm_param.vm_param_name, vmac1, sizeof(vm_param.vm_param_name));

        vif1opmode = aml_wifi_get_vif1_opmode();
        if ((vif1opmode >= WIFINET_M_IBSS) && (vif1opmode <= WIFINET_M_P2P_DEV)) {
            vm_param.vm_param_opmode = vif1opmode;
        } else {
            vm_param.vm_param_opmode = WIFINET_M_P2P_DEV;
        }
        ret = drv_priv->net_ops->wifi_mac_create_vmac(wm_mac, &vm_param, 0);
        if (ret) {
            ERROR_DEBUG_OUT("create %s vmac failed\n", vm_param.vm_param_name);
            goto err_ret;
        }
    }

    drv_priv->net_ops->wifi_mac_sta_attach(wm_mac);
    
    {
        int v4 = register_inetaddr_notifier(&aml_inetaddr_cb);
        int v6 = register_inet6addr_notifier(&aml_inet6addr_cb);
        if (v4 != 0) {
            ERROR_DEBUG_OUT("register_inetaddr_notifier failed: %d\n", v4);
            inet_notifier_registered = 0;
        } else {
            inet_notifier_registered = 1;
        }
        if (v6 != 0) {
            ERROR_DEBUG_OUT("register_inet6addr_notifier failed: %d\n", v6);
            inet6_notifier_registered = 0;
        } else {
            inet6_notifier_registered = 1;
        }
        ret = (v4 != 0) ? v4 : v6;
    }
    
    drv_priv->drv_ops.drv_set_bmfm_info(drv_priv, 0, 0, 0, 0);
    drv_hal_enable_coexist(drv_priv->drv_config.cfg_wifi_bt_coexist_support);

    rtnl_lock();
    ndev = drv_priv->drv_wnet_vif_table[0]->vm_ndev;
    if (register_netdevice(ndev))
    {
        ERROR_DEBUG_OUT("ERROR::%s: unable to register device\n", ndev->name);
        rtnl_unlock();
        goto err_ret;
    }
    
    primary_ndev_registered = ndev;

    if (!single_vif && drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC] != NULL) {
        ndev = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC]->vm_ndev;
        if (register_netdevice(ndev))
        {
            ERROR_DEBUG_OUT("ERROR::%s: unable to register device\n", ndev->name);
            rtnl_unlock();
            goto err_ret;
        }
    }
    rtnl_unlock();

    return ret;

err_ret:
    
    if (primary_ndev_registered) {
        rtnl_lock();
        unregister_netdevice(primary_ndev_registered);
        rtnl_unlock();
        primary_ndev_registered = NULL;
    }
    if (inet6_notifier_registered) {
        unregister_inet6addr_notifier(&aml_inet6addr_cb);
        inet6_notifier_registered = 0;
    }
    if (inet_notifier_registered) {
        unregister_inetaddr_notifier(&aml_inetaddr_cb);
        inet_notifier_registered = 0;
    }
    aml_driv_detach(drv_priv);
    return -ENODEV;
}

int drv_dev_remove(void)
{
    struct hal_private *hal_priv = hal_get_priv();
    struct drv_private *drv_priv = drv_get_drv_priv();
    struct wifi_mac *wm_mac = wifi_mac_get_mac_handle();

    hal_priv->hi_task_stop = 1;
    drv_priv->net_ops->wifi_mac_mac_exit(wm_mac);

    unregister_inetaddr_notifier(&aml_inetaddr_cb);
    unregister_inet6addr_notifier(&aml_inet6addr_cb);
    aml_driv_detach(drv_priv);

    AML_OUTPUT("<running>\n");
    return 0;
}

struct aml_hal_call_backs hal_call_back_table =
{
    
    .get_defaultcfg = drv_get_default_cfg,
    .mic_error_event = drv_mic_error_event,
    .intr_tx_handle = drv_tx_irq_tasklet,
    .intr_tx_ok_timeout = drv_tx_ok_timeout,
    .intr_tx_pkt_clear = drv_tx_pkt_clear,
    .intr_rx_handle = drv_intr_rx_ok,
    .pmf_encrypt_pkt_handle = pmf_encrypt_pkt_handle,
    .intr_bcn_send = drv_intr_bcn_send_ok,
    .intr_dtim_send = drv_intr_dtim_send_ok,
    .intr_ba_recv = drv_intr_ba_recv_ok,
    .get_stationid = drv_get_sta_id,
    .intr_gotodeepsleep = drv_goto_deep_sleep,
    .intr_gotowakeup = drv_goto_wakeup,
    .dev_probe = drv_dev_probe,
    .dev_remove = drv_dev_remove,
    .intr_p2p_opps_cwend = drv_intr_p2p_opps_cwend,
    .intr_p2p_noa_start = drv_intr_p2p_noa_start,
    .intr_fw_event = drv_intr_fw_event,
    .intr_tx_nulldata_handle = drv_intr_tx_null_data,
    .intr_beacon_miss_handle = drv_intr_beacon_miss,
    .drv_intr_bt_info_change  = drv_intr_bt_info_change,
    .drv_pwrsave_wake_req = drv_pwrsave_wake_req,
};

struct aml_hal_call_backs* get_hal_call_back_table(void)
{
    return (struct aml_hal_call_backs *)&hal_call_back_table;
}
