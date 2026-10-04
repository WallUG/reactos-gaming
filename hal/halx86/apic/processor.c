/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HAL Processor Routines
 * COPYRIGHT:   Copyright 2010 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

KAFFINITY HalpActiveProcessors;
KAFFINITY HalpDefaultInterruptAffinity;

/* Canonical x86 TPR table (same one ApicSetIrql uses). */
extern const UCHAR HalpIRQLtoTPR[32];

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
HaliHaltSystem(VOID)
{
    /* Disable interrupts and halt the CPU */
    _disable();
    __halt();
}

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalAllProcessorsStarted(VOID)
{
    /* Do nothing */
    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalProcessorIdle(VOID)
{
    /* Sync hardware TPR with the PCR before HLT (lazy mode can leave
     * TPR stuck high, killing the clock). RULE: TPR == IRQL always,
     * NO clamp: if the PCR is at HIGH (someone else's leak), lowering
     * TPR opens the gate with PCR=31 and the first nested ISR returns
     * to garbage. Must use the canonical HalpIRQLtoTPR[] table: the
     * amd64-style (_irql<<4)&0xF0 formula is wrong on x86 (with
     * HIGH(31) the shift gives 0xF0 and masks every vector <0xF0,
     * i.e. devices, clock, IPI -> deaf AP in HLT). */
    {
        ULONG _irql = (ULONG)__readfsbyte(FIELD_OFFSET(KPCR, Irql));
        ULONG _tpr;
        if (_irql > 31) _irql = 31;
        _tpr = HalpIRQLtoTPR[_irql];
        *(volatile ULONG *)(0xFFFE0000 + 0x80) = _tpr;
        /* Lazy coherence: this direct writer publishes the hard level
         * into IRR; without it a later lower compares against a stale
         * IRR, skips the hardware restore and TPR stays stuck high
         * (irreversible deafness: lazy lower never brings it down
         * because IRR says otherwise). */
        KeGetPcr()->IRR = (KIRQL)_irql;
#ifdef DBG
        {
            /* Witness, mute when healthy: only speaks (cap 4) if the
             * old formula would have programmed a different TPR. */
            static LONG _tprDbg = 0;
            ULONG _old = ((_irql << 4) & 0xF0);
            if (_old != _tpr && InterlockedIncrement(&_tprDbg) <= 4)
            {
                DPRINT1("TPR-FIX: irql=%lu applied=%02lx shift-would-be=%02lx\n",
                        _irql, _tpr, _old);
            }
        }
#endif
    }

    /* Enable interrupts and halt the processor */
    _enable();
    __halt();
}

/* EOF */
