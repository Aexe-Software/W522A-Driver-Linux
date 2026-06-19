#include "wifi_hal_com.h"

#define  I2C_CLK_QTR   0x4  

static void write_byte_8ba(unsigned char Bus, unsigned char SlaveAddr,
    unsigned char RegAddr, unsigned char Data)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    hif->hif_ops.hi_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_LIST_REG0);
    tmp = (I2C_END  << 16)             |
            (I2C_DATA << 12)             |    
            (I2C_DATA << 8)              |    
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    hif->hif_ops.hi_write_word(I2C_TOKEN_LIST_REG0, tmp);

    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG0,(Data << 8) | (RegAddr << 0));
    
    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    do {
        tmp  = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 1000) {
            pr_err("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));

}

static unsigned char read_byte_8ba(unsigned char Bus, unsigned char SlaveAddr, unsigned char RegAddr)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    hif->hif_ops.hi_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END  << 24)             |
            (I2C_DATA_LAST << 20)        |  
            (I2C_SLAVE_ADDR_READ << 16)  |
            (I2C_START << 12)            |
            (I2C_DATA << 8)              |  
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    hif->hif_ops.hi_write_word(I2C_TOKEN_LIST_REG0, tmp);

    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG0,(RegAddr << 0));
    
    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    do {
        tmp  = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 1000) {
            pr_err("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));

    tmp  = hif->hif_ops.hi_read_word(I2C_TOKEN_RDATA_REG0) & 0xff;
    return (unsigned char)tmp;
}

static void write_word_32ba(unsigned char Bus, unsigned char SlaveAddr,
    unsigned int StartToken, unsigned int Data)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    hif->hif_ops.hi_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_LIST_REG0);
    tmp =   (I2C_END << 28)              |    
            (I2C_DATA << 24)             |    
            (I2C_DATA << 20)             |    
            (I2C_DATA << 16)             |    
            (I2C_DATA << 12)             |    
            (I2C_DATA << 8)              |    
            (I2C_SLAVE_ADDR_WRITE << 4)  |
            (I2C_START << 0);
    hif->hif_ops.hi_write_word(I2C_TOKEN_LIST_REG0, tmp);

    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG0,StartToken | (Data<<8));
    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG1,(Data >> 24));
    
    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    tmp = 0;
    do {
        tmp  = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 100000) {
            ERROR_DEBUG_OUT("-------[ERR]-----> i2c[W] err\n");
            break;
        }
    } while (tmp & (1 << 2));
}

static unsigned int read_word_32ba(unsigned int SlaveAddr, unsigned int RegAddr)
{
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int tmp,cnt = 0;

    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp = (tmp & (~(0x3FF << I2C_CLOCK_OFFSET))) | (I2C_CLK_QTR << I2C_CLOCK_OFFSET);
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    hif->hif_ops.hi_write_word(I2C_SLAVE_ADDR, SlaveAddr);
    
    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_LIST_REG0);
    tmp =    (I2C_DATA  << 28)            |
             (I2C_DATA  << 24)            |
             (I2C_DATA  << 20)            |
             (I2C_SLAVE_ADDR_READ  << 16) |  
             (I2C_START << 12)            |  
             (I2C_DATA  << 8)             |  
             (I2C_SLAVE_ADDR_WRITE << 4)  |  
             (I2C_START << 0);               
    hif->hif_ops.hi_write_word(I2C_TOKEN_LIST_REG0, tmp);

    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_LIST_REG1);
    tmp = (I2C_END       << 4) | (I2C_DATA_LAST << 0);
    hif->hif_ops.hi_write_word(I2C_TOKEN_LIST_REG1, tmp);

    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG0,RegAddr << 0);
    hif->hif_ops.hi_write_word(I2C_TOKEN_WDATA_REG1,0);
    
    tmp = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);
    tmp &= (~(1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);
    tmp |= ( (1 << 0));
    hif->hif_ops.hi_write_word(I2C_CONTROL_REG, tmp);

    tmp = 0;
    do {
        tmp  = hif->hif_ops.hi_read_word(I2C_CONTROL_REG);

        cnt++;
        if (cnt == 100000) {
            ERROR_DEBUG_OUT("-------[ERR]-----> i2c[R] err\n");
            break;
        }
    } while( tmp & (1 << 2));

    tmp = hif->hif_ops.hi_read_word(I2C_TOKEN_RDATA_REG0);
    return tmp;
}

unsigned int rf_i2c_read(unsigned int reg_addr)
{
    unsigned char bus = 0;
    unsigned int slave_addr = 0x7a;
    unsigned int read_data = 0;
    unsigned int start_token;

    start_token = 0x04;

    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    read_data = read_word_32ba(slave_addr, 0x0);
    return read_data;
}

void rf_i2c_write(unsigned int reg_addr, unsigned int data)
{
    unsigned char bus = 0;
    unsigned int slave_addr = 0x7a;
    unsigned int start_token;

    start_token = 0x00;  
    write_word_32ba(bus, slave_addr, 0x00, data);

    start_token = 0x04;  
    write_word_32ba(bus, slave_addr, start_token, reg_addr);

    write_byte_8ba(bus, slave_addr, 0x8, bus);
}

int new_set_reg(unsigned int address,unsigned int data)
{
    struct hal_private *HalPriv = hal_get_priv();
    if (((address >> 24) & 0xff) == 0xff ) {
#ifdef USE_T902X_RF
        rf_i2c_write( address & 0x00ffffff,data );
#endif
    } else if (((address >> 24) & 0xf0) == 0xf0 ) {
#ifdef USE_T902X_RF
        rf_i2c_write( address & 0xffffffff,data );
        pr_debug("%s(%d) 0x%x\n", __func__, __LINE__, data);
#endif
    } else {
        HalPriv->hif->hif_ops.hi_write_word(address, data);
    }

    return 0;
}

int new_get_reg(unsigned int address,unsigned int *data)
{
    struct hal_private *HalPriv = hal_get_priv();

    if (((address >> 24) & 0xff) == 0xff ) {
#ifdef USE_T902X_RF
        *data = rf_i2c_read(address & 0x00ffffff); 
#endif
    } else if (((address >> 24) & 0xf0) == 0xf0 ) {
#ifdef USE_T902X_RF
        *data = rf_i2c_read(address & 0xffffffff); 
         pr_debug("%s(%d) 0x%x\n", __func__, __LINE__, address);
#endif
    } else {
        *data = HalPriv->hif->hif_ops.hi_read_word(address);
    }

    return 0;
}

void i2c_set_reg_fragment(unsigned int addr,unsigned int bit_end,
                             unsigned int bit_start,unsigned int value)
{

     unsigned int tmp = 0;
     unsigned int bitwidth = 0;
     int max_value = 0;
     bitwidth = bit_end - bit_start + 1;
     max_value = (bitwidth == 32) ? -1 : (1 << (bitwidth)) - 1;

    ASSERT((bitwidth > 0)||(bit_start <= 31)||(bit_end <= 31));

    new_get_reg(addr, &tmp);
    tmp &= ~(max_value << bit_start); 
    tmp |= ((value & max_value) << bit_start);

    new_set_reg(addr,tmp);

}

unsigned int i2c_get_reg_fragment(int address, int bit_end, int bit_start)
{
    unsigned int tmp;
    int bitwidth= bit_end-bit_start + 1;
    int max_value = (bitwidth==32)?-1: (1<<(bitwidth)) - 1;

    new_get_reg(address, &tmp);
    tmp >>= bit_start;
    tmp &= max_value;
    return (tmp);
}

unsigned int rf_read_register(unsigned int addr)
{
    unsigned int data;

    addr = addr|(0xff<<24);
    new_get_reg(addr, &data);

    return data;
}
void rf_write_register(unsigned int addr,unsigned int data)
{
    addr = addr|(0xff<<24);
    new_set_reg(addr,data);
}
unsigned int fi_ahb_read(unsigned int addr)
{
    unsigned int data;

    new_get_reg(addr, &data);

    return data;
}
void fi_ahb_write(unsigned int addr,unsigned int data)
{
    
    new_set_reg(addr,data);
}

void efuse_manual_write(unsigned int bit, unsigned int addr)
{

    unsigned int addr_int;
    unsigned int tmp;
    int k;

    tmp = fi_ahb_read(0xf04004);
    tmp = (tmp & ~(1 << 27)) | (0 << 27) | (1 << 31);  
    tmp = (tmp & (~(1<<24|1<<12)));                    
    
    new_set_reg(0xf04004, tmp);

    udelay(2);   

    tmp = fi_ahb_read(0xf0400c);
    tmp = (tmp & ~(1 << 27)) | (1<< 27);               
    
    new_set_reg(0xf0400c, tmp);

    udelay(1);   

    tmp = fi_ahb_read(0xf0400c);
    tmp = (tmp & ~(1 << 31));  
    
    new_set_reg(0xf0400c, tmp);

    tmp = (tmp & ~(1 << 30));  
    
    new_set_reg(0xf0400c, tmp);

    tmp = (tmp & ~(1 << 29));  
    tmp = (tmp & ~(1 << 28));  
    
    new_set_reg(0xf0400c, tmp);

    addr_int    = (bit << 4) | (addr & 0xF);    

    tmp = fi_ahb_read(0xf0400c);
    tmp = (tmp & ~((1 << 31) | (1 << 30) | (1 << 29) | (1 << 28) | (0xFFF << 0)));
    tmp = (tmp | (addr_int << 0)) ;   
    
    new_set_reg(0xf0400c, tmp);

    udelay(1); 

    tmp = fi_ahb_read(0xf0400c);
    tmp = (tmp & ~(1 << 28)) | (1 << 28);  
    
    new_set_reg(0xf0400c, tmp);

    udelay(2);  
    for ( k = 0; k < 6; k++) {  
        
        tmp = fi_ahb_read(0xf0400c);
    }

    tmp = (tmp & ~(1 << 28));  
    
    new_set_reg(0xf0400c, tmp);

    udelay(1);

    tmp = fi_ahb_read(0xf0400c);
    tmp = tmp | (1<<29);    
    
    new_set_reg(0xf0400c, tmp);

    tmp = tmp | (1<<30);    
    
    new_set_reg(0xf0400c, tmp);

    tmp = tmp | (1<<31);    
    
    new_set_reg(0xf0400c, tmp);

    udelay(1);

    tmp = fi_ahb_read(0xf0400c);
    tmp = (tmp & ~(1 << 27)) | (0 << 27);
    
    new_set_reg(0xf0400c, tmp);

    udelay(1);

    tmp = fi_ahb_read(0xf04004);
    tmp = (tmp & ~(1 << 27)) | (1 << 27);
    
    new_set_reg(0xf04004, tmp);

}

unsigned int efuse_manual_read(unsigned int addr)
{
    unsigned int tmp;
    unsigned int   rdata = 0 ;
    int i = 0;

    tmp  =  0x1F1F01BE ;    
    new_set_reg (0x00F04010,  tmp);    

    tmp  =  0x80000000 ;    
    new_set_reg (0x00F04004,  tmp);     

    udelay(2);     
    tmp  =  0xE0000000 ;    
    new_set_reg ( 0x00F0400C,  tmp );

    udelay(1);     
    tmp  =  tmp & (~(1<<31)) ;    
    tmp  =  tmp | (1 << 30) | (1 << 29) ;    
    tmp  =  tmp & (~(1<<28)) ;
    tmp  =  tmp | (addr & 0x1fff) ;   
    new_set_reg (0x00F0400C,  tmp);

    tmp  =  tmp|(1<< 28) ;      
    new_set_reg (0x00F0400C,  tmp);

    udelay(1);     
    tmp  =  tmp  &  (~(1<<28));   
    new_set_reg (0x00F0400C,  tmp);

    tmp  =  0x00000000 ;
    new_set_reg (0x00F04004, tmp);

    tmp  =  fi_ahb_read (0x00F0400C) ;
    for  ( i = 0;  i < 4;  i= i+1 ) {
        tmp = (tmp & (~(0x7 << 24))) | (i<<24) ;    
        new_set_reg (0x00F0400C, tmp);

        tmp  =  fi_ahb_read (0x00F0400C) ;
        tmp = (tmp >> 16) & (0xFF);
        rdata = rdata | (tmp << (8*i)) ;   
    }

    tmp  =  0xE0000000 ;    
                          
    new_set_reg (0x00F0400C,  tmp);

    tmp  =  0x88000000 ;    
                         
    new_set_reg (0x00F04004, tmp);

    return (rdata);
}
