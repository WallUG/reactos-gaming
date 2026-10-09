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
    /* Sync hardware TPR with the PCR before HLT and publish into
     * IRR (keeps the raise-publish/lower-lazy invariant exact, so a
     * later lower restores instead of leaving TPR stuck high).
     * Canonical HalpIRQLtoTPR[] table (the (_irql<<4)&0xF0 formula
     * is wrong on x86: HIGH(31) shifts to 0xF0, masking vectors
     * below it). No clamp (see history). */
    {
        ULONG _irql = (ULONG)__readfsbyte(FIELD_OFFSET(KPCR, Irql));
        ULONG _tpr;
        if (_irql > 31) _irql = 31;
        _tpr = HalpIRQLtoTPR[_irql];
        *(volatile ULONG *)(0xFFFE0000 + 0x80) = _tpr;
        KeGetPcr()->IRR = (KIRQL)_irql;
    }

    /* Enable interrupts and halt the processor */
    _enable();
    __halt();
}

/* EOF */
