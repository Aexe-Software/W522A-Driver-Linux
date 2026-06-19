
#ifndef CO_SHARED_FIFO_H
#define CO_SHARED_FIFO_H

#ifdef HAL_FPGA_VER
#include "osdep.h"
#endif

#define CO_STATUS_GROUP_COMMON  (0 << 8)

#define CO_STATUS_OK  ((unsigned short)(CO_STATUS_GROUP_COMMON + 0))
#define CO_FAIL  ((unsigned short)(CO_STATUS_GROUP_COMMON + 1))
#define CO_BAD_PARAM  ((unsigned short)(CO_STATUS_GROUP_COMMON + 4))

#define CO_NO_ELT_IN_USE  ((unsigned short)(CO_STATUS_GROUP_COMMON + 12))
#define CO_IDX_OUT_OF_RANGE  ((unsigned short)(CO_STATUS_GROUP_COMMON + 13))
#define AL_BEGIN_CRITICAL_PATH  COMMON_LOCK()
#define  AL_END_CRITICAL_PATH  COMMON_UNLOCK()

#define CO_SF_PROD_ID        0
#define CO_SF_BLOCK_DEF_NBR  2

#ifndef __INLINE
#endif

#ifndef CO_STATUS_GROUP_COMMON

#endif

struct _CO_SF_BLOCK_IDX
{
    unsigned int Flag;  
    unsigned int In;    
    unsigned int Out;   
} ;

struct _CO_SHARED_FIFO
{
    SYS_TYPE EltBasePhy; 
    void *EltBasePtr; 
    unsigned int EltNbr; 
    unsigned int EltSize; 
    unsigned int BlockNbr; 
    
    struct _CO_SF_BLOCK_IDX IdxTab[CO_SF_BLOCK_DEF_NBR+2]; 
} ;

void CO_SharedFifoInit(struct _CO_SHARED_FIFO* SharedFifoPtr, SYS_TYPE  EltBasePhy, void *EltBasePtr,
    unsigned int EltNbr, unsigned int EltSize, unsigned char BlockNbr);

unsigned short CO_SharedFifoPut(struct _CO_SHARED_FIFO* const SharedFifoPtr, unsigned char const BlockID,
    unsigned int const NbElt);

unsigned short CO_SharedFifoPut_NoCritSect(struct _CO_SHARED_FIFO* const SharedFifoPtr, unsigned char const BlockID,
    unsigned int const NbElt);

unsigned short CO_SharedFifoGet(struct _CO_SHARED_FIFO* const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt, unsigned char **ReturnEltPtr);

unsigned short CO_SharedFifo_Pre_Get(struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt, unsigned char **ReturnEltPtr);

unsigned short CO_SharedFifoGet_NoCritSect(struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt, unsigned char **ReturnEltPtr);

unsigned short CO_SharedFifo_Pre_Get_NoCritSect(struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt, unsigned char** ReturnEltPtr);

void CO_SharedFifoGetNElt(struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID, const unsigned int NbElt,
    unsigned char **ReturnEltPtr);

unsigned int CO_SharedFifoNbElt(struct _CO_SHARED_FIFO* SharedFifoPtr, unsigned char BlockID);
unsigned int CO_SharedFifoNbEltCont(struct _CO_SHARED_FIFO* SharedFifoPtr, unsigned char BlockID);
unsigned int CO_SharedFifoNbEltCont_NonRound(struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID);
void CO_SharedFifo_Dump(struct _CO_SHARED_FIFO* const  SharedFifoPtr, const unsigned char BlockID);
void qfifo_mng_info_dump(struct _CO_SHARED_FIFO* const  SharedFifoPtr);
unsigned char CO_SharedFifoEmpty(struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID);

static __INLINE struct _CO_SF_BLOCK_IDX *CO_SharedFifoGetInBlockIdxPtr(struct _CO_SHARED_FIFO *const SharedFifoPtr,
    const unsigned char BlockID)
{
    
    return &(SharedFifoPtr->IdxTab[BlockID]);
}

static __INLINE unsigned char * CO_SharedFifoPick
    (struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID)
{
    
    return (unsigned char*)((unsigned long)(SharedFifoPtr->EltBasePtr)
                 + (unsigned int)(SharedFifoPtr->IdxTab[BlockID].Out * SharedFifoPtr->EltSize));
}
#endif 
