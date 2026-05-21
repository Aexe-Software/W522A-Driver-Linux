/*
 ****************************************************************************************
 *
 * Copyright (C) Amlogic 2010-2014
 *
 * Project: 11N 80211 driver  layer Software
 *
 * Description:
 *  driver layer configuration function
 *
 *
 ****************************************************************************************
 */
#include "wifi_drv_config.h"
#include "wifi_drv_main.h"
#include "wifi_drv_if.h"
#include "wifi_debug.h"


int drv_set_config(void * dev, enum cip_param_id id, int data)
{
    struct drv_private *drv_priv = ( struct drv_private *)dev;
    struct hal_private* hal_priv = hal_get_priv();
    
    pr_debug("%s(%d) id 0x%x data 0x%x\n", __func__, __LINE__, id, data);

    switch (id) {
        case CHIP_PARAM_TXCHAINMASK:
            drv_priv->drv_config.cfg_txchainmask = data;
            break;

        case CHIP_PARAM_RXCHAINMASK:
            drv_priv->drv_config.cfg_rxchainmask = data;
            break;

        case CHIP_PARAM_TXCHAINMASKLEGACY:
            drv_priv->drv_config.cfg_txchainmasklegacy = data;
            break;

        case CHIP_PARAM_RXCHAINMASKLEGACY:
            drv_priv->drv_config.cfg_rxchainmasklegacy = data;
            break;

        case CHIP_PARAM_CHAINMASK_SEL:
            drv_priv->drv_config.cfg_chainmask_sel = data;
            break;

        case CHIP_PARAM_AMPDU:
            /*
             * v16u CFG-RXAGGR-DECOUPLE:
             *
             * Pre-v16u this case mirrored `data` into both cfg_txaggr AND
             * cfg_rxaggr. That coupling is the root cause of the v16t
             * regression where drv_rx_addbareq() started refusing every
             * inbound ADDBA-Req with
             *   "v15l: drv_rx_addbareq REFUSED tid=0 (cfg_rxaggr=0)"
             *
             * The reason: there are several runtime paths that legitimately
             * want to disable TX A-MPDU but NOT RX A-MPDU:
             *   - wifi_cfg80211.c::vm_cfg80211_set_bitrate_mask() — when
             *     the user fixes a single legacy rate it does
             *       wifi_mac_config(wifimac, CHIP_PARAM_AMPDU, 0)
             *     so TX no longer aggregates, but the AP still wants to
             *     send us A-MPDU on the downlink.
             *   - vm_cfg80211_vendor_cmd_set() VM_NL80211_VENDER_SUBCMD_AMPDU
             *     with usr_data=0 — same intent (and same collateral kill).
             *   - scan/roam transitions that toggle CHIP_PARAM_AMPDU.
             *
             * Symptom: download throughput on 2.4 GHz collapses from
             * ~80 Mbps to a few kbit/s because every MPDU now needs an
             * individual ACK (no Block-Ack window).
             *
             * Fix: only touch cfg_txaggr here. cfg_rxaggr is now ONLY
             * mutated by CHIP_PARAM_AMPDU_RX (explicit) and by the boot
             * config-file parser (aml_wifi_drv_cfg_0.conf -> cfg_rxaggr=1).
             */
            pr_debug("<running> %s %d cfg_txaggr<-%d (cfg_rxaggr preserved=%d)\n",
                     __func__, __LINE__, (int)data,
                     drv_priv->drv_config.cfg_rxaggr);
            drv_priv->drv_config.cfg_txaggr = data;
            break;

        case CHIP_PARAM_AMPDU_RX:
            /* v16u: surface explicit RX A-MPDU writes so dmesg shows
             * who/when toggles RX aggregation, and warn loudly on a
             * downgrade to 0 because RX-aggregation OFF is the single
             * biggest cause of download throughput cliffs. */
            if (drv_priv->drv_config.cfg_rxaggr && !data) {
                pr_warn("v16u: CHIP_PARAM_AMPDU_RX -> 0 — RX A-MPDU disabled, "
                        "throughput on the downlink will collapse to single-MPDU "
                        "ACKs. Check the caller stack.\n");
                dump_stack();
            } else {
                pr_debug("<running> %s %d cfg_rxaggr<-%d\n",
                         __func__, __LINE__, (int)data);
            }
            drv_priv->drv_config.cfg_rxaggr = data;
            break;

        case CHIP_PARAM_AMPDU_LIMIT:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            drv_priv->drv_config.cfg_ampdu_limit = data;
            break;

        case CHIP_PARAM_AMPDU_SUBFRAMES:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            drv_priv->drv_config.cfg_ampdu_subframes = data;
            break;

        case CHIP_PARAM_AGGR_PROT:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            drv_priv->drv_config.cfg_aggr_prot = data;
            break;

        case CHIP_PARAM_TXPOWER_LIMIT:
            drv_priv->drv_config.cfg_txpowlimit = data;
            break;

        case CHIP_PARAM_BURST_ACK:
            pr_debug("<running> %s %d CHIP_PARAM_BURST_ACK\n",__func__,__LINE__);
            drv_priv->drv_config.cfg_burst_ack = data;
            break;

        case CHIP_PARAM_ACK_POLICY:
            pr_debug("<running> %s %d CHIP_PARAM_ACK_POLICY\n",__func__,__LINE__);
            drv_priv->drv_config.cfg_ampduackpolicy= data;
            break;

        case CHIP_PARAM_40MSUPPORT:
            drv_priv->drv_config.cfg_40Msupport= data;
            break;

        case CHIP_PARAM_AMSDU_ENABLE:
            drv_priv->drv_config.cfg_txamsdu = data;
            break;

        case CHIP_PARAM_USE_EAP_LOWEST_RATE:
            drv_priv->drv_config.cfg_eap_lowest_rate = data;
            break;

        case CHIP_PARAM_RETRY_LIMIT:
            drv_priv->drv_config.cfg_retrynum = data;
            drv_hal_setretrynum( data);
            break;

        case CHIP_PARAM_HTSUPPORT:
            drv_priv->drv_config.cfg_htsupport= data;
            break;
			
	  case CHIP_PARAM_VHTSUPPORT:
              drv_priv->drv_config.cfg_vhtsupport= data;
	  	break;

        case CHIP_PARAM_DISABLE_RATECONTROL:
            drv_priv->drv_config.cfg_disratecontrol= data;
            break;

        case CHIP_PARAM_AMPDU_ONE_FRAME:
            drv_priv->drv_config.cfg_ampdu_oneframe= data;
            break;

        case CHIP_PARAM_TXLIVETIME:
            drv_priv->drv_config.cfg_ampdu_livetime= data;
            drv_hal_txlivetime(data);
            break;

        case  CHIP_PARAM_HWCSUM:
            drv_priv->drv_config.cfg_checksumoffload= data;
            break;

        case  CHIP_PARAM_HWTKIPMIC:
            drv_priv->drv_config.cfg_tkipmicsupport= data;
            break;

        case CHIP_PARAM_DEBUG:
            aml_debug= data;
            break;

        case CHIP_PARAM_DBG_RXERR_RESET:
            hal_priv->hal_ops.Hal_RxerrRst_trigger(data);
            break;

        case CHIP_PARAM_RTC_ENABLE:
            drv_priv->drv_config.cfg_rtcenable  = data;
            break;

        case CHIP_PARAM_SHORTPREAMBLE:
            pr_debug("%s(%d),Before set short preamble.\n\n",__func__,__LINE__);
            drv_priv->drv_config.cfg_shortpreamble = data;
            break;

         case CHIP_PARAM_DYNAMIC_BW:
            drv_priv->drv_config.cfg_dynamic_bw = data;
            break;

         case CHIP_PARAM_EAT_COUNT:
            drv_priv->drv_config.cfg_eat_count_max = data;
            break;

         case CHIP_PARAM_AGGR_THRESH:
            drv_priv->drv_config.cfg_aggr_thresh = data;
            break;

         case CHIP_PARAM_HRTIMER_INTERVAL:
            drv_priv->drv_config.cfg_hrtimer_interval = data;
            break;

        default:
            return (-1);
    }

    return (0);
}

int drv_get_config(void *dev, enum cip_param_id id)
{
    struct drv_private *drv_priv = (struct drv_private *)dev;
    int supported = 0;

    switch (id) {
        case CHIP_PARAM_TXCHAINMASK:
            supported = drv_priv->drv_config.cfg_txchainmask;
            break;

        case CHIP_PARAM_RXCHAINMASK:
            supported = drv_priv->drv_config.cfg_rxchainmask;
            break;

        case CHIP_PARAM_TXCHAINMASKLEGACY:
            supported = drv_priv->drv_config.cfg_txchainmasklegacy;
            break;

        case CHIP_PARAM_RXCHAINMASKLEGACY:
            supported = drv_priv->drv_config.cfg_rxchainmasklegacy;
            break;

        case CHIP_PARAM_CHAINMASK_SEL:
            supported = drv_priv->drv_config.cfg_chainmask_sel;
            break;

        case CHIP_PARAM_AMPDU:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            supported = drv_priv->drv_config.cfg_txaggr;
            break;

        case CHIP_PARAM_AMPDU_RX:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            supported = drv_priv->drv_config.cfg_rxaggr;
            break;

        case CHIP_PARAM_AMPDU_LIMIT:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            supported = drv_priv->drv_config.cfg_ampdu_limit;
            break;

        case CHIP_PARAM_AMPDU_SUBFRAMES:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            supported = drv_priv->drv_config.cfg_ampdu_subframes;
            break;

        case CHIP_PARAM_AGGR_PROT:
            pr_debug("<running> %s %d \n",__func__,__LINE__);
            supported = drv_priv->drv_config.cfg_aggr_prot;
            break;

        case CHIP_PARAM_TXPOWER_LIMIT:
            supported = drv_priv->drv_config.cfg_txpowlimit;
            break;

        case CHIP_PARAM_BURST_ACK:
            supported = drv_priv->drv_config.cfg_burst_ack;
            break;

        case CHIP_PARAM_ACK_POLICY:
            supported = drv_priv->drv_config.cfg_ampduackpolicy;
            break;

        case CHIP_PARAM_40MSUPPORT:
            supported = drv_priv->drv_config.cfg_40Msupport;
            break;

        case CHIP_PARAM_USE_EAP_LOWEST_RATE:
            supported = drv_priv->drv_config.cfg_eap_lowest_rate;
            break;

        case CHIP_PARAM_HWCSUM:
            supported = drv_priv->drv_config.cfg_checksumoffload;
            break;

        case CHIP_PARAM_HWTKIPMIC:
            supported = drv_priv->drv_config.cfg_tkipmicsupport;
            break;

        case CHIP_PARAM_WMM:
            supported = drv_priv->drv_config.cfg_haswme;
            break;

        case CHIP_PARAM_HT:
            supported = drv_priv->drv_config.cfg_htsupport;
            break;

        case CHIP_PARAM_VHT:
            supported = drv_priv->drv_config.cfg_vhtsupport;
            break;

        case CHIP_PARAM_UAPSD:
            supported = drv_priv->drv_config.cfg_uapsdsupported;
            break;

        case CHIP_PARAM_DS:
            supported =  drv_priv->drv_config.cfg_dssupport;
            break;

        case CHIP_PARAM_CIPHER_WEP:
            supported =  drv_priv->drv_config.cfg_wepsupport;
            break;

        case CHIP_PARAM_CIPHER_AES_CCM:
            supported =  drv_priv->drv_config.cfg_aessupport;
            break;

        case CHIP_PARAM_CIPHER_WPI:
            supported =  drv_priv->drv_config.cfg_wapisupport;
            break;

        case CHIP_PARAM_CIPHER_TKIP:
            supported =  drv_priv->drv_config.cfg_tkipsupport;
            break;

        case CHIP_PARAM_DISRATECONTROL:
            supported =  drv_priv->drv_config.cfg_disratecontrol;
            break;

        case CHIP_PARAM_BWAUTOCONTROL:
            supported = drv_priv->drv_config.cfg_bw_ctrl;
            break;

        case CHIP_PARAM_DEBUG:
            supported = aml_debug;
            break;

        case CHIP_PARAM_EAT_COUNT:
            supported = drv_priv->drv_config.cfg_eat_count_max;
            break;

        case CHIP_PARAM_AGGR_THRESH:
            supported = drv_priv->drv_config.cfg_aggr_thresh;
            break;

        case CHIP_PARAM_HRTIMER_INTERVAL:
            supported = drv_priv->drv_config.cfg_hrtimer_interval;
            break;

        default:
            supported = 0;
            break;
    }

    return supported;
}

static unsigned int process_drv_cfg_content(char *varbuf, unsigned int len)
{
    char *dp;
    bool findNewline;
    int column;
    unsigned int buf_len, n;
    unsigned int pad = 0;

    dp = varbuf;
    findNewline = false;
    column = 0;

    for (n = 0; n < len; n++) {
        if (varbuf[n] == '\r')
            continue;

        if (findNewline && varbuf[n] != '\n')
            continue;
        findNewline = false;
        if (varbuf[n] == '#') {
            findNewline = true;
            continue;
        }
        if (varbuf[n] == '\n') {
            if (column == 0)
                continue;
            *dp++ = 0;
            column = 0;
            continue;
        }
        *dp++ = varbuf[n];
        column++;
    }
    buf_len = (unsigned int)(dp - varbuf);
    if (buf_len % 4) {
        pad = 4 - buf_len % 4;
        if (pad && (buf_len + pad <= len)) {
            buf_len += pad;
        }
    }

    while (dp < varbuf + n)
        *dp++ = 0;

    return buf_len;
}

extern unsigned char get_s16_item(char *varbuf, int len, char *item, short *item_value);
extern unsigned char get_s8_item(char *varbuf, int len, char *item, char *item_value);
static unsigned char parse_drv_cfg_param(char *varbuf, int len)
{
    struct drv_private *drv_priv = drv_get_drv_priv();
    short tmp_s16 = 0;

    if (drv_priv == NULL || varbuf == NULL || len <= 0)
        return 1;

    get_s8_item(varbuf, len, "cfg_band", &drv_priv->drv_config.cfg_band);
    if (drv_priv->drv_config.cfg_band != 2 && drv_priv->drv_config.cfg_band != 5 && drv_priv->drv_config.cfg_band != 6)
        drv_priv->drv_config.cfg_band = 6;
    if (get_s16_item(varbuf, len, "cfg_txpowlimit", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_txpowlimit = clamp_t(short, tmp_s16, 0, 20);
    if (get_s16_item(varbuf, len, "cfg_ampdu_subframes", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_ampdu_subframes = clamp_t(short, tmp_s16, DEFAULT_TXAMPDU_SUB_MIN, DEFAULT_TXAMPDU_SUB_MAX);
    if (get_s8_item(varbuf, len, "cfg_dynamic_bw", &drv_priv->drv_config.cfg_dynamic_bw) == 0) {
        drv_priv->drv_config.cfg_dynamic_bw = !!drv_priv->drv_config.cfg_dynamic_bw;
        drv_set_config((void *)drv_priv, CHIP_PARAM_DYNAMIC_BW, drv_priv->drv_config.cfg_dynamic_bw);
    }
    get_s8_item(varbuf, len, "cfg_wifi_bt_coexist_support", &drv_priv->drv_config.cfg_wifi_bt_coexist_support);
    drv_priv->drv_config.cfg_wifi_bt_coexist_support = !!drv_priv->drv_config.cfg_wifi_bt_coexist_support;
    get_s8_item(varbuf, len, "cfg_mac_mode", &drv_priv->drv_config.cfg_mac_mode);
    get_s8_item(varbuf, len, "cfg_burst_ack", &drv_priv->drv_config.cfg_burst_ack);
    drv_priv->drv_config.cfg_burst_ack = !!drv_priv->drv_config.cfg_burst_ack;
    if (get_s16_item(varbuf, len, "cfg_40Msupport", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_40Msupport = !!tmp_s16;
    get_s8_item(varbuf, len, "cfg_txaggr", &drv_priv->drv_config.cfg_txaggr);
    drv_priv->drv_config.cfg_txaggr = !!drv_priv->drv_config.cfg_txaggr;
    /* v15k: removed unconditional v15i HOSTAP BA-window force-disable here.
     * The runtime AMPDU entry point in drv_tx_start_aggr() (wifi_drv_xmit.c
     * around line 805) already gates with `wnet_vif->vm_opmode !=
     * WIFINET_M_HOSTAP`, so leaving cfg_txaggr=1 only enables AMPDU for
     * STA/MONITOR/IBSS modes. HOSTAP stays on the single-MPDU TX path that
     * avoids the v15i BA-window deadlock. */
    get_s8_item(varbuf, len, "cfg_rxaggr", &drv_priv->drv_config.cfg_rxaggr);
    drv_priv->drv_config.cfg_rxaggr = !!drv_priv->drv_config.cfg_rxaggr;
    if (drv_priv->drv_config.cfg_txaggr && !drv_priv->drv_config.cfg_rxaggr) {
        pr_warn("aml_wifi: cfg_txaggr=1 cfg_rxaggr=0; TX-only AMPDU test mode, keep ADDBA non-blocking\n");
    }
    if (get_s16_item(varbuf, len, "cfg_txamsdu", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_txamsdu = !!tmp_s16;
    pr_info("aml_wifi: parsed aggr cfg tx=%u rx=%u txamsdu=%u ampdu_subframes=%u\n",
            drv_priv->drv_config.cfg_txaggr,
            drv_priv->drv_config.cfg_rxaggr,
            drv_priv->drv_config.cfg_txamsdu,
            drv_priv->drv_config.cfg_ampdu_subframes);
    if (get_s16_item(varbuf, len, "cfg_retry_limit", &tmp_s16) == 0) {
        tmp_s16 = clamp_t(short, tmp_s16, 1, 31);
        drv_set_config((void *)drv_priv, CHIP_PARAM_RETRY_LIMIT, tmp_s16 << 8 | tmp_s16);
    }
    get_s8_item(varbuf, len, "cfg_eat_count_max", &drv_priv->drv_config.cfg_eat_count_max);
    drv_priv->drv_config.cfg_eat_count_max = clamp_t(unsigned char, drv_priv->drv_config.cfg_eat_count_max, 0, 8);
    if (get_s16_item(varbuf, len, "cfg_aggr_thresh", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_aggr_thresh = clamp_t(short, tmp_s16, 1, 128);
    /* v16p AP-CFG-FULL: previously the keys below were defined in struct
     * drv_config and CHIP_PARAM_* enum but the .conf parser never read
     * them, so users editing aml_wifi_drv_cfg_0.conf saw no effect.
     * Parse them now, then mirror via drv_set_config() where the setter
     * has additional HAL side-effects (TXLIVETIME → drv_hal_txlivetime). */
    if (get_s16_item(varbuf, len, "cfg_ampdu_limit", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_ampdu_limit = clamp_t(int, (unsigned short)tmp_s16, 1, 65535);
    if (get_s16_item(varbuf, len, "cfg_aggr_prot", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_aggr_prot = !!tmp_s16;
    if (get_s16_item(varbuf, len, "cfg_ampdu_oneframe", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_ampdu_oneframe = !!tmp_s16;
    if (get_s16_item(varbuf, len, "cfg_ampdu_livetime", &tmp_s16) == 0) {
        int lt = clamp_t(int, tmp_s16, 1, 1000);
        drv_set_config((void *)drv_priv, CHIP_PARAM_TXLIVETIME, lt);
    }
    if (get_s16_item(varbuf, len, "cfg_no_aggr_thresh", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_no_aggr_thresh = clamp_t(unsigned short, (unsigned short)tmp_s16, 1, 128);
    if (get_s16_item(varbuf, len, "cfg_ampduackpolicy", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_ampduackpolicy = (unsigned char)tmp_s16;
    get_s8_item(varbuf, len, "cfg_hrtimer_interval", &drv_priv->drv_config.cfg_hrtimer_interval);
    drv_priv->drv_config.cfg_hrtimer_interval = clamp_t(unsigned char, drv_priv->drv_config.cfg_hrtimer_interval, 1, 10);
    if (get_s16_item(varbuf, len, "cfg_listen_interval", &tmp_s16) == 0)
        drv_priv->drv_config.cfg_listen_interval = clamp_t(unsigned short, tmp_s16, 1, 255);
    get_s8_item(varbuf, len, "cfg_uapsd", &drv_priv->drv_config.cfg_uapsdsupported);
    drv_priv->drv_config.cfg_uapsdsupported = !!drv_priv->drv_config.cfg_uapsdsupported;
    get_s8_item(varbuf, len, "cfg_disable_ratecontrol", &drv_priv->drv_config.cfg_disratecontrol);
    drv_priv->drv_config.cfg_disratecontrol = !!drv_priv->drv_config.cfg_disratecontrol;
    get_s8_item(varbuf, len, "cfg_disable_fw_sleep", &drv_priv->drv_config.cfg_disable_fw_sleep);
    drv_priv->drv_config.cfg_disable_fw_sleep = !!drv_priv->drv_config.cfg_disable_fw_sleep;
    get_s8_item(varbuf, len, "cfg_latency_retry_enable", &drv_priv->drv_config.cfg_latency_retry_enable);
    drv_priv->drv_config.cfg_latency_retry_enable = !!drv_priv->drv_config.cfg_latency_retry_enable;
    get_s8_item(varbuf, len, "cfg_latency_retry_good", &drv_priv->drv_config.cfg_latency_retry_good);
    drv_priv->drv_config.cfg_latency_retry_good = clamp_t(unsigned char, drv_priv->drv_config.cfg_latency_retry_good, 1, 127);
    get_s8_item(varbuf, len, "cfg_latency_retry_fair", &drv_priv->drv_config.cfg_latency_retry_fair);
    drv_priv->drv_config.cfg_latency_retry_fair = clamp_t(unsigned char, drv_priv->drv_config.cfg_latency_retry_fair, 1, 127);
    get_s8_item(varbuf, len, "cfg_latency_rssi_good", &drv_priv->drv_config.cfg_latency_rssi_good);
    get_s8_item(varbuf, len, "cfg_latency_rssi_fair", &drv_priv->drv_config.cfg_latency_rssi_fair);
    get_s8_item(varbuf, len, "cfg_latency_snr_good", &drv_priv->drv_config.cfg_latency_snr_good);
    get_s8_item(varbuf, len, "cfg_latency_snr_fair", &drv_priv->drv_config.cfg_latency_snr_fair);
    AML_OUTPUT("======>>>>>> drv_config cfg_band = %d\n", drv_priv->drv_config.cfg_band);

    return 0;
}



int drv_cfg_load_from_file(void)
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
    mm_segment_t fs;
#endif
    struct file *fp;
    int size, len;
    char *content =  NULL;


    char conf_path[30] = "/etc/aml-wifi";
    unsigned char cfg_file[100];

    sprintf(cfg_file, "%s/aml_wifi_drv_cfg_%d.conf", conf_path, 0);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
    fs = get_fs();
    set_fs(KERNEL_DS);
#endif

    fp = filp_open(cfg_file, O_RDONLY, 0);

    if (IS_ERR(fp)) {
        fp = NULL;
        goto err;
    }

    size = (int)i_size_read(fp->f_path.dentry->d_inode);
    if (size <= 0) {
        filp_close(fp, NULL);
        goto err;
    }

    content = ZMALLOC(size + 1, "aml_drv_cfg", GFP_KERNEL);

    if (content == NULL) {
        filp_close(fp, NULL);
        goto err;
    }

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0))
    if (kernel_read(fp, content, size, &fp->f_pos) != size) {
#else
    if (vfs_read(fp, content, size, &fp->f_pos) != size) {
#endif
        FREE(content, "aml_drv_cfg");
        filp_close(fp, NULL);
        goto err;
    }
    content[size] = 0;

    len = process_drv_cfg_content(content, size);
    parse_drv_cfg_param(content, len);

    FREE(content, "aml_drv_cfg");
    filp_close(fp, NULL);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
    set_fs(fs);
#endif
    return 0;
err:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
    set_fs(fs);
#endif
    return 1;
}
