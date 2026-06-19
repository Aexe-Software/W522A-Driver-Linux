
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

int wifi_mac_inject_radiotap(struct sk_buff *skb, struct net_device *ndev,
			     struct wlan_net_vif *wnet_vif);

int wifi_mac_monitor_rx_radiotap(struct wlan_net_vif *wnet_vif,
				 struct sk_buff *skb,
				 struct wifi_mac_rx_status *rs);

void wifi_mac_monitor_netif_up(struct wlan_net_vif *wnet_vif);

#define vm_netdev_priv(netdev)((struct wlan_net_vif *) ( ((struct vm_netdev_priv_indicator *)netdev_priv(netdev))->priv ))

#endif
