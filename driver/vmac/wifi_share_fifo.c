
#include "wifi_hal_com.h"

void CO_SharedFifo_Dump(struct _CO_SHARED_FIFO* const  SharedFifoPtr, const unsigned char BlockID)
{
    struct _CO_SF_BLOCK_IDX *BlockOutIdxPtr;
    BlockOutIdxPtr = &SharedFifoPtr->IdxTab[ BlockID ];
    pr_debug("b%d I%d,O%d\n",BlockID,BlockOutIdxPtr->In,BlockOutIdxPtr->Out);
}

void qfifo_mng_info_dump(struct _CO_SHARED_FIFO* const  SharedFifoPtr)
{
    int i_block = 0;
    for (i_block = 0; i_block < 3; i_block++)
        CO_SharedFifo_Dump(SharedFifoPtr, i_block);
}

void CO_SharedFifoInit(struct _CO_SHARED_FIFO  *SharedFifoPtr, SYS_TYPE EltBasePhy, void *EltBasePtr, unsigned int EltNbr,
    unsigned int EltSize, unsigned char BlockNbr)
{
    int i; 

    SharedFifoPtr->EltBasePhy = EltBasePhy;
    SharedFifoPtr->EltBasePtr = EltBasePtr;
    SharedFifoPtr->EltNbr     = EltNbr;
    SharedFifoPtr->EltSize    = EltSize;
    SharedFifoPtr->BlockNbr   = BlockNbr;

    for (i = CO_SF_PROD_ID; i < BlockNbr; i++)
    {
        SharedFifoPtr->IdxTab[i].Flag = 0;
        SharedFifoPtr->IdxTab[i].In = 1;
        SharedFifoPtr->IdxTab[i].Out = 1;
    }

    SharedFifoPtr->IdxTab[CO_SF_PROD_ID].In = 0;
}

static unsigned char *CO_SharedFifoGetPtr(const struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned int Idx)
{
    ASSERT(Idx < SharedFifoPtr->EltNbr) ;

    return (unsigned char*)((unsigned long)(SharedFifoPtr->EltBasePtr)
                 + (unsigned long)(Idx * SharedFifoPtr->EltSize));
}

unsigned short CO_SharedFifoGet(struct _CO_SHARED_FIFO* const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int  NbElt, unsigned char**  ReturnEltPtr )
{
    unsigned short Status;

    AL_BEGIN_CRITICAL_PATH;
    Status = CO_SharedFifoGet_NoCritSect( SharedFifoPtr, BlockID, NbElt, ReturnEltPtr );
    AL_END_CRITICAL_PATH;

    return Status;
}

unsigned short
CO_SharedFifoGet_NoCritSect(struct _CO_SHARED_FIFO* const  SharedFifoPtr, const unsigned char BlockID,
    const unsigned int  NbElt, unsigned char**  ReturnEltPtr)
{
    struct _CO_SF_BLOCK_IDX* BlockOutIdxPtr;
    unsigned short Status;
    unsigned int TmpIdx;
    unsigned int NbBlocElt; 
    {
        BlockOutIdxPtr = &SharedFifoPtr->IdxTab[ BlockID ];

        NbBlocElt = CO_SharedFifoNbEltCont( SharedFifoPtr, BlockID );
        if ( NbElt > NbBlocElt )
        {
            Status = CO_NO_ELT_IN_USE;
            *ReturnEltPtr =NULL;
        }
        else
        {
            
            *ReturnEltPtr = CO_SharedFifoGetPtr( SharedFifoPtr, BlockOutIdxPtr->Out );
            
            TmpIdx = BlockOutIdxPtr->Out + NbElt;
            
            if ( TmpIdx >= SharedFifoPtr->EltNbr )
            {
                BlockOutIdxPtr->Out = TmpIdx - SharedFifoPtr->EltNbr;
            }
            else
            {
                BlockOutIdxPtr->Out = TmpIdx;
            }
            Status = CO_STATUS_OK ;
        }
    }
    return Status;
}

unsigned short CO_SharedFifo_Pre_Get(struct _CO_SHARED_FIFO* const SharedFifoPtr, const unsigned char BlockID,
     const unsigned int NbElt, unsigned char** ReturnEltPtr)
{
    unsigned short Status;

    AL_BEGIN_CRITICAL_PATH;
    Status = CO_SharedFifo_Pre_Get_NoCritSect( SharedFifoPtr, BlockID, NbElt, ReturnEltPtr );
    AL_END_CRITICAL_PATH;

    return Status;
}

unsigned short CO_SharedFifo_Pre_Get_NoCritSect(struct _CO_SHARED_FIFO* const  SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt,  unsigned char** ReturnEltPtr )
{
    struct _CO_SF_BLOCK_IDX* BlockOutIdxPtr;
    unsigned short Status; 
    unsigned int NbBlocElt; 
    {
        
        BlockOutIdxPtr = &SharedFifoPtr->IdxTab[ BlockID ];

        NbBlocElt = CO_SharedFifoNbEltCont( SharedFifoPtr, BlockID);
        
        if ( NbElt > NbBlocElt )
        {
            Status = CO_NO_ELT_IN_USE;
            *ReturnEltPtr =NULL;
        }
        else
        {
            if (BlockOutIdxPtr->Out ==0)
                Status = CO_IDX_OUT_OF_RANGE ;
            else
                Status = CO_STATUS_OK ;
        }
    }
    return Status;
}

void CO_SharedFifoGetNElt(struct _CO_SHARED_FIFO *const SharedFifoPtr, const unsigned char BlockID,
    const unsigned int NbElt, unsigned char **ReturnEltPtr)
{
    struct _CO_SF_BLOCK_IDX  *BlockOutIdxPtr;   
    unsigned int  TmpIdx;  

    AL_BEGIN_CRITICAL_PATH;

    BlockOutIdxPtr = &(SharedFifoPtr->IdxTab[BlockID]);

    *ReturnEltPtr = CO_SharedFifoGetPtr(SharedFifoPtr, BlockOutIdxPtr->Out);

    TmpIdx = BlockOutIdxPtr->Out + NbElt;

    if (TmpIdx >= SharedFifoPtr->EltNbr)
    {
        BlockOutIdxPtr->Out = TmpIdx - SharedFifoPtr->EltNbr;
    }
    else
    {
        
        BlockOutIdxPtr->Out = TmpIdx;
    }

    AL_END_CRITICAL_PATH;
}

unsigned short CO_SharedFifoPut(struct _CO_SHARED_FIFO * const SharedFifoPtr, unsigned char const BlockID, unsigned int const NbElt)
{
    unsigned short Status; 

    AL_BEGIN_CRITICAL_PATH;

    Status = CO_SharedFifoPut_NoCritSect(SharedFifoPtr, BlockID, NbElt);

    AL_END_CRITICAL_PATH;

    return Status;
}

unsigned short CO_SharedFifoPut_NoCritSect(struct _CO_SHARED_FIFO * const SharedFifoPtr,
    unsigned char const BlockID, unsigned int const NbElt)
{
    struct _CO_SF_BLOCK_IDX  *BlockInIdxPtr; 
    unsigned int TmpIdx; 
    unsigned short Status = CO_STATUS_OK; 
    unsigned char NextBlockID = BlockID + 1; 

    if (NextBlockID == SharedFifoPtr->BlockNbr)
    {
        NextBlockID = CO_SF_PROD_ID;
    }
    
    BlockInIdxPtr = CO_SharedFifoGetInBlockIdxPtr(SharedFifoPtr, NextBlockID);

    TmpIdx = BlockInIdxPtr->In + NbElt;

    if (TmpIdx >= SharedFifoPtr->EltNbr)
    {
        BlockInIdxPtr->In = TmpIdx - SharedFifoPtr->EltNbr;
    }
    else
    {
        
        BlockInIdxPtr->In = TmpIdx;
    }

    return Status;
}

unsigned int CO_SharedFifoNbElt(struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID)
{
    
    unsigned int NbElt;
    struct _CO_SF_BLOCK_IDX *BlockIdxPtr; 

    BlockIdxPtr = &(SharedFifoPtr->IdxTab[BlockID]);

    NbElt = SharedFifoPtr->EltNbr + BlockIdxPtr->In - BlockIdxPtr->Out;

    if (NbElt >= SharedFifoPtr->EltNbr)
    {
        NbElt -= SharedFifoPtr->EltNbr;
    }

    return NbElt;
}

unsigned int CO_SharedFifoNbEltCont_NonRound(struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID)
{
    
    unsigned int NbElt; 
    struct _CO_SF_BLOCK_IDX *BlockIdxPtr; 

    BlockIdxPtr = &(SharedFifoPtr->IdxTab[BlockID]);
    
    if (BlockIdxPtr->In >= BlockIdxPtr->Out)
    {
        
        NbElt = BlockIdxPtr->In - BlockIdxPtr->Out;
    }
    else
    {
        
        NbElt = SharedFifoPtr->EltNbr - BlockIdxPtr->Out;
    }

    return NbElt;
}

unsigned int CO_SharedFifoNbEltCont(struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID)
{
    
    unsigned int NbElt; 
    struct _CO_SF_BLOCK_IDX *BlockIdxPtr; 

    BlockIdxPtr = &(SharedFifoPtr->IdxTab[BlockID]);
    
    if (BlockIdxPtr->In >= BlockIdxPtr->Out)
    {
        
        NbElt = BlockIdxPtr->In - BlockIdxPtr->Out;
    }
    else
    {
        
        NbElt = SharedFifoPtr->EltNbr + (BlockIdxPtr->In - BlockIdxPtr->Out);
    }

    return NbElt;
}

unsigned char CO_SharedFifoEmpty
    (struct _CO_SHARED_FIFO *SharedFifoPtr, unsigned char BlockID)
{
    unsigned char is_empty;
    struct _CO_SF_BLOCK_IDX *BlockIdxPtr = &(SharedFifoPtr->IdxTab[BlockID]);  

    AL_BEGIN_CRITICAL_PATH;
    is_empty = (BlockIdxPtr->In != BlockIdxPtr->Out);
    AL_END_CRITICAL_PATH;

    return is_empty;
}
