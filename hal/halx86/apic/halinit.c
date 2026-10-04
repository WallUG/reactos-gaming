/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the APIC HAL
 * COPYRIGHT:   Copyright 2011 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu);

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    if (ProcessorNumber == 0)
    {
        HalpParseApicTables(LoaderBlock);
    }

    HalpSetupProcessorsTable(ProcessorNumber);

    /* Initialize the local APIC for this cpu */
    ApicInitializeLocalApic(ProcessorNumber);

    /* Initialize profiling data (but don't start it) */
    HalInitializeProfiling();

    /* Initialize the timer */
    //ApicInitializeTimer(ProcessorNumber);
}

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    DPRINT1("Using HAL: APIC %s %s\n",
            (HalpBuildType & PRCB_BUILD_UNIPROCESSOR) ? "UP" : "SMP",
            (HalpBuildType & PRCB_BUILD_DEBUG) ? "DBG" : "REL");

    HalpPrintApicTables();

    /* Enable clock interrupt handler */
    HalpEnableInterruptHandler(IDT_INTERNAL,
                               0,
                               APIC_CLOCK_VECTOR,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);

    /* Enable profile interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               APIC_PROFILE_VECTOR,
                               APIC_PROFILE_LEVEL,
                               HalpProfileInterrupt,
                               Latched);

    /* Register the IPI entry (0xE1 -> HalpIpiInterrupt, dispatching to
     * the kernel's KiIpiServiceRoutine). Must be in Phase0 so the IDT
     * copy of every AP includes it. */
    KeRegisterInterruptHandler(APIC_IPI_VECTOR, HalpIpiInterrupt);

    /* Wait for 4 RTC edges before the first STI. Each RegC read
     * acknowledges (line to LOW) and rearms the next edge; on exit
     * there are real transitions and a latched IRR. Counting edges
     * (not fixed time) makes it host-speed independent; the cap
     * avoids hanging if the RTC never ticks (boot then proceeds
     * exactly as before). */
    {
        ULONG i, Edges = 0;
        UCHAR LastC;
        HalpAcquireCmosSpinLock();
        LastC = HalpReadCmos(RTC_REGISTER_C);
        HalpReleaseCmosSpinLock();
        for (i = 0; (i < 16000) && (Edges < 4); i++)
        {
            UCHAR C;
            KeStallExecutionProcessor(40);
            HalpAcquireCmosSpinLock();
            C = HalpReadCmos(RTC_REGISTER_C);
            HalpReleaseCmosSpinLock();
            if ((C ^ LastC) & 0x40) Edges++;
            LastC = C;
        }
    }
}

VOID
HalpInitPhase1(VOID)
{
    /* Initialize DMA. NT does this in Phase 0 */
    HalpInitDma();
}

/* EOF */
