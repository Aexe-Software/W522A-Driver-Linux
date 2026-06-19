
#ifndef _DRV_HAL_THR_CONF_H
#define _DRV_HAL_THR_CONF_H

#define STA1_VMAC0_KEY_LEN  	32

#define STA1_VMAC0_IBSS				0

#define DP_SEC_KEY_AUTH 1
#define DP_SEC_KEY_SUPP 0

#define TYPE_COMMON                  1  
#define TYPE_AMPDU                   2  
#define TYPE_AMSDU                   3  
#define TYPE_AMSDU_AMPDU             4  
#define TYPE_COMMON_NOACK            5  
#define TYPE_BURST_ACK               6  
#define TYPE_BURST_BA                7
#define TYPE_STOP_TX                 8

#define TYPE_MANAGE 0

#define STA1_VMAC0_SEND_TYPE  TYPE_BURST_BA 

#define STA1_VMAC0_SEND_RATE		0x85
#define  STA1_VMAC0_SEND_40M        0 

#define STA1_VMAC0_AGG_NUM			2

#define WIFI_IS_GREENFIELD          BIT(5)

#define STA2_VMAC1_SEND_TID_CTRL  		1
#define STA2_VMAC1_SEND_TID  			3      
#define STA2_VMAC1_SEND_FRAME_NUM  		1500
#define STA2_VMAC1_RX_FRAME_DUMP  		1
#define STA1_VMAC0_SEND_LEN			    1200

#define STA2_VMAC1_TKIPMIC_HW		0
#define STA2_TCPIP_CHECKSUM			0

#endif