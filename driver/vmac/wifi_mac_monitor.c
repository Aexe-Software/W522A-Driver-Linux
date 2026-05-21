/*
 ****************************************************************************************
 *
 * Copyright (C) Amlogic 2010-2014
 *
 * Project: 11N 80211 mac  layer Software
 *
 * Description:
 *     monitor interface
 *
 *
 ****************************************************************************************
 */

#include "wifi_mac_com.h"
#include "wifi_hal_com.h"
/* v15p: <linux/unaligned.h> exists upstream from 6.12. Pre-6.12 kernels
 * (Armbian 6.5 / 6.6 / 6.8 / 6.11) still expose the same helpers via
 * <asm/unaligned.h>. The driver targets a wide kernel range so use the
 * version-guarded include. */
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

static int vm_cfg80211_monitor_if_open(struct net_device *ndev)
{
    int ret = 0;
    DPRINTF(AML_DEBUG_CFG80211,"%s <%s>\n", __func__, DEV_NAME(ndev));
    return ret;
}
static int vm_cfg80211_monitor_if_close(struct net_device *ndev)
{
    int ret = 0;
    DPRINTF(AML_DEBUG_CFG80211,"%s <%s>\n", __func__, DEV_NAME(ndev));
    return ret;
}

static int vm_cfg80211_monitor_if_xmit_entry(struct sk_buff *skb, struct net_device *ndev)
{
    int ret = 0;
    int rtap_len;
    int dot11_hdr_len = 24;
    int snap_len = 6;
    unsigned char *pdata;
    unsigned short frame_ctl = 0;
    unsigned char src_mac_addr[6];
    unsigned char dst_mac_addr[6];
    struct wifi_frame *wh;
    struct wlan_net_vif *wnet_vif = vm_netdev_priv(ndev);
    struct ieee80211_radiotap_header *rtap_hdr;

    DPRINTF(AML_DEBUG_CFG80211,"<%s>:%s ++\n", wnet_vif->vm_ndev->name,__func__);
#if 1
    if (unlikely(os_skb_get_pktlen(skb) < sizeof(struct ieee80211_radiotap_header)))
        goto fail;

    rtap_hdr = (struct ieee80211_radiotap_header *)os_skb_data(skb);
    if (unlikely(rtap_hdr->it_version))
        goto fail;
    rtap_len = ieee80211_get_radiotap_len(os_skb_data(skb));
    if (unlikely(os_skb_get_pktlen(skb) < rtap_len))
        goto fail;
    if (rtap_len != 14)
    {
        DPRINTF(AML_DEBUG_CFG80211,"radiotap len (should be 14): %d\n", rtap_len);
        goto fail;
    }
    os_skb_pull(skb, rtap_len);
    wh = (struct wifi_frame *)os_skb_data(skb);

    if (WIFINET_IS_ACTION(wh))
    {
        DPRINTF(AML_DEBUG_CFG80211, "%s, do: scan_abort\n", __func__);

        preempt_scan(wnet_vif->vm_ndev, 100, 100);
    }


    frame_ctl = *(unsigned short*)wh->i_fc; // need to check twice
    if ((frame_ctl & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA)
    {

        dot11_hdr_len= wifi_mac_hdrsize(wh);
        memcpy(dst_mac_addr, wh->i_addr1, sizeof(dst_mac_addr));
        memcpy(src_mac_addr, wh->i_addr2, sizeof(src_mac_addr));
        os_skb_pull(skb, dot11_hdr_len + snap_len - sizeof(src_mac_addr) * 2);
        pdata = (unsigned char*)os_skb_data(skb);
        memcpy(pdata, dst_mac_addr, sizeof(dst_mac_addr));
        memcpy(pdata + sizeof(dst_mac_addr), src_mac_addr, sizeof(src_mac_addr));
        // DPRINTF(AML_DEBUG_CFG80211,"should be eapol packet\n");
        DPRINTF(AML_DEBUG_CFG80211, "<running> %s %d should be eapol packet\n",__func__,__LINE__);

        ret =  wifi_mac_hardstart(skb, wnet_vif->vm_ndev);
        return ret;
    }
    else if ((frame_ctl & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE)) == cpu_to_le16(IEEE80211_STYPE_ACTION))
    {
#ifdef CONFIG_P2P
        if (vm_p2p_parse_negotiation_frames(wnet_vif->vm_p2p, os_skb_data(skb), &os_skb_get_pktlen(skb), true)== false)
        {
            DPRINTF(AML_DEBUG_CFG80211|AML_DEBUG_ERROR, "%s %d print frame err\n", __func__,__LINE__);
            goto fail;
        }
#endif
        if ( vm_cfg80211_send_mgmt(wnet_vif,os_skb_data(skb),  os_skb_get_pktlen(skb)) != 0)
        {
            DPRINTF(AML_DEBUG_CFG80211|AML_DEBUG_ERROR, "%s %d send frame err\n", __func__,__LINE__);
            goto fail;
        }
    }
    else if ((frame_ctl & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT)
    {
        /* v15d: forward arbitrary 802.11 mgmt subtypes (DEAUTH,
         * DISASSOC, AUTH, ASSOC_REQ/RESP, REASSOC_REQ/RESP,
         * PROBE_REQ/RESP, BEACON, ATIM) through the same firmware
         * TX path as ACTION. The skb already contains the full
         * 802.11 header from userspace; vm_cfg80211_send_mgmt
         * copies it into a fresh skb, attaches a sta from the vif
         * sta-table (lookup by addr1), sets WME_AC_VO priority and
         * submits via wifi_mac_tx_mgmt_frm -> drv_ops.tx_start.
         *
         * Per-frame TX power (radiotap.tx_power) is intentionally
         * NOT honored: firmware command interface only supports
         * per-VAP power. Injection itself works fine without it
         * for aireplay-ng / mdk4 / hostapd-mana / wifite use cases.
         */
        if (vm_cfg80211_send_mgmt(wnet_vif, os_skb_data(skb),
                                  os_skb_get_pktlen(skb)) != 0)
        {
            DPRINTF(AML_DEBUG_CFG80211|AML_DEBUG_ERROR,
                    "<inject> mgmt subtype=0x%x send err\n",
                    frame_ctl & IEEE80211_FCTL_STYPE);
            goto fail;
        }
    }
    else
    {
        /* CONTROL frames (RTS/CTS/ACK/BlockAck) are generated by HW
         * only on this chip; drop and log subtype for diagnostics. */
        DPRINTF(AML_DEBUG_CFG80211,
                "<inject> drop ctl/rsvd fc=0x%x\n",
                frame_ctl & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE));
    }
#endif
fail:
    wifi_mac_free_skb(skb);
    return 0;
}
/* v15j: radiotap injection helper for the *primary* vif's netdev.
 *
 * When the user switches w522a (the primary vif netdev) directly to
 * monitor type with `iw dev w522a set type monitor` + `ip link up`,
 * the netdev is ARPHRD_IEEE80211_RADIOTAP and userspace tools like
 * aireplay-ng / mdk4 / packetspammer / wifite send 802.11 frames
 * pre-pended with a radiotap header through .ndo_start_xmit.
 *
 * The primary vif's .ndo_start_xmit is wifi_mac_hardstart(), which
 * normally expects an Ethernet frame and decaps/encaps it through
 * the AMSDU/AMPDU pipelines as a STA-mode data frame. That path
 * panics or silently drops radiotap frames.
 *
 * This helper mirrors the radiotap-handling logic from
 * vm_cfg80211_monitor_if_xmit_entry() above (used by the secondary
 * mon0 netdev added via cfg80211_add_iface), but:
 *
 *   - takes wnet_vif explicitly (the primary netdev uses raw
 *     netdev_priv()->wnet_vif, not the indicator wrapper);
 *   - drops DATA + CONTROL frames instead of recursing back into
 *     wifi_mac_hardstart() (which would loop forever on this path);
 *   - dispatches MGMT subtypes (DEAUTH, DISASSOC, AUTH, ASSOC_*,
 *     REASSOC_*, PROBE_*, BEACON, ATIM) and ACTION through
 *     vm_cfg80211_send_mgmt() -> wifi_mac_tx_mgmt_frm() ->
 *     drv_ops.tx_start, which is what we actually want for
 *     aireplay-ng-style injection on a monitor-mode primary vif.
 *
 * Per-frame TX power / rate from radiotap is intentionally NOT
 * honoured: firmware only exposes a per-VAP TX power setpoint and
 * the rate adaptor lives in firmware; injection itself works fine
 * without per-frame overrides for the aireplay-ng / mdk4 use case.
 */
int wifi_mac_inject_radiotap(struct sk_buff *skb, struct net_device *ndev,
			     struct wlan_net_vif *wnet_vif)
{
	int rtap_len;
	unsigned short frame_ctl = 0;
	struct wifi_frame *wh;
	struct ieee80211_radiotap_header *rtap_hdr;

	/* v16b: was printk(KERN_INFO ...) every 32nd inject; under
	 * aireplay-ng / mdk4 burst this floods dmesg. Demote to
	 * pr_debug; enable via dyndebug if needed. */
	{
		static unsigned long _inj_cnt;
		if ((_inj_cnt++ & 0x1f) == 0) {
			pr_debug("inject_radiotap cnt=%lu "
				 "skb=%p len=%u opmode=%d ndev_type=%d\n",
				 _inj_cnt, skb, skb ? os_skb_get_pktlen(skb) : 0,
				 wnet_vif ? wnet_vif->vm_opmode : -1,
				 ndev ? ndev->type : -1);
		}
	}

	if (unlikely(skb == NULL))
		return NETDEV_TX_OK;

	/* v15k: kernel-skbs (i.e. ones the kernel hands us through
	 * .ndo_start_xmit) have an uninitialised skb->cb. Passing them
	 * to wifi_mac_free_skb() invokes wifi_mac_recycle_txdesc() which
	 * blindly dereferences ((wifi_mac_tx_info*)skb->cb)->ptxdesc and
	 * splices that garbage pointer into wifimac->txdesc_freequeue --
	 * eventually corrupting the freelist. Use dev_kfree_skb_any()
	 * here and on every error path below. */
	if (unlikely(os_skb_get_pktlen(skb) <
			sizeof(struct ieee80211_radiotap_header)))
		goto fail;

	rtap_hdr = (struct ieee80211_radiotap_header *)os_skb_data(skb);
	if (unlikely(rtap_hdr->it_version))
		goto fail;
	rtap_len = ieee80211_get_radiotap_len(os_skb_data(skb));
	if (unlikely(rtap_len < 0 ||
			os_skb_get_pktlen(skb) < (unsigned int)rtap_len))
		goto fail;

	os_skb_pull(skb, rtap_len);
	if (unlikely(os_skb_get_pktlen(skb) < sizeof(struct wifi_frame)))
		goto fail;
	wh = (struct wifi_frame *)os_skb_data(skb);
	frame_ctl = *(unsigned short *)wh->i_fc;

	if (WIFINET_IS_ACTION(wh))
		preempt_scan(wnet_vif->vm_ndev, 100, 100);

	if ((frame_ctl & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT) {
		if (vm_cfg80211_send_mgmt(wnet_vif, os_skb_data(skb),
					  os_skb_get_pktlen(skb)) != 0) {
			net_warn_ratelimited(
				"<inject-primary> mgmt subtype=0x%x send err\n",
				frame_ctl & IEEE80211_FCTL_STYPE);
			goto fail;
		}
		DPRINTF(AML_DEBUG_CFG80211,
			"<inject-primary> mgmt subtype=0x%x sent, len=%u\n",
			frame_ctl & IEEE80211_FCTL_STYPE,
			os_skb_get_pktlen(skb));
		/* skb consumed by vm_cfg80211_send_mgmt (which copies into
		 * a fresh skb). Free this caller-owned one. */
		goto fail;
	}

	/* DATA on a primary-vif monitor injection path is ambiguous:
	 * the vif may not be associated, no STA -> wifi_mac_hardstart
	 * would crash. Drop. CONTROL frames are HW-generated. */
	DPRINTF(AML_DEBUG_CFG80211,
		"<inject-primary> drop non-mgmt fc=0x%x\n",
		frame_ctl & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE));

fail:
	if (skb)
		dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* v15k: monitor RX path -- radiotap-wrap a raw 802.11 frame and feed it
 * to netif_rx() so airodump-ng / tcpdump / wireshark can see 802.11
 * traffic on the primary vif when its ndev->type is
 * ARPHRD_IEEE80211_RADIOTAP.
 *
 * Layout of the radiotap header we prepend (15 bytes total):
 *
 *   offset  field                          bytes
 *   ----------------------------------------------
 *    0      it_version (=0)                  1
 *    1      it_pad                           1
 *    2      it_len (LE, =15)                 2
 *    4      it_present (LE, FLAGS|RATE|      4
 *                       CHANNEL|DBM_ANTSIG)
 *    8      flags (=0)                       1
 *    9      rate (in 500 kbps units)         1
 *   10      channel freq (LE, MHz)           2
 *   12      channel flags (LE)               2
 *   14      dbm_antsignal (s8)               1
 *   ----------------------------------------------
 *   total                                   15
 *
 * The fields are intentionally minimal: airodump-ng and aircrack-ng's
 * osdep linux backend only need FLAGS, RATE, CHANNEL and signal to
 * function. TSFT is omitted on purpose to keep the header layout free
 * of 8-byte alignment padding (radiotap fields are aligned to their
 * natural size relative to the start of the radiotap header).
 */
#define MON_RTAP_PRESENT \
	((1u << IEEE80211_RADIOTAP_FLAGS)       | \
	 (1u << IEEE80211_RADIOTAP_RATE)        | \
	 (1u << IEEE80211_RADIOTAP_CHANNEL)     | \
	 (1u << IEEE80211_RADIOTAP_DBM_ANTSIGNAL))

#define MON_RTAP_HDRLEN	15

int wifi_mac_monitor_rx_radiotap(struct wlan_net_vif *wnet_vif,
				 struct sk_buff *skb,
				 struct wifi_mac_rx_status *rs)
{
	struct net_device *ndev;
	unsigned char *rtap;
	unsigned int payload_len;
	unsigned short chan_freq = 0;
	unsigned short chan_flags = 0;
	unsigned char rate_500kbps = 0;
	signed char dbm = 0;
	int chan;

	if (unlikely(wnet_vif == NULL || skb == NULL))
		return -EINVAL;

	ndev = wnet_vif->vm_ndev;
	if (unlikely(ndev == NULL))
		return -EINVAL;

	/* Without these we can't deliver. Don't try to allocate a fresh
	 * skb -- callers in the hot RX path are not allowed to sleep. */
	if (unlikely(skb_headroom(skb) < MON_RTAP_HDRLEN)) {
		struct sk_buff *nskb = skb_realloc_headroom(skb, MON_RTAP_HDRLEN);
		if (nskb == NULL)
			return -ENOMEM;
		/* Caller still owns the original skb on failure (we
		 * already returned non-zero above) but on success we
		 * must consume it. The caller passes skb=NULL after a
		 * successful return, so swap pointers locally. */
		dev_kfree_skb_any(skb);
		skb = nskb;
	}

	payload_len = os_skb_get_pktlen(skb);

	/* Compute radiotap channel-frequency (in MHz) and channel-flags
	 * from the channel *number* we get from the rx descriptor. The
	 * vendor RX-status's rs_channel is the 802.11 channel index
	 * (1..14 in 2.4 GHz, 36..196 in 5 GHz). When the descriptor
	 * doesn't carry one, fall back to the radio's currently
	 * programmed primary channel number. */
	chan = rs ? rs->rs_channel : 0;
	if (chan == 0 && wnet_vif->vm_wmac &&
	    wnet_vif->vm_wmac->wm_curchan &&
	    wnet_vif->vm_wmac->wm_curchan != WIFINET_CHAN_ERR) {
		chan = wnet_vif->vm_wmac->wm_curchan->chan_pri_num;
	}
	if (chan >= 1 && chan <= 13) {
		chan_freq = 2407 + chan * 5;
		chan_flags = 0x00a0; /* 2GHz | OFDM (close enough; airodump
				      * does not care for capture) */
	} else if (chan == 14) {
		chan_freq = 2484;
		chan_flags = 0x00a0;
	} else if (chan >= 36 && chan <= 196) {
		chan_freq = 5000 + chan * 5;
		chan_flags = 0x0140; /* 5GHz | OFDM */
	}

	if (rs) {
		/* The vendor driver reports rs_datarate in 100 kbps units
		 * for legacy rates (e.g. 60 for 6 Mbps). Convert to the
		 * 500 kbps units radiotap expects. Cap at 0xff. */
		unsigned int r = (unsigned int)rs->rs_datarate;
		if (r > 0 && r < 510)
			rate_500kbps = (unsigned char)(r / 5);
		else if (r >= 510)
			rate_500kbps = 0xfe;

		/* rs_rssi is reported as an unsigned-biased value in the
		 * vendor frames; the canonical conversion the rest of the
		 * driver uses is `(rssi - 256)` for dBm (see
		 * wifi_mac_if.c::wifi_mac_rx_complete, beacon branch).
		 * Clamp to a sane s8 range. */
		if (rs->rs_rssi >= 0 && rs->rs_rssi < 256)
			dbm = (signed char)(rs->rs_rssi - 256);
		else if (rs->rs_rssi >= -127 && rs->rs_rssi < 0)
			dbm = (signed char)rs->rs_rssi;
		else
			dbm = -90;
	}

	rtap = (unsigned char *)skb_push(skb, MON_RTAP_HDRLEN);
	if (unlikely(rtap == NULL))
		return -ENOMEM;

	rtap[0] = 0;                              /* it_version */
	rtap[1] = 0;                              /* it_pad */
	put_unaligned_le16(MON_RTAP_HDRLEN, rtap + 2);
	put_unaligned_le32(MON_RTAP_PRESENT, rtap + 4);
	rtap[8] = 0;                              /* FLAGS */
	rtap[9] = rate_500kbps;                   /* RATE */
	put_unaligned_le16(chan_freq, rtap + 10); /* CHANNEL freq */
	put_unaligned_le16(chan_flags, rtap + 12);/* CHANNEL flags */
	rtap[14] = (unsigned char)dbm;            /* DBM_ANTSIGNAL */

	skb->dev = ndev;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	/* ETH_P_802_2 is what mac80211 uses for radiotap-encapsulated
	 * monitor frames; it makes the kernel deliver to AF_PACKET raw
	 * sockets without trying to demux as IP/ARP. */
	skb->protocol = htons(ETH_P_802_2);

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += payload_len;

	netif_rx(skb);
	/* v16b: every 256th RX log; under heavy 802.11 traffic this still
	 * floods. Demote to pr_debug. */
	{
		static unsigned long _mon_rx_log_cnt;
		if ((_mon_rx_log_cnt++ & 0xff) == 0) {
			pr_debug("monitor rx#%lu len=%u "
				 "rate=%u dbm=%d freq=%u\n",
				 _mon_rx_log_cnt, payload_len,
				 (unsigned)rate_500kbps, (int)dbm,
				 (unsigned)chan_freq);
		}
	}
	return 0;
}

void wifi_mac_monitor_netif_up(struct wlan_net_vif *wnet_vif)
{
	struct net_device *ndev;

	if (wnet_vif == NULL)
		return;

	ndev = wnet_vif->vm_ndev;
	if (ndev == NULL)
		return;

	/* Bring the netdev fully up so the qdisc / packet_direct_xmit
	 * checks in dev.c don't drop outgoing radiotap frames before
	 * .ndo_start_xmit is even reached. cfg80211_register_netdevice()
	 * sets IF_OPER_DORMANT for all wifi netdevs (so wpa_supplicant
	 * has a chance to take ownership of the link); for monitor mode
	 * there is no supplicant, so we have to clear dormant ourselves. */
	netif_dormant_off(ndev);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	netif_wake_queue(ndev);

	/* v16b: was printk(KERN_INFO ...); only called once per netif-up so
	 * not a flood, but the 'v15k:' tag should not be in production
	 * logs. Demote to pr_debug so it surfaces only with dynamic_debug. */
	pr_debug("<%s>: monitor netif up "
		 "(carrier_on + queue_started + dormant_off) "
		 "flags=0x%x state=0x%lx\n",
		 ndev->name, ndev->flags, ndev->state);
}

static void vm_cfg80211_monitor_if_destructor(struct net_device *ndev)
{
    struct wlan_net_vif *wnet_vif = vm_netdev_priv(ndev);

    DPRINTF(AML_DEBUG_ANY,"<%s>:<%s>:%s\n", wnet_vif->vm_ndev->name,ndev->name,__func__);

    if (ndev->ieee80211_ptr)
        FREE((unsigned char *)ndev->ieee80211_ptr,"mwdev");
    
    free_netdev(ndev);
}
static int vm_cfg80211_go_if_xmit_entry(struct sk_buff *skb, struct net_device *ndev)
{
    struct wlan_net_vif *wnet_vif = vm_netdev_priv(ndev);

    DPRINTF(AML_DEBUG_ANY,"<%s>:<%s>:%s\n", wnet_vif->vm_ndev->name,ndev->name,__func__);

    dump_memory_internal(skb->data,32);
    return wifi_mac_hardstart(skb, wnet_vif->vm_ndev);
}

static int vm_cfg80211_monitor_set_mac_addr(struct net_device *ndev, void *addr)
{
    int ret = 0;
    DPRINTF(AML_DEBUG_CFG80211,"%s,<%s>:mac<%s> \n", __func__,ndev->name,ether_sprintf(ndev->dev_addr));
    return ret;
}
static const struct net_device_ops vm_cfg80211_monitor_if_ops =
{
    .ndo_open = vm_cfg80211_monitor_if_open,
    .ndo_stop = vm_cfg80211_monitor_if_close,
    .ndo_start_xmit = vm_cfg80211_monitor_if_xmit_entry,
    .ndo_set_mac_address = vm_cfg80211_monitor_set_mac_addr,
};
struct net_device *vm_cfg80211_add_monitor_if(struct wlan_net_vif *wnet_vif ,
const char *name)

{
    int ret = 0;
    struct net_device* ndev = NULL;
    struct vm_wdev_priv *pwdev_priv = wdev_to_priv(wnet_vif->vm_wdev);
    struct vm_netdev_priv_indicator *pnpi;
    struct wireless_dev* wdev = NULL;
    DPRINTF(AML_DEBUG_CFG80211,"%s <%s>\n", __func__, name);
    if (!name )
    {
        ret = -EINVAL;
        goto out;
    }
    if ((strnicmp(name, pwdev_priv->ifname_mon, strlen(name)) ==0)
        && pwdev_priv->pmon_ndev)
    {
        ndev = pwdev_priv->pmon_ndev;
        DPRINTF(AML_DEBUG_CFG80211,"%s, monitor interface(%s) has existed\n", __func__, name);
        goto out;
    }
    ndev = alloc_etherdev(sizeof(struct vm_netdev_priv_indicator));
    if (!ndev)
    {
        ret = -ENOMEM;
        goto out;
    }

    ndev->type = ARPHRD_IEEE80211_RADIOTAP;
    strncpy(ndev->name, name, IFNAMSIZ);
    ndev->name[IFNAMSIZ - 1] = 0;
    dev_addr_mod(ndev, 0, wnet_vif->vm_myaddr, WIFINET_ADDR_LEN);
    ndev->hard_header_len = DEFAULT_HARD_HDR_LEN;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
    ndev->priv_destructor = vm_cfg80211_monitor_if_destructor;
#else
    ndev->destructor = vm_cfg80211_monitor_if_destructor;
#endif
    if (wnet_vif->vm_wmac->wm_caps & WIFINET_C_HWCSUM)
    {
        //ndev->features |= (NETIF_F_HW_CSUM |NETIF_F_IP_CSUM);
        netdev_setcsum(ndev,1);
    }
    ndev->netdev_ops = &vm_cfg80211_monitor_if_ops;
    pnpi = netdev_priv(ndev);
    pnpi->priv = wnet_vif;
    pnpi->sizeof_priv = sizeof(wnet_vif);
    /*  wdev */
    wdev = (struct wireless_dev *)ZMALLOC(sizeof(struct wireless_dev),"mwdev", GFP_KERNEL);
    if (!wdev)
    {
        ERROR_DEBUG_OUT("MALLOC wireless_dev fail\n");
        ret = -ENOMEM;
        goto out;
    }

    wdev->wiphy = wnet_vif->vm_wdev->wiphy;
    wdev->netdev = ndev;
    wdev->iftype = NL80211_IFTYPE_MONITOR;
    ndev->ieee80211_ptr = wdev;
    ret = register_netdevice(ndev);
    if (ret)
    {
        goto out;
    }
    pwdev_priv->pmon_ndev = ndev;
    /* v16b: pre-v16b read IFNAMSIZ+1 (17) bytes from `name`, which the
     * caller typically supplies as a NUL-terminated string much
     * shorter than 17 chars (e.g. "mon0" = 5 bytes). The extra reads
     * past the end of `name` were uninitialised-memory reads. Use
     * strscpy() which respects the source string length and always
     * NUL-terminates the destination. */
    strscpy(pwdev_priv->ifname_mon, name, sizeof(pwdev_priv->ifname_mon));
    return  ndev;

out:
    if (ret && ndev)
    {
        free_netdev(ndev);
        ndev = NULL;
    }
    ERROR_DEBUG_OUT("ndev=%p, pmon_ndev=%p, ret=%d\n", ndev, pwdev_priv->pmon_ndev, ret);
    return ndev;
}

static int __vm_cfg80211_ioctl(struct net_device *ndev, void __user *data, int cmd)
{
    DPRINTF(AML_DEBUG_IOCTL, "%s %d cmd 0x%x\n", __func__, __LINE__, cmd);
    return -EOPNOTSUPP;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
static int vm_cfg80211_ioctl_wrapper(struct net_device *ndev, struct ifreq *ifr,
        void __user *data, int cmd)
{
    return __vm_cfg80211_ioctl(ndev, data, cmd);
}

#else

static int vm_cfg80211_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
    return __vm_cfg80211_ioctl(ndev, ifr->ifr_data, cmd);
}

#endif

static const struct net_device_ops vm_cfg80211_go_if_ops =
{
    .ndo_open = vm_cfg80211_monitor_if_open,
    .ndo_stop = vm_cfg80211_monitor_if_close,
    .ndo_start_xmit = vm_cfg80211_go_if_xmit_entry,
    .ndo_set_mac_address = vm_cfg80211_monitor_set_mac_addr,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
    .ndo_siocdevprivate         = vm_cfg80211_ioctl_wrapper,
#else
    .ndo_do_ioctl               = vm_cfg80211_ioctl,
#endif
};
struct net_device *vm_cfg80211_add_p2p_go_if(struct wlan_net_vif *wnet_vif ,
const char *name, enum nl80211_iftype type)
{
    int ret = 0;
    struct net_device* ndev = NULL;
    struct vm_wdev_priv *pwdev_priv = NULL;
    struct vm_netdev_priv_indicator *pnpi;
    // struct WIFINET_MAC *wifimac = vmac->vm_wmpriv;
    struct wireless_dev *vwdev;
    enum wifi_mac_opmode networkType, oldnetworkType;

    DPRINTF(AML_DEBUG_CFG80211, "%s %s,type %d\n", __func__,name,type);
    if (!name )
    {
        ret = -EINVAL;
        goto out;
    }
    ndev = alloc_etherdev(sizeof(struct vm_netdev_priv_indicator));
    if (!ndev)
    {
        ret = -ENOMEM;
        goto out;
    }
    vwdev = (struct wireless_dev *)ZMALLOC(sizeof(struct wireless_dev),"vwdev", GFP_KERNEL);
    if (!vwdev)
    {
        AML_OUTPUT("ERROR ENOMEM\n");
        ret = -ENOMEM;
        /* v16b: pre-v16b leaked ndev (allocated above with
         * alloc_etherdev). Free it before returning. */
        free_netdev(ndev);
        return NULL;
    }

#ifdef NEW_WIPHY
    vwdev->wiphy = wiphy_new(&vm_cfg80211_ops, sizeof(struct vm_wdev_priv));
    if (!vwdev->wiphy)
    {
        AML_OUTPUT("ERROR ENOMEM\n");
        ret = -ENOMEM;
        /* v16b: pre-v16b leaked both ndev and vwdev. Free in
         * reverse order of allocation. */
        FREE((unsigned char *)vwdev, "vwdev");
        free_netdev(ndev);
        return NULL;

    }
    AML_PRINT(AML_DBG_MODULES_P2P, "%s,type %d\n", name,type);

    set_wiphy_dev(vwdev->wiphy, ndev );
    vwdev->wiphy->interface_modes = BIT(NL80211_IFTYPE_P2P_GO)|BIT(NL80211_IFTYPE_P2P_CLIENT);

    vwdev->wiphy->cipher_suites = aml_cipher_suites;
    vwdev->wiphy->n_cipher_suites = ARRAY_SIZE(aml_cipher_suites);

    vwdev->wiphy->bands[IEEE80211_BAND_2GHZ] = aml_spt_band_alloc(IEEE80211_BAND_2GHZ);
    vwdev->wiphy->bands[IEEE80211_BAND_5GHZ] = aml_spt_band_alloc(IEEE80211_BAND_5GHZ);

    if (wnet_vif->vm_pwrsave.ips_sta_psmode != WIFINET_PWRSAVE_NONE)
        vwdev->wiphy->flags |= WIPHY_FLAG_PS_ON_BY_DEFAULT;
    else
        vwdev->wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;
    vwdev->wiphy->mgmt_stypes = vm_cfg80211_default_mgmt_stypes;
    vwdev->wiphy->addresses = wnet_vif->vm_myaddr;
    vwdev->wiphy->n_addresses = 1;
    pwdev_priv = wdev_to_priv(vwdev);
    pwdev_priv->pmon_ndev = NULL;
    pwdev_priv->pGo_ndev = NULL;
    pwdev_priv->ifname_mon[0] = '\0';
    pwdev_priv->ifname_go[0] = '\0';
    pwdev_priv->vm_wdev = vwdev;
    pwdev_priv->wnet_vif = wnet_vif;
    pwdev_priv->scan_request = NULL;
    spin_lock_init(&pwdev_priv->scan_req_lock);
    pwdev_priv->connect_request = NULL;
    AML_PRINT(AML_DBG_MODULES_P2P, "%s,type %d\n", name,type);

    spin_lock_init(&pwdev_priv->connect_req_lock);
    os_timer_ex_initialize(&pwdev_priv->connect_timeout, CFG80211_CONNECT_TIMER_OUT,
                           vm_cfg80211_connect_timeout_timer, wnet_vif);

    AML_OUTPUT("<running>\n");
    ret = wiphy_register(vwdev->wiphy);
    if (ret < 0)
    {
        AML_OUTPUT("ERROR register wiphy\n");
        ret = -ENOMEM;
        /* v16b: pre-v16b leaked the wiphy, the vwdev and ndev on
         * wiphy_register() failure. wiphy_new() return values must
         * be released via wiphy_free() (not kfree) per cfg80211 API.
         * The vwdev->wiphy pointer is the wiphy itself so we
         * release it in the canonical order. */
        wiphy_free(vwdev->wiphy);
        FREE((unsigned char *)vwdev, "vwdev");
        free_netdev(ndev);
        return NULL;
    }
    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d\n", name,type);
#else
    vwdev->wiphy =    wnet_vif->vm_wdev->wiphy;
    pwdev_priv = wdev_to_priv(vwdev);
#endif
    vwdev->iftype = type;
    ndev->ieee80211_ptr = vwdev;
    vwdev->netdev = ndev;
    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d\n",name,type);
    //ndev->type = ARPHRD_ETHER;
    strncpy(ndev->name, name, IFNAMSIZ);
    ndev->name[IFNAMSIZ - 1] = 0;
    ndev->netdev_ops = &vm_cfg80211_go_if_ops;

    pnpi = netdev_priv(ndev);
    pnpi->priv = wnet_vif;
    pnpi->sizeof_priv = sizeof(wnet_vif);
    dev_addr_mod(ndev, 0, wnet_vif->vm_myaddr, WIFINET_ADDR_LEN);

    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d\n",name,type);

    ret = register_netdevice(ndev);
    if (ret)
    {
        goto out;
    }
    pwdev_priv->pGo_ndev = ndev;
    pwdev_priv->pGo_wdev = vwdev;
    /* v16b: same fix as above for ifname_mon. See line ~555. */
    strscpy(pwdev_priv->ifname_go, name, sizeof(pwdev_priv->ifname_go));

    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d\n",name,type);

    networkType = nl80211_iftype_2_drv_opmode(type);
    if (networkType < 0)
    {
        goto out;
    }

    oldnetworkType = wnet_vif->vm_opmode;
    if (oldnetworkType != networkType)
    {
        struct wifi_mac *wifimac = wnet_vif->vm_wmac;
        DPRINTF(AML_DEBUG_CFG80211, "%s %d\n", __func__, __LINE__);
        preempt_scan(wnet_vif->vm_ndev, 100, 100);
        wnet_vif->wnet_vif_replaycounter++;
        //wifi_mac_stop(vmac->vm_dev);
        /* v16b: rest of the driver reads vm_state with READ_ONCE() to
         * defend against torn / cached reads across CPUs (see
         * wifi_mac_if.c:4725). The only writer that did NOT match was
         * here in the monitor path. Use WRITE_ONCE for consistency so
         * the optimizer does not split the store on a UP-relaxed
         * build. */
        WRITE_ONCE(wnet_vif->vm_state, WIFINET_S_INIT);
        wifi_mac_scan_vdetach(wnet_vif);
        if (wifimac->drv_priv->drv_ops.change_interface(wifimac->drv_priv,
                                                        wnet_vif->wnet_vif_id,
                                                        wnet_vif,
                                                        (enum hal_op_mode)networkType,
                                                        wnet_vif->vm_myaddr, 0))
        {
            ERROR_DEBUG_OUT("Unable to add an interface for driver.\n");
            /* v16b: ndev was already register_netdevice'd above. Pre-v16b
             * returned NULL without unregistering, leaking the
             * netdev (and its wdev) permanently. Also wifi_mac_free_vmac
             * is a no-op in the current codebase (see wifi_mac_if.c:122)
             * so it provided no actual cleanup. Properly unregister
             * the ndev so cfg80211 / netdev refcounts go back to zero
             * and the destructor (vm_cfg80211_monitor_if_destructor)
             * gets a chance to free wdev/wiphy too. */
            wifi_mac_free_vmac(wnet_vif);
            unregister_netdevice(ndev);
            return NULL;
        }
        wnet_vif->vm_opmode = networkType;
        wifi_mac_scan_vattach(wnet_vif);
        //wifi_mac_vmac_setup_forchvif(wifimac, vmac, NULL, networkType);
        if (wnet_vif->vm_opmode == WIFINET_M_HOSTAP)
        {
            if (wnet_vif->vm_max_aid == 0)
                wnet_vif->vm_max_aid = WIFINET_AID_DEF;
            else if (wnet_vif->vm_max_aid > WIFINET_AID_MAX)
                wnet_vif->vm_max_aid = WIFINET_AID_MAX;
        }
        wifi_mac_open(wnet_vif->vm_ndev);
#ifdef  CONFIG_P2P
        if (wnet_vif->vm_p2p_support)
        {
            vm_p2p_set_role(wnet_vif->vm_p2p, NL80211_IFTYPE_2_p2p_role(type));
            DPRINTF(AML_DEBUG_CFG80211, "%s %d new p2p_role=%d\n", __func__, __LINE__, vm_p2p_role(wnet_vif->vm_p2p));
        }
#endif

        if (WIFINET_M_HOSTAP == networkType)
        {
            DPRINTF(AML_DEBUG_CFG80211, "%s %d mode change over\n", __func__, __LINE__);
            wait_for_ap_run(wnet_vif, 5000, 100);
            DPRINTF(AML_DEBUG_CFG80211, "%s %d wait_for_ap_run over\n", __func__, __LINE__);
        }
        wnet_vif->vm_ndev->ieee80211_ptr->iftype = type;
    }
out:
    if (ret && ndev)
    {
        free_netdev(ndev);
        ndev = NULL;
    }

    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d, ndev=%p, ret=%d\n",name,type, ndev, ret);
    return ndev;
}

