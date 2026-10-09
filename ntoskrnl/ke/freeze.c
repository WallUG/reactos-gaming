/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/freeze.c
 * PURPOSE:         Routines for freezing and unfreezing processors for
 *                  kernel debugger synchronization.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Freeze data (per-CPU: KdEnterDebugger can run concurrently on both
 * CPUs on i386 (no freeze IPI), and a single global saved IRQL raced:
 * one CPU's restore clobbered the other's, leaving IRQL stuck at
 * DISPATCH and tripping PAGED_CODE asserts later. Enter/exit always
 * pair on the same CPU (no migration at HIGH IRQL).) */
KIRQL KiOldIrql[MAXIMUM_PROCESSORS];
ULONG KiFreezeFlag;

/* FUNCTIONS ******************************************************************/

BOOLEAN
NTAPI
KeFreezeExecution(IN PKTRAP_FRAME TrapFrame,
                  IN PKEXCEPTION_FRAME ExceptionFrame)
{
    BOOLEAN Enable;
    KIRQL OldIrql;

#ifndef CONFIG_SMP
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);
#endif

    /* Disable interrupts, get previous state and set the freeze flag */
    Enable = KeDisableInterrupts();
    KiFreezeFlag = 4;

#ifndef CONFIG_SMP
    /* Raise IRQL if we have to */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        OldIrql = KeRaiseIrqlToDpcLevel();
#else
    /* Raise IRQL to HIGH_LEVEL */
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
#endif

#ifdef CONFIG_SMP
    /* Architecture specific freeze code */
    KxFreezeExecution();
#endif

    /* Save the old IRQL to be restored on unfreeze */
    KiOldIrql[KeGetCurrentProcessorNumber()] = OldIrql;

    /* Return whether interrupts were enabled */
    return Enable;
}

VOID
NTAPI
KeThawExecution(IN BOOLEAN Enable)
{
    ULONG Cpu = KeGetCurrentProcessorNumber();
#ifdef CONFIG_SMP
    /* Architecture specific thaw code */
    KxThawExecution();
#endif

    /* Clear the freeze flag */
    KiFreezeFlag = 0;

    /* Cleanup CPU caches */
    KeFlushCurrentTb();

    /* Restore the old IRQL */
#ifndef CONFIG_SMP
    if (KiOldIrql[Cpu] < DISPATCH_LEVEL)
#endif
    KeLowerIrql(KiOldIrql[Cpu]);

    /* Re-enable interrupts */
    KeRestoreInterrupts(Enable);
}
