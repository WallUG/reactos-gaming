/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GNU GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/apic.c
 * PURPOSE:         HAL APIC Management and Control Code
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 * REFERENCES:      https://web.archive.org/web/20190407074221/http://www.joseflores.com/docs/ExploringIrql.html
 *                  https://www.codeproject.com/KB/system/soviet_kernel_hack.aspx
 *                  http://bbs.unixmap.net/thread-2022-1-1.html (DEAD_LINK)
 *                  https://codemachine.com/articles/interrupt_dispatching.html
 *                  https://www.osronline.com/article.cfm%5Earticle=211.htm
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

#ifndef _M_AMD64
#define APIC_LAZY_IRQL
#endif

/* GLOBALS ********************************************************************/

ULONG ApicVersion;
UCHAR HalpVectorToIndex[256];
extern PPROCESSOR_IDENTITY HalpProcessorIdentity;

/* MADT override tables (strong definitions in madt.c when the ACPI
 * component links; weak identity defaults here so HALs without ACPI
 * still link and behave as before: identity routing, driver Mode). */
__attribute__((weak)) ULONG HalpPicVectorRedirect[16] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
__attribute__((weak)) UCHAR HalpPicPolarity[16] = {0};
__attribute__((weak)) UCHAR HalpPicTrigger[16] = {0};

#ifndef _M_AMD64
const UCHAR
HalpIRQLtoTPR[32] =
{
    0x00, /*  0 PASSIVE_LEVEL */
    0x3d, /*  1 APC_LEVEL */
    0x41, /*  2 DISPATCH_LEVEL */
    0x41, /*  3 \  */
    0x51, /*  4  \ */
    0x61, /*  5  | */
    0x71, /*  6  | */
    0x81, /*  7  | */
    0x91, /*  8  | */
    0xa1, /*  9  | */
    0xb1, /* 10  | */
    0xb1, /* 11  | */
    0xb1, /* 12  | */
    0xb1, /* 13  | */
    0xb1, /* 14  | */
    0xb1, /* 15 DEVICE IRQL */
    0xb1, /* 16  | */
    0xb1, /* 17  | */
    0xb1, /* 18  | */
    0xb1, /* 19  | */
    0xb1, /* 20  | */
    0xb1, /* 21  | */
    0xb1, /* 22  | */
    0xb1, /* 23  | */
    0xb1, /* 24  | */
    0xb1, /* 25  / */
    0xb1, /* 26 /  */
    0xc1, /* 27 PROFILE_LEVEL */
    0xd1, /* 28 CLOCK2_LEVEL */
    0xe1, /* 29 IPI_LEVEL */
    0xef, /* 30 POWER_LEVEL */
    0xff, /* 31 HIGH_LEVEL */
};

const KIRQL
HalVectorToIRQL[16] =
{
       0, /* 00 PASSIVE_LEVEL */
    0xff, /* 10 */
    0xff, /* 20 */
       1, /* 3D APC_LEVEL */
       2, /* 41 DISPATCH_LEVEL */
       4, /* 50 \ */
       5, /* 60  \ */
       6, /* 70  | */
       7, /* 80 DEVICE IRQL */
       8, /* 90  | */
       9, /* A0  / */
      10, /* B0 /  */
      27, /* C1 PROFILE_LEVEL */
      28, /* D1 CLOCK2_LEVEL */
      29, /* E1 IPI_LEVEL / EF POWER_LEVEL */
      31, /* FF HIGH_LEVEL */
};
#endif

/* PRIVATE FUNCTIONS **********************************************************/

FORCEINLINE
ULONG
IOApicRead(UCHAR Register)
{
    /* Select the register, then do the read */
    ASSERT(Register <= 0x3F);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOREGSEL), Register);
    return READ_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOWIN));
}

FORCEINLINE
VOID
IOApicWrite(UCHAR Register, ULONG Value)
{
    /* Select the register, then do the write */
    ASSERT(Register <= 0x3F);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOREGSEL), Register);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOWIN), Value);
}

FORCEINLINE
VOID
ApicWriteIORedirectionEntry(
    UCHAR Index,
    IOAPIC_REDIRECTION_REGISTER ReDirReg)
{
    ASSERT(Index < APIC_MAX_IRQ);
    IOApicWrite(IOAPIC_REDTBL + 2 * Index, ReDirReg.Long0);
    IOApicWrite(IOAPIC_REDTBL + 2 * Index + 1, ReDirReg.Long1);
}

FORCEINLINE
IOAPIC_REDIRECTION_REGISTER
ApicReadIORedirectionEntry(
    UCHAR Index)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    ASSERT(Index < APIC_MAX_IRQ);
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Index);
    ReDirReg.Long1 = IOApicRead(IOAPIC_REDTBL + 2 * Index + 1);

    return ReDirReg;
}

FORCEINLINE
VOID
ApicRequestSelfInterrupt(IN UCHAR Vector, UCHAR TriggerMode)
{
    ULONG Flags;
    APIC_INTERRUPT_COMMAND_REGISTER Icr;
    APIC_INTERRUPT_COMMAND_REGISTER IcrStatus;

    /*
     * The IRR registers are spaced 16 bytes apart and hold 32 status bits each.
     * Pre-compute the register and bit that match our vector.
     */
    ULONG VectorHigh = Vector / 32;
    ULONG VectorLow = Vector % 32;
    ULONG Irr = APIC_IRR + 0x10 * VectorHigh;
    ULONG IrrBit = 1UL << VectorLow;

    /* Setup the command register */
    Icr.LongLong = 0;
    Icr.Vector = Vector;
    Icr.MessageType = APIC_MT_Fixed;
    Icr.TriggerMode = TriggerMode;
    Icr.DestinationShortHand = APIC_DSH_Self;

    /* Disable interrupts so that we can change IRR without being interrupted */
    Flags = __readeflags();
    _disable();

    /* Wait for the APIC to be idle. Bounded: a stuck ICR delivery
     * would hang mute with IF=0 (seen live on SMP bring-up). */
    {
        ULONG _icr = 10000000UL;
        do
        {
            IcrStatus.Long0 = ApicRead(APIC_ICR0);
        } while (IcrStatus.DeliveryStatus && --_icr);
    }

    /* Write high dword first, then low dword to send the interrupt */
    ApicWrite(APIC_ICR1, Icr.Long1);
    ApicWrite(APIC_ICR0, Icr.Long0);

    /* Wait until we see the interrupt request.
     * It will stay in requested state until we re-enable interrupts.
     * Bounded (1K): if TPR masks the vector the IRR bit never sets
     * and an unbounded wait burns ~10-30ms with IF=0 each time.
     * 1K suffices for the healthy case (the set is immediate). */
    {
        ULONG _irr = 1000UL;
        while (!(ApicRead(Irr) & IrrBit) && --_irr)
        {
            YieldProcessor();
        }
    }

    /* Finally, restore the original interrupt state */
    if (Flags & EFLAGS_INTERRUPT_MASK)
    {
        _enable();
    }
}

FORCEINLINE
VOID
ApicSendEOI(void)
{
    ApicWrite(APIC_EOI, 0);
}

FORCEINLINE
KIRQL
ApicGetProcessorIrql(VOID)
{
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_PPR));
}

FORCEINLINE
KIRQL
ApicGetCurrentIrql(VOID)
{
#ifdef _M_AMD64
    return (KIRQL)__readcr8();
#elif defined(APIC_LAZY_IRQL)
    /* Return the field in the PCR */
    return (KIRQL)__readfsbyte(FIELD_OFFSET(KPCR, Irql));
#else
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_TPR));
#endif
}

FORCEINLINE
VOID
ApicSetIrql(KIRQL Irql)
{
    /* Always program the hardware TPR on raise (lazy-only leaves
     * TPR=0: level IRQs re-fire inside their own ISR -> 0x0F, seen
     * with atapi on QEMU). NOTE: no IRR publish here (a publish
     * destabilized KVM at AP bring-up); the lower sweep below owns
     * exactness instead. */
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);
    ApicWrite(APIC_TPR, IrqlToTpr(Irql));
}
#define ApicRaiseIrql ApicSetIrql

#ifdef APIC_LAZY_IRQL
FORCEINLINE
VOID
ApicLowerIrql(KIRQL Irql)
{
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);

    /* Is the new Irql lower than set in the TPR? */
    if (Irql < KeGetPcr()->IRR)
    {
        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = Irql;

        /* Need to lower it back */
        ApicWrite(APIC_TPR, IrqlToTpr(Irql));
    }

    /* Net (class-only): if hard TPR is above the requested class,
     * correct downward. Exact-equality sweep destabilized KVM at AP
     * bring-up (4x identical mminit deaths); investigate
     * HalpIRQLtoTPR[] before re-tightening. */
    {
        ULONG _tpr = ApicRead(APIC_TPR) & 0xFF;
        ULONG _exp = HalpIRQLtoTPR[Irql];
        if ((_tpr & 0xF0) > (_exp & 0xF0))
        {
            ApicWrite(APIC_TPR, _exp);
            KeGetPcr()->IRR = Irql;
        }
    }
}
#else
#define ApicLowerIrql ApicSetIrql
#endif

UCHAR
FASTCALL
HalpIrqToVector(UCHAR Irq)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    /* Message-based (MSI/MSI-X) IRQs have no RTE: report free so the
     * caller allocates a vector instead of asserting on the window
     * (seen live: PnP satisfying IRQ 0x30 -> Register <= 0x3F fired
     * into KDB). */
    if (Irq >= APIC_MAX_IRQ) return APIC_FREE_VECTOR;

    /* Read low dword of the redirection entry */
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Irq);

    /* Return the vector */
    return (UCHAR)ReDirReg.Vector;
}

KIRQL
FASTCALL
HalpVectorToIrql(UCHAR Vector)
{
    return TprToIrql(Vector);
}

UCHAR
FASTCALL
HalpVectorToIrq(UCHAR Vector)
{
    return HalpVectorToIndex[Vector];
}

VOID
NTAPI
HalpSendEOI(VOID)
{
    ApicSendEOI();
}

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu)
{
    APIC_BASE_ADDRESS_REGISTER BaseRegister;
    APIC_SPURIOUS_INERRUPT_REGISTER SpIntRegister;
    LVT_REGISTER LvtEntry;

    /* Enable the APIC if it wasn't yet */
    BaseRegister.LongLong = __readmsr(MSR_APIC_BASE);
    BaseRegister.Enable = 1;
    BaseRegister.BootStrapCPUCore = (Cpu == 0);
    __writemsr(MSR_APIC_BASE, BaseRegister.LongLong);

    /* Set spurious vector and SoftwareEnable to 1 */
    SpIntRegister.Long = ApicRead(APIC_SIVR);
    SpIntRegister.Vector = APIC_SPURIOUS_VECTOR;
    SpIntRegister.SoftwareEnable = 1;
    SpIntRegister.FocusCPUCoreChecking = 0;
    ApicWrite(APIC_SIVR, SpIntRegister.Long);

    /* Read the version and save it globally */
    if (Cpu == 0) ApicVersion = ApicRead(APIC_VER);

    /* Set the mode to flat (max 8 CPUs supported!) */
    ApicWrite(APIC_DFR, APIC_DF_Flat);

    /* Set logical apic ID */
    ApicWrite(APIC_LDR, ApicLogicalId(Cpu) << 24);

    /* Set the spurious ISR: full trap frame + EOI exit discipline,
     * same as the clock ISR (a bare iret handler can't EOI safely
     * if the vector ever fires under SMP). */
    KeRegisterInterruptHandler(APIC_SPURIOUS_VECTOR, HalpSpuriousInterrupt);

    /* Create a template LVT */
    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_FREE_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    LvtEntry.DeliveryStatus = 0;
    LvtEntry.RemoteIRR = 0;
    LvtEntry.TriggerMode = APIC_TGM_Edge;
    LvtEntry.Mask = 1;
    LvtEntry.TimerMode = 0;

    /* Initialize and mask LVTs */
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_THRMLVTR, LvtEntry.Long);
    ApicWrite(APIC_PCLVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT0LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT1LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT2LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT3LVTR, LvtEntry.Long);

    /* LINT0 */
    LvtEntry.Vector = APIC_SPURIOUS_VECTOR;
    LvtEntry.MessageType = APIC_MT_ExtInt;
    ApicWrite(APIC_LINT0, LvtEntry.Long);

    /* Enable LINT1 (NMI) */
    LvtEntry.Mask = 0;
    LvtEntry.Vector = APIC_NMI_VECTOR;
    LvtEntry.MessageType = APIC_MT_NMI;
    LvtEntry.TriggerMode = APIC_TGM_Level;
    ApicWrite(APIC_LINT1, LvtEntry.Long);

    /* Leave the error LVT masked (no handler registered; an
     * unmasked error vector fires Unexpected prematurely). */
    LvtEntry.Mask = 1;
    /* Clear latched ESR (write-0 + read, per Intel spec). */
    ApicWrite(APIC_ESR, 0);
    (void)ApicRead(APIC_ESR);

    /* Enable error LVTR */
    LvtEntry.Vector = APIC_ERROR_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    ApicWrite(APIC_ERRLVTR, LvtEntry.Long);

    /* Set the IRQL from the PCR */
    ApicSetIrql(KeGetPcr()->Irql);
#ifdef APIC_LAZY_IRQL
    /* Save the new hard IRQL in the IRR field */
    KeGetPcr()->IRR = KeGetPcr()->Irql;
#endif
}

/* Route device IRQs across CPUs. IRQs 10/11 (USB/IDE) and 14/15
 * (legacy IDE) stay on the BSP: on an AP a stuck level line is never
 * cleaned (its ISR doesn't run there yet) -> storm + timeouts, while
 * on the BSP the owner ISR cleans and I/O completes. */
UCHAR
HalpRoundRobinLapicId(UCHAR Index)
{
    static ULONG _rr = 0;
    ULONG _n = KeNumberProcessors ? KeNumberProcessors : 1;
    ULONG _cpu;
    if (_n > 8) _n = 8;
    if (_n <= 1) return (UCHAR)(ApicRead(APIC_ID) >> 24);
    /* TEMP-DEBUG AP-death hunt (real HW hangs in KiReleaseDispatcherLock
     * right after RTEDBG idx11, AFFDBG pairs complete so migration is
     * fine: the AP itself dies taking a device IRQ (wild driver call)
     * and BSP wedges waiting for it. Pin everything to the BSP caller
     * so the AP never takes devices (clock/IPI/TLB still work); if HW
     * boots, confirmed. Revert-or-policy after test. */
    (void)_rr; (void)_cpu; (void)Index;
    {
        static BOOLEAN _pinmsg = FALSE;
        if (!_pinmsg)
        {
            _pinmsg = TRUE;
            DbgPrint("PINHACK active: all device IRQs to BSP only\n");
        }
    }
    return (UCHAR)(ApicRead(APIC_ID) >> 24);
}

UCHAR
NTAPI
HalpAllocateSystemInterrupt(
    _In_ UCHAR Irq,
    _In_ UCHAR Vector)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    ASSERT(Irq < APIC_MAX_IRQ);
    ASSERT(HalpVectorToIndex[Vector] == APIC_FREE_VECTOR);

    /* Setup a redirection entry: Fixed/Physical, destination set for
     * real in Enable (entry goes in masked). */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.DeliveryStatus = 0;
    ReDirReg.Polarity = 0;
    ReDirReg.RemoteIRR = 0;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Reserved = 0;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    /* Initialize entry */
    ApicWriteIORedirectionEntry(Irq, ReDirReg);

    /* Save irq in the table */
    HalpVectorToIndex[Vector] = Irq;

    return Vector;
}

ULONG
NTAPI
HalpGetRootInterruptVector(
    _In_ ULONG BusInterruptLevel,
    _In_ ULONG BusInterruptVector,
    _Out_ PKIRQL OutIrql,
    _Out_ PKAFFINITY OutAffinity)
{
    UCHAR Vector;
    KIRQL Irql;

    /* Get the vector currently registered */
    Vector = HalpIrqToVector(BusInterruptLevel);

    /* Check if it's used */
    if (Vector != APIC_FREE_VECTOR)
    {
        /* If the owner matches, fast path as before; otherwise the
         * tables desynced across reservations (this IRQ's vector
         * belongs to another IRQ) -> fall through to alloc a free
         * vector (correct for sharing/collisions). */
        if (HalpVectorToIndex[Vector] == BusInterruptLevel)
        {
            /* Calculate IRQL */
            *OutIrql = HalpVectorToIrql(Vector);
            goto Exit;
        }
        DbgPrint("HalpGetRootInterruptVector: IRQ %lu stale vec %02x (owner %02x), reallocating\n",
                 BusInterruptLevel, Vector, HalpVectorToIndex[Vector]);
    }
    {
        ULONG Offset;

        /* Outer loop to find alternative slots, when all IRQLs are in use */
        for (Offset = 0; Offset < 15; Offset++)
        {
            /* Loop allowed IRQL range */
            for (Irql = CLOCK_LEVEL - 1; Irql >= CMCI_LEVEL; Irql--)
            {
                /* Calculate the vactor */
                Vector = IrqlToTpr(Irql) + Offset;

                /* Check if the vector is free */
                if (HalpVectorToIrq(Vector) == APIC_FREE_VECTOR)
                {
                    /* Found one, allocate the interrupt */
                    Vector = HalpAllocateSystemInterrupt(BusInterruptLevel, Vector);
                    *OutIrql = Irql;
                    goto Exit;
                }
            }
        }

        DPRINT1("Failed to get an interrupt vector for IRQ %lu\n", BusInterruptLevel);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

Exit:

    *OutAffinity = HalpDefaultInterruptAffinity;
    ASSERT(HalpDefaultInterruptAffinity);

    return Vector;
}

VOID
NTAPI
ApicInitializeIOApic(VOID)
{
    PHARDWARE_PTE Pte;
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ULONG Vector;

    /* Map the I/O Apic page */
    Pte = HalAddressToPte(IOAPIC_BASE);
    Pte->PageFrameNumber = IOAPIC_PHYS_BASE / PAGE_SIZE;
    Pte->Valid = 1;
    Pte->Write = 1;
    Pte->Owner = 1;
    Pte->CacheDisable = 1;
    Pte->Global = 1;
    _ReadWriteBarrier();
    /* Invalidate TLB after touching the PTE, otherwise the CPU may
     * keep using the stale translation -> corruption/GPF. */
    __invlpg((PVOID)IOAPIC_BASE);

    /* Setup a redirection entry */
    ReDirReg.Vector = APIC_FREE_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.DeliveryStatus = 0;
    ReDirReg.Polarity = 0;
    ReDirReg.RemoteIRR = 0;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Reserved = 0;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    /* Loop all table entries */
    for (Index = 0; Index < APIC_MAX_IRQ; Index++)
    {
        /* Initialize entry */
        ApicWriteIORedirectionEntry(Index, ReDirReg);
    }

    /* Init the vactor to index table */
    for (Vector = 0; Vector <= 255; Vector++)
    {
        HalpVectorToIndex[Vector] = APIC_FREE_VECTOR;
    }

    /* Enable the timer interrupt (but keep it masked) */
    ReDirReg.Vector = APIC_CLOCK_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.TriggerMode = APIC_TGM_Level;
    ReDirReg.Mask = 1;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;
    ApicWriteIORedirectionEntry(APIC_CLOCK_INDEX, ReDirReg);
}

VOID
NTAPI
HalpInitializePICs(IN BOOLEAN EnableInterrupts)
{
    ULONG_PTR EFlags;

    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    /* Initialize and mask the PIC */
    HalpInitializeLegacyPICs();

    /* Initialize the I/O APIC */
    ApicInitializeIOApic();

    /* Manually reserve some vectors */
    HalpVectorToIndex[APC_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[DISPATCH_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_IPI_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_CLOCK_VECTOR] = 8;
    HalpVectorToIndex[CLOCK_IPI_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_SPURIOUS_VECTOR] = APIC_RESERVED_VECTOR;

    /* Set interrupt handlers in the IDT */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, HalpClockInterrupt);
    KeRegisterInterruptHandler(CLOCK_IPI_VECTOR, HalpClockIpi);
#ifndef _M_AMD64
    KeRegisterInterruptHandler(APC_VECTOR, HalpApcInterrupt);
    KeRegisterInterruptHandler(DISPATCH_VECTOR, HalpDispatchInterrupt);
#endif

    /* Register the vectors for APC and dispatch interrupts */
    HalpRegisterVector(IDT_INTERNAL, 0, APC_VECTOR, APC_LEVEL);
    HalpRegisterVector(IDT_INTERNAL, 0, DISPATCH_VECTOR, DISPATCH_LEVEL);

    /* Restore interrupt state */
    if (EnableInterrupts) EFlags |= EFLAGS_INTERRUPT_MASK;
    __writeeflags(EFlags);
}


/* SPURIOUS + IPI TRAPS *******************************************************/

VOID
FASTCALL
HalpSpuriousInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    /* Enter trap (sets up the frame the exit path validates,
     * same discipline as the clock ISR). */
    KiEnterInterruptTrap(TrapFrame);

    KiEoiHelper(TrapFrame);
}

VOID
FASTCALL
HalpIpiInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL Irql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

    /* Start the interrupt */
    if (!HalBeginSystemInterrupt(IPI_LEVEL, APIC_IPI_VECTOR, &Irql))
    {
        /* Spurious, just end the interrupt */
        KiEoiHelper(TrapFrame);
        return;
    }

    /* Dispatch to the kernel's generic (exported) IPI service routine */
    KiIpiServiceRoutine(TrapFrame, NULL);

    /* End the interrupt */
    KiEndInterrupt(Irql, TrapFrame);
}


/* SOFTWARE INTERRUPT TRAPS ***************************************************/

#ifndef _M_AMD64
VOID
DECLSPEC_NORETURN
FASTCALL
HalpApcInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KPROCESSOR_MODE ProcessorMode;
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == APC_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(APC_LEVEL, APC_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Save the old IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < APC_LEVEL);
#endif

    /* Raise to APC_LEVEL */
    ApicRaiseIrql(APC_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Kernel or user APC? */
    if (KiUserTrap(TrapFrame)) ProcessorMode = UserMode;
    else if (TrapFrame->EFlags & EFLAGS_V86_MASK) ProcessorMode = UserMode;
    else ProcessorMode = KernelMode;

    /* Enable interrupts and call the kernel's APC interrupt handler */
    _enable();
    KiDeliverApc(ProcessorMode, NULL, TrapFrame);

    /* Disable interrupts */
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}

VOID
DECLSPEC_NORETURN
FASTCALL
HalpDispatchInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == DISPATCH_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(DISPATCH_LEVEL, DISPATCH_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Get the current IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < DISPATCH_LEVEL);
#endif

    /* Raise to DISPATCH_LEVEL */
    ApicRaiseIrql(DISPATCH_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Enable interrupts and call the kernel's DPC interrupt handler */
    _enable();
    KiDispatchInterrupt();
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}
#endif


/* SOFTWARE INTERRUPTS ********************************************************/


VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    /* Convert irql to vector and request an interrupt */
    ApicRequestSelfInterrupt(IrqlToSoftVector(Irql), APIC_TGM_Edge);
}

VOID
FASTCALL
HalClearSoftwareInterrupt(
    IN KIRQL Irql)
{
    /* Nothing to do */
}


/* SYSTEM INTERRUPTS **********************************************************/

BOOLEAN
NTAPI
HalEnableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql,
    IN KINTERRUPT_MODE InterruptMode)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT((IrqlToTpr(Irql) & 0xF0) == (Vector & 0xF0));

    /* Get the irq for this vector */
    Index = HalpVectorToIndex[Vector];

    /* Check if its valid */
    if (Index == APIC_FREE_VECTOR)
    {
        /* Interrupt is not in use */
        return FALSE;
    }

    /* Read the redirection entry */
    ReDirReg = ApicReadIORedirectionEntry(Index);

    /* Check if the interrupt is already enabled */
    if (ReDirReg.Mask == FALSE)
    {
        /* If the vector matches, there is nothing more to do,
           otherwise something is wrong. */
        return (ReDirReg.Vector == Vector);
    }

    /* Set up the redirection entry: Fixed/Physical, routed across
     * CPUs (10/11/14/15 stay on the BSP, see helper). */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.Destination = HalpRoundRobinLapicId(Index);
    ReDirReg.TriggerMode = (InterruptMode == LevelSensitive) ?
        APIC_TGM_Level : APIC_TGM_Edge;
    ReDirReg.Mask = FALSE;

    /* Honor MADT interrupt overrides for ISA IRQs (board wiring wins
     * over the driver-supplied Mode): find the SourceIrq whose Global
     * maps to this RTE index and apply explicit polarity/trigger.
     * Wrong trigger here means missed (hang) or storming (0x0F) IRQs;
     * seen live on Q35+ (storm) and real PCH (silence). */
    {
        UCHAR _irq;
        BOOLEAN _explicitPol = FALSE;
        for (_irq = 0; _irq < 16; _irq++)
        {
            if (HalpPicVectorRedirect[_irq] != Index) continue;
            if (HalpPicPolarity[_irq] == 1)
            {
                ReDirReg.Polarity = 0; /* active-high */
                _explicitPol = TRUE;
            }
            else if (HalpPicPolarity[_irq] == 3)
            {
                ReDirReg.Polarity = 1; /* active-low */
                _explicitPol = TRUE;
            }
            if (HalpPicTrigger[_irq] == 1)
                ReDirReg.TriggerMode = APIC_TGM_Edge;
            else if (HalpPicTrigger[_irq] == 3)
                ReDirReg.TriggerMode = APIC_TGM_Level;
            break;
        }
        /* No explicit MADT polarity (conforms/absent): a level IRQ on a
         * legacy pin (< 16) is a PCI INTx, which idles HIGH (open-drain
         * active-low). Leaving it active-high makes the IOAPIC see a
         * permanent assert -> instant storm on unmask (both EHCI lines
         * flooding in the first tick on real PCH, while QEMU q35 - whose
         * PCI model is active-high - stays clean and PIC HAL - which
         * programs no polarity at all - is fast; APIC-UP storms too, so
         * SMP is exonerated). Linux parity (PCI conforms => low/level).
         * Edge keeps the ISA default (high). Native GSIs (>= 16) are
         * left alone. Explicit MADT polarity above still wins. */
        if (!_explicitPol &&
            InterruptMode == LevelSensitive &&
            Index < 16)
            ReDirReg.Polarity = 1; /* active-low */
    }

    /* Write back the entry. Mask first: the RTE is two 32-bit MMIO
     * writes (non-atomic); an AP taking this IRQ between them vectors
     * through a torn entry (mixed old/new vector/dest -> wild jump on
     * QEMU/flash, silent hang on HW when BSP then waits for the dead
     * AP). Masked RTEs latch (level re-fires, edge may lose one during
     * PnP only); pending IRQs fire right after unmask on the full
     * entry. Seen live: reset right after idx11 programming, hang
     * after its re-programming. */
    ReDirReg.Mask = TRUE;
    ApicWriteIORedirectionEntry(Index, ReDirReg);
    ReDirReg.Mask = FALSE;

    /* Write back the entry (unmasked) */
    ApicWriteIORedirectionEntry(Index, ReDirReg);

    /* TEMP-DEBUG IRQ routing hunt: dump every programmed RTE (compare
     * with board MADT on real HW; wrong trigger/dest/mask hangs device
     * completion waits). Revert after test. */
    {
        IOAPIC_REDIRECTION_REGISTER _rd;
        _rd = ApicReadIORedirectionEntry(Index);
        DbgPrint("RTEDBG idx %02u vec %02x trig %s pol %s dst %02x mask %u\n",
                 (unsigned)Index, (unsigned)Vector,
                 _rd.TriggerMode ? "level" : "edge",
                 _rd.Polarity ? "low" : "high",
                 (unsigned)_rd.Destination, (unsigned)_rd.Mask);
    }

    return TRUE;
}

VOID
NTAPI
HalDisableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT(Vector < RTL_NUMBER_OF(HalpVectorToIndex));

    Index = HalpVectorToIndex[Vector];

    /* Read lower dword of redirection entry */
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Index);

    /* Mask it */
    ReDirReg.Mask = 1;

    /* Write back lower dword */
    IOApicWrite(IOAPIC_REDTBL + 2 * Index, ReDirReg.Long0);
}

VOID
NTAPI
HalRearmSystemInterrupt(
    IN ULONG Vector)
{
    /* Clear ONLY the mask bit of a storm-masked RTE (no reprogram:
     * vector/dest/trigger stay as programmed). Self-healing for the
     * kernel storm guard: unmask on driver connect (chained connects
     * never call HalEnableSystemInterrupt, so a guard mask would
     * otherwise deafen the line forever). No-op if already unmasked
     * or vector unmapped. */
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    if (Vector >= RTL_NUMBER_OF(HalpVectorToIndex)) return;
    Index = HalpVectorToIndex[Vector];
    if (Index >= APIC_MAX_IRQ) return;
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Index);
    /* TEMP-DIAG mask-vs-wire (revert): live RTE state every ~10s per
     * index. If livemask=1 algo re-enmascara por detras (SMM?); si 0
     * con INT pendiente y sin ISR, el cable no afirma (wire/router). */
    {
        static volatile LONG _rteN[APIC_MAX_IRQ] = {0};
        /* Rearm corre ~2x/s por vector: imprime 1 de cada 20 (~10s). */
        if ((InterlockedIncrement(&_rteN[Index]) % 20) == 1)
        {
            DbgPrint("RTELIVE idx %u mask %lu pol %s trig %s vec %02lx irr %lu\n",
                     (unsigned)Index,
                     (unsigned long)ReDirReg.Mask,
                     ReDirReg.Polarity ? "low" : "high",
                     ReDirReg.TriggerMode ? "level" : "edge",
                     (unsigned long)ReDirReg.Vector,
                     (unsigned long)ReDirReg.RemoteIRR);
        }
    }
    if (ReDirReg.Mask == 0) return;
    ReDirReg.Mask = 0;
    IOApicWrite(IOAPIC_REDTBL + 2 * Index, ReDirReg.Long0);
}

BOOLEAN
NTAPI
HalBeginSystemInterrupt(
    IN KIRQL Irql,
    IN ULONG Vector,
    OUT PKIRQL OldIrql)
{
    KIRQL CurrentIrql;

    /* Get the current IRQL */
    CurrentIrql = ApicGetCurrentIrql();

#ifdef APIC_LAZY_IRQL
    /* Check if this interrupt is allowed */
    if (CurrentIrql >= Irql)
    {
        IOAPIC_REDIRECTION_REGISTER RedirReg;
        UCHAR Index;

        /* End this interrupt */
        ApicSendEOI();

        /* Get the irq for this vector */
        Index = HalpVectorToIndex[Vector];

        /* Check if it's valid */
        if (Index < APIC_MAX_IRQ)
        {
            /* Read the I/O redirection entry */
            RedirReg = ApicReadIORedirectionEntry(Index);

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, (UCHAR)RedirReg.TriggerMode);
       }
       else
       {
            /* This should be a reserved vector! */
            ASSERT(Index == APIC_RESERVED_VECTOR);

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, APIC_TGM_Edge);
       }

        /* Raise TPR only AFTER the send: requested with TPR still low,
         * the IRR accepts it; once raised it stays pending until soft
         * lowers, then the ISR runs. Raising first risks the IRR never
         * accepting the re-request (lost interrupt). */
        ApicWrite(APIC_TPR, IrqlToTpr(CurrentIrql));

        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = CurrentIrql;

        /* Pretend it was a spurious interrupt */
        return FALSE;
    }
#endif
    /* Save the current IRQL */
    *OldIrql = CurrentIrql;

    /* Set the new IRQL */
    ApicRaiseIrql(Irql);

    /* Turn on interrupts */
    _enable();

    /* Success */
    return TRUE;
}

VOID
NTAPI
HalEndSystemInterrupt(
    IN KIRQL OldIrql,
    IN PKTRAP_FRAME TrapFrame)
{
    /* Send an EOI */
    ApicSendEOI();

    /* EOI the PIC too (dual-safe). On q35, device IRQs arrive via
     * PIC/ExtINT (LVT0) but nobody EOIs the PIC (APIC only): the ISR
     * stays stuck and every later IRQ from the same PIC is lost.
     * Nonspecific EOI to inactive PICs is a no-op (safe). */
    __outbyte(0xA0, 0x20);
    __outbyte(0x20, 0x20);

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);
}


/* IRQL MANAGEMENT ************************************************************/

#ifndef _M_AMD64
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Read the current TPR and convert it to an IRQL */
    return ApicGetCurrentIrql();
}

VOID
FASTCALL
KfLowerIrql(
    IN KIRQL OldIrql)
{
#if DBG
    /* Validate correct lower */
    if (OldIrql > ApicGetCurrentIrql())
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif
    /* Set the new IRQL */
    ApicLowerIrql(OldIrql);
}

KIRQL
FASTCALL
KfRaiseIrql(
    IN KIRQL NewIrql)
{
    KIRQL OldIrql;

    /* Read the current IRQL */
    OldIrql = ApicGetCurrentIrql();
#if DBG
    /* Validate correct raise */
    if (OldIrql > NewIrql)
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Convert the new IRQL to a TPR value and write the register */
    ApicRaiseIrql(NewIrql);

    /* Return old IRQL */
    return OldIrql;
}

KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    return KfRaiseIrql(DISPATCH_LEVEL);
}

KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    return KfRaiseIrql(SYNCH_LEVEL);
}

#endif /* !_M_AMD64 */

