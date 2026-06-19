
#include "wifi_mac_com.h"
#include "wifi_hal_com.h"

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
    if (unlikely(rtap_len < sizeof(struct ieee80211_radiotap_header) ||
                 os_skb_get_pktlen(skb) < rtap_len))
        goto fail;
    os_skb_pull(skb, rtap_len);
    if (unlikely(os_skb_get_pktlen(skb) < sizeof(struct wifi_frame)))
        goto fail;
    wh = (struct wifi_frame *)os_skb_data(skb);

    if (WIFINET_IS_ACTION(wh))
    {
        DPRINTF(AML_DEBUG_CFG80211, "%s, do: scan_abort\n", __func__);

        preempt_scan(wnet_vif->vm_ndev, 100, 100);
    }

    frame_ctl = get_unaligned_le16(wh->i_fc); 
    if ((frame_ctl & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA)
    {

        dot11_hdr_len= wifi_mac_hdrsize(wh);
        if (unlikely(os_skb_get_pktlen(skb) < dot11_hdr_len + snap_len))
            goto fail;
        memcpy(dst_mac_addr, wh->i_addr1, sizeof(dst_mac_addr));
        memcpy(src_mac_addr, wh->i_addr2, sizeof(src_mac_addr));
        os_skb_pull(skb, dot11_hdr_len + snap_len - sizeof(src_mac_addr) * 2);
        pdata = (unsigned char*)os_skb_data(skb);
        memcpy(pdata, dst_mac_addr, sizeof(dst_mac_addr));
        memcpy(pdata + sizeof(dst_mac_addr), src_mac_addr, sizeof(src_mac_addr));
        
        DPRINTF(AML_DEBUG_CFG80211, "<running> %s %d should be eapol packet\n",__func__,__LINE__);

        ret =  wifi_mac_hardstart(skb, wnet_vif->vm_ndev);
        return ret;
    }
    else if ((frame_ctl & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE)) == IEEE80211_STYPE_ACTION)
    {
        if ( vm_cfg80211_send_mgmt(wnet_vif,os_skb_data(skb),  os_skb_get_pktlen(skb)) != 0)
        {
            DPRINTF(AML_DEBUG_CFG80211|AML_DEBUG_ERROR, "%s %d send frame err\n", __func__,__LINE__);
            goto fail;
        }
    }
    else if ((frame_ctl & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT)
    {
        
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
        
        DPRINTF(AML_DEBUG_CFG80211,
                "<inject> drop ctl/rsvd fc=0x%x\n",
                frame_ctl & (IEEE80211_FCTL_FTYPE|IEEE80211_FCTL_STYPE));
    }
#endif
fail:
    if (skb)
        dev_kfree_skb_any(skb);
    return 0;
}

int wifi_mac_inject_radiotap(struct sk_buff *skb, struct net_device *ndev,
			     struct wlan_net_vif *wnet_vif)
{
	int rtap_len;
	unsigned short frame_ctl = 0;
	struct wifi_frame *wh;
	struct ieee80211_radiotap_header *rtap_hdr;

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

	if (unlikely(os_skb_get_pktlen(skb) <
			sizeof(struct ieee80211_radiotap_header)))
		goto fail;

	rtap_hdr = (struct ieee80211_radiotap_header *)os_skb_data(skb);
	if (unlikely(rtap_hdr->it_version))
		goto fail;
	rtap_len = ieee80211_get_radiotap_len(os_skb_data(skb));
	if (unlikely(rtap_len < sizeof(struct ieee80211_radiotap_header) ||
			os_skb_get_pktlen(skb) < (unsigned int)rtap_len))
		goto fail;

	os_skb_pull(skb, rtap_len);
	if (unlikely(os_skb_get_pktlen(skb) < sizeof(struct wifi_frame)))
		goto fail;
	wh = (struct wifi_frame *)os_skb_data(skb);
	frame_ctl = get_unaligned_le16(wh->i_fc);

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
		
		goto fail;
	}

	DPRINTF(AML_DEBUG_CFG80211,
		"<inject-primary> drop non-mgmt fc=0x%x\n",
		frame_ctl & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE));

fail:
	if (skb)
		dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

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

	if (unlikely(skb_headroom(skb) < MON_RTAP_HDRLEN)) {
		struct sk_buff *nskb = skb_realloc_headroom(skb, MON_RTAP_HDRLEN);
		if (nskb == NULL)
			return -ENOMEM;
		
		dev_kfree_skb_any(skb);
		skb = nskb;
	}

	payload_len = os_skb_get_pktlen(skb);

	chan = rs ? rs->rs_channel : 0;
	if (chan == 0 && wnet_vif->vm_wmac &&
	    wnet_vif->vm_wmac->wm_curchan &&
	    wnet_vif->vm_wmac->wm_curchan != WIFINET_CHAN_ERR) {
		chan = wnet_vif->vm_wmac->wm_curchan->chan_pri_num;
	}
	if (chan >= 1 && chan <= 13) {
		chan_freq = 2407 + chan * 5;
		chan_flags = 0x00a0; 
	} else if (chan == 14) {
		chan_freq = 2484;
		chan_flags = 0x00a0;
	} else if (chan >= 36 && chan <= 196) {
		chan_freq = 5000 + chan * 5;
		chan_flags = 0x0140; 
	}

	if (rs) {
		
		unsigned int r = (unsigned int)rs->rs_datarate;
		if (r > 0 && r < 510)
			rate_500kbps = (unsigned char)(r / 5);
		else if (r >= 510)
			rate_500kbps = 0xfe;

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

	rtap[0] = 0;                              
	rtap[1] = 0;                              
	put_unaligned_le16(MON_RTAP_HDRLEN, rtap + 2);
	put_unaligned_le32(MON_RTAP_PRESENT, rtap + 4);
	rtap[8] = 0;                              
	rtap[9] = rate_500kbps;                   
	put_unaligned_le16(chan_freq, rtap + 10); 
	put_unaligned_le16(chan_flags, rtap + 12);
	rtap[14] = (unsigned char)dbm;            

	skb->dev = ndev;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	
	skb->protocol = htons(ETH_P_802_2);

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += payload_len;

	netif_rx(skb);
	
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

	netif_dormant_off(ndev);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	netif_wake_queue(ndev);

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
        strscpy(ndev->name, name, IFNAMSIZ);
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
        
        netdev_setcsum(ndev,1);
    }
    ndev->netdev_ops = &vm_cfg80211_monitor_if_ops;
    pnpi = netdev_priv(ndev);
    pnpi->priv = wnet_vif;
    pnpi->sizeof_priv = sizeof(wnet_vif);
    
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
        
        free_netdev(ndev);
        return NULL;
    }

    vwdev->wiphy =    wnet_vif->vm_wdev->wiphy;
    pwdev_priv = wdev_to_priv(vwdev);
    vwdev->iftype = type;
    ndev->ieee80211_ptr = vwdev;
    vwdev->netdev = ndev;
    AML_PRINT(AML_DBG_MODULES_P2P,"%s,type %d\n",name,type);
    
        strscpy(ndev->name, name, IFNAMSIZ);
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
        
        WRITE_ONCE(wnet_vif->vm_state, WIFINET_S_INIT);
        wifi_mac_scan_vdetach(wnet_vif);
        if (wifimac->drv_priv->drv_ops.change_interface(wifimac->drv_priv,
                                                        wnet_vif->wnet_vif_id,
                                                        wnet_vif,
                                                        (enum hal_op_mode)networkType,
                                                        wnet_vif->vm_myaddr, 0))
        {
            ERROR_DEBUG_OUT("Unable to add an interface for driver.\n");
            
            wifi_mac_free_vmac(wnet_vif);
            unregister_netdevice(ndev);
            return NULL;
        }
        wnet_vif->vm_opmode = networkType;
        wifi_mac_scan_vattach(wnet_vif);
        
        if (wnet_vif->vm_opmode == WIFINET_M_HOSTAP)
        {
            if (wnet_vif->vm_max_aid == 0)
                wnet_vif->vm_max_aid = WIFINET_AID_DEF;
            else if (wnet_vif->vm_max_aid > WIFINET_AID_MAX)
                wnet_vif->vm_max_aid = WIFINET_AID_MAX;
        }
        wifi_mac_open(wnet_vif->vm_ndev);

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
