
#ifndef _WIFI_CFG80211_COMPAT_H_
#define _WIFI_CFG80211_COMPAT_H_

#include <linux/types.h>
#include <net/cfg80211.h>

#if defined(AML_CFG80211_CSN_HAS_PUNCTURED)
#define aml_cfg80211_ch_switch_notify(dev, chandef, link_id) \
	cfg80211_ch_switch_notify((dev), (chandef), (link_id), 0)
#elif defined(AML_CFG80211_CSN_HAS_LINK_ID)
#define aml_cfg80211_ch_switch_notify(dev, chandef, link_id) \
	cfg80211_ch_switch_notify((dev), (chandef), (link_id))
#else
#define aml_cfg80211_ch_switch_notify(dev, chandef, link_id) \
	cfg80211_ch_switch_notify((dev), (chandef))
#endif

#if defined(AML_CFG80211_CSSN_HAS_PUNCTURED)
#define aml_cfg80211_ch_switch_started_notify(dev, chandef, link_id, count, quiet) \
	cfg80211_ch_switch_started_notify((dev), (chandef), (link_id), (count), (quiet), 0)
#elif defined(AML_CFG80211_CSSN_HAS_QUIET)
#define aml_cfg80211_ch_switch_started_notify(dev, chandef, link_id, count, quiet) \
	cfg80211_ch_switch_started_notify((dev), (chandef), (link_id), (count), (quiet))
#elif defined(AML_CFG80211_CSSN_HAS_LINK_ID)
#define aml_cfg80211_ch_switch_started_notify(dev, chandef, link_id, count, quiet) \
	cfg80211_ch_switch_started_notify((dev), (chandef), (link_id), (count))
#else
#define aml_cfg80211_ch_switch_started_notify(dev, chandef, link_id, count, quiet) \
	cfg80211_ch_switch_started_notify((dev), (chandef), (count))
#endif

#endif 
