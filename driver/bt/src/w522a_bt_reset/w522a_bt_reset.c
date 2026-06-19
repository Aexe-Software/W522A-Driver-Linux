/*
 * w522a_bt_reset - Power-on the Amlogic W1 / W155Sx Bluetooth subsystem
 * via the SDIO HIF exported by aml_sdio (struct w1_g_w1_hif_ops).
 *
 * This is a 1:1 port of the vendor power-on sequence found in
 * github.com/aml-streambox/bt-amlogic ->
 *   aml_bt/sdio_driver_bt/bt_hal_plateform.c :: config_bt_pmu_reg(is_power_on=true)
 *
 * Vendor sequence (with reverse-engineered explanations):
 *   1. SDIO_FUNC1 reg 0x231 |= BIT(5)       wifi-keep-alive
 *   2. SDIO_FUNC1 reg 0x231 &= ~BIT(0)      release pmu fsm hold
 *   3. PMU A14 = 0x1f                       set BT work flag + masks
 *   4. PMU A17 = 0x700                      power on
 *   5. PMU A20 = 0x0                        RAM up
 *   6. PMU A18 = 0x1700                     de-isolate
 *   7. PMU A22 = 0x704                      device reset (only some bits)
 *   8. PMU A12 bb_reset_man toggle :        clear bits 6/7 then
 *      A12 |= 0x80   (bb_reset_man=1, val=0)
 *      A12 |= 0xc0   (bb_reset_man=1, val=1)
 *      A12 |= 0x80   (bb_reset_man=1, val=0)
 *      A12 |= 0x00   (bb_reset_man=0)
 *   9. SDIO_FUNC1 reg 0x231 |= (PMU_PWR_OFF<<1) | BIT(0)
 *  10. SDIO_FUNC1 reg 0x231 &= ~0x1f         release FSM, start transitioning
 *  11. poll PMU A15 & 0xF until == PMU_ACT_MODE (0x6)
 *
 * After step 11 the BT subsystem is in ACT_MODE and the BT CPU runs
 * its ROM bootloader, which then listens on UART for TCI (0xfef1/0xfef2)
 * firmware download.
 *
 * NOTE: the previous version of this module only touched A17/A18/A20/A22
 * with zeroes and bit-banged A12.RG_START_CPU_VAL directly. That bypasses
 * the PMU FSM (the A12 START_CPU bits are only meaningful with MAN=1, and
 * the PMU FSM was never told to leave PWR_OFF state).  Result: TCI worked
 * (because ROM was reachable), firmware downloaded, but after start_chip
 * the CPU could not access baseband peripherals because A14 (BT work flag)
 * was 0 and the FSM never entered ACT_MODE -> chip stayed silent on HCI.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>

/* aml_sdio exports g_hw_interface.hif_ops as a symbol named w1_g_w1_hif_ops.
 * We don't pull the full vendor headers in (they require the whole tree);
 * instead we mirror the struct as a fixed-size pointer table.  The layout
 * has been verified by reading project_w1/vmac/wifi_hif.h in the vendor
 * tree, and the existing (working) bt_hi_write_word at p[23] confirms it.
 */
struct amlw_hif_ops_min { void *p[29]; };
extern struct amlw_hif_ops_min w1_g_w1_hif_ops;

/* offsets into amlw_hif_ops (see vendor wifi_hif.h) */
#define HIF_BOTTOM_WRITE8   0   /* int (*)(u8 func, int addr, u8 data)    */
#define HIF_BOTTOM_READ8    1   /* u8  (*)(u8 func, int addr)             */
#define HIF_BT_WRITE_WORD   23  /* void(*)(u32 addr, u32 data)            */
#define HIF_BT_READ_WORD    24  /* u32 (*)(u32 addr)                      */

#define SDIO_BWRITE8(f,a,d)  (((int (*)(unsigned char, int, unsigned char)) \
                              w1_g_w1_hif_ops.p[HIF_BOTTOM_WRITE8])((f),(a),(d)))
#define SDIO_BREAD8(f,a)     (((unsigned char (*)(unsigned char, int))     \
                              w1_g_w1_hif_ops.p[HIF_BOTTOM_READ8])((f),(a)))
#define BT_WRITE_WORD(a,d)   (((void (*)(unsigned int, unsigned int))      \
                              w1_g_w1_hif_ops.p[HIF_BT_WRITE_WORD])((a),(d)))
#define BT_READ_WORD(a)      (((unsigned int (*)(unsigned int))            \
                              w1_g_w1_hif_ops.p[HIF_BT_READ_WORD])((a)))

/* SDIO HOST_REQ register (FUNC1 byte register at 0x231) */
#define SDIO_FUNC1                 1
#define RG_BT_SDIO_PMU_HOST_REQ    0x231

/* PMU FSM states (only the ones we need) */
#define PMU_PWR_OFF                0x0
#define PMU_ACT_MODE               0x6

/* BT PMU register block (mapped at 0xf03000 from SoC SDIO BT slave view) */
#define CHIP_BT_PMU_REG_BASE       0xf03000
#define RG_BT_PMU_A11              (CHIP_BT_PMU_REG_BASE + 0x2c)
#define RG_BT_PMU_A12              (CHIP_BT_PMU_REG_BASE + 0x30)
#define RG_BT_PMU_A13              (CHIP_BT_PMU_REG_BASE + 0x34)
#define RG_BT_PMU_A14              (CHIP_BT_PMU_REG_BASE + 0x38)
#define RG_BT_PMU_A15              (CHIP_BT_PMU_REG_BASE + 0x3c)
#define RG_BT_PMU_A16              (CHIP_BT_PMU_REG_BASE + 0x40)
#define RG_BT_PMU_A17              (CHIP_BT_PMU_REG_BASE + 0x44)
#define RG_BT_PMU_A18              (CHIP_BT_PMU_REG_BASE + 0x48)
#define RG_BT_PMU_A20              (CHIP_BT_PMU_REG_BASE + 0x50)
#define RG_BT_PMU_A22              (CHIP_BT_PMU_REG_BASE + 0x58)

static unsigned int settle_ms = 100;
module_param(settle_ms, uint, 0644);
MODULE_PARM_DESC(settle_ms, "Final settle delay after PMU reaches ACT_MODE (ms)");

static unsigned int dump_only;
module_param(dump_only, uint, 0644);
MODULE_PARM_DESC(dump_only, "If non-zero, only dump PMU regs and exit");

static unsigned int wait_act_ms = 1000;
module_param(wait_act_ms, uint, 0644);
MODULE_PARM_DESC(wait_act_ms, "Max time to wait for PMU FSM to reach ACT_MODE (ms)");

static void bt_dump(const char *tag)
{
	unsigned int a11, a12, a13, a14, a15, a16, a17, a18, a20, a22;
	unsigned char host_req;

	a11 = BT_READ_WORD(RG_BT_PMU_A11);
	a12 = BT_READ_WORD(RG_BT_PMU_A12);
	a13 = BT_READ_WORD(RG_BT_PMU_A13);
	a14 = BT_READ_WORD(RG_BT_PMU_A14);
	a15 = BT_READ_WORD(RG_BT_PMU_A15);
	a16 = BT_READ_WORD(RG_BT_PMU_A16);
	a17 = BT_READ_WORD(RG_BT_PMU_A17);
	a18 = BT_READ_WORD(RG_BT_PMU_A18);
	a20 = BT_READ_WORD(RG_BT_PMU_A20);
	a22 = BT_READ_WORD(RG_BT_PMU_A22);
	host_req = SDIO_BREAD8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ);
	pr_info("w522a_bt_reset: %s host_req=0x%02x A11=0x%08x A12=0x%08x A13=0x%08x A14=0x%08x A15=0x%08x A16=0x%08x A17=0x%08x A18=0x%08x A20=0x%08x A22=0x%08x\n",
		tag, host_req, a11, a12, a13, a14, a15, a16, a17, a18, a20, a22);
}

static int __init bt_reset_init(void)
{
	unsigned int value_pmu_A12;
	unsigned int bt_pmu_status;
	unsigned char host_req_status;
	unsigned long deadline;

	if (!w1_g_w1_hif_ops.p[HIF_BT_READ_WORD] ||
	    !w1_g_w1_hif_ops.p[HIF_BT_WRITE_WORD] ||
	    !w1_g_w1_hif_ops.p[HIF_BOTTOM_READ8] ||
	    !w1_g_w1_hif_ops.p[HIF_BOTTOM_WRITE8]) {
		pr_err("w522a_bt_reset: aml_sdio HIF ops not available\n");
		return -ENODEV;
	}

	bt_dump("before");

	if (dump_only) {
		pr_info("w522a_bt_reset: dump_only requested, exiting\n");
		return 0;
	}

	/* --- step 1: wifi keep alive (BIT 5 of host_req) --------------- */
	host_req_status = SDIO_BREAD8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ);
	pr_info("w522a_bt_reset: host_req before = 0x%02x\n", host_req_status);
	host_req_status |= (1u << 5);
	SDIO_BWRITE8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ, host_req_status);

	/* --- step 2: BT work flag + masks ----------------------------- */
	BT_WRITE_WORD(RG_BT_PMU_A14, 0x1fu);
	pr_info("w522a_bt_reset: A14 set to 0x%08x\n",
		BT_READ_WORD(RG_BT_PMU_A14));

	/* --- step 3: release pmu fsm hold (clear BIT 0) --------------- */
	host_req_status = SDIO_BREAD8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ);
	host_req_status &= ~(1u << 0);
	SDIO_BWRITE8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ, host_req_status);
	msleep(10);

	/* --- step 4..7: power-on / de-isolate / device reset --------- */
	BT_WRITE_WORD(RG_BT_PMU_A17, 0x700u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A20, 0x0u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A18, 0x1700u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A22, 0x704u);
	msleep(10);

	pr_info("w522a_bt_reset: after power-on regs: A17=0x%08x A20=0x%08x A18=0x%08x A22=0x%08x\n",
		BT_READ_WORD(RG_BT_PMU_A17),
		BT_READ_WORD(RG_BT_PMU_A20),
		BT_READ_WORD(RG_BT_PMU_A18),
		BT_READ_WORD(RG_BT_PMU_A22));

	/* --- step 8: bb_reset_man toggle on A12 ----------------------- */
	value_pmu_A12 = BT_READ_WORD(RG_BT_PMU_A12) & 0xffffff3fu;
	BT_WRITE_WORD(RG_BT_PMU_A12, value_pmu_A12 | 0x80u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A12, value_pmu_A12 | 0xc0u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A12, value_pmu_A12 | 0x80u);
	msleep(1);
	BT_WRITE_WORD(RG_BT_PMU_A12, value_pmu_A12 | 0x00u);
	msleep(1);
	pr_info("w522a_bt_reset: A12 after bb_reset_man toggle = 0x%08x\n",
		BT_READ_WORD(RG_BT_PMU_A12));

	/* --- step 9: request PMU_PWR_OFF state + hold ----------------- */
	host_req_status = SDIO_BREAD8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ);
	host_req_status |= (PMU_PWR_OFF << 1) | (1u << 0);
	SDIO_BWRITE8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ, host_req_status);

	/* --- step 10: release FSM ------------------------------------- */
	host_req_status = SDIO_BREAD8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ);
	host_req_status &= ~0x1fu;
	SDIO_BWRITE8(SDIO_FUNC1, RG_BT_SDIO_PMU_HOST_REQ, host_req_status);
	msleep(20);

	/* --- step 11: wait for PMU_ACT_MODE --------------------------- */
	deadline = jiffies + msecs_to_jiffies(wait_act_ms);
	bt_pmu_status = BT_READ_WORD(RG_BT_PMU_A15);
	while ((bt_pmu_status & 0xfu) != PMU_ACT_MODE) {
		if (time_after(jiffies, deadline)) {
			pr_err("w522a_bt_reset: timeout waiting for PMU_ACT_MODE, A15=0x%08x (state=0x%x)\n",
			       bt_pmu_status, bt_pmu_status & 0xfu);
			bt_dump("timeout");
			return -ETIMEDOUT;
		}
		msleep(5);
		bt_pmu_status = BT_READ_WORD(RG_BT_PMU_A15);
	}
	pr_info("w522a_bt_reset: PMU reached ACT_MODE, A15=0x%08x\n", bt_pmu_status);

	if (settle_ms)
		msleep(settle_ms);

	bt_dump("after");
	pr_info("w522a_bt_reset: BT subsystem brought up (vendor sequence)\n");
	return 0;
}

static void __exit bt_reset_exit(void)
{
	pr_info("w522a_bt_reset: exit\n");
}

module_init(bt_reset_init);
module_exit(bt_reset_exit);

MODULE_AUTHOR("Devin <devin@cognition.ai>");
MODULE_DESCRIPTION("Amlogic W1/W155Sx Bluetooth subsystem power-on (vendor PMU sequence)");
MODULE_LICENSE("GPL");
