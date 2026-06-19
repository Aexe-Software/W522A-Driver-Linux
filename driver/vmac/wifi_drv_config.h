#ifndef _AUTO_CONFIG_H
#define _AUTO_CONFIG_H

#define DEFAULT_SELECT_MODE                 HAL_BAND_2G

#define DEFAULT_TXAMSDU_EN                  0   

#define DEFAULT_TXAMSDU_SUB_MAX              4
#define DEFAULT_TXAMSDU_SUB_MAX_BW80              8
#define DEFAULT_TXAMPDU_SUB_MIN               1
#define DEFAULT_TXAMPDU_SUB_MAX_FOR_HT        64
#define DEFAULT_TXAMPDU_SUB_MAX_COEX_ESCO     8   
#define DEFAULT_TXAMPDU_SUB_MAX_COEX               16   
#define DEFAULT_TXAMPDU_SUB_MAX              64

#define DEFAULT_VHT_STA_AMPDU_LIMIT           8

#define DEFAULT_TXAMPDU_LEN_MAX             65535 

#define DEFAULT_BLOCKACK_BITMAPSIZE         64
#define DEFAULT_RX_BA_WINDOW                64
#define DEFAULT_RX_REORDER_TIMEOUT          1
#define DEFAULT_AP_AMPDU_WAIT_TARGET        1
#define DEFAULT_AP_AMPDU_HT20_LIMIT         32
#define DEFAULT_AP_AMPDU_WIDE_LIMIT         64
#define DEFAULT_AP_VHT80_TXAGGR             1
#define DEFAULT_AP_TXAMPDU_EN               1
#define DEFAULT_AP_RXAMPDU_EN               1
#define DEFAULT_MONITOR_TXAMPDU_EN          0
#define DEFAULT_MONITOR_RXAMPDU_EN          0
#define DEFAULT_TXAMPDU_ONEFRAME        1
#define DEFAULT_RXCHAINMASKLEGACY           1
#define DEFAULT_TXCHAINMASKLEGACY           1
#define DEFAULT_RXCHAINMASK                 1
#define DEFAULT_TXCHAINMASK                 1
#define DEFAULT_BWC_ENABLE                  1
#define DEFAULT_HT_ENABLE                       1

#define DEFAULT_VHT_ENABLE                       1

#define DEFAULT_RATECONTROL_DIS                 0
#define DEFAULT_UAPSU_EN                    0
#define DEFAULT_TXRETRY_MAX                 14

#define DEFAULT_HW_TKIP_MIC                 1
#define DEFAULT_HW_CSUM                     1

#define DEFAULT_11N40M_SUPPORT              1
#define DEFAULT_AMPDUACKPOLICY              0xff
#define DEFAULT_TXAGG_PROT                  1
#define DEFAULT_TXPOWER                     11
#define DEFAULT_MCAST_EAPOL_NULLDATA_RATE_11N                      6500        
#define DEFAULT_MCAST_EAPOL_NULLDATA_RATE_11G                      1000        
#define DEFAULT_CONTRY                      0
#define DEFAULT_CACHESIZE                   32
#define DEFAULT_DSSUPPORT                   1
#define DEFAULT_WMMSUPPORT                  1
#define DEFAULT_MAX_VMAC                    4 

#define DEFAULT_EAT_COUNT_MAX 1
#define DEFAULT_MFP_EN 1
#define DEFAULT_AGGR_THRESH 35
#define DEFAULT_NO_AGGR_THRESH 20
#define DEFAULT_HRTIMER_INTERVAL 4

#ifdef DYNAMIC_BW
#define DEFAULT_SUPPORT_DYNAMIC_BW    1
#else
#define DEFAULT_SUPPORT_DYNAMIC_BW    0
#endif

#define DEFAULT_SUPPORT_WIFI_BT_COEXIST   0

#define DEFAULT_BURST_ENABLE                1

#define DEFAULT_CHANNEL_AUTO            0
#define DEFAULT_HARD_HDR_LEN            176

#define DEFAULT_CHANNEL                     149

#define DEFAULT_RTC_ENABLE          0
#define DEFAULT_INITIAL_POWERMODE  0 

#define DEFAULT_TXAMPDU_EN              1   
#define DEFAULT_RXAMPDU_EN              1    

#define SRAM_16KMODE 0  

extern int mac_addr0;
extern int mac_addr1;
extern int mac_addr2;
extern int mac_addr3;
extern int mac_addr4;
extern int mac_addr5;

extern unsigned short dhcp_offload;
extern unsigned char aml_insmod_flag;

enum cip_param_id
{
    CHIP_PARAM_TXCHAINMASK = 1,
    CHIP_PARAM_RXCHAINMASK,
    CHIP_PARAM_TXCHAINMASKLEGACY,
    CHIP_PARAM_RXCHAINMASKLEGACY,
    CHIP_PARAM_CHAINMASK_SEL,
    CHIP_PARAM_AMPDU,
    CHIP_PARAM_AMPDU_RX,
    CHIP_PARAM_AMPDU_LIMIT,
    CHIP_PARAM_AMPDU_SUBFRAMES,
    CHIP_PARAM_AGGR_PROT,
    CHIP_PARAM_TXPOWER_LIMIT,
    CHIP_PARAM_BURST_ACK,
    CHIP_PARAM_ACK_POLICY,
    CHIP_PARAM_40MSUPPORT,
    CHIP_PARAM_AMSDU_ENABLE,
    CHIP_PARAM_USE_EAP_LOWEST_RATE,
    CHIP_PARAM_SHORTGI_ENABLE,
    CHIP_PARAM_BWC_ENABLE,
    CHIP_PARAM_CBW_OFFSET,
    CHIP_PARAM_SHORTPREAMBLE,
    CHIP_PARAM_RDG,
    CHIP_PARAM_UAPSU_EN,
    CHIP_PARAM_BEACONINV,
    CHIP_PARAM_HWTKIPMIC,
    CHIP_PARAM_HWCSUM,
    CHIP_PARAM_RETRY_LIMIT,
    CHIP_PARAM_HTSUPPORT,
    CHIP_PARAM_DISABLE_RATECONTROL,
    CHIP_PARAM_AMPDU_ONE_FRAME,
    CHIP_PARAM_TXLIVETIME,
    CHIP_PARAM_DEBUG,
    CHIP_PARAM_DBG_RXERR_RESET,
    CHIP_PARAM_RTC_ENABLE,
    CHIP_PARAM_WMM,
    CHIP_PARAM_HT,
    CHIP_PARAM_VHT,
    CHIP_PARAM_UAPSD,
    CHIP_PARAM_DS,
    CHIP_PARAM_CIPHER_WEP,
    CHIP_PARAM_CIPHER_AES_CCM,
    CHIP_PARAM_CIPHER_WPI,
    CHIP_PARAM_CIPHER_TKIP,
    CHIP_PARAM_DISRATECONTROL,
    CHIP_PARAM_BWAUTOCONTROL,
    CHIP_PARAM_VHTSUPPORT,
    CHIP_PARAM_DYNAMIC_BW,
    CHIP_PARAM_MFP,
    CHIP_PARAM_EAT_COUNT,
    CHIP_PARAM_AGGR_THRESH,
    CHIP_PARAM_HRTIMER_INTERVAL,
};

enum wifi_mac_mode
{
    CFG_11B = 1,
    CFG_11G = 2,
    CFG_11N = 3,
    CFG_11AC = 4,
    DEFAULT_AUTO = 5,
};

enum wifi_band
{
    CFG_BAND_B = 2,
    CFG_BAND_A = 5,
    DEFAULT_BAND_ALL = 6,
};

int drv_get_config(void * dev, enum cip_param_id id);
int drv_set_config(void * dev, enum cip_param_id id, int data);
int drv_cfg_load_from_file(void);
#endif 
