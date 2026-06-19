
#include "wifi_hal_cmd.h"
#include "wifi_hal_com.h"
#include "wifi_hal.h"
#include "wifi_hal_txdesc.h"

#include "wifi_mac.h"

static struct rate_control our_rate_controls[ WIFI_11BG_MAX +1];
static struct ht_mcs_control our_htmcs_controls[ WIFI_11N_MAX+1];
static struct vht_mcs_control our_vhtmcs_controls[ WIFI_11AC_MAX+1];

static const unsigned short our_ack_time_short[WIFI_11BG_MAX+1]=
{
    314,
    162,
    127,
    117,
    60, 
    60, 
    48, 
    48, 
    44, 
    44, 
    44, 
    44, 
};

static const unsigned short our_ack_time_11b_long[]=
{
    314,
    258,
    223,
    213,
};

static const unsigned short our_ba_txtime_short[WIFI_11BG_MAX+1] =
{
    362 , 
    234 , 
    153 , 
    130 , 
    84 , 
    68 , 
    60 , 
    52 , 
    48 , 
    44 , 
    44 , 
    44 , 
};

static const unsigned short  our_ba_txtime_11b_long[4] =
{
    458 ,  
    330 ,  
    249 ,  
    226 ,  
};

static const unsigned short ht_bits_per_symbol[WIFI_11N_MAX+1][2] =
{
    
    {    26,   54 },     
    {    52,  108 },     
    {    78,  162 },     
    {   104,  216 },     
    {   156,  324 },     
    {   208,  432 },     
    {   234,  486 },     
    {   260,  540 },     
};

static const unsigned short vht_bits_per_symbol[WIFI_11AC_MAX+1][4] =
{
    
    {    26,     54,            117,     234},     
    {    52,     108,          234,     468},     
    {    78,     162,          351,     702},     
    {    104,   216,          468,     936 },     
    {    156,   324,          702,     1404},     
    {    208,   432,          936,     1872},     
    {    234,   486,          1053,   2106 },     
    {    260,   540,          1170,   2340},     
    {    312,   648,          1404,   2808},     
    {    0,      720,          1560,   3120},     

};

static const unsigned char ht_ba_txtime[WIFI_11N_MAX+1][2][2]=
{
    { {80 ,60  } , {76 ,58  } , }, 
    { {60 ,48  } , {58 ,47  } , }, 
    { {52 ,44  } , {51 ,44  } , }, 
    { {48 ,44  } , {47 ,44  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
};

static const unsigned char vht_ba_txtime[WIFI_11AC_MAX+1]=
{
    68, 
    44, 
    44, 
    32, 
    32, 
    32, 
    32, 
    32, 
    32, 
    32, 
};

static const unsigned char ht_ack_txtime[WIFI_11N_MAX+1][2][2] =
{
    { {60 ,48  } , {58 ,47  } , }, 
    { {48 ,44  } , {47 ,44  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
    { {44 ,40  } , {44 ,40  } , }, 
    { {40 ,40  } , {40 ,40  } , }, 
    { {40 ,40  } , {40 ,40  } , }, 
    { {40 ,40  } , {40 ,40  } , }, 
    { {40 ,40  } , {40 ,40  } , }, 
};

struct wifi_cfg_mib wifi_conf_mib;

unsigned char tmp_dot11_preamble_type;

void hal_tx_desc_init(void)
{
    struct rate_control* control;
    struct ht_mcs_control* htmcs_control;
    struct vht_mcs_control* vhtmcs_control;
    
    DBG_ENTER();

    memset(&wifi_conf_mib,0,sizeof(wifi_conf_mib));
    wifi_conf_mib.dot11FragmentationThreshold = 2000;
    wifi_conf_mib.dot11PreambleType =PREAMBLE_LONG;
    memset(&wifi_conf_mib.cmddata[0],-1, MAX_HI_CMD*sizeof(wifi_conf_mib.cmddata[0]));

    control = &our_rate_controls[ WIFI_11B_1M];
    control->bit_time   = 262144;
    control->rts_rate   = WIFI_11B_1M;
    control->ack_rate  = WIFI_11B_1M;
    control->tx_power = 0;

    control = &our_rate_controls[ WIFI_11B_2M ];
    control->bit_time   = 131072;
    control->rts_rate   = WIFI_11B_1M;
    control->ack_rate  = WIFI_11B_1M;
    control->tx_power = 0;

    control = &our_rate_controls[ WIFI_11B_5M ];
    control->bit_time = 47662;
    control->rts_rate = WIFI_11B_5M;
    control->ack_rate = WIFI_11B_5M;
    control->tx_power = 1;

    control = &our_rate_controls[ WIFI_11B_11M ];
    control->bit_time = 23831;
    control->rts_rate = WIFI_11B_5M;
    control->ack_rate = WIFI_11B_5M;
    control->tx_power = 1;

    control = &our_rate_controls[ WIFI_11G_6M ];
    control->bit_time = 43690;
    control->rts_rate = WIFI_11G_6M;
    control->ack_rate = WIFI_11G_6M;
    control->tx_power = 2;

    control = &our_rate_controls[ WIFI_11G_9M ];
    control->bit_time = 29127;
    control->rts_rate = WIFI_11G_6M;
    control->ack_rate = WIFI_11G_6M;
    control->tx_power = 2;

    control = &our_rate_controls[ WIFI_11G_12M ];
    control->bit_time = 21845;
    control->rts_rate = WIFI_11G_12M;
    control->ack_rate = WIFI_11G_12M;
    control->tx_power = 2;

    control = &our_rate_controls[ WIFI_11G_18M ];
    control->bit_time = 14563;
    control->rts_rate = WIFI_11G_12M;
    control->ack_rate = WIFI_11G_12M;
    control->tx_power = 3;

    control = &our_rate_controls[ WIFI_11G_24M ];
    control->bit_time = 10922;
    control->rts_rate = WIFI_11G_12M;
    control->ack_rate = WIFI_11G_12M;
    control->tx_power = 3;

    control = &our_rate_controls[ WIFI_11G_36M ];
    control->bit_time = 7281;
    control->rts_rate = WIFI_11G_24M;
    control->ack_rate = WIFI_11G_24M;
    control->tx_power = 3;

    control = &our_rate_controls[ WIFI_11G_48M ];
    control->bit_time = 5461;
    control->rts_rate = WIFI_11G_24M;
    control->ack_rate = WIFI_11G_24M;
    control->tx_power = 4;

    control = &our_rate_controls[ WIFI_11G_54M ];
    control->bit_time = 4854;
    control->rts_rate = WIFI_11G_24M;
    control->ack_rate = WIFI_11G_24M;
    control->tx_power = 5;
    
    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS0)];
    htmcs_control->rts_rate = WIFI_11G_6M;
    htmcs_control->ack_rate = WIFI_11G_6M;
    htmcs_control->tx_power = 6;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS1)];
    htmcs_control->rts_rate = WIFI_11G_6M;
    htmcs_control->ack_rate = WIFI_11G_6M;
    htmcs_control->tx_power = 6;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS2)];
    htmcs_control->rts_rate = WIFI_11G_6M;
    htmcs_control->ack_rate = WIFI_11G_6M;
    htmcs_control->tx_power = 6;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS3)];
    htmcs_control->rts_rate = WIFI_11G_12M;
    htmcs_control->ack_rate = WIFI_11G_12M;
    htmcs_control->tx_power = 7;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS4)];
    htmcs_control->rts_rate = WIFI_11G_12M;
    htmcs_control->ack_rate = WIFI_11G_12M;
    htmcs_control->tx_power = 7;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS5)];
    htmcs_control->rts_rate = WIFI_11G_12M;
    htmcs_control->ack_rate = WIFI_11G_12M;
    htmcs_control->tx_power = 7;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS6)];
    htmcs_control->rts_rate = WIFI_11G_24M;
    htmcs_control->ack_rate = WIFI_11G_24M;
    htmcs_control->tx_power = 8;

    htmcs_control = &our_htmcs_controls[GET_HT_MCS(WIFI_11N_MCS7)];
    htmcs_control->rts_rate = WIFI_11G_24M;
    htmcs_control->ack_rate = WIFI_11G_24M;
    htmcs_control->tx_power = 9;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS0)];
    vhtmcs_control->rts_rate = WIFI_11G_6M;
    vhtmcs_control->ack_rate = WIFI_11G_6M;
    vhtmcs_control->tx_power = 10;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS1)];
    vhtmcs_control->rts_rate = WIFI_11G_6M;
    vhtmcs_control->ack_rate = WIFI_11G_6M;
    vhtmcs_control->tx_power = 10;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS2)];
    vhtmcs_control->rts_rate = WIFI_11G_6M;
    vhtmcs_control->ack_rate = WIFI_11G_6M;
    vhtmcs_control->tx_power = 10;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS3)];
    vhtmcs_control->rts_rate = WIFI_11G_12M;
    vhtmcs_control->ack_rate = WIFI_11G_12M;
    vhtmcs_control->tx_power = 11;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS4)];
    vhtmcs_control->rts_rate = WIFI_11G_12M;
    vhtmcs_control->ack_rate = WIFI_11G_12M;
    vhtmcs_control->tx_power = 11;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS5)];
    vhtmcs_control->rts_rate = WIFI_11G_12M;
    vhtmcs_control->ack_rate = WIFI_11G_12M;
    vhtmcs_control->tx_power = 11;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS6)];
    vhtmcs_control->rts_rate = WIFI_11G_24M;
    vhtmcs_control->ack_rate = WIFI_11G_24M;
    vhtmcs_control->tx_power = 12;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS7)];
    vhtmcs_control->rts_rate = WIFI_11G_24M;
    vhtmcs_control->ack_rate = WIFI_11G_24M;
    vhtmcs_control->tx_power = 13;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS8)];
    vhtmcs_control->rts_rate = WIFI_11G_24M;
    vhtmcs_control->ack_rate = WIFI_11G_24M;
    vhtmcs_control->tx_power = 14;

    vhtmcs_control = &our_vhtmcs_controls[GET_VHT_MCS(WIFI_11AC_MCS9)];
    vhtmcs_control->rts_rate = WIFI_11G_24M;
    vhtmcs_control->ack_rate = WIFI_11G_24M;
    vhtmcs_control->tx_power = 15;

    DBG_EXIT();
}

unsigned int hal_tx_desc_nonht_mode(unsigned char date_rate ,unsigned char channel_bw)
{
    if ((date_rate == WIFI_11B_1M)||(date_rate == WIFI_11B_2M))
    {
        return ERP_DSSS;
    }
    else if ((date_rate == WIFI_11B_11M)||(date_rate == WIFI_11B_5M))
    {
        return ERP_CCK;
    }

    if ((channel_bw == CHAN_BW_40M ||channel_bw == CHAN_BW_80M ) &&
        !IS_MCS_RATE(date_rate) )
    {
        return NON_HT_DUP_OFDM;
    }

    return ERP_OFDM;
}

unsigned char hal_tx_desc_get_power( unsigned char rate )
{
    if (IS_HT_RATE(rate))
    {
        return  our_htmcs_controls[ GET_HT_MCS(rate) ].tx_power;
    }
    else if (IS_VHT_RATE(rate))
    {
        return  our_vhtmcs_controls[ GET_HT_MCS(rate) ].tx_power;
    }
    else
    {
        return   our_rate_controls[ rate ].tx_power;
    }

}

static inline unsigned char Hal_TxDescriptor_GetPreamble( unsigned char rate,unsigned char preambletype )
{
    return ( IS_OFMD_RATE(rate) ? 20: ((preambletype==PREAMBLE_SHORT)?96:192) );
}

static unsigned char Hal_TxDescriptor_GetAckRate( unsigned char data_rate )
{
    if (IS_HT_RATE(data_rate) )
    {
        data_rate = GET_HT_MCS(data_rate);
        ASSERT( data_rate <= WIFI_11N_MAX );
        return our_htmcs_controls[ data_rate ].ack_rate;
    }
    else if (IS_VHT_RATE(data_rate) )
    {
        data_rate = GET_VHT_MCS(data_rate);
        ASSERT( data_rate <= WIFI_11AC_MAX );
        return our_vhtmcs_controls[ data_rate ].ack_rate;
    }
    else
    {
        ASSERT( data_rate <= WIFI_11BG_MAX );
        return our_rate_controls[ data_rate ].ack_rate;
    }
}

static unsigned char Hal_TxDescriptor_GetRtsRate( unsigned char data_rate )
{
    if (IS_HT_RATE(data_rate) )
    {
        data_rate = GET_HT_MCS(data_rate);
        ASSERT( data_rate <= WIFI_11N_MAX );
        return our_htmcs_controls[ data_rate ].rts_rate;
    }
    else if (IS_VHT_RATE(data_rate) )
    {
        data_rate = GET_VHT_MCS(data_rate);
        ASSERT( data_rate <= WIFI_11AC_MAX );
        return our_vhtmcs_controls[ data_rate ].rts_rate;
    }
    else
    {
        ASSERT( data_rate <= WIFI_11BG_MAX );
        return our_rate_controls[ data_rate ].rts_rate;
    }
}

static inline unsigned char Hal_TxDescriptor_HT_GetPreamble(unsigned char mcs ,unsigned char green)
{

    return (green)?( HT_SIG + HT_GF_STF+ HT_LTF1+ HT_LTF_STREAM1)
        :( L_STF + L_LTF + L_SIG + HT_SIG + HT_STF + HT_LTF_STREAM1);
}

static inline unsigned char Hal_TxDescriptor_VHT_GetPreamble(unsigned char mcs ,unsigned char green)
{

    green = 0;
    
    return (green + L_STF + L_LTF + L_SIG + VHT_SIG_A  + VHT_STF +VHT_LTF+VHT_SIG_B);
}

static unsigned short Hal_TxDescriptor_HT_GetAckTime( unsigned char ackmcs,
    unsigned char b_shortGI,unsigned char b_rifs,unsigned char b_40M)
{
    
    ackmcs = GET_HT_MCS(ackmcs);
    return (b_rifs?2:16) + ht_ack_txtime[ackmcs][b_shortGI][b_40M];
}

static inline unsigned short Hal_TxDescriptor_HT_GetBATime( unsigned char ackmcs,
    unsigned char b_shortGI,unsigned char b_rifs,unsigned char b_40M)
{
    
    ackmcs = GET_HT_MCS(ackmcs);
    return (b_rifs?2:16)                                   
           +ht_ba_txtime[ackmcs][b_shortGI][b_40M];
}

static unsigned short hal_txdescriptor_ht_gettxTime(unsigned char mcs ,unsigned short pktlen,
    unsigned char shortGI,unsigned char bw,unsigned char green)
{
    unsigned int nbits = 0;
    unsigned short duration,nsymbits,nsymbols;

    mcs = GET_HT_MCS(mcs);
    if (bw > BW_40) {
        bw = BW_40;
    }

    nbits = (pktlen << 3) + OFDM_PLCP_BITS;
    nsymbits = ht_bits_per_symbol[mcs][bw];
    nsymbols = (nbits + nsymbits - 1) / nsymbits;
    if (!shortGI)
        duration = SYMBOL_TIME(nsymbols);
    else
        duration = SYMBOL_TIME_HALFGI(nsymbols);

    duration += Hal_TxDescriptor_HT_GetPreamble(mcs,green);
    
    return duration;
}

static unsigned short Hal_TxDescriptor_VHT_GetTxTime(unsigned char mcs ,unsigned short pktlen,
    unsigned char shortGI, unsigned char bandwidth, unsigned char green)
{
    unsigned int nbits = 0;
    unsigned short duration,nsymbits,nsymbols;

    mcs = GET_VHT_MCS(mcs);
    
    nbits = (pktlen << 3) + OFDM_PLCP_BITS;
    nsymbits = vht_bits_per_symbol[mcs][bandwidth];

    if (nsymbits == 0) {
        pr_debug("Hal_TxDescriptor_VHT_GetTxTime warming nsymbits=%d, bw=%d, mcs=%d\n ", nsymbits, bandwidth, mcs);
        nsymbols = 1;

    } else {
        nsymbols = (nbits + nsymbits - 1) / nsymbits;
    }

    if (!shortGI)
        duration = SYMBOL_TIME(nsymbols);
    else
        duration = SYMBOL_TIME_HALFGI(nsymbols);

    duration += Hal_TxDescriptor_VHT_GetPreamble(mcs,green);
    
    return duration;
}

__INLINE static unsigned short Hal_TxDescriptor_HT_GetDataTime(unsigned char mcs,
    unsigned short pktlen,unsigned char b_40M)
{

    unsigned int nbits=0;
    unsigned short duration,nsymbits,nsymbols;

    mcs = GET_HT_MCS(mcs);
    if (b_40M > BW_40) {
        b_40M = BW_40;
    }
    
    nbits = (pktlen << 3);
    nsymbits = ht_bits_per_symbol[mcs][b_40M];
    nsymbols = (nbits + nsymbits - 1) / nsymbits;
    duration = SYMBOL_TIME(nsymbols);

    return duration;
}

static unsigned short Hal_TxDescriptor_GetDuration( unsigned char rate, unsigned short length )
{
    unsigned short duration =0;
    length *= 8;

    if ( IS_OFMD_RATE(rate)  )
    {
        length += 16 + 6;
    }

    duration = ( our_rate_controls[ rate ].bit_time * length + ( ( 1 << 18 ) - 1 ) ) >> 18;

    if ( IS_OFMD_RATE(rate)  )
    {
        duration = ( duration + 3 ) & ~0x3;
    }

    return duration;
}

static unsigned short Hal_TxDescriptor_GetLegacyTxTime(unsigned char data_rate,
    unsigned short pktlen,unsigned char preambletype )
{
    return Hal_TxDescriptor_GetPreamble( data_rate,preambletype )
           + Hal_TxDescriptor_GetDuration( data_rate, pktlen );
}

unsigned short hal_tx_desc_get_len(unsigned char rate ,unsigned short pktlen,
        unsigned char shortGI,unsigned char bw,unsigned char green)
{
        if (IS_HT_RATE(rate))
                return ((hal_txdescriptor_ht_gettxTime(rate,pktlen,shortGI,bw,green) - 20)>>2)*3-3 ;
        else if (IS_VHT_RATE(rate)) {
            return ((Hal_TxDescriptor_VHT_GetTxTime(rate,pktlen,shortGI,bw,green) - 20)>>2)*3-3 ;
        }else
                return pktlen;

}

static unsigned short Hal_TxDescriptor_GetAckTimeout( unsigned char data_rate,unsigned char preambletype)
{
    if (IS_HT_RATE(data_rate) || IS_VHT_RATE(data_rate)) {
        return 80 + PHY_TEST;
    } else {
        return 20 + PHY_TEST;
    }
}

static unsigned short Hal_TxDescriptor_GetAckTime(
    unsigned char data_rate ,
    unsigned char preambletype,
    unsigned char b_shortGI,
    unsigned char b_rifs,
    unsigned char b_40M)
{
    unsigned char AckRate = Hal_TxDescriptor_GetAckRate(data_rate);
    if (IS_HT_RATE(AckRate) )
    {
        return Hal_TxDescriptor_HT_GetAckTime(AckRate, b_shortGI, b_rifs,b_40M);
    }
    else if (IS_VHT_RATE(AckRate) )
    {
        return Hal_TxDescriptor_HT_GetAckTime(AckRate, b_shortGI, b_rifs,b_40M);
    }
    else
    {
        if ((preambletype == PREAMBLE_SHORT)||IS_OFMD_RATE(data_rate))
        {
            return our_ack_time_short[AckRate]+PHY_TEST;
        }
        else
        {
            return our_ack_time_11b_long[AckRate]+PHY_TEST;
        }
    }
}

static unsigned short Hal_TxDescriptor_GetBATime(
        unsigned char data_rate,
        unsigned char preambletype,
        unsigned char b_shortGI,
        unsigned char b_rifs,
        unsigned char      bw)
{
    unsigned char ackrate = Hal_TxDescriptor_GetAckRate(data_rate);

    if (IS_HT_RATE(ackrate))
    {
        ackrate = GET_HT_MCS(ackrate);
        return (b_rifs?2:16)
               +ht_ba_txtime[ackrate][b_shortGI][bw]+PHY_TEST;
    }
    else if (IS_VHT_RATE(ackrate))
    {
        ackrate = GET_VHT_MCS(ackrate);
        return (b_rifs?2:16)
               +vht_ba_txtime[ackrate]+PHY_TEST;
    }
    else
    {
        if ((preambletype == PREAMBLE_SHORT)||IS_OFMD_RATE(data_rate))
        {
            return    our_ba_txtime_short[ackrate] +PHY_TEST;
        }
        else
        {
            return   our_ba_txtime_11b_long[ackrate]+PHY_TEST ; 
        }
    }
}

static unsigned int Hal_TxDescriptor_GetTxTime(struct hi_agg_tx_desc* HiTxDesc)
{
        
        if (IS_HT_RATE(DESC_RATE)) {
                return  hal_txdescriptor_ht_gettxTime(DESC_RATE,
                                                      DESC_LEN,
                                                      DESC_IS_SHORTGI,
                                                      DESC_BANDWIDTH,
                                                      DESC_IS_GREEN);
        }
        else if (IS_VHT_RATE(DESC_RATE)) {
                return  Hal_TxDescriptor_VHT_GetTxTime(DESC_RATE,
                                                      DESC_LEN,
                                                      DESC_IS_SHORTGI,
                                                      DESC_BANDWIDTH,
                                                      DESC_IS_GREEN);
        }
        else {
                return Hal_TxDescriptor_GetLegacyTxTime(DESC_RATE,
                                                        DESC_LEN,
                                                        DESC_PREMBLETYPE);
        }
}

static __INLINE unsigned int Hal_TxDescriptor_GetTxTimeBA(struct hi_agg_tx_desc* HiTxDesc)
{
        return Hal_TxDescriptor_GetTxTime( HiTxDesc)
               +Hal_TxDescriptor_GetBATime(DESC_RATE,DESC_PREMBLETYPE,DESC_IS_SHORTGI,DESC_RIFS,  DESC_BANDWIDTH);
}

static __INLINE unsigned int Hal_TxDescriptor_GetCtsTime(struct hi_agg_tx_desc* HiTxDesc)
{
        if (IS_HT_RATE(DESC_RATE)) {
                return 16 
                       + hal_txdescriptor_ht_gettxTime( DESC_RATE, DESC_LEN,
                            DESC_IS_SHORTGI, DESC_BANDWIDTH, DESC_IS_GREEN) 
                       + Hal_TxDescriptor_GetAckTime(DESC_RATE, PREAMBLE_SHORT,
                            DESC_IS_SHORTGI, DESC_RIFS, DESC_BANDWIDTH);
        }
        else if (IS_VHT_RATE(DESC_RATE)) {
                return 16  
                       + Hal_TxDescriptor_VHT_GetTxTime( DESC_RATE, DESC_LEN,
                            DESC_IS_SHORTGI, DESC_BANDWIDTH, DESC_IS_GREEN) 
                       + Hal_TxDescriptor_GetAckTime(DESC_RATE, PREAMBLE_SHORT,
                            DESC_IS_SHORTGI, DESC_RIFS, DESC_BANDWIDTH); 
        }
        else {
                ASSERT( DESC_RATE <= WIFI_11BG_MAX );
                if ((DESC_PREMBLETYPE == PREAMBLE_SHORT)||IS_OFMD_RATE(DESC_RATE)) {
                        return ( IS_OFMD_RATE(DESC_RATE) ?  16 :10 )          
                               + Hal_TxDescriptor_GetPreamble( DESC_RATE,DESC_PREMBLETYPE ) 
                               + Hal_TxDescriptor_GetDuration( DESC_RATE, DESC_LEN )
                               + our_ack_time_short[our_rate_controls[ DESC_RATE ].ack_rate] 
                               ;
                }
                else {
                        return (10 )          
                               + Hal_TxDescriptor_GetPreamble( DESC_RATE,DESC_PREMBLETYPE ) 
                               + Hal_TxDescriptor_GetDuration( DESC_RATE, DESC_LEN )
                               + our_ack_time_11b_long[our_rate_controls[ DESC_RATE ].ack_rate] 
                               ;
                }
        }
}

unsigned char hal_mac_frame_type( unsigned int frame_control, unsigned int type )
{
    return (( frame_control & ( FRAME_TYPE_MASK | FRAME_SUBTYPE_MASK ) ) == type);
}

void assign_tx_desc_pn(unsigned char is_bc, unsigned char vid,
    unsigned char sta_id, struct hi_tx_desc *tx_page, unsigned char encrypt_type) {
    struct hal_private *hal_priv = hal_get_priv();
    unsigned long long *PN = NULL;

    if (encrypt_type == WIFI_NO_WEP) {
        return;
    }

    PN_LOCK();
    if (is_bc) {
        PN = (unsigned long long *)hal_priv->mRepCnt[vid].txPN;
    } else {
        PN = (unsigned long long *)hal_priv->uRepCnt[vid][sta_id].txPN[TX_UNICAST_REPCNT_ID];
    }
    
    switch (encrypt_type) {
        case WIFI_TKIP:
            memcpy(&tx_page->TxOption.PN[0],(unsigned char *)PN, 8);
            *PN = (*PN) + 1;
            break;

        case WIFI_AES:
            memcpy(&tx_page->TxOption.PN[0],(unsigned char *)PN,8);
            *PN = (*PN) + 1;
            break;

        case WIFI_WPI:
            memcpy(&tx_page->TxOption.PN[0], (unsigned char *)PN, MAX_PN_LEN);
            if (is_bc) {
                hal_wpi_pn_self_plus(PN);

            } else {
                hal_wpi_pn_self_plus_plus(PN);
            }
            break;

        default:
            break;
    }
    PN_UNLOCK();
}

void hal_tx_desc_build(struct hi_agg_tx_desc* HiTxDesc,
    struct hi_tx_priv_hdr* HI_TxPriv,struct hi_tx_desc *pTxDPape)
{
    struct hw_tx_vector_bits * TxVector_Bit= (struct hw_tx_vector_bits * )&pTxDPape->TxVector;
    unsigned short bw  = ( HiTxDesc->FLAG & WIFI_CHANNEL_BW_MASK)>>WIFI_CHANNEL_BW_OFFSET;
    struct wifi_qos_frame *wh = NULL;
    unsigned char is_bc;

    tmp_dot11_preamble_type = (DESC_RATE==WIFI_11B_1M) ? PREAMBLE_LONG:wifi_conf_mib.dot11PreambleType;

    TxVector_Bit->tv_reserved0 = 0;
    TxVector_Bit->tv_fw_duration_valid = 0;
    TxVector_Bit->tv_rty_flag = 0;

    if ((DESC_FLAG & (WIFI_IS_BLOCKACK | WIFI_IS_NOACK)) == (WIFI_IS_BLOCKACK | WIFI_IS_NOACK )) {
        TxVector_Bit->tv_ack_policy =BLOCKACK;
    }else if (DESC_FLAG& WIFI_IS_NOACK) {
        TxVector_Bit->tv_ack_policy = NOACK;
    }else {
        TxVector_Bit->tv_ack_policy =  ACK;
    }

    wh = ( struct wifi_qos_frame *)pTxDPape->txdata;
    if ((IS_VHT_RATE( DESC_RATE)) && !(HiTxDesc->FLAG & WIFI_IS_AGGR)) {
        TxVector_Bit->tv_single_ampdu = ((pTxDPape->MPDUBufFlag & (HW_LAST_AGG_FLAG|HW_FIRST_AGG_FLAG))
                                                        == (HW_LAST_AGG_FLAG|HW_FIRST_AGG_FLAG)); 
        if (TxVector_Bit->tv_single_ampdu) {
            TxVector_Bit->tv_ack_policy =  ACK;
            wh->i_qos[0] &= ~( 0x3<<5 );  
        }

    } else {
        TxVector_Bit->tv_single_ampdu = 0;
    }

    TxVector_Bit->tv_wnet_vif_id = DESC_VID;
    TxVector_Bit->tv_ack_to_en = ((DESC_FLAG & WIFI_IS_NOACK) == 0);
    TxVector_Bit->tv_ack_timeout = Hal_TxDescriptor_GetAckTimeout(DESC_RATE,DESC_PREMBLETYPE);

    TxVector_Bit->tv_ht.htbit.tv_format = hw_rate2tv_format(DESC_RATE);
    TxVector_Bit->tv_ht.htbit.tv_nonht_mod  = (hal_tx_desc_nonht_mode(DESC_RATE,bw));
    TxVector_Bit->tv_ht.htbit.tv_tx_pwr = hal_tx_desc_get_power(DESC_RATE);
    TxVector_Bit->tv_ht.htbit.tv_tx_rate = DESC_RATE;
    TxVector_Bit->tv_ht.htbit.tv_Channel_BW = bw&0x3;
    TxVector_Bit->tv_ht.htbit.tv_preamble_type = DESC_PREMBLETYPE;
    TxVector_Bit->tv_ht.htbit.tv_GI_type = (!!(DESC_FLAG & WIFI_IS_SHORTGI));
    TxVector_Bit->tv_ht.htbit.tv_antenna_set = 0;

    TxVector_Bit->tv_L_length = hal_tx_desc_get_len(DESC_RATE,DESC_LEN,
        DESC_IS_SHORTGI,DESC_BANDWIDTH,DESC_IS_GREEN);
    TxVector_Bit->tv_sounding = 0;

    if (IS_VHT_RATE( DESC_RATE) && !(HiTxDesc->FLAG & WIFI_IS_AGGR)) {
        TxVector_Bit->tv_length = DESC_LEN + 4; 
        TxVector_Bit->tv_ampdu_flag = 1;
    }
    else {
        TxVector_Bit->tv_length = DESC_LEN;
        TxVector_Bit->tv_ampdu_flag = DESC_IS_AMPDU;
    }

    TxVector_Bit->tv_STBC = 0;
    TxVector_Bit->tv_FEC_coding = (hw_rate2tv_format(DESC_RATE) != 0) ?
        (!!(HiTxDesc->FLAG2&TX_DESCRIPTER_ENABLE_TX_LDPC)) : 0;
    TxVector_Bit->tv_num_exten_ss = 0;
    TxVector_Bit->tv_num_tx= 1;
    TxVector_Bit->tv_expansion_mat_type= 1;
    TxVector_Bit->tv_no_sig_extn= ((DESC_RATE>WIFI_11B_11M)?0:1);

    TxVector_Bit->tv_txop_time = 0;
    
    TxVector_Bit->tv_burst_en = ((!!(DESC_FLAG &WIFI_IS_BURST)) | (!!(WIFI_IS_BLOCKACK&DESC_FLAG )));
    TxVector_Bit->tv_SequenceNum=HI_TxPriv->Seqnum<<SEQUENCE_CONTROL_SEQUENCE_SHIFT;
    TxVector_Bit->tv_FrameControl = HI_TxPriv->FrameControl;

    if ( hal_mac_frame_type(TxVector_Bit->tv_FrameControl,
        FRAME_TYPE_CONTROL | FRAME_SUBTYPE_PS_POLL))
    {
        TxVector_Bit->tv_fw_duration_valid = 1;
    }
    else
    {
        TxVector_Bit->tv_fw_duration_valid = 0;
    }

    TxVector_Bit->tv_sq_valid= 0;
    TxVector_Bit->tv_fc_valid = 1;
    TxVector_Bit->tv_du_valid= 1;
    TxVector_Bit->tv_beacon_flag =!! (HiTxDesc->FLAG2 & TX_DESCRIPTER_BEACON);

    TxVector_Bit->tv_tid_num =0;
    TxVector_Bit->tv_control_wrapper=0;
    TxVector_Bit->tv_llength_hw_calc = 0;
    TxVector_Bit->tv_htc_modify=0;

    TxVector_Bit->tv_txcsum_enbale = 1;

    if (HiTxDesc->FLAG & WIFI_IS_PMF)
        TxVector_Bit->tv_encrypted_disable = 1;
    else
        TxVector_Bit->tv_encrypted_disable = 0;

    TxVector_Bit->tv_tkip_mic_enable = 1;
    TxVector_Bit->tv_dyn_bw_nonht = 0;
    TxVector_Bit->tv_txop_ps_not = 0;
    TxVector_Bit->tv_reserved2 = 0; 
    TxVector_Bit->tv_usr_postion = 0;
    TxVector_Bit->tv_group_id = 0;
    TxVector_Bit->tv_partial_id = 0;
    TxVector_Bit->tv_num_users = 0;
    TxVector_Bit->tv_num_sts = 0;
    TxVector_Bit->tv_ch_bw_nonht = SW_CBW20;
    TxVector_Bit->tv_ht.htbit.tv_cfend_flag = 0;
    TxVector_Bit->tv_ht.htbit.tv_bar_flag = 0;
    TxVector_Bit->tv_ht.htbit.tv_beamformed = 0;
    TxVector_Bit->tv_ht.htbit.tv_rts_flag = 0;
    TxVector_Bit->tv_ht.htbit.tv_cts_flag = 0;

    if (HiTxDesc->FLAG&WIFI_IS_DYNAMIC_BW) {
        TxVector_Bit->tv_dyn_bw_nonht = 1;
        TxVector_Bit->tv_ht.htbit.tv_check_cca_bw = 1;
        TxVector_Bit->tv_ch_bw_nonht = bw & 0x3;
    }

    is_bc = ((HiTxDesc->FLAG & WIFI_IS_Group) ? 1 : 0);
    pTxDPape->TxOption.is_bc = is_bc;
    pTxDPape->TxOption.key_type = HiTxDesc->EncryptType;
    pTxDPape->TxOption.KeyIdex = HiTxDesc->KeyId;
    pTxDPape->TxPriv.AggrLen = HiTxDesc->AGGR_len;
    pTxDPape->TxPriv.aggr_page_num = HiTxDesc->aggr_page_num;
    
    if (HiTxDesc->FLAG &WIFI_IS_BLOCKACK)
    {
        if (!(HiTxDesc->FLAG &WIFI_IS_NOACK)) 
            pTxDPape->TxPriv.FrameExchDur = Hal_TxDescriptor_GetTxTimeBA(HiTxDesc);
        else  
            pTxDPape->TxPriv.FrameExchDur = Hal_TxDescriptor_GetTxTime(HiTxDesc);
    }
    else
    {
        if (!(HiTxDesc->FLAG &WIFI_IS_NOACK))
            pTxDPape->TxPriv.FrameExchDur = Hal_TxDescriptor_GetCtsTime(HiTxDesc);
        else 
            pTxDPape->TxPriv.FrameExchDur =  Hal_TxDescriptor_GetTxTime(HiTxDesc);
    }
    
    pTxDPape->TxPriv.Flag = HiTxDesc->FLAG;
    pTxDPape->TxPriv.Flag2= HiTxDesc->FLAG2;
    pTxDPape->TxPriv.TID= HiTxDesc->TID;
    pTxDPape->TxPriv.hostcallbackid = 0;
    pTxDPape->TxPriv.StaId= HiTxDesc->StaId;
    pTxDPape->TxPriv.TxCurrentRate= HiTxDesc->CurrentRate;
    pTxDPape->TxPriv.TxRate[0]= HiTxDesc->CurrentRate;
    pTxDPape->TxPriv.TxRate[1]= HiTxDesc->TxTryRate1;
    pTxDPape->TxPriv.TxRate[2]= HiTxDesc->TxTryRate2;
    pTxDPape->TxPriv.TxRate[3]= HiTxDesc->TxTryRate3;
    pTxDPape->TxPriv.TxRetry[0]= HiTxDesc->TxTryNum0;
    pTxDPape->TxPriv.TxRetry[1]= HiTxDesc->TxTryNum1+pTxDPape->TxPriv.TxRetry[0];
    pTxDPape->TxPriv.TxRetry[2]= HiTxDesc->TxTryNum2+pTxDPape->TxPriv.TxRetry[1];
    pTxDPape->TxPriv.TxRetry[3]= HiTxDesc->TxTryNum3+pTxDPape->TxPriv.TxRetry[2];
    pTxDPape->TxPriv.hostcallbackid =0;

    pTxDPape->TxPriv.AggrNum= HiTxDesc->MpduNum;
    pTxDPape->TxPriv.AggrLen= HiTxDesc->AGGR_len;
    pTxDPape->TxPriv.SN= HI_TxPriv->Seqnum;
    pTxDPape->TxPriv.txretrynum=0;
    pTxDPape->TxPriv.txsretrynum=0;
    pTxDPape->TxPriv.pTxInfo=0;
    pTxDPape->TxPriv.vid =HiTxDesc->vid;
    if (HiTxDesc->FLAG2 & TX_DESCRIPTER_P2P_PS_NOA_TRIGRSP) {
        pTxDPape->TxPriv.HiP2pNoaCountNow = HiTxDesc->HiP2pNoaCountNow;
    }

    if ((pTxDPape->TxPriv.Flag & WIFI_IS_Group) == WIFI_IS_Group) {
        pTxDPape->TxPriv.txstatus = TX_DESCRIPTOR_STATUS_SUCCESS;
    }
}

void hal_tx_desc_build_sub(struct hi_tx_priv_hdr* HI_TxPriv,
    struct hi_tx_desc *pTxDPape, struct hi_tx_desc *pFirstTxDPape)
{
    unsigned char is_bc;
    ASSERT(HI_TxPriv);
    ASSERT(pTxDPape);
    ASSERT(pFirstTxDPape);

    memcpy(pTxDPape,pFirstTxDPape,HI_TXDESC_DATAOFFSET);

    pTxDPape->TxPriv.PageNum = hal_calc_mpdu_page(HI_TxPriv->MPDULEN);
    pTxDPape->MPDUBufFlag = 0;
    pTxDPape->MPDUBufFlag = HW_FIRST_MPDUBUF_FLAG|HW_LAST_MPDUBUF_FLAG;
    pTxDPape->MPDUBufFlag |= HW_MPDU_LEN_SET(HI_TxPriv->MPDULEN);
    pTxDPape->MPDUBufFlag |= HW_BUFFER_LEN_SET(HI_TxPriv->Delimiter);
    if ((HI_TxPriv->Flag & WIFI_MORE_AGG)==0) {
        pTxDPape->MPDUBufFlag |= HW_LAST_AGG_FLAG;
    }
    if (pTxDPape->TxPriv.PageNum == 1) {
        pTxDPape->MPDUBufFlag |= HW_LAST_MPDUBUF_FLAG;
    }

    is_bc = ((pFirstTxDPape->TxPriv.Flag & WIFI_IS_Group) ? 1 : 0);
    pTxDPape->TxOption.is_bc = is_bc;
    pTxDPape->TxOption.key_type = HI_TxPriv->EncryptType;
    pTxDPape->TxOption.KeyIdex = pFirstTxDPape->TxOption.KeyIdex;

}

unsigned int max_send_packet_len(unsigned char rate,unsigned char bw, unsigned char short_gi, unsigned char streams)
{
    unsigned int preamble_time = 0;
    unsigned int mcs = 0;
    unsigned int max_symbol_number = 0;
    unsigned int max_data_field_tx_time = 0;

    if( IS_HT_RATE(rate) )
    {
        preamble_time = Hal_TxDescriptor_HT_GetPreamble(rate, 0);
    }
    else if ( IS_VHT_RATE(rate) )
    {
        preamble_time = Hal_TxDescriptor_VHT_GetPreamble(rate, 0);
    }
    else
    {
        ERROR_DEBUG_OUT("rate error, rate=0x%x\n", rate);
        return 0;
    }

     max_data_field_tx_time = 4095 -  preamble_time;
    
     if( !short_gi )
     {
        max_symbol_number = max_data_field_tx_time>>2;   
     }
     else
     {
        max_symbol_number = max_data_field_tx_time*10/36; 
     }

    mcs = GET_MCS(rate);
    if ((mcs > 9) || (bw > 2))
    {
        ERROR_DEBUG_OUT("mcs or bw error, rate=0x%x, bw=%d\n", rate, bw);
        return 0;
    }

    if ((mcs == 9)  && (bw == 0))
    {
        
        mcs = 8;
    }

    if (IS_HT_RATE(rate))
    {
        if (bw > BW_40) {
            bw = BW_40;
        }

        if (mcs > 7) {
            mcs = 7;
        }

        return ht_bits_per_symbol[mcs][bw]*max_symbol_number >> 3;
    }
    else
    {
        
        return vht_bits_per_symbol[mcs][bw]*max_symbol_number>>3;
    }
}

char hw_rate2tv_format(unsigned char rate)
{
    if (IS_VHT_RATE(rate))
    {
        return VHT;
    }
    else if (IS_HT_RATE(rate))
    {
        return HT_MF;
    }
    else
    {
        return NON_HT;
    }
}
