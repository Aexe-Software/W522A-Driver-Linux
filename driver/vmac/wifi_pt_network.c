
#include "wifi_pt_network.h"
#include "wifi_hal_com.h"
#include "wifi_skbbuf.h"

struct _Network  my_network;

const char the_hex_table[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F'
};

enum {
    ACCESS_TX_QUEUE_SIZE = 128,
};

enum {
    NET_STATE_IDLE,
    NET_STATE_SEND_PING,
    NET_STATE_SEND_UDP,
    NET_STATE_SEND_SIM_TEST,
    NET_STATE_SEND_TCP,
};

typedef struct _Access {
    struct _Queue tx_queue;
    void  *tx_queue_buffer[ACCESS_TX_QUEUE_SIZE];

    struct _Queue manage_queue;
    void  *manage_queue_buffer[ACCESS_TX_QUEUE_SIZE];
} Access;

static Access  g_NetworkTx;
static Access  *pNetworkTx = &g_NetworkTx;
static unsigned int the_src_ip_address = IP_ADDRESS( 10, 68,7, 124 );
static unsigned int the_dst_ip_address = IP_ADDRESS( 10, 68,7, 121 );

enum {
    NET_ETHER_TYPE_ARP         = 0x0806,
    NET_ETHER_TYPE_IP          = 0x0800,
    NET_ETHER_TYPE_SIM_TEST    = 0x9999,
    NET_ETHER_TYPE_SIM_TEST_LENGTH = 1500,
    NET_ARP_OPCODE_REQUEST     = 1,
    NET_ARP_OPCODE_REPLY       = 2,
    NET_ICMP_TYPE_ECHO_REQUEST = 0x0800,
    NET_ICMP_TYPE_ECHO_REPLY   = 0x0000,
    NET_IP_PROTOCOL_ICMP       = 1,
    NET_IP_PROTOCOL_TCP        = 6,
    NET_IP_PROTOCOL_UDP        = 17,
};

static unsigned char
MacFrame_IsType(unsigned int frame_control,unsigned int type )
{
    return ((frame_control & (FRAME_TYPE_MASK | FRAME_SUBTYPE_MASK)) == type);
}

static unsigned char IsManageFrame(unsigned char* buffer)
{
    unsigned int frame_control = *(unsigned int*)buffer;

    return MacFrame_IsType(frame_control,FRAME_TYPE_MANAGEMENT);
}

static void B2B_Net_RequestTransmit(struct sk_buff *skb,
    unsigned short len,unsigned short Tid)
{
    struct _TxDescriptor *descriptor = TxDescriptor_Create();
    unsigned char temp;
    ASSERT(descriptor != NULL);
    descriptor->skb = skb;
    descriptor->len= len;
    descriptor->Tid= Tid;
    if (IsManageFrame(os_skb_data(skb))) {
        temp = Queue_Enqueue( &pNetworkTx->manage_queue, descriptor );
    }
    else {
        temp = Queue_Enqueue( &pNetworkTx->tx_queue, descriptor );
    }
}

unsigned short
MacFrame_GetHeaderSize(unsigned int frame_control)
{
    unsigned short header_size = 24; 

    if (frame_control & FRAME_CONTROL_FROM_DS
        && frame_control & FRAME_CONTROL_TO_DS ) {
        header_size += 6;
    }

    if (MacFrame_IsType(frame_control, FRAME_TYPE_DATA | FRAME_SUBTYPE_QOS)) {
        header_size += 2;
        if (frame_control&FRAME_CONTROL_ORDERED)
            header_size += 4;
    }

    if (MacFrame_IsType(frame_control, FRAME_TYPE_CONTROL | FRAME_SUBTYPE_PS_POLL) ) {
        header_size = 16;
    }

    return header_size;
}

static inline void
b2b_write_16b( unsigned char* address, unsigned short value )
{
    address[1] = ( value >> 0 ) & 0xFF;
    address[0] = ( value >> 8 ) & 0xFF;
}
static inline void
b2b_write_32b( unsigned char* address, unsigned int value )
{
    address[3] = ( value >>  0 ) & 0xFF;
    address[2] = ( value >>  8 ) & 0xFF;
    address[1] = ( value >> 16 ) & 0xFF;
    address[0] = ( value >> 24 ) & 0xFF;
}

static inline unsigned short b2b_read_16b( unsigned char* address )
{
    return address[1] | ( address[0] << 8 );
}

static inline unsigned short b2b_read_16l( unsigned char* address )
{
#ifdef CTRL_BYTE
    return address[0] | ( address[1] << 8 );
#else
    return * (unsigned short *)address ;
#endif
}

static inline unsigned int
b2b_read_32b( unsigned char* address )
{
    return (address[3] | (address[2] << 8) |
            (address[1] << 16) | (address[0] << 24));
}

static inline void
b2b_write_32l(unsigned char *address, unsigned int value )
{

    address[0] = ( value >>  0 ) & 0xFF;
    address[1] = ( value >>  8 ) & 0xFF;
    address[2] = ( value >> 16 ) & 0xFF;
    address[3] = ( value >> 24 ) & 0xFF;

    * (unsigned int *)address = value;

}

static inline void
b2b_write_16l( unsigned char *address, unsigned short value )
{

    address[0] = ( value >>  0 ) & 0xFF;
    address[1] = ( value >>  8 ) & 0xFF;
}

static inline void
B2b_Address_Write( unsigned char *cursor, unsigned char *address )
{
        *cursor++ = *address++;
        *cursor++ = *address++;
        *cursor++ = *address++;
        *cursor++ = *address++;
        *cursor++ = *address++;
        *cursor++ = *address++;
}

static unsigned short
B2b_Net_WriteSimcheck(unsigned char* start,
    unsigned short length,unsigned char tid)
{

        int i;
        static unsigned char sendPacketseq[MAX_TID] = {0};
        for (i = 0; i<length; i ++)
                start[i] = sendPacketseq[tid];
        sendPacketseq[tid] ++;
        return length;
}

static void RcvFrameTestSeq(unsigned char *packet,unsigned char tid)
{
        return;
}

static void RcvFrame(unsigned char *packet,int length,unsigned char tid)
{
    int i;
    static unsigned char sendPacketseq[MAX_TID]= {0};

    for (i = 0; i<length; i ++){
        if (packet[i] != sendPacketseq[tid]) {
            
            break;
        }
    }
    sendPacketseq[tid] ++;
    return;
}
static void Net_Send( unsigned char type, unsigned char* input)
{
    unsigned int header_length;
    struct sk_buff *skb = TxBuffer_Alloc();
    unsigned char *buffer = os_skb_data(skb);
    DBG_HAL_THR_ENTER();

    if (buffer != NULL) {
        unsigned char* cursor = buffer;
        unsigned short duration = 0;
        unsigned int frame_control = FRAME_TYPE_DATA | FRAME_SUBTYPE_DATA | FRAME_CONTROL_TO_DS;

        if( TrcConfMib.dot11RDSupport){
            frame_control |= FRAME_CONTROL_ORDERED;
        }
        if( TrcConfMib.dot11EncryptType != 0 ){
            frame_control |= FRAME_CONTROL_PROTECTED;
        }
        if ( TrcConfMib.tid != 8 ) {
            frame_control |= FRAME_SUBTYPE_QOS;
        }

        b2b_write_32l( cursor, frame_control | duration << DURATION_ID_SHIFT );
        cursor += 4;
        B2b_Address_Write( cursor, TrcConfMib.the_bssid );                
        cursor += 6;

        B2b_Address_Write( cursor, my_network.src_mac_address );       
        cursor += 6;

        B2b_Address_Write( cursor, my_network.dst_mac_address );   
        cursor += 6;
        
        b2b_write_16l( cursor, htole16((TrcConfMib.SN[TrcConfMib.tid]++) << SEQUENCE_CONTROL_SEQUENCE_SHIFT));
        cursor += 2;

        if ( frame_control & FRAME_SUBTYPE_QOS )
        {
            if ((TrcConfMib.testflag & WIFI_IS_NOACK) == WIFI_IS_NOACK)
            {
                if (TrcConfMib.testtype == TYPE_AMPDU){
                    
                    b2b_write_16l( cursor, TrcConfMib.tid|(3<<5) );
                }else{
                    
                    WRITE_16L( cursor, TrcConfMib.tid|(1<<5) );
                }
            }else {
                
                b2b_write_16l( cursor, TrcConfMib.tid );
            }

            cursor += 2;
        }
        header_length = cursor - buffer;          

        *cursor++ = 0xAA;                                  
        *cursor++ = 0xAA;                                  
        *cursor++ = 0x03;                                  
        *cursor++ = 0x00;                                  
        *cursor++ = 0x00;
        *cursor++ = 0x00;

        b2b_write_16b( cursor, NET_ETHER_TYPE_SIM_TEST );
        cursor += 2;

        B2b_Net_WriteSimcheck( cursor,gB2BTestCasePacket.pkt_length,TrcConfMib.tid );
        cursor = buffer + gB2BTestCasePacket.pkt_length;

        B2B_Net_RequestTransmit(skb,cursor-buffer,TrcConfMib.tid);
        
    }
    else {
        my_network.tx_pool_error_count++;
        ASSERT(0);
        pr_debug( "NULL TxBuffer_Create\n" );
    }
    DBG_HAL_THR_EXIT();
}

void Net_Receive(unsigned char *packet,unsigned short len,unsigned char rssi)
{
        unsigned int frame_control;
        unsigned short ether_type;
        unsigned short opcode;
        unsigned char tid = 9;
        unsigned short qosCtrl;
        int headerlength;
        
        frame_control = *(unsigned int*)packet;
        headerlength = MacFrame_GetHeaderSize( frame_control );
        if (MacFrame_IsType( frame_control, FRAME_TYPE_DATA|FRAME_SUBTYPE_QOS  )) {
                qosCtrl = b2b_read_16b( packet + 24);
                tid = qosCtrl&0xF;
                
                RcvFrameTestSeq(packet,tid);
        }
        else {
                tid = 8;
                
                RcvFrameTestSeq(packet,tid);
                headerlength = 24;
        }

        packet += headerlength;
        
        ether_type = b2b_read_16b( packet + 6 );
        packet += 8;

        if ( ether_type == NET_ETHER_TYPE_IP ) {
                
                unsigned int dst_ip_address = b2b_read_32b( packet + 16 );

                if ( dst_ip_address == my_network.src_ip_address ) {
                        unsigned char  header_length = ( *packet & 0x0F ) * 4;
                        unsigned short total_length = b2b_read_16b( packet + 2 );
                        unsigned char  protocol = *( packet + 9 );

                        if ( protocol == NET_IP_PROTOCOL_ICMP ) {
                                unsigned short type = b2b_read_16b( packet + header_length );

                                if ( type == NET_ICMP_TYPE_ECHO_REQUEST ) {
                                        
                                        Net_Send( NET_MESSAGE_PING_REPLY, packet);

                                        my_network.rx_ping_request_count++;
                                }
                                else if ( type == NET_ICMP_TYPE_ECHO_REPLY ) {
                                        my_network.rx_ping_reply_count++;
                                }
                                else {
                                        my_network.rx_bad_icmp_type_count++;
                                }

                                my_network.rx_byte_count += total_length - header_length - 8;
                        }
                        else if ( protocol == NET_IP_PROTOCOL_UDP ) {
                                my_network.rx_byte_count += total_length - header_length - 8;

                                my_network.rx_udp_count++;
                        }
                        else {
                                my_network.rx_bad_ip_protocol_count++;
                        }
                }
                else {
                        my_network.rx_bad_ip_address_count++;
                }
        }
        else if ( ether_type == NET_ETHER_TYPE_ARP ) {
                unsigned int target_ip_address  = b2b_read_32b( packet + 24 );

                if ( target_ip_address == my_network.src_ip_address ) {
                        B2b_Address_Write( my_network.dst_mac_address, packet + 8 );

                        opcode = b2b_read_16b( packet + 6 );
                        if ( opcode == NET_ARP_OPCODE_REQUEST ) {
                                
                                Net_Send( NET_MESSAGE_ARP_REPLY, 0);

                                my_network.rx_arp_request_count++;
                        }
                        else {
                                my_network.rx_arp_reply_count++;
                        }
                }
        }
        else if (NET_ETHER_TYPE_SIM_TEST == ether_type) {
                RcvFrame(packet,NET_ETHER_TYPE_SIM_TEST_LENGTH,tid);
        }
        else {
                my_network.rx_bad_ethertype_count++;
        }
}

void Net_Task(void)
{
#ifdef STA2_TCPIP_CHECKSUM
#if STA2_TCPIP_CHECKSUM
    my_network.state = 4;
#else
    my_network.state = 3;
#endif
#else
    my_network.state = 3;
#endif
    switch ( my_network.state ) {
    case 2:
        Net_Send( NET_MESSAGE_UDP, 0);
        break;
    case 1: 
        Net_Send( NET_MESSAGE_PING_REQUEST, 0);
        break;
    case 3: 
        Net_Send( NET_SIM_CHECK, 0);
        break;
    case 4: 
        Net_Send( NET_TCP_IP, 0);
        break;
    default:
        break;
    }
}

unsigned char * Net_GetNextPacketDes(void)
{
    DBG_HAL_THR_ENTER();
    
    Net_Task();
    if (!Queue_IsEmpty(&pNetworkTx->tx_queue))
        return (unsigned char *)Queue_Dequeue( &pNetworkTx->tx_queue );
    DBG_HAL_THR_EXIT();
    return NULL;
}

void local_mac_addr_update()
{
	my_network.src_mac_address = TrcConfMib.the_mac_address;
}
void dst_mac_addr_update()
{
	my_network.dst_mac_address= TrcConfMib.the_desc_address;
}
void network_init(void)
{
    DBG_HAL_THR_ENTER();

    my_network.src_mac_address = TrcConfMib.the_mac_address;
    my_network.dst_mac_address = TrcConfMib.the_desc_address;
    my_network.src_ip_address  = the_src_ip_address;
    my_network.dst_ip_address  = the_dst_ip_address;
    my_network.tx_rate = PHY_RATE_54;
    my_network.state = NET_STATE_IDLE;

    Queue_Create( &pNetworkTx->tx_queue, pNetworkTx->tx_queue_buffer, ACCESS_TX_QUEUE_SIZE );
    Queue_Create( &pNetworkTx->manage_queue, pNetworkTx->manage_queue_buffer, ACCESS_TX_QUEUE_SIZE );

    DBG_HAL_THR_EXIT();
}

void Init_B2B_Resource(void)
{
    mib_init();
    network_init();
    TxTask_Initialize();
}

void prepare_test_hal_layer_thr_init(int usrtesttype)
{
    DBG_HAL_THR_ENTER();
    if ( usrtesttype > 11 || usrtesttype < 1 )
    {
        pr_warn("Warning: Not supported testing type.\n");
        return;
    }

    pr_debug("b2b_init: test type 0x%08x\n", usrtesttype);

    Task_Schedule(usrtesttype);

    DBG_HAL_THR_EXIT();
}
