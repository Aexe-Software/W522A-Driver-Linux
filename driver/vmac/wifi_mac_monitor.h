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

#ifndef __NET80211_MONITOR_H__
#define __NET80211_MONITOR_H__

#define	SIOCG80211STATS		(SIOCDEVPRIVATE+1)
#define	SIOC80211IFDESTROY	 	(SIOCDEVPRIVATE+2)

struct wifi_mac_rx_status;
struct wlan_net_vif;


struct net_device *vm_cfg80211_add_p2p_go_if(struct wlan_net_vif *wnet_vif ,
	const char *name,

enum nl80211_iftype type);
struct net_device *vm_cfg80211_add_monitor_if(struct wlan_net_vif *wnet_vif ,
			const char *name);

/* v15j: monitor-mode radiotap injection on the *primary* vif's netdev.
 * Strips a radiotap header, dispatches the wrapped 802.11 mgmt/action
 * frame through vm_cfg80211_send_mgmt(), drops DATA + CONTROL frames.
 * Used by wifi_mac_hardstart() as a fast path when ndo_start_xmit is
 * invoked with dev->type == ARPHRD_IEEE80211_RADIOTAP (i.e. after
 * `iw dev <if> set type monitor` + `ip link up`). Returns NETDEV_TX_OK
 * unconditionally. */
int wifi_mac_inject_radiotap(struct sk_buff *skb, struct net_device *ndev,
			     struct wlan_net_vif *wnet_vif);

/* v15k: monitor-mode RX path. Wraps the raw 802.11 frame in a minimal
 * radiotap header (FLAGS + RATE + CHANNEL + DBM_ANTSIGNAL) and delivers
 * it through netif_rx() with skb->protocol = htons(ETH_P_802_2) and
 * skb->dev = primary vif netdev (whose ->type is ARPHRD_IEEE80211_RADIOTAP).
 *
 * Callers must pass a *raw 802.11* skb (the buffer the firmware handed
 * up before any decapsulation/LLC stripping). This helper takes
 * ownership of the skb on success and returns 0; on failure it returns
 * non-zero and the skb is left untouched (caller frees).
 *
 * Used by wifi_mac_input() in WIFINET_M_MONITOR mode to make airodump-ng,
 * tcpdump (-y IEEE802_11_RADIO), and wireshark see 802.11 traffic.
 */
int wifi_mac_monitor_rx_radiotap(struct wlan_net_vif *wnet_vif,
				 struct sk_buff *skb,
				 struct wifi_mac_rx_status *rs);

/* v15k: bring monitor netdev into a fully running state -- carrier on,
 * queue started, dormant off. Called from wifi_mac_initial() once the
 * MONITOR vif's state machine reaches WIFINET_S_CONNECTED. Without this
 * the netdev stays in operstate=DOWN (NO-CARRIER) and the kernel core
 * (e.g. __dev_queue_xmit, packet_direct_xmit) silently drops outgoing
 * radiotap frames from aireplay-ng / mdk4 before they ever reach
 * .ndo_start_xmit. */
void wifi_mac_monitor_netif_up(struct wlan_net_vif *wnet_vif);

#define vm_netdev_priv(netdev)((struct wlan_net_vif *) ( ((struct vm_netdev_priv_indicator *)netdev_priv(netdev))->priv ))

#endif
