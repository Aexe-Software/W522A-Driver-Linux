
#include "wifi_hal_com.h"
#include "wifi_hif.h"

static const struct reg_table wifi_fpga_regtable[] =
{
    
#ifdef WITH_RF
    { 0x0000b080, 0x00001100},
    { 0x0000b228, 0x004000a0 }, 
#else
    { 0x0000b080, 0x00001010},
    { 0x00008090, 0x00000104},
#endif
    
};

int get_snr(void) {
    int snr = 0;
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int reg_tmp = 0x01;

    hif->hif_ops.hi_write_word(RG_PHY_ADR_2C20, reg_tmp);
    snr = hif->hif_ops.hi_read_word(RG_PHY_STS_REG_0 + 3 *sizeof(unsigned int));

    return (snr & 0xffff);
}

void phy_stc(void)
{
    int i;
    unsigned int reg[8] = {0};
    unsigned int trig_num[4] = {0};
    unsigned int min_num[4]  = {0};
    unsigned int max_num[4]  = {0};
    unsigned int avg_num[4]  = {0};

    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int reg_tmp = 0x01;

    hif->hif_ops.hi_write_word(RG_PHY_ADR_2C20, reg_tmp);

    for(i = 0; i< 8; i++)
    {
        reg[i] = hif->hif_ops.hi_read_word(RG_PHY_STS_REG_0 + i *sizeof(unsigned int));
    }
        
    trig_num[0] = reg[0]&0xffff;
    trig_num[1] = (reg[0]>>16)&0xffff;
    trig_num[2] = reg[4]&0xffff;
    trig_num[3] = (reg[4]>>16)&0xffff;

    min_num[0] = (reg[1]>>16)&0xffff;
    min_num[1] = (reg[3]>>16)&0xffff;
    min_num[2] = (reg[5]>>16)&0xffff;
    min_num[3] = (reg[7]>>16)&0xffff;   

    max_num[0] = (reg[1])&0xffff;
    max_num[1] = (reg[3])&0xffff;
    max_num[2] = (reg[5])&0xffff;
    max_num[3] = (reg[7])&0xffff;   

    avg_num[0] = reg[2]&0xffff;
    avg_num[1] = (reg[2]>>16)&0xffff;
    avg_num[2] = reg[6]&0xffff;
    avg_num[3] = (reg[6]>>16)&0xffff;

    pr_debug("phy statistic(dec): \n");
    pr_debug("0) CP1   detect:%8d  avg:%8d  min:%8d  max:%8d  \n", trig_num[0],avg_num[0],min_num[0],max_num[0]);
    pr_debug("1) L-SIG   SNR :%8d  avg:%8d  min:%8d  max:%8d  \n", trig_num[1],avg_num[1],min_num[1],max_num[1]);
    pr_debug("2) data CRC err:%8d  avg:%8d  min:%8d  max:%8d  \n", trig_num[2],avg_num[2],min_num[2],max_num[2]);
    pr_debug("3) data CRC OK:%8d  avg:%8d  min:%8d  max:%8d  \n", trig_num[3],avg_num[3],min_num[3],max_num[3]);

}

unsigned int cca_busy_check(void)
{
    unsigned int v0 = 0, v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    struct AGC_OB_CCA_RES_BITS *data0 = NULL;
    struct AGC_STF_LIMIT_CCA_COND_EN_BITS *data1 = NULL;
    struct AGC_CCA_COND_TS_CTRL_BITS *data3 = NULL;
    unsigned int reg_tmp = 0x13000026;
    struct hw_interface* hif = hif_get_hw_interface();
     
    set_reg_fragment(RG_AGC_STF_LIMIT_CCA_COND_EN, 28,20,0x1ff);
    hif->hif_ops.hi_write_word(RG_AGC_STS_REG_0, reg_tmp);
    v0 = hif->hif_ops.hi_read_word(RG_AGC_OB_CCA_RES);
    data0 = (struct AGC_OB_CCA_RES_BITS *)&v0;
    v1 = hif->hif_ops.hi_read_word(RG_AGC_STF_LIMIT_CCA_COND_EN);
    data1 = (struct AGC_STF_LIMIT_CCA_COND_EN_BITS *)&v1;
    v3 =  hif->hif_ops.hi_read_word(RG_AGC_CCA_COND_TS_CTRL);
    
    data3 = (struct AGC_CCA_COND_TS_CTRL_BITS *)&v3;
    data3->cca_cond_ts_num = 0x0ffff;
    data3->cca_cond_ts = 0xc8;

    hif->hif_ops.hi_write_word(RG_AGC_CCA_COND_TS_CTRL, v3);
    if (data0->cca_cond_rdy && data1->cca_cond_en)
    {
        v2 = hif->hif_ops.hi_read_word(RG_AGC_OB_CCA_COND01);
        v4 = hif->hif_ops.hi_read_word(RG_AGC_OB_CCA_COND23);
        pr_debug("cca: ts 0x%x, ts_num %d, cond0 %d cond1 %d cond2 %d cond3 %d \n",
                data3->cca_cond_ts, data3->cca_cond_ts_num,
                v2 & 0xffff,  (v2 >> 16) & 0xffff,  v4 & 0xffff, (v4 >> 16 ) & 0xffff);
    }

    return 0;

}

void get_phy_stc_info(unsigned int *arr)
{
    int i;
    unsigned int reg[8] = {0};
    unsigned int trig_num[4] = {0};
    unsigned int min_num[4]  = {0};
    unsigned int max_num[4]  = {0};
    unsigned int avg_num[4]  = {0};
    unsigned int noise_floor = 0;

    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int reg_tmp = 0x01;

    hif->hif_ops.hi_write_word(RG_PHY_ADR_2C20, reg_tmp);

    udelay(320);

    for (i = 0; i < 8; i++) {
        reg[i] = hif->hif_ops.hi_read_word(RG_PHY_STS_REG_0 + i *sizeof(unsigned int));
    }
    noise_floor = hif->hif_ops.hi_read_word(RG_AGC_OB_ANT_NFLOOR);

    trig_num[0] = reg[0] & 0xffff;
    trig_num[1] = (reg[0] >> 16) & 0xffff;
    trig_num[2] = reg[4] & 0xffff;
    trig_num[3] = (reg[4] >> 16) & 0xffff;

    min_num[0] = (reg[1] >> 16) & 0xffff;
    min_num[1] = (reg[3] >> 16) & 0xffff;
    min_num[2] = (reg[5] >> 16) & 0xffff;
    min_num[3] = (reg[7] >> 16) & 0xffff;

    max_num[0] = (reg[1]) & 0xffff;
    max_num[1] = (reg[3]) & 0xffff;
    max_num[2] = (reg[5]) & 0xffff;
    max_num[3] = (reg[7]) & 0xffff;

    avg_num[0] = reg[2] & 0xffff;
    avg_num[1] = (reg[2] >> 16) & 0xffff;
    avg_num[2] = reg[6] & 0xffff;
    avg_num[3] = (reg[6] >> 16) & 0xffff;

    arr[0] = avg_num[0]; 
    arr[1] = avg_num[1]; 
    arr[2] = avg_num[2]; 
    arr[3] = avg_num[3]; 
    arr[4] = (noise_floor >> 12) & 0x3ff;
    arr[5] = noise_floor & 0x3ff;
}

void phy_register_set(void)
{

#ifdef WITH_RF
#ifndef RF_T9026
    hif->hif_ops.hi_write_word(RG_IQ_SWAP_CTRL, 0x01100002);
#endif
#else
    hif->hif_ops.hi_write_word(RG_ADDA_ADR_88, 0x200a0000);
#endif

}

void coexit_bt_thread_enable(void)
{
    unsigned int bt_prd,bt_act,bt_offset;
    struct hw_interface* hif = hif_get_hw_interface();
    unsigned int testbus_num = 6;

    bt_prd = 7500 * 10;
    bt_act = 1500 * 10;
    bt_offset = 3500;
    pr_debug("%s(%d)\n",__func__,__LINE__);
    
    hif->hif_ops.hi_write_word(RG_COEX_RF_STABLE_CTRL, 0x03002000 | (testbus_num << 26));
    
    hif->hif_ops.hi_write_word(RG_COEX_PRIORITY_M6, 0x2);
    hif->hif_ops.hi_write_word(RG_COEX_PRD_NUM_M6, bt_prd);
    hif->hif_ops.hi_write_word(RG_COEX_ACT_NUM_M6, bt_act);
    hif->hif_ops.hi_write_word(RG_COEX_OFFSET_NUM_M6, bt_offset);
    
    hif->hif_ops.hi_write_word(RG_COEX_INT_OFFSET_M6, bt_act + 5);
    hif->hif_ops.hi_write_word(RG_COEX_INI2_OFFSET_M6, 2000);
    hif->hif_ops.hi_write_word(RG_COEX_BT_OWNER_CTRL, 0x1001003f);
    set_reg_fragment(RG_COEX_HS5W_MANUAL4,0,0,1);
    
    pr_debug("%s(%d) BT priority=%d (%dus %dus %dus)--\n",
        __func__,__LINE__,hif->hif_ops.hi_read_word(RG_COEX_PRIORITY_M6),
        bt_prd,bt_act,bt_offset);
}
