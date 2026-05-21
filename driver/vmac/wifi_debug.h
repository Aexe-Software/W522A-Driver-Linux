#ifndef _AML_DEBUG_H_
#define _AML_DEBUG_H_

#include <linux/bitops.h>
#include <linux/kernel.h>

#ifndef BIT
#define BIT(n)    (1UL << (n))
#endif

#define  CTRL_BYTE
#define LINEBYTE 32
#define ASCII_IN  1

enum
{
    AML_DEBUG_ACL              = BIT(0),
    AML_DEBUG_XMIT             = BIT(1),
    AML_DEBUG_KEY              = BIT(2),
    AML_DEBUG_STATE            = BIT(3),
    AML_DEBUG_RATE             = BIT(4),
    AML_DEBUG_RECV             = BIT(5),
    AML_DEBUG_P2P              = BIT(6),
    AML_DEBUG_CFG80211         = BIT(7),
    AML_DEBUG_SCAN             = BIT(8),
    AML_DEBUG_LOCK             = BIT(9),
    AML_DEBUG_INIT             = BIT(10),
    AML_DEBUG_ROAM             = BIT(11),
    AML_DEBUG_NODE             = BIT(12),
    AML_DEBUG_PLATFORM         = BIT(13),
    AML_DEBUG_ACTION           = BIT(14),
    AML_DEBUG_IOCTL            = BIT(15),
    AML_DEBUG_CONNECT          = BIT(16),
    AML_DEBUG_TIMER            = BIT(17),
    AML_DEBUG_ADDBA            = BIT(18),
    AML_DEBUG_NETDEV           = BIT(19),
    AML_DEBUG_HAL              = BIT(20),
    AML_DEBUG_BEACON           = BIT(21),
    AML_DEBUG_UAPSD            = BIT(22),
    AML_DEBUG_BWC              = BIT(23),
    AML_DEBUG_ELEMID           = BIT(24),
    AML_DEBUG_PWR_SAVE         = BIT(25),
    AML_DEBUG_DEBUG            = BIT(26),
    AML_DEBUG_INFO             = BIT(27),
    AML_DEBUG_WARNING          = BIT(28),
    AML_DEBUG_ERROR            = BIT(29),
    AML_DEBUG_WME              = BIT(30),
    AML_DEBUG_DOTH             = BIT(31),
    AML_DEBUG_ANY              = 0xffffffff
};

enum
{
    AML_DEBUG_VHT      = BIT(0),
    AML_DEBUG_VHT_ANY  = 0xffffffff
};

#define AML_DEBUG_LEVEL (AML_DEBUG_STATE|AML_DEBUG_ERROR|AML_DEBUG_WARNING|AML_DEBUG_INIT|\
                            AML_DEBUG_CFG80211)

#define DBG_HAL_THR_ENTER()
#define DBG_HAL_THR_EXIT()

extern int aml_debug;
extern unsigned long long g_dbg_info_enable;
extern unsigned long long g_dbg_modules;

/*
 * DPRINTF — fixed: wrap pr_debug in its own braces to avoid -Wempty-body
 * when the macro is used after an if/else without {} in caller code.
 */
#define DPRINTF(_m, ...) do {                   \
        if (aml_debug & (_m))                   \
            pr_debug(__VA_ARGS__);              \
    } while (0)

enum
{
    AML_DBG_OFF = 0,
    AML_DBG_ON  = 1,
};

enum
{
    AML_DBG_MODULES_P2P      = BIT(0),
    AML_DBG_MODULES_RATE_CTR = BIT(1),
    AML_DBG_MODULES_TX       = BIT(2),
    AML_DBG_MODULES_HAL_TX   = BIT(3),
    AML_DBG_MODULES_TX_ERROR = BIT(4),
    AML_DBG_MODULES_SCAN     = BIT(5),
    AML_DEBUG_MODULES_ALL    = 0xffffffffffffffff,
};

#define AML_PRINT(_m, format, ...) do {                                         \
        if (g_dbg_modules & (_m)) {                                             \
            if ((_m) == AML_DBG_MODULES_P2P)                                    \
                pr_debug("[p2p] <%s> %d " format "", __func__, __LINE__, ##__VA_ARGS__); \
            else if ((_m) == AML_DBG_MODULES_RATE_CTR)                          \
                pr_debug("[mi_rate] <%s> %d " format "", __func__, __LINE__, ##__VA_ARGS__); \
            else if ((_m) == AML_DBG_MODULES_TX)                                \
                pr_debug("[TX] <%s> %d " format "", __func__, __LINE__, ##__VA_ARGS__); \
            else if ((_m) == AML_DBG_MODULES_TX_ERROR)                          \
                pr_debug("[TX_ERROR] <%s> %d " format "", __func__, __LINE__, ##__VA_ARGS__); \
            else if ((_m) == AML_DBG_MODULES_SCAN)                              \
                pr_debug("[SCAN] <%s> %d " format "", __func__, __LINE__, ##__VA_ARGS__); \
        }                                                                        \
    } while (0)

#define ERROR_DEBUG_OUT(format, ...) do {                               \
        pr_err("FUNCTION: %s LINE: %d:" format "", __func__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#define AML_OUTPUT(format, ...) do {                                    \
        pr_debug("<%s> %d:" format "", __func__, __LINE__, ##__VA_ARGS__); \
    } while (0)


#include "wifi_pt_init.h"
extern struct _B2B_Platform_Conf gB2BPlatformConf;


/*
 * OS_SPIN_* macros — fixed for mainline kernel / Armbian:
 *
 * Original code used bare {if (...) pr_debug(...); spin_lock...} which
 * triggers -Wempty-body because the semicolon after pr_debug ends the
 * if-body, leaving spin_lock outside the if — and gcc warns about the
 * empty if-body when DEBUG_LOCK is disabled.
 *
 * Fix: use do { } while(0) wrappers with proper braces around the if.
 * DEBUG_LOCK is left defined but the pr_debug paths only fire when
 * AML_DEBUG_LOCK bit is set at runtime (default: off).
 */

/* ---- Spinlock (IRQ save) --------------------------------- */
/* БАГ 5 fix: pr_debug переміщено ПІСЛЯ spin_lock_irqsave для LOCK
 * і ДО spin_unlock_irqrestore для UNLOCK. Виклик pr_debug (dynamic_debug)
 * до взяття блокування може спробувати взяти внутрішній лок у IRQ-контексті
 * раніше ніж irqsave вимкнув переривання → deadlock або BUG().
 */
#define OS_SPIN_LOCK_IRQ(a, b) do {                                         \
        spin_lock_irqsave((a), (b));                                        \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, lock ++\n", __func__, __LINE__, (a));       \
    } while (0)

#define OS_SPIN_UNLOCK_IRQ(a, b) do {                                       \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, lock --\n", __func__, __LINE__, (a));       \
        spin_unlock_irqrestore((a), (b));                                   \
    } while (0)

/* ---- Spinlock (BH) --------------------------------------- */
#define OS_SPIN_LOCK_BH(a) do {                                             \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, lock_bh ++\n", __func__, __LINE__, (a));   \
        spin_lock_bh((a));                                                  \
    } while (0)

#define OS_SPIN_UNLOCK_BH(a) do {                                           \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, lock_bh --\n", __func__, __LINE__, (a));   \
        spin_unlock_bh((a));                                                \
    } while (0)

/* ---- Spinlock (plain) ------------------------------------ */
#define OS_SPIN_LOCK(a) do {                                                \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, spin ++\n", __func__, __LINE__, (a));      \
        spin_lock((a));                                                     \
    } while (0)

#define OS_SPIN_UNLOCK(a) do {                                              \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, spin --\n", __func__, __LINE__, (a));      \
        spin_unlock((a));                                                   \
    } while (0)

/* ---- Read-write lock ------------------------------------- */
#define OS_WRITE_LOCK(a) do {                                               \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock ++\n", __func__, __LINE__, (a));     \
        write_lock((a));                                                    \
    } while (0)

#define OS_WRITE_UNLOCK(a) do {                                             \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock --\n", __func__, __LINE__, (a));     \
        write_unlock((a));                                                  \
    } while (0)

#define OS_WRITE_LOCK_BH(a) do {                                            \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock_bh ++\n", __func__, __LINE__, (a));  \
        write_lock_bh((a));                                                 \
    } while (0)

#define OS_WRITE_UNLOCK_BH(a) do {                                          \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock_bh --\n", __func__, __LINE__, (a));  \
        write_unlock_bh((a));                                               \
    } while (0)

#define OS_WRITE_LOCK_IRQ(a, b) do {                                        \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock_irq ++\n", __func__, __LINE__, (a)); \
        write_lock_irqsave((a), (b));                                       \
    } while (0)

#define OS_WRITE_UNLOCK_IRQ(a, b) do {                                      \
        if (aml_debug & AML_DEBUG_LOCK)                                     \
            pr_debug("%s,%d,%p, wlock_irq --\n", __func__, __LINE__, (a)); \
        write_unlock_irqrestore((a), (b));                                  \
    } while (0)

/* ---- Mutex ----------------------------------------------- */
#define OS_MUTEX_LOCK(a)   do { mutex_lock(a);   } while (0)
#define OS_MUTEX_UNLOCK(a) do { mutex_unlock(a); } while (0)


#if defined(FPGA) || defined(CHIP)
#define PRINT(...)          do { pr_debug(__VA_ARGS__); } while (0)
#define PRINT_ERR(...)      do { pr_err(__VA_ARGS__); } while (0)
#define PUTC(character)     pr_debug("%c", character)
#define PUTX8(size, value)  pr_debug("%02x", value)
#define PUTU8(value)        pr_debug("%u", value)
#define PUTS(...)           pr_debug(__VA_ARGS__)
#define PUTU(number)        pr_debug("%d\n", number)
#define DBG_ENTER()
#define DBG_EXIT()
#endif


#ifndef ASSERT
#define ASSERT(exp) do {                                                    \
        if (!(exp)) {                                                       \
            pr_err("=>=>=>=>=>assert %s,%d\n", __func__, __LINE__);        \
        }                                                                   \
    } while (0)
#endif

#ifdef __KERNEL__
#include <asm/page.h>

#define KASSERT(exp, msg) do {                  \
        if (unlikely(!(exp))) {                 \
            pr_err msg;                         \
            WARN_ON_ONCE(1);                    \
        }                                       \
    } while (0)

#endif /* __KERNEL__ */

void address_print(unsigned char *address);
void IPv4_address_print(unsigned char *address);
void dump_memory_internal(unsigned char *data, int len);
void address_read(unsigned char *cursor, unsigned char *address);

unsigned short READ_16L(const unsigned char *address);
void WRITE_16L(unsigned char *address, unsigned short value);
unsigned int READ_32L(const unsigned char *address);
void WRITE_32L(unsigned char *address, unsigned int value);

unsigned short READ_16B(const unsigned char *address);
void WRITE_16B(unsigned char *address, unsigned short value);
unsigned int READ_32B(const unsigned char *address);
void WRITE_32B(unsigned char *address, unsigned int value);

void ie_dbg(unsigned char *ie);

#endif /* _AML_DEBUG_H_ */
