/* SPDX-License-Identifier: GPL-2.0
 * wifi_cfg80211_compat.h — cfg80211 channel-switch API shims
 *
 * БАГ 4 fix: оригінал використовував AML_CFG80211_HAS_LINK_ID для ОБОХ
 * функцій (ch_switch_notify і ch_switch_started_notify), але ці функції
 * отримали link_id в РІЗНИХ версіях ядра. Треба окремі прапори.
 *
 * install.sh генерує vmac/aml_cfg80211_autoconf.h ПЕРЕД збіркою,
 * парсячи реальний cfg80211.h через awk і встановлюючи:
 *
 *   AML_CFG80211_CSN_HAS_LINK_ID      — ch_switch_notify     v2+
 *   AML_CFG80211_CSN_HAS_PUNCTURED    — ch_switch_notify     v3+
 *   AML_CFG80211_CSSN_HAS_LINK_ID     — ch_switch_started_notify v2+
 *   AML_CFG80211_CSSN_HAS_QUIET       — ch_switch_started_notify v3+
 *   AML_CFG80211_CSSN_HAS_PUNCTURED   — ch_switch_started_notify v4+
 *
 * Немає кастів. Немає __builtin_choose_expr. Тільки прямі виклики.
 */
#ifndef _WIFI_CFG80211_COMPAT_H_
#define _WIFI_CFG80211_COMPAT_H_

#include <linux/types.h>
#include <net/cfg80211.h>

/* ------------------------------------------------------------------
 * cfg80211_ch_switch_notify
 *   v1 (< 5.19):  (dev, chandef)
 *   v2 (5.19+):   (dev, chandef, link_id)
 *   v3 (6.3+):    (dev, chandef, link_id, punctured)
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * cfg80211_ch_switch_started_notify
 *   v1 (< 5.19):  (dev, chandef, count)
 *   v2 (5.19-6.0):(dev, chandef, link_id, count)
 *   v3 (6.1-6.2): (dev, chandef, link_id, count, quiet)
 *   v4 (6.3+):    (dev, chandef, link_id, count, quiet, punctured)
 * ------------------------------------------------------------------ */
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

#endif /* _WIFI_CFG80211_COMPAT_H_ */
