
#include "wifi_mac_com.h"
#include "wifi_drv_xmit.h"

static int w522a_rx_debug = 0;
module_param_named(rxdebug, w522a_rx_debug, int, 0644);
MODULE_PARM_DESC(rxdebug, "W522A RX reorder debug trace (0=off, 1=key events, 2=verbose)");

#define W522A_RX_DESYNC_RUN 1

static void drv_rx_flush_tid(struct drv_private *drv_priv, struct drv_rx_scoreboard *RxTidState, int drop);

static unsigned long drv_rx_reorder_timeout(struct drv_private *drv_priv,
    struct aml_driver_nsta *drv_sta)
{
    unsigned char timeout = DRV_RX_TIMEOUT;
    struct wifi_station *sta;

    if (drv_priv == NULL)
        return clamp_t(unsigned long, timeout, 1, 200);

    timeout = drv_priv->drv_config.cfg_rx_reorder_timeout;

    if (drv_sta != NULL) {
        sta = (struct wifi_station *)smp_load_acquire(&drv_sta->net_nsta);
        if (sta != NULL && sta->sta_wnet_vif != NULL &&
            sta->sta_wnet_vif->vm_opmode == WIFINET_M_HOSTAP)
            timeout = drv_priv->drv_config.cfg_ap_rx_reorder_timeout;
    }

    if (timeout == 0)
        timeout = DRV_RX_TIMEOUT;

    return clamp_t(unsigned long, timeout, 1, 200);
}

static int drv_rx_ring_pending(struct drv_rx_scoreboard *RxTidState)
{
    return (RxTidState->baw_tail - RxTidState->baw_head) &
        (DRV_TID_MAX_BUFS - 1);
}

static int drv_rxaggr_enabled_for_sta(struct drv_private *drv_priv,
    struct aml_driver_nsta *drv_sta)
{
    struct wifi_station *sta;

    if (drv_priv == NULL)
        return 0;

    if (drv_sta == NULL)
        return drv_priv->drv_config.cfg_rxaggr;

    sta = (struct wifi_station *)smp_load_acquire(&drv_sta->net_nsta);
    if (sta != NULL && sta->sta_wnet_vif != NULL &&
        sta->sta_wnet_vif->vm_opmode == WIFINET_M_HOSTAP)
        return drv_priv->drv_config.cfg_ap_rxaggr;

    if (sta != NULL && sta->sta_wnet_vif != NULL &&
        sta->sta_wnet_vif->vm_opmode == WIFINET_M_MONITOR)
        return drv_priv->drv_config.cfg_monitor_rxaggr;

    return drv_priv->drv_config.cfg_rxaggr;
}

int drv_rx_init( struct drv_private *drv_priv, int nbufs)
{
    return 0;
}

void
drv_set_addba_rsp( struct drv_private *drv_priv, void * nsta,
                       unsigned char tid_index, unsigned short statuscode)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);
    struct drv_rx_scoreboard *RxTidState;

    if (drv_sta == NULL || tid_index >= WME_NUM_TID)
        return;
    RxTidState = &drv_sta->rx_scb[tid_index];

    RxTidState->userstatuscode = statuscode;
    DPRINTF(AML_DEBUG_ADDBA,"<running> %s %d tid_index %d RxTidState->userstatuscode  %d,drv_sta %p \n",
        __func__,__LINE__,tid_index,RxTidState->userstatuscode,drv_sta );
}

void
drv_clr_addba_rsp_status( struct drv_private *drv_priv, void * nsta)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);
    struct drv_rx_scoreboard *RxTidState;
    int i;

    for (i = 0; i < ARRAY_LENGTH(drv_sta->rx_scb); i++)
    {
        RxTidState = &drv_sta->rx_scb[i];
        RxTidState->userstatuscode = WIFINET_STATUS_SUCCESS;
    }
}

int drv_rx_addbareq(struct drv_private *drv_priv, void *nsta, unsigned char dialogtoken,
    struct wifi_mac_ba_parameterset *baparamset, unsigned short batimeout, struct wifi_mac_ba_seqctrl basequencectrl)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);
    unsigned short tid_index;
    struct drv_rx_scoreboard *RxTidState;
    unsigned short rx_ba_window;
    int rxaggr_enabled;

    if (drv_priv == NULL || drv_sta == NULL || baparamset == NULL) {
        printk_ratelimited(KERN_WARNING
            "W522A: drv_rx_addbareq REFUSED null input drv=%p sta=%p ba=%p\n",
            drv_priv, drv_sta, baparamset);
        return 0;
    }

    tid_index = baparamset->tid;
    if (tid_index >= WME_NUM_TID) {
        printk_ratelimited(KERN_WARNING
            "W522A: drv_rx_addbareq REFUSED bad tid=%u (max=%u)\n",
            tid_index, WME_NUM_TID - 1);
        return 0;
    }
    RxTidState = &drv_sta->rx_scb[tid_index];
    
    {
        struct wifi_station *_sta;
        unsigned short raw_baw = drv_priv->drv_config.cfg_rx_ba_window;
        if (drv_sta != NULL) {
            _sta = (struct wifi_station *)smp_load_acquire(&drv_sta->net_nsta);
            if (_sta != NULL && _sta->sta_wnet_vif != NULL &&
                _sta->sta_wnet_vif->vm_opmode == WIFINET_M_HOSTAP)
                raw_baw = drv_priv->drv_config.cfg_ap_rx_ba_window;
        }
        rx_ba_window = clamp_t(unsigned short, raw_baw, 1, WME_MAX_BA);
    }
    rxaggr_enabled = drv_rxaggr_enabled_for_sta(drv_priv, drv_sta);

    {
        static unsigned long _addba_log_cnt;
        if ((_addba_log_cnt++ & 0x3f) == 0) {
            printk(KERN_INFO "W522A: drv_rx_addbareq cnt=%lu tid=%u "
                   "bufsz=%u policy=%u role_rxaggr=%u sta_rx=%u ap_rx=%u mon_rx=%u dialog=%u\n",
                   _addba_log_cnt, tid_index,
                   baparamset->buffersize, baparamset->bapolicy,
                   rxaggr_enabled, drv_priv->drv_config.cfg_rxaggr,
                   drv_priv->drv_config.cfg_ap_rxaggr,
                   drv_priv->drv_config.cfg_monitor_rxaggr, dialogtoken);
        }
    }

    DRV_RXTID_LOCK_BH(RxTidState);

    if (!rxaggr_enabled)
    {
        static unsigned long _rxaggr_off_log_cnt;
        DPRINTF(AML_DEBUG_ADDBA, "<running> %s %d \n",__func__,__LINE__);
        if ((_rxaggr_off_log_cnt++ & 0x3f) == 0)
            printk(KERN_INFO "W522A: drv_rx_addbareq REFUSED "
                   "tid=%u (role_rxaggr=0 cnt=%lu)\n",
                   tid_index, _rxaggr_off_log_cnt);
        RxTidState->statuscode = WIFINET_STATUS_REFUSED;
    }
    else if (RxTidState->userstatuscode != WIFINET_STATUS_SUCCESS)
    {
        DPRINTF(AML_DEBUG_ADDBA,"<running> %s %d tid_index %d RxTidState->userstatuscode  %d \n",
            __func__,__LINE__,tid_index,RxTidState->userstatuscode );
        RxTidState->statuscode = RxTidState->userstatuscode;
    }
    else
    {
        if (RxTidState->rx_addba_exchangecomplete && RxTidState->pRxDesc != NULL)
        {
            os_timer_ex_cancel(&RxTidState->timer, CANCEL_NO_SLEEP);
            drv_rx_flush_tid(drv_priv, RxTidState, 0);
            RxTidState->rx_addba_exchangecomplete = 0;
        }

        if (baparamset->buffersize) {
            RxTidState->baw_size = clamp_t(unsigned short,
                baparamset->buffersize, 1, rx_ba_window);
        } else {
            RxTidState->baw_size = rx_ba_window;
        }

        RxTidState->seq_next = basequencectrl.startseqnum;
        RxTidState->baw_head = 0;
        RxTidState->baw_tail = 0;
        RxTidState->statuscode               = WIFINET_STATUS_SUCCESS;
        RxTidState->baparamset.bapolicy      = WIFINET_BA_POLICY_IMMEDIATE;
        RxTidState->baparamset.buffersize    = RxTidState->baw_size;
        RxTidState->batimeout                = 0;

        if (RxTidState->pRxDesc == NULL)
        {
            RxTidState->pRxDesc = (struct drv_rxdesc *)NET_MALLOC(
                                  DRV_TID_MAX_BUFS * sizeof(struct drv_rxdesc),
                                  GFP_ATOMIC, "drv_rx_addbareq.RxTidState->pRxDesc");
            if (RxTidState->pRxDesc != NULL)
                memset(RxTidState->pRxDesc, 0, DRV_TID_MAX_BUFS * sizeof(struct drv_rxdesc));
            DPRINTF(AML_DEBUG_ADDBA,"<running> %s %d RxTidState->pRxDesc= %p\n", __func__, __LINE__, RxTidState->pRxDesc);
        }
        else
        {
            memset(RxTidState->pRxDesc, 0, DRV_TID_MAX_BUFS * sizeof(struct drv_rxdesc));
        }

        if (RxTidState->pRxDesc == NULL)
        {
            RxTidState->statuscode = WIFINET_STATUS_REFUSED;
        }
        else
        {
            DPRINTF(AML_DEBUG_ADDBA,"<running> %s %d \n",__func__,__LINE__);
            RxTidState->rx_addba_exchangecomplete = 1;
            RxTidState->rx_old_run = 0;
            printk_ratelimited(KERN_INFO
                "W522A: drv_rx_addbareq ACCEPT tid=%u win=%u start=%u dialog=%u\n",
                tid_index, RxTidState->baw_size,
                basequencectrl.startseqnum, dialogtoken);
        }
    }

    RxTidState->dialogtoken              = dialogtoken;
    RxTidState->baparamset.amsdusupported = 0;
    RxTidState->baparamset.tid           = tid_index;

    DRV_RXTID_UNLOCK_BH(RxTidState);
    return 0;
}

void drv_rx_addbarsp(struct drv_private *drv_priv, void * nsta, unsigned char tid_index)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);
    
    void *nsta_safe;
    struct wifi_station *sta;
    struct drv_rx_scoreboard *RxTidState;

    if (drv_sta == NULL || tid_index >= WME_NUM_TID)
        return;
    nsta_safe = (void *)smp_load_acquire(&drv_sta->net_nsta);
    RxTidState = &drv_sta->rx_scb[tid_index];

    if (nsta_safe == NULL) {
        DPRINTF(AML_DEBUG_ADDBA, "%s: sta torn down, skip\n", __func__);
        return;
    }
    sta = (struct wifi_station *)nsta_safe;

    if ( RxTidState->statuscode == WIFINET_STATUS_SUCCESS)
    {
        hal_phy_addba_ok(
                        sta->sta_wnet_vif->wnet_vif_id,
                        drv_hal_nsta_staid((struct wifi_station *)nsta_safe),
                        tid_index,
                        drv_sta->rx_scb[tid_index].seq_next,
                        drv_sta->rx_scb[tid_index].baw_size,
                        BA_RESPONDER,
                        BA_IMMIDIATE);
    }
    else
    {
        DPRINTF(AML_DEBUG_ADDBA,"%s %d fail,RxTidState->statuscode  %d tid_index %d,drv_sta %p\n",
            __func__,__LINE__,RxTidState->statuscode,tid_index,drv_sta );
    }
}

void drv_rx_delba(
    struct drv_private *drv_priv, void * nsta,
    struct wifi_mac_delba_parameterset *delbaparamset,
    unsigned short reasoncode
)
{
    struct aml_driver_nsta *drv_sta = DRIVER_NODE(nsta);
    
    void *nsta_safe;
    struct wifi_station *sta;
    unsigned short tid_index;

    if (drv_sta == NULL || delbaparamset == NULL)
        return;
    tid_index = delbaparamset->tid;
    
    if (tid_index >= WME_NUM_TID) {
        printk_ratelimited(KERN_WARNING
            "W522A: drv_rx_delba ignored bad tid=%u (max=%u)\n",
            tid_index, WME_NUM_TID - 1);
        return;
    }
    nsta_safe = (void *)smp_load_acquire(&drv_sta->net_nsta);

    if (nsta_safe == NULL) {
        DPRINTF(AML_DEBUG_ADDBA, "%s: sta torn down, skip\n", __func__);
        return;
    }
    sta = (struct wifi_station *)nsta_safe;

    hal_phy_delt_ba_ok(
                    sta->sta_wnet_vif->wnet_vif_id,
                    drv_hal_nsta_staid((struct wifi_station *)nsta_safe),
                    tid_index,
                    delbaparamset->initiator?BA_RESPONDER:BA_INITIATOR);
    if (delbaparamset->initiator)
        drv_rxampdu_del(drv_priv, drv_sta, tid_index);
    else
        drv_txampdu_del(drv_priv, drv_sta, tid_index);
}

static int
drv_rx_bar(struct drv_private *drv_priv, struct aml_driver_nsta *drv_sta,  struct sk_buff *skbbuf)
{
    struct wifi_mac_frame_bar *bar;
    int tid_index;
    unsigned short seqnum;
    struct drv_rx_scoreboard *RxTidState;
    int index, desc_id;
    struct sk_buff *twbuf;
    struct wifi_mac_rx_status* rs;
    
    void *nsta_safe = (void *)smp_load_acquire(&drv_sta->net_nsta);

    if (nsta_safe == NULL) {
        os_skb_free(skbbuf);
        return WIFINET_FC0_TYPE_CTL;
    }

    drv_priv->drv_stats.rx_bar_cnt++;
    bar = (struct wifi_mac_frame_bar *)  os_skb_data(skbbuf);
    tid_index = (bar->i_ctl & WIFINET_BAR_CTL_TID_M) >> WIFINET_BAR_CTL_TID_S;
    
    if (tid_index >= WME_NUM_TID) {
        drv_priv->drv_stats.rx_bardrop_cnt++;
        os_skb_free(skbbuf);
        return WIFINET_FC0_TYPE_CTL;
    }
    seqnum = le16toh(bar->i_seq) >> WIFINET_SEQ_SEQ_SHIFT;
    RxTidState = &drv_sta->rx_scb[tid_index];
    DRV_RXTID_LOCK_BH(RxTidState);
    if (!RxTidState->rx_addba_exchangecomplete || RxTidState->pRxDesc == NULL ||
        RxTidState->baw_size == 0)
    {
        if (w522a_rx_debug)
            printk_ratelimited(KERN_INFO
                "W522A: rxdbg BAR drop-no-ba tid=%d seq=%u complete=%d desc=%p baw=%u head=%d tail=%d next=%u\n",
                tid_index, seqnum, RxTidState->rx_addba_exchangecomplete,
                RxTidState->pRxDesc, RxTidState->baw_size,
                RxTidState->baw_head, RxTidState->baw_tail,
                RxTidState->seq_next);
        DRV_RXTID_UNLOCK_BH(RxTidState);
        drv_priv->drv_stats.rx_bardrop_cnt++;
        os_skb_free(skbbuf);
        return WIFINET_FC0_TYPE_CTL;
    }

    index = DRV_BA_INDEX(RxTidState->seq_next, seqnum);
    if (w522a_rx_debug)
        printk_ratelimited(KERN_INFO
            "W522A: rxdbg BAR tid=%d seq=%u idx=%d next=%u head=%d tail=%d pending=%d baw=%u\n",
            tid_index, seqnum, index, RxTidState->seq_next,
            RxTidState->baw_head, RxTidState->baw_tail,
            drv_rx_ring_pending(RxTidState), RxTidState->baw_size);

    if ((index > RxTidState->baw_size) &&
        (index > (WIFINET_SEQ_MAX - (RxTidState->baw_size << 2))))
    {
        if (w522a_rx_debug)
            printk_ratelimited(KERN_INFO
                "W522A: rxdbg BAR drop-old tid=%d seq=%u idx=%d next=%u head=%d tail=%d pending=%d baw=%u\n",
                tid_index, seqnum, index, RxTidState->seq_next,
                RxTidState->baw_head, RxTidState->baw_tail,
                drv_rx_ring_pending(RxTidState), RxTidState->baw_size);
        DRV_RXTID_UNLOCK_BH(RxTidState);
        drv_priv->drv_stats.rx_bardrop_cnt++;
        os_skb_free(skbbuf);
        return WIFINET_FC0_TYPE_CTL;
    }

    desc_id = (RxTidState->baw_head + index) & (DRV_TID_MAX_BUFS - 1);
    while ((RxTidState->baw_head != RxTidState->baw_tail) &&
           (RxTidState->baw_head != desc_id))
    {
        twbuf = RxTidState->pRxDesc[RxTidState->baw_head].rx_wbuf;
        rs = &RxTidState->pRxDesc[RxTidState->baw_head].rs;
        RxTidState->pRxDesc[RxTidState->baw_head].rx_wbuf = NULL;

        if (twbuf != NULL)
        {
            drv_priv->net_ops->wifi_mac_input(nsta_safe, twbuf, rs);
        }

        CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
    }

    while (RxTidState->baw_head != RxTidState->baw_tail &&
           RxTidState->pRxDesc[RxTidState->baw_head].rx_wbuf != NULL)
    {
        twbuf = RxTidState->pRxDesc[RxTidState->baw_head].rx_wbuf;
        rs = &RxTidState->pRxDesc[RxTidState->baw_head].rs;
        RxTidState->pRxDesc[RxTidState->baw_head].rx_wbuf = NULL;

        drv_priv->net_ops->wifi_mac_input(nsta_safe, twbuf, rs);

        CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
    }
    DRV_RXTID_UNLOCK_BH(RxTidState);
    os_skb_free(skbbuf);
    return WIFINET_FC0_TYPE_CTL;
}

int drv_rx_input( struct drv_private *drv_priv, void *nsta,
    struct sk_buff *skbbuf, struct wifi_mac_rx_status* rs)
{
    struct wifi_frame *wh;
    struct wifi_qos_frame *whqos;
    struct WIFINET_S_FRAME_QOS_ADDR4 *whqos_4addr;
    struct drv_rx_scoreboard *RxTidState;
    struct drv_rxdesc *pRxDesc;
    unsigned char type, subtype;
    int b_mcast, tid, index, desc_id, rxdiff, is4addr;
    unsigned short rxseq;
    struct aml_driver_nsta *drv_sta = (struct aml_driver_nsta *)nsta;
    
    void *nsta_safe = (void *)smp_load_acquire(&drv_sta->net_nsta);

    if (nsta_safe == NULL) {
        os_skb_free(skbbuf);
        return -1;
    }

    wh = (struct wifi_frame *)os_skb_data(skbbuf);
    is4addr = (wh->i_fc[1] & WIFINET_FC1_DIR_MASK) == WIFINET_FC1_DIR_DSTODS;
    drv_priv->drv_stats.rx_ampdu_cnt++;
    if (w522a_rx_debug && ((drv_priv->drv_stats.rx_ampdu_cnt &
        (w522a_rx_debug > 1 ? 0x3ff : 0xfff)) == 0)) {
        printk_ratelimited(KERN_INFO
            "W522A: rx-stats ampdu=%u ok=%u nonqos=%u dup=%u bar=%u bardrop=%u skipped=%u\n",
            drv_priv->drv_stats.rx_ampdu_cnt,
            drv_priv->drv_stats.rx_ok_cnt,
            drv_priv->drv_stats.rx_nonqos_cnt,
            drv_priv->drv_stats.rx_dup_cnt,
            drv_priv->drv_stats.rx_bar_cnt,
            drv_priv->drv_stats.rx_bardrop_cnt,
            drv_priv->drv_stats.rx_skipped_cnt);
    }

    if ((wh->i_fc[0] & WIFINET_FC0_VERSION_MASK) != WIFINET_FC0_VERSION_0)
    {
        os_skb_free(skbbuf);
        return -1;
    }

    type = wh->i_fc[0] & WIFINET_FC0_TYPE_MASK;
    subtype = wh->i_fc[0] & WIFINET_FC0_SUBTYPE_MASK;
    b_mcast = WIFINET_IS_MULTICAST(wh->i_addr1);

    wifi_mac_scan_chking_leakap(nsta_safe, wh);

    if (WIFINET_IS_BAR(wh))
    {
        return drv_rx_bar(drv_priv, drv_sta, skbbuf);
    }

    if (type != WIFINET_FC0_TYPE_DATA ||
        subtype != WIFINET_FC0_SUBTYPE_QOS || (b_mcast))
    {
        drv_priv->drv_stats.rx_nonqos_cnt++;
        
        return drv_priv->net_ops->wifi_mac_input(nsta_safe, skbbuf, rs);
    }

    if (is4addr)
    {
        whqos_4addr = (struct WIFINET_S_FRAME_QOS_ADDR4 *) wh;
        tid = whqos_4addr->i_qos[0] & WIFINET_QOS_TID;
    }
    else
    {
        whqos = (struct wifi_qos_frame *) wh;
        tid = whqos->i_qos[0] & WIFINET_QOS_TID;
    }
    
    if (tid >= WME_NUM_TID) {
        drv_priv->drv_stats.rx_nonqos_cnt++;
        return drv_priv->net_ops->wifi_mac_input(nsta_safe, skbbuf, rs);
    }
    RxTidState = &drv_sta->rx_scb[tid];

    DRV_RXTID_LOCK_BH(RxTidState);
    if (!RxTidState->rx_addba_exchangecomplete)
    {
        DRV_RXTID_UNLOCK_BH(RxTidState);
        drv_priv->drv_stats.rx_nonqos_cnt++;
        return drv_priv->net_ops->wifi_mac_input(nsta_safe, skbbuf, rs);
    }

    rxseq = le16toh(*(unsigned short *)wh->i_seq) >> WIFINET_SEQ_SEQ_SHIFT;
    if (RxTidState->seq_reset)
    {
        RxTidState->seq_reset = 0;
        RxTidState->seq_next = rxseq;
    }

    index = DRV_BA_INDEX(RxTidState->seq_next, rxseq);

    if (index > (WIFINET_SEQ_MAX - (RxTidState->baw_size << 2)))
    {
        
        if (RxTidState->baw_head == RxTidState->baw_tail &&
            ++RxTidState->rx_old_run >= W522A_RX_DESYNC_RUN)
        {
            if (w522a_rx_debug)
                printk_ratelimited(KERN_INFO
                    "W522A: rx-desync-resync tid=%d rxseq=%u old_next=%u run=%d\n",
                    tid, rxseq, RxTidState->seq_next, RxTidState->rx_old_run);
            RxTidState->rx_old_run = 0;
            RxTidState->seq_next = rxseq;
            index = 0;
        }
        else
        {
            if (w522a_rx_debug)
                printk_ratelimited(KERN_INFO
                    "W522A: rxdbg drop-old tid=%d rxseq=%u idx=%d next=%u run=%d head=%d tail=%d pending=%d baw=%u\n",
                    tid, rxseq, index, RxTidState->seq_next, RxTidState->rx_old_run,
                    RxTidState->baw_head, RxTidState->baw_tail,
                    drv_rx_ring_pending(RxTidState), RxTidState->baw_size);
            DRV_RXTID_UNLOCK_BH(RxTidState);
            os_skb_free(skbbuf);
            return WIFINET_FC0_TYPE_DATA;
        }
    }
    
    RxTidState->rx_old_run = 0;

    if (index >= RxTidState->baw_size)
    {
        while (index >= RxTidState->baw_size)
        {
            pRxDesc = RxTidState->pRxDesc + RxTidState->baw_head;
            if (pRxDesc->rx_wbuf != NULL)
            {
                drv_priv->net_ops->wifi_mac_input(nsta_safe, pRxDesc->rx_wbuf,
                                                 &pRxDesc->rs);
                drv_priv->drv_stats.rx_ok_cnt++;

                pRxDesc->rx_wbuf = NULL;
            }
            CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
            CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
            index --;
        }
    }

    desc_id = (RxTidState->baw_head + index) & (DRV_TID_MAX_BUFS - 1);
    pRxDesc = RxTidState->pRxDesc + desc_id;

    if (pRxDesc->rx_wbuf != NULL)
    {
        if (w522a_rx_debug)
            printk_ratelimited(KERN_INFO
                "W522A: rxdbg dup tid=%d rxseq=%u idx=%d desc=%d next=%u head=%d tail=%d pending=%d baw=%u\n",
                tid, rxseq, index, desc_id, RxTidState->seq_next,
                RxTidState->baw_head, RxTidState->baw_tail,
                drv_rx_ring_pending(RxTidState), RxTidState->baw_size);
        DRV_RXTID_UNLOCK_BH(RxTidState);
        drv_priv->drv_stats.rx_dup_cnt++;
        
        os_skb_free(skbbuf);
        return WIFINET_FC0_TYPE_DATA;
    }

    rxdiff = (RxTidState->baw_tail - RxTidState->baw_head) & (DRV_TID_MAX_BUFS - 1);
    pRxDesc->rx_wbuf = skbbuf;
    pRxDesc->rx_time = OS_GET_TIMESTAMP();
    pRxDesc->rs= *rs;

    if (index >= rxdiff)
    {
        RxTidState->baw_tail = desc_id;
        CIRCLE_Add_One(RxTidState->baw_tail, DRV_TID_MAX_BUFS);
    }

    if (w522a_rx_debug &&
        (index >= 8 || drv_rx_ring_pending(RxTidState) >= (RxTidState->baw_size >> 1)))
    {
        printk_ratelimited(KERN_INFO
            "W522A: rxdbg queue tid=%d rxseq=%u idx=%d desc=%d rxdiff=%d next=%u head=%d tail=%d pending=%d baw=%u\n",
            tid, rxseq, index, desc_id, rxdiff, RxTidState->seq_next,
            RxTidState->baw_head, RxTidState->baw_tail,
            drv_rx_ring_pending(RxTidState), RxTidState->baw_size);
    }

    while (RxTidState->baw_head != RxTidState->baw_tail)
    {
        pRxDesc = RxTidState->pRxDesc + RxTidState->baw_head;
        if (!pRxDesc->rx_wbuf)
        {
            break;
        }
        drv_priv->drv_stats.rx_ok_cnt++;
        drv_priv->net_ops->wifi_mac_input(nsta_safe, pRxDesc->rx_wbuf, &pRxDesc->rs);
        pRxDesc->rx_wbuf = NULL;

        CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
    }

    if (RxTidState->baw_head != RxTidState->baw_tail) {
        
        if (!os_timer_ex_active(&RxTidState->timer)) {
            os_set_timer_period(&RxTidState->timer,
                clamp_t(unsigned long, drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta), 1, 200));
            os_timer_ex_start(&RxTidState->timer);
        }
    } else
        os_timer_ex_cancel(&RxTidState->timer, CANCEL_NO_SLEEP);
    DRV_RXTID_UNLOCK_BH(RxTidState);
    return WIFINET_FC0_TYPE_DATA;
}

static void drv_rx_sort_timer_ex(SYS_TYPE param1, SYS_TYPE param2,
                                 SYS_TYPE param3, SYS_TYPE param4,
                                 SYS_TYPE param5)
{
    struct drv_rx_scoreboard *RxTidState = (struct drv_rx_scoreboard *) param1;
    struct aml_driver_nsta *drv_sta = RxTidState->drv_sta;
    struct drv_private *drv_priv = drv_sta->sta_drv_priv;
    int rearm_uses_custom_period = 0;
    struct drv_rxdesc *pRxDesc;
    int count = 0;
    unsigned long diff;
    int bawhead;
    
    void *nsta_safe = (void *)smp_load_acquire(&drv_sta->net_nsta);

    if (nsta_safe == NULL) {
        
        DRV_RXTID_LOCK_BH(RxTidState);
        drv_rx_flush_tid(drv_priv, RxTidState, 1);
        DRV_RXTID_UNLOCK_BH(RxTidState);
        return;
    }

    DRV_RXTID_LOCK_BH(RxTidState);
    bawhead = RxTidState->baw_head;
    while (bawhead != RxTidState->baw_tail)
    {
        pRxDesc = RxTidState->pRxDesc + bawhead;
        if (!pRxDesc->rx_wbuf)
        {
            
            count++;
            CIRCLE_Add_One(bawhead, DRV_TID_MAX_BUFS);
            CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
            CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
            continue;
        }
        
        diff = (long)OS_GET_TIMESTAMP() - (long)pRxDesc->rx_time;
        if (diff < drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta))
        {
            if (w522a_rx_debug)
                printk_ratelimited(KERN_INFO
                    "W522A: rxdbg timer-wait count=%d diff=%lu timeout=%lu next=%u head=%d scan=%d tail=%d pending=%d\n",
                    count, diff, drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta),
                    RxTidState->seq_next, RxTidState->baw_head, bawhead,
                    RxTidState->baw_tail, drv_rx_ring_pending(RxTidState));
            os_set_timer_period(&RxTidState->timer,
                drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta) - diff);
            rearm_uses_custom_period = 1;
            break;
        }

        drv_priv->drv_stats.rx_ok_cnt++;
        drv_priv->net_ops->wifi_mac_input(nsta_safe, pRxDesc->rx_wbuf, &pRxDesc->rs);
        pRxDesc->rx_wbuf = NULL;

        CIRCLE_Add_One(bawhead, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
        if (w522a_rx_debug)
            printk_ratelimited(KERN_INFO
                "W522A: rxdbg timer-release count=%d diff=%lu next=%u head=%d tail=%d pending=%d\n",
                count + 1, diff, RxTidState->seq_next, RxTidState->baw_head,
                RxTidState->baw_tail, drv_rx_ring_pending(RxTidState));
        count = 0;
    }

    if (RxTidState->baw_head != RxTidState->baw_tail) {
        if (!rearm_uses_custom_period)
            os_set_timer_period(&RxTidState->timer,
                drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta));
        os_timer_ex_start(&RxTidState->timer);
    }
    DRV_RXTID_UNLOCK_BH(RxTidState);
}

static int drv_rx_sort_timer(void *context)
{
    drv_hal_add_workitem((WorkHandler)drv_rx_sort_timer_ex, NULL,
                         (SYS_TYPE)context, (SYS_TYPE)0, (SYS_TYPE)0,
                         (SYS_TYPE)0,(SYS_TYPE)0);

    return OS_TIMER_NOT_REARMED;
}

static void
drv_rx_flush_tid(struct drv_private *drv_priv, struct drv_rx_scoreboard *RxTidState, int drop)
{
    struct drv_rxdesc *pRxDesc;

    while (RxTidState->baw_head != RxTidState->baw_tail)
    {
        pRxDesc = RxTidState->pRxDesc + RxTidState->baw_head;
        if (!pRxDesc->rx_wbuf)
        {
            CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
            CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
            drv_priv->drv_stats.rx_skipped_cnt++;

            continue;
        }

        drv_priv->drv_stats.rx_ok_cnt++;

        if (drop)
        {
            os_skb_free(pRxDesc->rx_wbuf);
        }
        else
        {
            
            void *nsta_safe = (void *)smp_load_acquire(&RxTidState->drv_sta->net_nsta);
            if (nsta_safe != NULL) {
                drv_priv->net_ops->wifi_mac_input(nsta_safe, pRxDesc->rx_wbuf,
                                                 &pRxDesc->rs);
            } else {
                os_skb_free(pRxDesc->rx_wbuf);
            }
        }
        pRxDesc->rx_wbuf = NULL;

        CIRCLE_Add_One(RxTidState->baw_head, DRV_TID_MAX_BUFS);
        CIRCLE_Add_One(RxTidState->seq_next, WIFINET_SEQ_MAX);
    }
}

void
drv_rxampdu_del(struct drv_private *drv_priv, struct aml_driver_nsta *drv_sta, unsigned char tid_index)
{
    struct drv_rx_scoreboard *RxTidState = &drv_sta->rx_scb[tid_index];
    unsigned long lockflags;

    DRV_RXTID_LOCK_IRQ(RxTidState,lockflags);
    if (!RxTidState->rx_addba_exchangecomplete)
    {
        DRV_RXTID_UNLOCK_IRQ(RxTidState,lockflags);
        return;
    }

    os_timer_ex_cancel(&RxTidState->timer, CANCEL_NO_SLEEP);
    drv_rx_flush_tid(drv_priv, RxTidState, 0);
    RxTidState->rx_addba_exchangecomplete = 0;

    if (RxTidState->pRxDesc != 0)
    {
        NET_FREE(RxTidState->pRxDesc, "drv_rx_addbareq.RxTidState->pRxDesc");
        RxTidState->pRxDesc = NULL;
    }
    DRV_RXTID_UNLOCK_IRQ(RxTidState,lockflags);
}

void
drv_rx_nsta_init(struct drv_private *drv_priv, struct aml_driver_nsta *drv_sta)
{
    if (drv_priv->drv_config.cfg_rxaggr || drv_priv->drv_config.cfg_ap_rxaggr)
    {
        struct drv_rx_scoreboard *RxTidState;
        int tid_index;

        for (tid_index = 0, RxTidState = &drv_sta->rx_scb[tid_index]; tid_index < WME_NUM_TID; tid_index++, RxTidState++)
        {
            RxTidState->drv_sta = drv_sta;
            RxTidState->seq_reset = 1;
            RxTidState->seq_next = 0;
            RxTidState->baw_size = WME_MAX_BA;
            RxTidState->baw_head = RxTidState->baw_tail = 0;
            RxTidState->rx_old_run = 0;
            RxTidState->pRxDesc = NULL;

            os_timer_ex_initialize(&RxTidState->timer,
                drv_rx_reorder_timeout(drv_priv, RxTidState->drv_sta), drv_rx_sort_timer, RxTidState);

            DRV_RXTID_LOCK_INIT(RxTidState);

            RxTidState->rx_addba_exchangecomplete = 0;
            RxTidState->userstatuscode = WIFINET_STATUS_SUCCESS;
        }
    }
}

void drv_rx_nsta_clean(struct drv_private *drv_priv, struct aml_driver_nsta *drv_sta)
{
    if (drv_priv->drv_config.cfg_rxaggr || drv_priv->drv_config.cfg_ap_rxaggr)
    {
        struct drv_rx_scoreboard *RxTidState;
        int tid_index,i;
        unsigned long lockflags;

        for (tid_index = 0, RxTidState = &drv_sta->rx_scb[tid_index]; tid_index < WME_NUM_TID;
             tid_index++, RxTidState++)
        {

            DRV_RXTID_LOCK_IRQ(RxTidState,lockflags);

            if (!RxTidState->rx_addba_exchangecomplete)
            {
                DRV_RXTID_UNLOCK_IRQ(RxTidState,lockflags);
                continue;
            }
            os_timer_ex_del(&RxTidState->timer, CANCEL_NO_SLEEP);

            drv_rx_flush_tid(drv_priv, RxTidState, 1);

            for (i = 0; i < DRV_TID_MAX_BUFS; i++)
            {
                ASSERT(RxTidState->pRxDesc[i].rx_wbuf == NULL);
            }

            if (RxTidState->pRxDesc)
            {
                NET_FREE(RxTidState->pRxDesc, "drv_rx_addbareq.RxTidState->pRxDesc");
                RxTidState->pRxDesc = NULL;
            }
            RxTidState->rx_addba_exchangecomplete = 0;

            DRV_RXTID_UNLOCK_IRQ(RxTidState,lockflags);
            DRV_RXTID_LOCK_DESTROY(RxTidState);
        }
    }
}

void drv_rx_nsta_free(struct drv_private *drv_priv, struct aml_driver_nsta *drv_sta)
{
    drv_rx_nsta_clean(drv_priv,drv_sta);
}
