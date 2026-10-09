/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor freeze support for i386
 * COPYRIGHT:
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Tracks KxFreezeExecution lowerings so KxThawExecution can undo them.
 * KeThawExecution restores the pre-freeze IRQL with KeLowerIrql, which
 * bugchecks if asked to go upward (e.g. back to HIGH_LEVEL after an
 * entry that started at HIGH_LEVEL). Raising here first keeps that
 * restore a pure lowering. Nesting-safe: freeze/thaw always pair. */
static LONG KxFreezeLowerCount = 0;

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    /*
     * i386 still uses the legacy IPI model, which has no freeze IPI,
     * so there is no way to freeze the other processors here. The
     * calling CPU is already at HIGH_LEVEL (see KeFreezeExecution),
     * which is sufficient for early-boot debugger traffic.
     *
     * This runs inside KdEnterDebugger: it must stay silent. Any
     * DbgPrint/UNIMPLEMENTED here re-enters the debugger through
     * int2D and recurses until the kernel stack overflows, ending
     * in a 0x7F double fault during KdInitSystem symbol loading.
     *
     * Lower back to DISPATCH_LEVEL: with no freeze happening, HIGH_LEVEL
     * only prevents useful work inside the debugger (pool allocations,
     * e.g. KDB symbol loading for boot drivers, are illegal above
     * DISPATCH_LEVEL and bugcheck with BAD_POOL_CALLER). Interrupts stay
     * disabled throughout the session anyway (see KeDisableInterrupts
     * in KeFreezeExecution), so this changes no masking, only the IRQL
     * value, matching what UP builds (which raise to DPC only) do.
     */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        KeLowerIrql(DISPATCH_LEVEL);
        InterlockedIncrement(&KxFreezeLowerCount);
    }
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    /* Undo the KxFreezeExecution lowering (see above) so that the
     * KeLowerIrql(KiOldIrql) in KeThawExecution never goes upward.
     * Raising to HIGH_LEVEL when already there is a harmless no-op.
     * Must stay silent (same re-entrancy reason as above). */
    if (InterlockedDecrement(&KxFreezeLowerCount) >= 0)
    {
        KIRQL OldIrql;
        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    }
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    UNREFERENCED_PARAMETER(ProcessorIndex);

    /* Processor switching is not supported on i386. Must stay silent
     * for the same re-entrancy reason as above. */
    return ContinueError;
}
