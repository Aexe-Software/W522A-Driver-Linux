
#include "wifi_mac_com.h"
#include "wifi_rate_ctrl.h"

void drv_hal_attach( void *   drv_priv,void *cbptr)
{
    struct hal_private* hal_priv = hal_get_priv();
    hal_priv->hal_call_back = cbptr;

    hal_priv->dhcp_offload = dhcp_offload;
    hal_priv->hal_ops.hal_init(drv_priv);
    drv_hal_workitem_inital();
    return;
}

int drv_hal_detach(void)
{
    struct hal_private* hal_priv = hal_get_priv();

    hal_priv->hal_ops.hal_exit();
    drv_hal_workitem_free();
    pr_debug("<running> %s %d \n",__func__,__LINE__);
    return 0;
}

void drv_hal_setupratetable(struct drv_rate_table *rt)
{
    int i;
    unsigned char code;

    if (rt->dot11rate_to_idx[0] != 0)
    {
        
        return;
    }

    memset( rt->dot11rate_to_idx, 0xff, sizeof(rt->dot11rate_to_idx));
    for (i = 0; i < rt->rateCount; i++)
    {
        unsigned char cix;

        if ((rt->info[i].phy == CCK) || (rt->info[i].phy == WOFDM))
        {
            code = WIFINET_GET_RATE_VAL (rt->info[i].dot11Rate);
        }
        else
        {
            code = rt->info[i].vendor_rate_code;
        }
        cix = rt->info[i].controlRate;

        rt->dot11rate_to_idx[code] = i;
        DPRINTF(AML_DEBUG_RATE,"%s(%d) dot11rate_to_idx[0x%x]=%d\n", __func__, __LINE__, code, i);
        
        rt->info[i].lpAckDuration = drv_hal_calc_txtime(rt,WLAN_CTRL_FRAME_SIZE, cix, 0);
        rt->info[i].spAckDuration = drv_hal_calc_txtime(rt,WLAN_CTRL_FRAME_SIZE, cix, 1);
    }
}

struct drv_rate_table amluno_11bgnac_table =
{
    30,  
    { 0 },
    {
        
         {  HAL_RATECTRL|HAL_SUPPORT,     CCK,      1000,    WIFI_11B_1M,    0x00, (WIFINET_RATE_BASIC| 2),   0 },
         {  HAL_RATECTRL|HAL_SUPPORT,     CCK,      2000,    WIFI_11B_2M,    0x04, (WIFINET_RATE_BASIC| 4),   1 },
        {  HAL_RATECTRL|HAL_SUPPORT,     CCK,      5500,    WIFI_11B_5M,    0x04, (WIFINET_RATE_BASIC|11),   2 },
         {  HAL_RATECTRL|HAL_SUPPORT,     CCK,     11000,   WIFI_11B_11M,    0x04, (WIFINET_RATE_BASIC|22),   3 },
          {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,    6000,    WIFI_11G_6M,    0x00,        12,   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,    9000,    WIFI_11G_9M,    0x00,        18,   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   12000,   WIFI_11G_12M,    0x00,        24,   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   18000,   WIFI_11G_18M,    0x00,        36,   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   24000,   WIFI_11G_24M,    0x00,        48,   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   36000,   WIFI_11G_36M,    0x00,        72,   8 },
        {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   48000,   WIFI_11G_48M,    0x00,        96,   8 },
        {  HAL_RATECTRL|HAL_SUPPORT,     WOFDM,   54000,   WIFI_11G_54M,    0x00,       108,   8 },

         { HAL_RATECTRL|HAL_SUPPORT,     HT,       6500,      WIFI_11N_MCS0,    0x00,         0,   4 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      13000,      WIFI_11N_MCS1,    0x00,         1,   6 },
        { HAL_RATECTRL|HAL_SUPPORT,     HT,      19500,      WIFI_11N_MCS2,    0x00,         2,   6 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      26000,      WIFI_11N_MCS3,    0x00,         3,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      39000,      WIFI_11N_MCS4,    0x00,         4,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      52000,      WIFI_11N_MCS5,    0x00,         5,   8 },
        { HAL_RATECTRL|HAL_SUPPORT,     HT,      58500,      WIFI_11N_MCS6,    0x00,         6,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      65000,      WIFI_11N_MCS7,    0x00,         7,   8 },

           { HAL_RATECTRL|HAL_SUPPORT,     HT,       7200,      WIFI_11N_MCS0,    0x00,         0,   4 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      14200,      WIFI_11N_MCS1,    0x00,         1,   6 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      21700,      WIFI_11N_MCS2,    0x00,         2,   6 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      28900,      WIFI_11N_MCS3,    0x00,         3,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      43300,      WIFI_11N_MCS4,    0x00,         4,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      57800,      WIFI_11N_MCS5,    0x00,         5,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      65000,      WIFI_11N_MCS6,    0x00,         6,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,      72200,      WIFI_11N_MCS7,    0x00,         7,   8 },

         { HAL_RATECTRL|HAL_SUPPORT,     HT,      13500,      WIFI_11N_MCS0,    0x00,         0,   4 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      27000,      WIFI_11N_MCS1,    0x00,         1,   6 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      40500,      WIFI_11N_MCS2,    0x00,         2,   6 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      54000,      WIFI_11N_MCS3,    0x00,         3,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      81000,      WIFI_11N_MCS4,    0x00,         4,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,     108000,      WIFI_11N_MCS5,    0x00,         5,   8 },
        { HAL_RATECTRL|HAL_SUPPORT,     HT,     121500,      WIFI_11N_MCS6,    0x00,         6,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,     135000,      WIFI_11N_MCS7,    0x00,         7,   8 },

         { HAL_RATECTRL|HAL_SUPPORT,     HT,      15000,      WIFI_11N_MCS0,    0x00,         0,   4 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      30000,      WIFI_11N_MCS1,    0x00,         1,   6 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      45000,      WIFI_11N_MCS2,    0x00,         2,   6 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      60000,      WIFI_11N_MCS3,    0x00,         3,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,      90000,      WIFI_11N_MCS4,    0x00,         4,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,     120000,      WIFI_11N_MCS5,    0x00,         5,   8 },
          { HAL_RATECTRL|HAL_SUPPORT,     HT,     135000,      WIFI_11N_MCS6,    0x00,         6,   8 },
         { HAL_RATECTRL|HAL_SUPPORT,     HT,     150000,      WIFI_11N_MCS7,    0x00,         7,   8 },

         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       6500,       WIFI_11AC_MCS0,       0x00,          0,                   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      13000,       WIFI_11AC_MCS1,        0x00,         1,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      19500,       WIFI_11AC_MCS2,        0x00,         2,                   6 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      26000,       WIFI_11AC_MCS3,        0x00,         3,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      39000,       WIFI_11AC_MCS4,        0x00,         4,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      52000,       WIFI_11AC_MCS5,        0x00,         5,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      58500,       WIFI_11AC_MCS6,        0x00,         6,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      65000,       WIFI_11AC_MCS7,        0x00,         7,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      78000,       WIFI_11AC_MCS8,        0x00,         8,                   8 },

         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       7200,       WIFI_11AC_MCS0,        0x00,         0,                   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      14400,       WIFI_11AC_MCS1,        0x00,         1,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      21700,       WIFI_11AC_MCS2,        0x00,         2,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      28900,       WIFI_11AC_MCS3,        0x00,         3,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      43300,       WIFI_11AC_MCS4,        0x00,         4,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      57800,       WIFI_11AC_MCS5,        0x00,         5,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      65000,       WIFI_11AC_MCS6,        0x00,         6,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      72200,       WIFI_11AC_MCS7,        0x00,         7,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      86700,       WIFI_11AC_MCS8,        0x00,         8,                   8 },

         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       13500,      WIFI_11AC_MCS0,       0x00,         0,                   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       27000,      WIFI_11AC_MCS1,       0x00,         1,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       40500,      WIFI_11AC_MCS2,       0x00,         2,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       54000,      WIFI_11AC_MCS3,       0x00,         3,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       81000,      WIFI_11AC_MCS4,       0x00,         4,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      108000,      WIFI_11AC_MCS5,       0x00,         5,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      121500,      WIFI_11AC_MCS6,       0x00,         6,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      135000,      WIFI_11AC_MCS7,       0x00,         7,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      162000,      WIFI_11AC_MCS8,       0x00,         8,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      180000,      WIFI_11AC_MCS9,       0x00,         9,                   8 },

         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       15000,      WIFI_11AC_MCS0,       0x00,         0,                  4 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       30000,      WIFI_11AC_MCS1,       0x00,         1,                  6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       45000,      WIFI_11AC_MCS2,       0x00,         2,                  6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       60000,      WIFI_11AC_MCS3,       0x00,         3,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       90000,      WIFI_11AC_MCS4,       0x00,         4,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      120000,      WIFI_11AC_MCS5,       0x00,         5,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      135000,      WIFI_11AC_MCS6,       0x00,         6,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      150000,      WIFI_11AC_MCS7,       0x00,         7,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      180000,      WIFI_11AC_MCS8,       0x00,         8,                  8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      200000,      WIFI_11AC_MCS9,       0x00,         9,                  8 },

         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       29300,      WIFI_11AC_MCS0,       0x00,         0,                   4 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       58500,      WIFI_11AC_MCS1,       0x00,         1,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       87800,      WIFI_11AC_MCS2,       0x00,         2,                   6 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      117000,      WIFI_11AC_MCS3,       0x00,         3,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      175500,      WIFI_11AC_MCS4,       0x00,         4,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      243000,      WIFI_11AC_MCS5,       0x00,         5,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      263300,      WIFI_11AC_MCS6,       0x00,         6,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      292500,      WIFI_11AC_MCS7,       0x00,         7,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      351000,      WIFI_11AC_MCS8,       0x00,         8,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      390000,      WIFI_11AC_MCS9,       0x00,         9,                   8 },

          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       32500,      WIFI_11AC_MCS0,       0x00,         0,                   4 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       65000,      WIFI_11AC_MCS1,       0x00,         1,                   6 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,       97500,      WIFI_11AC_MCS2,       0x00,         2,                   6 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      130000,      WIFI_11AC_MCS3,       0x00,         3,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      195000,      WIFI_11AC_MCS4,       0x00,         4,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      260000,      WIFI_11AC_MCS5,       0x00,         5,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      292500,      WIFI_11AC_MCS6,       0x00,         6,                   8 },
          {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      325000,      WIFI_11AC_MCS7,       0x00,         7,                   8 },
           {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      390000,      WIFI_11AC_MCS8,       0x00,         8,                   8 },
         {  HAL_RATECTRL|HAL_SUPPORT,    VHT_PHY,      433300,      WIFI_11AC_MCS9,       0x00,         9,                   8 },

    },
};

#undef  WOFDM
#undef  CCK
#undef   HT
#undef   VHT_PHY
struct drv_rate_table *drv_hal_get_rate_tbl(int mode)
{
    struct drv_rate_table *rt = NULL;

    switch (mode)
    {
        case WIFINET_MODE_11B:
        case WIFINET_MODE_11BG:
        case WIFINET_MODE_11G:
        case WIFINET_MODE_11GN:
        case WIFINET_MODE_11N:
        case WIFINET_MODE_11BGN:
        case WIFINET_MODE_11AC :
        case WIFINET_MODE_11NAC :
        case WIFINET_MODE_11GNAC :
            rt = &amluno_11bgnac_table;
            break;
        default:
            DPRINTF( AML_DEBUG_HAL|AML_DEBUG_ERROR, "%s: invalid mode 0x%x\n", __func__, mode);
            return NULL;
    }
    DPRINTF(AML_DEBUG_RATE, "%s(%d): mode=0x%x\n", __func__, __LINE__, mode);

    return rt;
}

unsigned short
drv_hal_calc_txtime(const struct drv_rate_table *rates,
                      unsigned int frameLen,
                      unsigned short rateix,
                      int shortPreamble)
{
    unsigned int numBits, phyTime, txTime;
    unsigned int kbps;

    kbps = rates->info[rateix].rateKbps;
    
    if (kbps == 0) return 0;
    switch (rates->info[rateix].phy)
    {
        case WIFINET_T_CCK:
            phyTime = CCK_PREAMBLE_BITS + CCK_PLCP_BITS;
            if (shortPreamble && rates->info[rateix].shortPreamble)
                phyTime >>= 1;

            numBits = frameLen << 3;
            txTime = CCK_SIFS_TIME + phyTime + ((numBits * 1000)/kbps);
            break;

        case WIFINET_T_OFDM:
        {
            unsigned int bitsPerSymbol, numSymbols;

            bitsPerSymbol = (kbps * OFDM_SYMBOL_TIME) / 1000;

            numBits = OFDM_PLCP_BITS + (frameLen << 3);
            numSymbols = howmany(numBits, bitsPerSymbol);
            txTime = OFDM_SIFS_TIME + OFDM_PREAMBLE_TIME + (numSymbols * OFDM_SYMBOL_TIME);
        }
        break;

        default:
            txTime = 0;
            break;
    }
    return txTime;
}

unsigned char drv_hal_wnet_vif_staid(unsigned char vm_opmode,unsigned short sta_associd)
{
    return (vm_opmode == WIFINET_M_STA )? 1:sta_associd&0xff;
}
unsigned short drv_hal_staid(enum hal_op_mode hal_opmode,unsigned short sta_associd)
{
    return (hal_opmode == WIFI_M_STA )? 1:sta_associd&0x3fff;
}
unsigned char drv_hal_nsta_staid(struct wifi_station *sta)
{
    if (sta == NULL || sta->sta_wnet_vif == NULL)
        return 0;

    return drv_hal_wnet_vif_staid(sta->sta_wnet_vif->vm_opmode,sta->sta_associd);
}

static void
drv_hal_workitem_task(SYS_TYPE iparam)
{
    struct hal_private *hal_priv = hal_get_priv();
    unsigned char *EltPtr =NULL;
    struct _CO_SHARED_FIFO* pWorkFifo = NULL;
    struct hal_work_task *pWorkTask = NULL;

    if (hal_priv == NULL)
        return;

    pWorkFifo = &hal_priv->WorkFifo;
    while (CO_SharedFifoEmpty(pWorkFifo, CO_WORK_FREE))
    {
        unsigned short STATUS;

        STATUS = CO_SharedFifoGet(pWorkFifo, CO_WORK_FREE, 1, &EltPtr);
        if (STATUS != CO_STATUS_OK || EltPtr == NULL) {
            ERROR_DEBUG_OUT("work fifo get failed\n");
            break;
        }
        pWorkTask = (struct hal_work_task *)EltPtr;

        if (pWorkTask->task != NULL)
        {
            pWorkTask->task(pWorkTask->param1, pWorkTask->param2, pWorkTask->param3,
                pWorkTask->param4, pWorkTask->param5);
        }

        if (pWorkTask->taskcallback != NULL)
        {
            pWorkTask->taskcallback(pWorkTask->param1, pWorkTask->param2, pWorkTask->param3,
                pWorkTask->param4, pWorkTask->param5);
        }

        STATUS = CO_SharedFifoPut(pWorkFifo, CO_WORK_FREE, 1);
        if (STATUS != CO_STATUS_OK) {
            ERROR_DEBUG_OUT("work fifo put free failed\n");
            break;
        }
    }
}

void drv_hal_workitem_free(void)
{

}

int drv_hal_add_workitem(WorkHandler task, WorkHandler taskcallback, SYS_TYPE param1,
    SYS_TYPE param2, SYS_TYPE param3, SYS_TYPE param4, SYS_TYPE param5)
{
    struct hal_private* hal_priv = hal_get_priv();
    unsigned short STATUS;
    unsigned char *EltPtr = NULL;
    struct _CO_SHARED_FIFO *pWorkFifo = NULL;
    struct hal_work_task *pWorkTask = NULL;
    static void *task_p = NULL;
    static unsigned char task_repeat = 0;
    unsigned char left_count = 0;

    if (hal_priv == NULL || task == NULL)
        return -EINVAL;

    pWorkFifo = &hal_priv->WorkFifo;
    left_count = CO_SharedFifoNbEltCont(pWorkFifo, CO_WORK_GET);
    STATUS = CO_SharedFifoGet(pWorkFifo, CO_WORK_GET, 1, &EltPtr);
    if (STATUS != CO_STATUS_OK) {
        ERROR_DEBUG_OUT("work fifo overflow\n");
        CO_SharedFifo_Dump(pWorkFifo, CO_WORK_GET);
        CO_SharedFifo_Dump(pWorkFifo, CO_WORK_FREE);
        return -ENOSPC;
    }

    if ((left_count < 50) && task_p == (void *)task) {
        task_repeat++;

        if ((task_repeat % 10) == 0) {
            pr_debug("task is %p\n", task);
            task_repeat = 0;
        }

    } else {
        task_repeat = 0;
    }

    pWorkTask = ( struct hal_work_task *)EltPtr;
    if (pWorkTask) {
        pWorkTask->param1 = param1;
        pWorkTask->param2 = param2;
        pWorkTask->param3 = param3;
        pWorkTask->param4 = param4;
        pWorkTask->param5 = param5;
        pWorkTask->taskcallback = taskcallback;
        pWorkTask->task = task;
        task_p = (void *)task;

        STATUS = CO_SharedFifoPut(pWorkFifo, CO_WORK_GET, 1);
        if (STATUS != CO_STATUS_OK) {
            ERROR_DEBUG_OUT("work fifo overflow\n");
            CO_SharedFifo_Dump(pWorkFifo, CO_WORK_GET);
            CO_SharedFifo_Dump(pWorkFifo, CO_WORK_FREE);
            return -ENOSPC;
        }
        if (hal_priv->hal_ops.hal_call_task != NULL &&
            hal_priv->WorkFifo_task_id < MAX_WORK_TASK)
            hal_priv->hal_ops.hal_call_task(hal_priv->WorkFifo_task_id,(SYS_TYPE)NULL);
    }
    return 0;
}

int drv_hal_workitem_inital(void)
{
    struct hal_private* hal_priv = hal_get_priv();
    int res;

    ASSERT(&hal_priv->WorkFifo);
    ASSERT(hal_priv->WorkFifoBuf);

    CO_SharedFifoInit(&hal_priv->WorkFifo, (SYS_TYPE)hal_priv->WorkFifoBuf, (void *)hal_priv->WorkFifoBuf,
        WORK_ITEM_NUM, sizeof(struct  hal_work_task), CO_SF_WORK_NBR);

    pr_debug("%s hal_priv->WorkFifo:%p\n", __func__, &hal_priv->WorkFifo);
    res = (SYS_TYPE)hal_priv->hal_ops.hal_reg_task(drv_hal_workitem_task);

    if (res < 0) {
        pr_err("WorkFifo_task_id error !\n");
        return res;
    } else {
        hal_priv->WorkFifo_task_id = res;
    }
    return 0;
}
