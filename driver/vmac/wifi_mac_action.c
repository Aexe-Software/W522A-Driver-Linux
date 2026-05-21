#include "wifi_mac_com.h"
#include "wifi_cmd_func.h"

extern bool aml_char_is_hex_digit(char chTmp);

static struct wlan_net_vif *wifi_mac_get_action_vif(struct wifi_mac *wifimac)
{
    struct drv_private* drv_priv = wifimac->drv_priv;
    struct wlan_net_vif *p2p_vmac = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];

    if (atomic_read(&wifimac->wm_nrunning) != 1)
        return NULL;

    if (p2p_vmac != NULL && p2p_vmac->vm_state == WIFINET_S_CONNECTED)
        return p2p_vmac;

    return drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];
}

int wifi_mac_send_addba_req(char* buf, int tid)
{
    char temp[3];
    int i = 0;
    struct wifi_mac_action_mgt_args actionargs;
    struct wifi_mac *wifimac = wifi_mac_get_mac_handle();
    struct wlan_net_vif *selected_wnet_vif = NULL;

    selected_wnet_vif = wifi_mac_get_action_vif(wifimac);
    if (selected_wnet_vif == NULL) {
        pr_warn("%s, no sta connected\n", __func__);
        return -1;
    }

    DPRINTF(AML_DEBUG_WARNING,"<running> %s %d tid_index = %d. \n", __func__,__LINE__, tid);

    actionargs.category = AML_CATEGORY_BACK;
    actionargs.action = WIFINET_ACTION_BA_ADDBA_REQUEST;
    actionargs.arg1 = tid;
    actionargs.arg2 = WME_MAX_BA;
    actionargs.arg3 = 0;

    if (selected_wnet_vif->vm_opmode == WIFINET_M_HOSTAP) {
        temp[2] = 0; /* end of string '\0' */

        for (i = 0 ; i < ETH_ALEN ; i++) {
            if (aml_char_is_hex_digit(buf[i * 3]) == false || aml_char_is_hex_digit(buf[i * 3 + 1]) == false) {
                pr_err("%s invalid 8-bit hex format for address offset:%u\n", __func__, i);
            }

            if (i < ETH_ALEN - 1 && buf[i * 3 + 2] != ':') {
                pr_err("%s invalid separator after address offset:%u\n", __func__, i);
            }

            temp[0] = buf[i * 3];
            temp[1] = buf[i * 3 + 1];
            if (sscanf(temp, "%hhx", &selected_wnet_vif->vm_mainsta->sta_macaddr[i]) != 1) {
                pr_err("%s sscanf fail for address offset:0x%03x\n", __func__, i);
            }
        }
    }

    wifi_mac_send_action(selected_wnet_vif->vm_mainsta, (void *)&actionargs);

    return 0;
}

int wifi_mac_send_coexist_mgmt(const char* buf)
{
    char type[16];
    char value[32];
    struct wifi_mac_action_mgt_args actionargs;
    struct wifi_mac *wifimac = wifi_mac_get_mac_handle();
    struct wlan_net_vif *selected_wnet_vif = NULL;
    int parsed;
    /* v16c (B19): sscanf "%s %s" without width specifiers — userspace can
     * trivially overflow type[16]/value[32] on stack by sending strings
     * longer than the buffers without spaces. Buffer overflow is
     * exploitable from any caller with CAP_NET_ADMIN (sysfs/iwpriv write).
     * Fix: bound widths to buffer size minus 1, and check parse result so
     * uninitialized stack data does not propagate into action frame. */
    type[0] = '\0';
    value[0] = '\0';
    parsed = sscanf(buf, "%15s %31s", type, value);
    if (parsed < 2) {
        pr_warn("%s: sscanf parsed %d/2 fields from buf; ignoring\n", __func__, parsed);
        return -1;
    }

    selected_wnet_vif = wifi_mac_get_action_vif(wifimac);
    if (selected_wnet_vif == NULL) {
        pr_warn("%s, no sta connected\n", __func__);
        return -1;
    }

    DPRINTF(AML_DEBUG_WARNING,"<running> %s %d type = %s, value=%s. \n", __func__,__LINE__, type, value);

    actionargs.category = AML_CATEGORY_PUBLIC;
    actionargs.action = WIFINET_ACT_PUBLIC_BSSCOEXIST;
    actionargs.arg1 = 0;
    actionargs.arg2 = 0;
    actionargs.arg3 = 0;
    wifi_mac_send_action(selected_wnet_vif->vm_mainsta, (void *)&actionargs);

    return 0;
}

int wifi_mac_send_wmm_ac_addts(char** buf)
{
    struct wifi_mac_action_mgt_args actionargs;
    struct wifi_mac *wifimac = wifi_mac_get_mac_handle();
    struct wlan_net_vif *selected_wnet_vif = NULL;

    selected_wnet_vif = wifi_mac_get_action_vif(wifimac);
    if (selected_wnet_vif == NULL) {
        pr_warn("%s, no sta connected\n", __func__);
        return -1;
    }

    selected_wnet_vif->vm_wmm_ac_params.direction = simple_strtoul(*buf, NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.tid = simple_strtoul(*(buf + 1), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.up = simple_strtoul(*(buf + 2), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.size = simple_strtoul(*(buf + 3), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.mean_data_rate = simple_strtoul(*(buf + 4), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.phyrate = simple_strtoul(*(buf + 5), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.sba = simple_strtoul(*(buf + 6), NULL, 0);
    selected_wnet_vif->vm_wmm_ac_params.dialog_token = simple_strtoul(*(buf + 7), NULL, 0);


    DPRINTF(AML_DEBUG_WARNING,"<running> %s %d mean_data_rate=%d, direction=%d, tid=%d, up=%d, size=%d, phyrate=%d, sba=%d, dialog_token=%d. \n", __func__,__LINE__, selected_wnet_vif->vm_wmm_ac_params.mean_data_rate,
                  selected_wnet_vif->vm_wmm_ac_params.direction, selected_wnet_vif->vm_wmm_ac_params.tid, selected_wnet_vif->vm_wmm_ac_params.up, selected_wnet_vif->vm_wmm_ac_params.size,
                  selected_wnet_vif->vm_wmm_ac_params.phyrate, selected_wnet_vif->vm_wmm_ac_params.sba, selected_wnet_vif->vm_wmm_ac_params.dialog_token);

    actionargs.category = AML_CATEGORY_WMM;
    actionargs.action = WIFINET_ACTION_ADDTS_REQ;
    actionargs.arg1 = 0;
    actionargs.arg2 = 0;
    actionargs.arg3 = 0;
    wifi_mac_send_action(selected_wnet_vif->vm_mainsta, (void *)&actionargs);

    return 0;
}

int wifi_mac_send_wmm_ac_delts(const char* buf)
{
    struct wifi_mac_action_mgt_args actionargs;
    struct wifi_mac *wifimac = wifi_mac_get_mac_handle();
    struct wlan_net_vif *selected_wnet_vif = NULL;
    int tid = 0;

    selected_wnet_vif = wifi_mac_get_action_vif(wifimac);
    if (selected_wnet_vif == NULL) {
        pr_warn("%s, no sta connected\n", __func__);
        return -1;
    }
    /* v16c (B19): pre-v16c did not check sscanf result. On parse failure
     * `tid` was left at its initializer (0) — silently using TID 0 instead
     * of telling the caller their input was bad. Now validate and propagate
     * the error rather than masking it. */
    if (sscanf(buf, "%d", &tid) != 1) {
        pr_warn("%s: cannot parse tid from buf\n", __func__);
        return -1;
    }
    if (tid < 0 || tid >= 8) {
        pr_warn("%s: tid %d out of range\n", __func__, tid);
        return -1;
    }
    DPRINTF(AML_DEBUG_WARNING,"<running> %s %d tid=%d. \n", __func__,__LINE__, tid);

    actionargs.category = AML_CATEGORY_WMM;
    actionargs.action = WIFINET_ACTION_DELTS;
    actionargs.arg1 = tid;
    actionargs.arg2 = 0;
    actionargs.arg3 = 0;
    wifi_mac_send_action(selected_wnet_vif->vm_mainsta, (void *)&actionargs);

    return 0;
}

