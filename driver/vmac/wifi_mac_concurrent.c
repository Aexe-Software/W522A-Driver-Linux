#include "wifi_mac_com.h"
#include "wifi_drv_xmit.h"

unsigned char concurrent_check_is_vmac_same_pri_channel(struct wifi_mac *wifimac)
{
    struct drv_private *drv_priv = NULL;
    struct wlan_net_vif *main_vmac = NULL;
    struct wlan_net_vif *p2p_vmac = NULL;

    if (wifimac == NULL || wifimac->drv_priv == NULL)
        return 1;

    drv_priv = wifimac->drv_priv;
    main_vmac = drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];
    p2p_vmac = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];

    if (main_vmac == NULL || p2p_vmac == NULL)
        return 1;

    if ((main_vmac->vm_curchan == WIFINET_CHAN_ERR) || (p2p_vmac->vm_curchan == WIFINET_CHAN_ERR)) {
        return 1;

    } else {
        return (main_vmac->vm_curchan->chan_pri_num == p2p_vmac->vm_curchan->chan_pri_num);
    }
}

unsigned char concurrent_check_vmac_is_AP(struct wifi_mac *wifimac)
{
    struct drv_private *drv_priv = NULL;
    struct wlan_net_vif *p2p_vmac = NULL;

    if (wifimac == NULL || wifimac->drv_priv == NULL)
        return 0;

    drv_priv = wifimac->drv_priv;
    p2p_vmac = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];
    if (p2p_vmac == NULL)
        return 0;
    if (p2p_vmac->vm_opmode == WIFINET_M_HOSTAP)
        return 1;
    else
        return 0;
}

struct wlan_net_vif *wifi_mac_running_main_wnet_vif(struct wifi_mac *wifimac)
{
    struct drv_private *drv_priv = NULL;
    struct wlan_net_vif *main_vmac = NULL;

    if (wifimac == NULL || wifimac->drv_priv == NULL)
        return NULL;

    drv_priv = wifimac->drv_priv;
    main_vmac = drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];

    if (main_vmac != NULL && main_vmac->vm_state == WIFINET_S_CONNECTED)
    {
        return main_vmac;
    }

    return NULL;
}

struct wlan_net_vif *wifi_mac_running_wnet_vif(struct wifi_mac *wifimac)
{
    struct drv_private *drv_priv = NULL;
    struct wlan_net_vif *main_vmac = NULL;
    struct wlan_net_vif *p2p_vmac = NULL;

    if (wifimac == NULL || wifimac->drv_priv == NULL)
        return NULL;

    drv_priv = wifimac->drv_priv;
    main_vmac = drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];
    p2p_vmac = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];

    if (atomic_read(&wifimac->wm_nrunning) != 1) {
        return NULL;
    }

    if (main_vmac != NULL && main_vmac->vm_state == WIFINET_S_CONNECTED)
    {
        return main_vmac;
    }
    else if (p2p_vmac != NULL && p2p_vmac->vm_state == WIFINET_S_CONNECTED)
    {
        return p2p_vmac;
    }

    return NULL;
}

void channel_switch_announce_trigger(struct wifi_mac *wifimac, struct wifi_channel *switch_chan)
{
    unsigned int delay_ms = 0, csa_count = 0;
    struct drv_private *drv_priv = wifimac->drv_priv;
    struct wlan_net_vif *p2p_wnet_vif = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];

    if (switch_chan == NULL) {
        ERROR_DEBUG_OUT("switch channel is null!\n");
        return;
    }

    if (p2p_wnet_vif == NULL || p2p_wnet_vif->vm_p2p == NULL)
        return;

    if (switch_chan->chan_pri_num == 0) {
        ERROR_DEBUG_OUT("err chan_pri_num is zero!\n");
        return;
    }

    while ((wifimac->wm_flags & WIFINET_F_DOTH) && (wifimac->wm_flags & WIFINET_F_CHANSWITCH) && (delay_ms < 1000)) {
        mdelay(1);
        delay_ms++;
    }

    if (p2p_wnet_vif->vm_curchan
        && (p2p_wnet_vif->vm_curchan->chan_pri_num == switch_chan->chan_pri_num)
        && (p2p_wnet_vif->vm_curchan->chan_bw == switch_chan->chan_bw)) {
        AML_OUTPUT("channel:%d, bw:%d, no need trigger csa!\n", switch_chan->chan_pri_num, switch_chan->chan_bw);
        return;
    }

    if ((p2p_wnet_vif->vm_dtim_period > 0) && (p2p_wnet_vif->vm_dtim_period < 5)) {
        csa_count = p2p_wnet_vif->vm_dtim_period;
    } else {
        csa_count = CSA_COUNT;
    }

    os_timer_ex_cancel(&wifimac->wm_csa_trigger_timer, CANCEL_NO_SLEEP);

    AML_OUTPUT("chan_pri_num:%d, bw:%d, csa_count:%d, delay_ms:%d\n",
            switch_chan->chan_pri_num, switch_chan->chan_bw, csa_count, delay_ms);

    if (IS_APSTA_CONCURRENT(aml_wifi_get_con_mode()) && concurrent_check_vmac_is_AP(wifimac)) {
        p2p_wnet_vif->vm_wmac->wm_flags |= WIFINET_F_DOTH;
        p2p_wnet_vif->vm_wmac->wm_flags |= WIFINET_F_CHANSWITCH;
        p2p_wnet_vif->vm_wmac->wm_doth_channel = switch_chan->chan_pri_num;
        p2p_wnet_vif->vm_wmac->wm_doth_tbtt = csa_count;
        p2p_wnet_vif->csa_target.switch_chan = *switch_chan;
        if (p2p_wnet_vif->vm_p2p->go_hidden_mode) {
            p2p_wnet_vif->vm_flags &= ~WIFINET_F_HIDESSID;
        }
    }
}
