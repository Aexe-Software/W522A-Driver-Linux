
#include "wifi_hal_com.h"
#include "wifi_hal_platform.h"
#include "fucode_em4.h"
#include "wifi_hif.h"
#include "wifi_common.h"
#include "version.h"
#include "wifi_drv_reg_ops.h"
#if defined (HAL_FPGA_VER) && !defined(NOT_AMLOGIC_PLATFORM)
#include <linux/amlogic/aml_gpio_consumer.h>
#endif
#include "wifi_mac_com.h"
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include "patch_fi_cmd.h"

#ifndef FILTER_NUM4
#define FILTER_NUM4 0
#endif
struct platform_wifi_gpio amlhal_gpio =
{

#if (USE_GPIO_IRQ==1)
    .gpio_irq = GPIOX_7, 
    .gpio_irq_mode = WIFI_GPIO_IRQ_LOW,
    .irq_num = 97,
    .filter_num = FILTER_NUM4,
    .clk_sel = GPIOX_20,
#endif

};
extern int wifi_irq_trigger_level(void);
extern int wifi_irq_num(void);
#if (USE_GPIO_IRQ==1)
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,14,29)
int platform_wifi_request_gpio_irq(void *data)
{
    unsigned int irq_num = wifi_irq_num();
    int ret = -1;
    int irq_trigger_type;

    amlhal_gpio.gpio_irq_mode = wifi_irq_trigger_level();
    amlhal_gpio.irq_num = irq_num;

    switch (amlhal_gpio.gpio_irq_mode)
    {
        case WIFI_GPIO_IRQ_FALLING:
            irq_trigger_type = GPIO_IRQ_FALLING;
            break;
        case WIFI_GPIO_IRQ_RISING:
            irq_trigger_type = GPIO_IRQ_RISING;
            break;
        case WIFI_GPIO_IRQ_HIGH:
            irq_trigger_type = GPIO_IRQ_HIGH;
            break;
        case WIFI_GPIO_IRQ_LOW:
            irq_trigger_type = GPIO_IRQ_LOW;
            break;
        default:
            ret = -1;
            goto exit;
    }
    do
    {
        int irq_flag;

        irq_flag = AML_GPIO_IRQ(irq_num, amlhal_gpio.filter_num, irq_trigger_type);
        msleep(10);
        pr_debug("irq_num %d irq_trigger_type %d irq_flag 0x%08x\n", irq_num,
               irq_trigger_type, irq_flag);
        ret = request_irq(irq_num, hal_irq_top, irq_flag, "WIFI_INT", data);
        if (ret < 0)
            DPRINTF(AML_DEBUG_ERROR,"%s(%d) request_irq err, ret=%d\n",__func__,__LINE__, ret);
    }
    while (0);
exit:
    return ret;
}

#else

unsigned int wifi_oob_irqflag = 0;
module_param(wifi_oob_irqflag, uint, 0644);
MODULE_PARM_DESC(wifi_oob_irqflag,
    "request_irq flags for OOB host-wake; 0=auto(TRIGGER_LOW|SHARED=0x88). "
    "0x84=HIGH|SHARED, 0x88=LOW|SHARED, 0x82=FALLING|SHARED, 0x81=RISING|SHARED");

static bool wifi_oob_irq_enable = false;
module_param(wifi_oob_irq_enable, bool, 0644);
MODULE_PARM_DESC(wifi_oob_irq_enable,
    "Enable OOB GPIO host-wake IRQ. Default off on mainline kernels because "
    "bad GPIO IRQ mappings can stall inside request_irq() during insmod.");

int platform_wifi_request_gpio_irq(void *data)
{
    unsigned int irq_num;
    int ret = -1;
    unsigned int irq_flag;

    if (!wifi_oob_irq_enable) {
        pr_warn("W522A: OOB GPIO IRQ disabled by default; skipping host-wake request_irq\n");
        amlhal_gpio.irq_num = 0;
        return -EOPNOTSUPP;
    }

    irq_num = wifi_irq_num();
    if (!irq_num) {
        pr_err("W522A: OOB GPIO IRQ resolve failed; not requesting irq 0\n");
        amlhal_gpio.irq_num = 0;
        return -EINVAL;
    }

    irq_flag = wifi_oob_irqflag ? wifi_oob_irqflag
                                : (IRQF_TRIGGER_LOW | IRQF_SHARED);

    DPRINTF(AML_DEBUG_INFO, "%s(%d) irq_flag=0x%x  irq=%d\n",
            __func__, __LINE__, irq_flag,  irq_num);

    do
    {
        ret = request_irq(irq_num, hal_irq_top, irq_flag, "WIFI_INT", data);
        if (ret < 0)
            DPRINTF(AML_DEBUG_ERROR,"%s(%d) request_irq err, ret=%d\n",__func__,__LINE__, ret);
    }
    while (0);

    amlhal_gpio.irq_num = (ret == 0) ? irq_num : 0;
    return ret;
}
#endif

void platform_wifi_free_gpio_irq(void *data)
{
    DPRINTF(AML_DEBUG_INIT, "%s(%d)\n", __func__, __LINE__);

    if (!amlhal_gpio.irq_num)
        return;

    disable_irq(amlhal_gpio.irq_num);
    free_irq(amlhal_gpio.irq_num,data);
    gpio_free(amlhal_gpio.gpio_irq);
    amlhal_gpio.irq_num = 0;
}
#endif

void platform_wifi_reset_cpu(void)
{
}

void platform_wifi_clk_source_sel(int is_ssv_clk)
{    
}

void platform_wifi_reset (void)
{
    
}

static inline void set_clk(unsigned int reg, unsigned int data)
{
    struct hw_interface* hif = hif_get_hw_interface();

    hif->hif_ops.hi_write_word(reg, data);

    OS_UDELAY(SWITCH_CLK_WAIT_US);
}

void hi_change_sram_concurrent_mode(void)
{
    unsigned int regdata;
    struct hw_interface* hif = hif_get_hw_interface();

#ifdef SRAM_CONCURRENT
#if  (SRAM_16KMODE == 0)
    
    regdata = hif->hif_ops.hi_read_word(RG_INTF_RTC_CLK_CTRL);
    regdata &= ~BIT(31);
    hif->hif_ops.hi_write_word(RG_INTF_RTC_CLK_CTRL, regdata);

    PRINT("++++++SRAM 64K++++++++++ \n");
#else 

    regdata = hif->hif_ops.hi_read_word(RG_INTF_RTC_CLK_CTRL);
    regdata |= BIT(31);
    hif->hif_ops.hi_write_word(RG_INTF_RTC_CLK_CTRL, regdata);

    PRINT("++++++SRAM 16K++++++++++ \n");
#endif  
#endif
}

void set_wifi_baudrate(unsigned int apb_clk)
{
    unsigned int data, uart_div;
    unsigned int wifi_dig_timebase;
    struct hw_interface *hif = hif_get_hw_interface();

    uart_div = apb_clk / (UART_BAUD_RATE * 4) - 1;
    PRINT("for uart baudrate0x%x\n",apb_clk);

    data = hif->hif_ops.hi_read_word(RG_UART_WORK_MODE);
    wifi_dig_timebase =  hif->hif_ops.hi_read_word(RG_INTF_CTRL_CLK);

    PRINT("uart mode 0x%x=0x%x\n", RG_UART_WORK_MODE, data);
    data &= (~(0xfff << 0)); 
    data |= uart_div;
    hif->hif_ops.hi_write_word(RG_UART_WORK_MODE, data);

    PRINT("set uart baudrate, apb_clk=%d, addr=0x%08x data=0x%08x\n",
          apb_clk, RG_UART_WORK_MODE, data);
}

int amlhal_resetmac(void)
{
    return 0;
}

int amlhal_resetsdio(void)
{
    return 0;
}

unsigned char hal_set_sys_clk_for_fpga(void)
{
    hi_change_sram_concurrent_mode();
    return 1;
}

static unsigned int bbpll_init(void)
{
    RG_DPLL_A0_FIELD_T rg_dpll_a0;
    RG_DPLL_A1_FIELD_T rg_dpll_a1;
    RG_DPLL_A2_FIELD_T rg_dpll_a2;
    RG_DPLL_A3_FIELD_T rg_dpll_a3;
    RG_DPLL_A4_FIELD_T rg_dpll_a4;
    RG_DPLL_A5_FIELD_T rg_dpll_a5;
    RG_DPLL_A6_FIELD_T rg_dpll_a6;

    rg_dpll_a0.data = 0x00000c01;
    aml_aon_write_reg(RG_DPLL_A0, rg_dpll_a0.data);

    rg_dpll_a1.data = 0x00000090;
    aml_aon_write_reg(RG_DPLL_A1, rg_dpll_a1.data);

    rg_dpll_a2.data = 0x40095000;
    aml_aon_write_reg(RG_DPLL_A2, rg_dpll_a2.data);

    rg_dpll_a3.data = 0x02fe78fd;
    aml_aon_write_reg(RG_DPLL_A3, rg_dpll_a3.data);

    rg_dpll_a4.data = 0x00050851;
    aml_aon_write_reg(RG_DPLL_A4, rg_dpll_a4.data);

    rg_dpll_a5.data = 0x00000138;
    aml_aon_write_reg(RG_DPLL_A5, rg_dpll_a5.data);

    rg_dpll_a6.data = 0x00000000;
    aml_aon_write_reg(RG_DPLL_A6, rg_dpll_a6.data);

    return 0;
}

static unsigned int bbpll_start (void)
{
    
    RG_DPLL_A2_FIELD_T rg_dpll_a2;
    RG_DPLL_A3_FIELD_T rg_dpll_a3;
    RG_DPLL_A4_FIELD_T rg_dpll_a4;
    RG_DPLL_A5_FIELD_T rg_dpll_a5;
    
    pr_debug("bbpll power on -------------->\n");
    pr_debug("1, start inter Ido \n");

    rg_dpll_a4.data = aml_aon_read_reg(RG_DPLL_A4);
    rg_dpll_a4.b.rg_wifi_bb_bt_dac_clk_div = 0x1;
    rg_dpll_a4.b.rg_wifi_bb_bt_adc_div = 0x5; 
    aml_aon_write_reg(RG_DPLL_A4, rg_dpll_a4.data);
    udelay(50);
    rg_dpll_a4.b.rg_wifi_bb_bt_dac_clk_div = 0x3;
    aml_aon_write_reg(RG_DPLL_A4, rg_dpll_a4.data);
    udelay(10);

    pr_debug("2, start pll core \n");

    rg_dpll_a3.data = aml_aon_read_reg(RG_DPLL_A3);
    rg_dpll_a3.b.rg_wifi_bb_pll_bias_en = 1;
    aml_aon_write_reg(RG_DPLL_A3, rg_dpll_a3.data);

    udelay(50);
    rg_dpll_a3.b.rg_wifi_bb_pll_rst = 0;
    aml_aon_write_reg(RG_DPLL_A3, rg_dpll_a3.data);

    udelay(80);
    rg_dpll_a2.data = aml_aon_read_reg(RG_DPLL_A2);
    rg_dpll_a2.b.rg_wifi_bb_pll_reve |= BIT(6);
    rg_dpll_a2.b.rg_wifi_bb_pll_reve |= BIT(4); 
    aml_aon_write_reg(RG_DPLL_A2, rg_dpll_a2.data);

    pr_debug("3, check \n");
    udelay(10);
    rg_dpll_a5.data = aml_aon_read_reg(RG_DPLL_A5);
    if (rg_dpll_a5.b.ro_wifi_bb_pll_done == 1)
    {
        pr_debug("bbpll done !\n");
        return 1;
    }
    else
    {
        ERROR_DEBUG_OUT("bbpll start failed !\n");
        return 0;
    }
}

static unsigned int bbpll_stop(void)
{
    RG_DPLL_A3_FIELD_T rg_dpll_a3;
    RG_DPLL_A4_FIELD_T rg_dpll_a4;

    pr_debug("bbpll power down -------------->\n");
    rg_dpll_a3.data = aml_aon_read_reg(RG_DPLL_A3);
    rg_dpll_a3.b.rg_wifi_bb_pll_bias_en = 0;
    aml_aon_write_reg(RG_DPLL_A3, rg_dpll_a3.data);
    udelay(5);
    rg_dpll_a3.b.rg_wifi_bb_pll_rst = 1;
    aml_aon_write_reg(RG_DPLL_A3, rg_dpll_a3.data);

    udelay(5);
    rg_dpll_a4.data = aml_aon_read_reg(RG_DPLL_A4);
    rg_dpll_a4.b.rg_wifi_bb_bt_dac_clk_div &= ~BIT(1);
    aml_aon_write_reg(RG_DPLL_A4, rg_dpll_a4.data);
    udelay(5);
    rg_dpll_a4.b.rg_wifi_bb_bt_dac_clk_div = 0;
    aml_aon_write_reg(RG_DPLL_A4, rg_dpll_a4.data);

    return 0;
}

static void wifi_cpu_clk_switch(unsigned int clk_cfg)
{
    struct hw_interface *hif = hif_get_hw_interface();

    hif->hif_ops.hi_write_word(RG_INTF_CPU_CLK,clk_cfg);

    pr_debug("%s(%d):cpu_clk_reg=0x%08x\n", __func__, __LINE__,
           hif->hif_ops.hi_read_word(RG_INTF_CPU_CLK));
}

extern unsigned char w1_wifi_in_insmod;
extern unsigned char w1_wifi_in_rmmod;
#ifdef ICCM_CHECK
static unsigned char buf_iccm_rd[ICCM_BUFFER_RD_LEN];
#endif
unsigned char hal_download_wifi_fw_img(void)
{
    unsigned char *bufferICCM;
    unsigned char *bufferDCCM;
    int len;
    int offset;
    int offset_base = 0;
    int rd_offset = 0;
    SYS_TYPE databyte = 0;
    unsigned int regdata;
    struct hw_interface *hif = hif_get_hw_interface();
    unsigned int to_sdio = ~(0);
    RG_PMU_A22_FIELD_T pmu_a22;
    RG_DPLL_A5_FIELD_T rg_dpll_a5;

    bufferICCM = fwICCM;
    bufferDCCM = fwDCCM;

    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_REG_BASE);
    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_ICCM_AHB_BASE);
    hif->hif_ops.hi_write_reg32(RG_SCFG_REG_FUNC, MAC_REG_BASE);
    hif->hif_ops.hi_write_reg32(RG_SCFG_EEPROM_FUNC, MAC_REG_BASE);

    hif->hif_ops.hi_write_word(RG_WIFI_RST_CTRL, to_sdio);
    PRINT("RG_SCFG_SRAM_FUNC %lx \n", hif->hif_ops.hi_read_reg32(RG_SCFG_SRAM_FUNC));

    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_REG_BASE);

    rg_dpll_a5.data = aml_aon_read_reg(RG_DPLL_A5);
    
    if (rg_dpll_a5.b.ro_wifi_bb_pll_done != 1) {
        bbpll_init();
        bbpll_start();
        pr_debug("bbpll  init ok!\n");
    } else {
        ERROR_DEBUG_OUT("bbpll  already init,not need to init!\n");
    }

    hal_set_sys_clk_for_fpga();
    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_ICCM_AHB_BASE);
    len = ALIGN(sizeof(fwICCM), 4);
    pr_debug("%s(%d): img len 0x%x, start download fw\n", __func__, __LINE__, len);

#ifdef ICCM_ROM
    
    offset = ICCM_ROM_LEN;
    len -= offset;
#else
    offset = 0;
#endif

    do {
        databyte = (len > SRAM_MAX_LEN) ? SRAM_MAX_LEN : len;
        if((offset + databyte) >= MAX_OFFSET) {
            offset_base += offset;
            hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_ICCM_AHB_BASE + offset_base);
            offset = 0;
        }

        hif->hif_ops.hi_write_sram(bufferICCM + offset + offset_base,
                                   (unsigned char*)(SYS_TYPE)offset,
                                   databyte);

        offset += databyte;
        len -= databyte;
    } while(len > 0);

#ifdef ICCM_CHECK
    offset_base = 0;
    offset = ICCM_ROM_LEN;
    len = ICCM_CHECK_LEN;
    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_ICCM_AHB_BASE);
    
    do {
        databyte = (len > SRAM_MAX_LEN) ? SRAM_MAX_LEN : len;

        if((offset + SRAM_MAX_LEN) >= MAX_OFFSET) {
            offset_base += offset;
            hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_ICCM_AHB_BASE + offset_base);
            offset = 0;
        }

        hif->hif_ops.hi_read_sram(buf_iccm_rd + rd_offset,
                                  (unsigned char*)(SYS_TYPE)offset,
                                  databyte);

        offset += databyte;
        rd_offset += databyte;
        len -= databyte;
    } while(len > 0);

    if (memcmp(buf_iccm_rd, (bufferICCM + ICCM_ROM_LEN), ICCM_CHECK_LEN)) {
        ERROR_DEBUG_OUT("Host HAL: write ICCM ERROR!!!! \n");
        return false;
    } else {
        PRINT("Host HAL: write ICCM SUCCESS!!!! \n");
    }
#endif

    len = DCCM_LEN;
    offset = 0;
    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_DCCM_AHB_BASE);

    do {
        databyte = (len > SRAM_MAX_LEN) ? SRAM_MAX_LEN : len;
        hif->hif_ops.hi_write_sram(bufferDCCM + offset,
            (unsigned char*)(SYS_TYPE)(0 + offset), databyte);

        offset += databyte;
        len -= databyte;
    } while(len > 0);

    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_REG_BASE);
    len = SRAM_LEN;
    memset(bufferDCCM, 0, len);
    PRINT("set sram zero for simulation, total=0x%x\n", len);
    hif->hif_ops.hi_write_sram(bufferDCCM, (unsigned char*)(SYS_TYPE)MAC_SRAM_BASE, len);

    hi_cfg_firmware();
    
    hif->hif_ops.hi_write_reg32(RG_SCFG_SRAM_FUNC, MAC_REG_BASE);

    regdata = hif->hif_ops.hi_read_reg32(RG_SCFG_REG_FUNC);
    PRINT("RG_SCFG_REG_FUNC redata %x \n", regdata);
    
#ifdef PROJECT_T9026
    wifi_cpu_clk_switch(0x4f070033);
    wifi_cpu_clk_switch(0x4f0700f3);

    if (0) {
        
        regdata = hif->hif_ops.hi_read_word(RG_WIFI_MAC_ARC_CTRL);
        
        regdata |= CPU_RUN;
        hif->hif_ops.hi_write_word(RG_WIFI_MAC_ARC_CTRL, regdata);
    } else {
        
        regdata = hif->hif_ops.hi_read_word(RG_WIFI_CPU_CTRL);
        regdata |= 0x10000;
        hif->hif_ops.hi_write_word(RG_WIFI_CPU_CTRL, regdata);
        PRINT("RG_WIFI_CPU_CTRL = %x redata= %x \n", RG_WIFI_CPU_CTRL, regdata);
        
        pmu_a22.data = hif->hif_ops.bt_hi_read_word(RG_PMU_A22);
        pmu_a22.b.rg_dev_reset_sw = 0x00;
        hif->hif_ops.bt_hi_write_word(RG_PMU_A22, pmu_a22.data);
        PRINT("RG_PMU_A22 = %x redata= %x \n", RG_PMU_A22, pmu_a22.data);
    }

#elif defined (PROJECT_W1)

    wifi_cpu_clk_switch(0x4f770033);
    
    hif->hif_ops.hi_write_word(RG_INTF_MAC_CLK, 0x00030001);
    if (aml_wifi_is_enable_rf_test())
        hal_dpd_memory_download();

    hif->hif_ops.hi_write_word(CMD_DOWN_FIFO_FDH_ADDR, 0);
    hif->hif_ops.hi_write_word(CMD_DOWN_FIFO_FDT_ADDR, 0);
    hif->hif_ops.hi_write_word(RG_WIFI_IF_RXPAGE_BUF_RDPTR, 0);
    hif->hif_ops.hi_write_word(RG_WIFI_IF_MAC_TXTABLE_RD_ID, 0);

    regdata = hif->hif_ops.hi_read_word(RG_WIFI_CPU_CTRL);
    regdata |= 0x10000;
    hif->hif_ops.hi_write_word(RG_WIFI_CPU_CTRL, regdata);
    PRINT("RG_WIFI_CPU_CTRL = %x redata= %x \n", RG_WIFI_CPU_CTRL, regdata);
    
    pmu_a22.data = hif->hif_ops.bt_hi_read_word(RG_PMU_A22);
    pmu_a22.b.rg_dev_reset_sw = 0x00;
    hif->hif_ops.bt_hi_write_word(RG_PMU_A22, pmu_a22.data);
    PRINT("RG_PMU_A22 = %x redata= %x \n", RG_PMU_A22, pmu_a22.data);
#endif

    pr_debug("fw download success!\n");
#ifdef SDIO_BUILD_IN
    w1_wifi_in_insmod = 0;
#endif

    return true;
}

int mac_addr0 = 0x00;
int mac_addr1 = 0x01;
int mac_addr2 = 0x02;
int mac_addr3 = 0x58;
int mac_addr4 = 0x00;
int mac_addr5 = 0xcc;

static int vif0opmode = -1;
static int vif1opmode = -1;

static char *vmac0 = "w522a";
static char *vmac1 = "p2p-w522a%d";
static unsigned int con_mode = (1 << WIFINET_M_STA);
static int en_rf_test = 0;
static int single_vif = 1;

static char *plt_ver = NULL;
struct version_info version_map[] = {
    {"gva", VERSION_GVA},
    {"gva_mrt", VERSION_GVA_MRT}

};

int aml_debug = AML_DEBUG_LEVEL;
int w522a_debug_printk = 0;
const unsigned char BROADCAST_ADDRESS[WIFINET_ADDR_LEN] = {0xff,0xff,0xff,0xff,0xff,0xff};
static char *mac_addr = "00:01:02:58:00:CC";
static char *country_code = "WW";
unsigned short dhcp_offload = 0;
static int sdblksize = BLKSIZE;
unsigned char aml_insmod_flag = 0;
char *hif_type = "SDIO";

#ifdef CONFIG_MAC_SUPPORT
extern u8 *wifi_get_mac(void);
#endif
extern void print_driver_version(void);

void aml_wifi_set_mac_addr(void)
{
    u64 timestamp;
#ifdef CONFIG_MAC_SUPPORT
    u8 addr[ETH_ALEN];
    u8 cbuf[50];
#if defined(NOT_AMLOGIC_PLATFORM)
    memset(addr, 0xff, ETH_ALEN);
#else
    memcpy(addr, wifi_get_mac(), ETH_ALEN);
#endif
    if (addr[0] != 0xff ||
        aml_read_macaddr_from_file(WIFIMAC_PATH, addr) == true) {
        mac_addr0 = addr[0];
        mac_addr1 = addr[1];
        mac_addr2 = addr[2];
        mac_addr3 = addr[3];
        mac_addr4 = addr[4];
        mac_addr5 = addr[5];
    } else {
        unsigned int efuse_data_l;
        unsigned int efuse_data_h;

        efuse_data_l = efuse_manual_read(0x1);
        efuse_data_h = efuse_manual_read(0x2);

        if ((efuse_data_h != 0) && (efuse_data_l != 0)) {
            mac_addr0 = (efuse_data_h & 0xff00) >> 8;
            mac_addr1 = efuse_data_h & 0x00ff;
            mac_addr2 = (efuse_data_l & 0xff000000) >> 24;
            mac_addr3 = (efuse_data_l & 0x00ff0000) >> 16;
            mac_addr4 = (efuse_data_l & 0xff00) >> 8;
            mac_addr5 = efuse_data_l & 0xff;
            aml_retrieve_from_file(WIFIMAC_PATH, cbuf, 48);

            sprintf(cbuf + 21, MAC_FMT, mac_addr0, mac_addr1, mac_addr2,
                    mac_addr3, mac_addr4, mac_addr5);
            if (aml_store_to_file(WIFIMAC_PATH, cbuf, strlen(cbuf)) > 0) {
                pr_debug("write the efuse mac to wifimac.txt\n");
            }
        } else {
#endif
            timestamp = get_jiffies_64();
            mac_addr3 = (timestamp & 0xff);
            mac_addr4 = ((timestamp >> 2) & 0xff);
            mac_addr5 = ((timestamp >> 4) & 0xff);
#ifdef CONFIG_MAC_SUPPORT
            aml_retrieve_from_file(WIFIMAC_PATH, cbuf, 48);

            sprintf(cbuf + 21, MAC_FMT, mac_addr0, mac_addr1, mac_addr2,
                    mac_addr3, mac_addr4, mac_addr5);
            if (aml_store_to_file(WIFIMAC_PATH, cbuf, strlen(cbuf)) > 0)
                pr_debug("write the random mac to wifimac.txt\n");
        }
    }
#endif

    if (mac_addr0 & 0x3) {
        pr_debug("change the mac addr from [0x%x] ", mac_addr0);

        mac_addr0 &= ~0x3;
        pr_debug("to [0x%x] \n", mac_addr0);
#ifdef CONFIG_MAC_SUPPORT
        aml_retrieve_from_file(WIFIMAC_PATH, cbuf, 48);

        sprintf(cbuf + 21, MAC_FMT, mac_addr0, mac_addr1, mac_addr2,
                mac_addr3, mac_addr4, mac_addr5);
        if (aml_store_to_file(WIFIMAC_PATH, cbuf, strlen(cbuf)) > 0)
            pr_debug("write the random mac to wifimac.txt\n");
#endif
    }
}

char *aml_wifi_get_country_code(void)
{
    return country_code;
}

int aml_wifi_get_vif0_opmode(void)
{
    return vif0opmode;
}

int aml_wifi_get_vif1_opmode(void)
{
    return vif1opmode;
}

char *aml_wifi_get_vif0_name(void)
{
    return vmac0;
}

char *aml_wifi_get_vif1_name(void)
{
    return vmac1;
}

unsigned int aml_wifi_get_con_mode(void)
{
    return con_mode;
}

int aml_wifi_get_single_vif(void)
{
    return single_vif;
}

void aml_wifi_set_con_mode(void *wifimac)
{
    unsigned int concurrent_mode = 0;
    struct drv_private *drv_priv = ((struct wifi_mac *)wifimac)->drv_priv;
    struct wlan_net_vif *main_vmac = drv_priv->drv_wnet_vif_table[NET80211_MAIN_VMAC];
    struct wlan_net_vif *p2p_vmac = drv_priv->drv_wnet_vif_table[NET80211_P2P_VMAC];

    if (main_vmac != NULL)
        concurrent_mode |= BIT(main_vmac->vm_opmode);
    if (p2p_vmac != NULL)
        concurrent_mode |= BIT(p2p_vmac->vm_opmode);
    if (concurrent_mode == 0)
        concurrent_mode = (1 << WIFINET_M_STA);
    if (con_mode != concurrent_mode) {
        con_mode = concurrent_mode;
        AML_OUTPUT("con_mode = 0x%02x",con_mode);
    }

}

char *aml_wifi_get_bus_type(void)
{
    return hif_type;
}

char *aml_wifi_get_fw_type(void)
{
    unsigned int regdata = hi_get_fw_version();
    if (regdata == FW_VERSION_W1) {
        return "w1";
    }
    else if (regdata == FW_VERSION_W1U) {
        return "w1u";
    }
    else {
        return "unknown";
    }
}

unsigned int aml_wifi_get_platform_verid(void)
{
    int i;

    if (plt_ver == NULL)
    {
        return 0;
    }

    for (i = 0; i < sizeof(version_map)/sizeof(struct version_info); ++i)
    {
        if (strcmp(version_map[i].version_name, plt_ver) == 0)
        {
            
            return version_map[i].version_id;
        }
    }
    return 0;
}

unsigned int aml_wifi_is_enable_rf_test(void)
{
    return en_rf_test;
}

extern unsigned char w1_wifi_in_insmod;
extern unsigned char w1_wifi_in_rmmod;
static int aml_insmod(void)
{
    int ret = 0;
    struct hw_interface * hif = hif_get_hw_interface();

#ifdef SDIO_BUILD_IN
    w1_wifi_in_insmod = 1;
#endif

    print_driver_version();
    pr_info("W522A: driver version: %s\n", DRIVERVERSION);
    pr_debug("%s(%d) dhcp_offload %d set done.\n\n",
        __func__,__LINE__, dhcp_offload);

    if(hif == NULL)
    {
        ERROR_DEBUG_OUT("hif = %p\n",hif);
        ret =  -1;
        goto insmod_failed;
    }
    memset(hif, 0, sizeof(struct hw_interface));

    ret = aml_sdio_init();
    if (ret) {
        goto  insmod_failed;
    }

    if (aml_wifi_is_enable_rf_test()) {
        pr_debug("---Aml drv---:Before calling %s:(%d) \n",__func__,__LINE__);
        Init_B2B_Resource();
    }

    aml_insmod_flag = 1;

    pr_debug("%s(%d) start...\n",__func__, __LINE__);
    return 0;

insmod_failed:
#ifdef SDIO_BUILD_IN
    w1_wifi_in_insmod = 0;
#endif

    return ret;
}

static void aml_rmmod(void)
{
#ifdef SDIO_BUILD_IN
    w1_wifi_in_rmmod = 1;
#endif
    pr_debug("===================aml_rmmod start====================\n");
    hal_exit_priv();
    pr_debug("===================aml_rmmod end====================\n");
    aml_insmod_flag = 0;
    
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0))
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0))
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
#endif

MODULE_LICENSE("GPL");
module_init(aml_insmod);
module_exit(aml_rmmod);

module_param(vif0opmode, int, S_IRUGO);
module_param(vif1opmode, int, S_IRUGO);
module_param(vmac0, charp, S_IRUGO);
module_param(vmac1, charp, S_IRUGO);
module_param(con_mode, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(single_vif, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param(plt_ver, charp, S_IRUGO);
module_param(sdblksize, int, S_IRUGO);
module_param(en_rf_test, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param_named(amldebug, aml_debug, int, 0644);
MODULE_PARM_DESC(amldebug, "Amlogic debug mask used by DPRINTF (0xffffffff enables all groups)");

module_param_named(debug_printk, w522a_debug_printk, int, 0644);
MODULE_PARM_DESC(debug_printk, "Route DPRINTF/AML_PRINT to dmesg (0=pr_debug, 1=ratelimited printk, 2=full printk)");

module_param(mac_addr, charp, 0644);
MODULE_PARM_DESC(mac_addr, "A string variable to describe wifi mac address");

module_param(dhcp_offload, ushort, S_IRUGO);
MODULE_PARM_DESC(dhcp_offload, "A short variable to control dhcp offload function");

module_param(country_code, charp, 0644);
MODULE_PARM_DESC(country_code,"A string variable to describe country code");
