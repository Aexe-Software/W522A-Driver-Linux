#ifndef _WIFI_PKT_DESC_H_
#define _WIFI_PKT_DESC_H_

#include <linux/list.h>
#include "wifi_drv_config.h"
#include "wifi_mac_var.h"
#include "wifi_debug.h"
#include "wifi_mac_sta.h"
#include "wifi_skbbuf.h"

#define DRV_TXDESC_RATE_NUM 4

#if defined (SYSTEM32)
    #define DRV_TXDESC_NUM 1664
#else
    #define DRV_TXDESC_NUM 1280
#endif

enum tx_frame_flag
{
    TX_OTHER_FRAME = 0,
    TX_P2P_GO_NEGO_CONF = 1,
    TX_P2P_OTHER_GO_NEGO_FRAME = 2,
    TX_P2P_PRESENCE_REQ = 3,
#ifdef CTS_VERIFIER_GAS
    TX_P2P_GAS = 4,
#endif
    TX_MGMT_ADDBA_RSP = 5,
    TX_MGMT_PROBE_REQ = 6,
    TX_MGMT_AUTH = 7,
    TX_MGMT_ASSOC_REQ = 8,
    TX_MGMT_EAPOL = 9,
    TX_MGMT_DHCP = 10,
    TX_MGMT_DEAUTH = 11,
    TX_MGMT_DISASSOC = 12,
    TX_MGMT_CSA_ACTION = 14,
};

struct wifi_mac_pkt_info
{
    unsigned short b_tcp:1,
        b_tcp_ack:1,
        b_tcp_ack_del:1,
        b_tcp_push:1,
        b_eap:1,
        b_dhcp:1,
        b_arp:1,
        b_tcp_saved_flag:1,
        b_tcp_free:1,
        b_icmp:1,
        b_latency:1;

    unsigned char eat_count;
    unsigned short tcp_src_port;
    unsigned short tcp_dst_port;
    unsigned int tcp_seqnum;
    unsigned int tcp_ack_seqnum;
};

struct wifi_drv_pkt_info
{
    unsigned char pkt_info_count;
    struct wifi_mac_pkt_info pkt_info[DEFAULT_TXAMSDU_SUB_MAX];
};

struct drv_txdesc
{
    struct list_head txdesc_queue; 
                                   
    struct drv_txdesc *txdesc_queue_last;
    struct drv_txdesc *txdesc_queue_lastframe;
    struct drv_txdesc *txdesc_queue_next;     
    struct sk_buff *txdesc_mpdu;     
    void *txdesc_sta;     
    unsigned short txdesc_flag;    
    struct wifi_mac_tx_info *txinfo;
    struct wifi_drv_pkt_info drv_pkt_info;
    struct aml_ratecontrol txdesc_rateinfo[DRV_TXDESC_RATE_NUM];

    int txdesc_pktnum;   
    int txdesc_agglen;  
    int txdesc_delimit;
    unsigned short txdesc_chanbw;
    unsigned short txdesc_framectrl;
    SYS_TYPE txdesc_ddraddr; 
    unsigned short txdesc_aggr_page_num;
    unsigned char rate_valid;
    unsigned char txdesc_in_hw; 
    enum tx_frame_flag txdesc_frame_flag;
};

struct wifi_skb_callback
{
    struct wifi_station *sta;
    unsigned short flags;
    unsigned char hdrsize;
    unsigned char u_tid;
} ;

struct wifi_mac_tx_info
{
    struct wifi_skb_callback cb;
    struct drv_txdesc *ptxdesc;

    unsigned char wnet_vif_id;      
    unsigned char key_index;      
    unsigned char key_type;    
    unsigned char tid_index;
    unsigned char queue_id;       
    unsigned char amsdunum;       
    unsigned short seqnum;      
    unsigned short packetlen;     
    unsigned short mpdulen;     

    unsigned int b_mcast:1,
        b_txfrag:1,
        b_mgmt:1,
        b_datapkt:1,
        b_qosdata:1,
        b_nulldata:1,
        ps:1,       
        b_buffered:1, 
        shortPreamble:1, 
        ht:1,
        b_BarPkt:1,
        b_PsPoll:1,
        b_noack:1,
        b_uapsd:1,
        b_rsten:1,  
        b_csten:1,  
        b_11n:1,
        b_aggr:1,
        b_Ampdu:1,
        b_hwchecksum:1, 
        b_hwtkipmic:1,
        b_amsdu:1,
        b_tcp_saved_flag:1,
        b_pmf:1;
} ;

#define TX_DESC_BUF_LOCK_INIT(_sc) spin_lock_init(&(_sc)->tx_desc_buf_lock)
#define TX_DESC_BUF_LOCK_DESTROY(_sc)
#define TX_DESC_BUF_LOCK(_sc) do {OS_SPIN_LOCK_IRQ(&(_sc)->tx_desc_buf_lock,(_sc)->tx_desc_buf_lock_flags);}while(0)
#define TX_DESC_BUF_UNLOCK(_sc) do{OS_SPIN_UNLOCK_IRQ(&(_sc)->tx_desc_buf_lock,(_sc)->tx_desc_buf_lock_flags);}while(0)

extern struct drv_txdesc *wifi_mac_alloc_txdesc(struct wifi_mac *wifimac);
extern void wifi_mac_recycle_txdesc(struct sk_buff *skbbuf);
extern struct sk_buff *wifi_mac_alloc_skb(struct wifi_mac *wifimac, unsigned int size);
extern void wifi_mac_free_skb(struct sk_buff *skb);

#endif
