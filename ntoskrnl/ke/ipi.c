/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/ipi.c
 * PURPOSE:         Inter-Processor Packet Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* PRIVATE FUNCTIONS *********************************************************/

#ifndef _M_AMD64

/* Packet context for KeIpiGenericCall. Lives on the caller's stack:
 * the caller does not return until every target has run the function
 * (see the TargetSet wait in KeIpiGenericCall), so this is safe. */
typedef struct _KIPI_GENERIC_CALL_CONTEXT
{
    PKIPI_BROADCAST_WORKER Function;
    ULONG_PTR Argument;
} KIPI_GENERIC_CALL_CONTEXT, *PKIPI_GENERIC_CALL_CONTEXT;

VOID
NTAPI
KiIpiGenericCallTarget(IN PKIPI_CONTEXT PacketContext,
                       IN PVOID BroadcastFunction,
                       IN PVOID Argument,
                       IN PVOID Count)
{
    PKIPI_GENERIC_CALL_CONTEXT Context = (PKIPI_GENERIC_CALL_CONTEXT)PacketContext;

    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(Count);

    /* Run the broadcast function with its argument */
    Context->Function(Context->Argument);
}

VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors,
          IN ULONG IpiRequest)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY TargetMask = 0;
    KAFFINITY Current;
    LONG i;

    /* Set the request bit on every started target processor */
    for (i = 0, Current = 1; i < 32; i++, Current <<= 1)
    {
        /* Skip processors outside the set (and never signal ourselves) */
        if (!(TargetProcessors & Current)) continue;
        if (Current == Prcb->SetMember) continue;

        /* Skip processors that were never started */
        TargetPrcb = (i < KeNumberProcessors) ? KiProcessorBlock[i] : NULL;
        if (TargetPrcb == NULL) continue;

        /* Raise the request and remember this target */
        InterlockedBitTestAndSet((PLONG)&TargetPrcb->IpiFrozen, IpiRequest);
        TargetMask |= Current;
    }

    /* Deliver all IPIs with a single request */
    if (TargetMask != 0)
    {
        HalRequestIpi(TargetMask);
    }
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetProcessors,
                IN PKIPI_WORKER WorkerFunction,
                IN PKIPI_BROADCAST_WORKER BroadcastFunction,
                IN ULONG_PTR Context,
                IN PULONG Count)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY TargetMask = 0;
    KAFFINITY Current;
    LONG i;
    ULONG TargetCount = 0;

    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Count);

    /* Build the effective target set: only started processors,
     * never ourselves (callers always run the local part directly) */
    for (i = 0, Current = 1; i < 32; i++, Current <<= 1)
    {
        /* Skip processors outside the set (and never signal ourselves) */
        if (!(TargetProcessors & Current)) continue;
        if (Current == Prcb->SetMember) continue;

        /* A processor without PRCB was never started: nothing to wait for */
        TargetPrcb = (i < KeNumberProcessors) ? KiProcessorBlock[i] : NULL;
        if (TargetPrcb == NULL) continue;

        TargetMask |= Current;
        TargetCount++;
    }

    /* No targets: callers still run their local part */
    if (TargetCount == 0) return;

    /* Publish the packet on our own PRCB. Targets pick it up through
     * our Prcb (see KiIpiServiceRoutine), so it stays valid until every
     * target has cleared its bit in TargetSet below. */
    Prcb->TargetSet = TargetMask;
    Prcb->WorkerRoutine = WorkerFunction;
    Prcb->CurrentPacket[0] = (PVOID)Context;
    Prcb->CurrentPacket[1] = (PVOID)(ULONG_PTR)TargetCount;
    Prcb->CurrentPacket[2] = (PVOID)0;
    KeMemoryBarrier();

    /* Claim every target and raise the rendezvous request */
    for (i = 0, Current = 1; i < 32; i++, Current <<= 1)
    {
        if (!(TargetMask & Current)) continue;

        TargetPrcb = KiProcessorBlock[i];

        /* Wait until any previous packet on the target is done.
         * Bounded + loud (same discipline as the TargetSet waits): a
         * stuck SignalDone (busy target that never clears, e.g. dead
         * AP) hung the boot mute here on real HW. Diagnose loudly;
         * we do NOT skip the target (its packet slot is genuinely
         * busy) so this only reports. */
        {
            ULONG _spin = 0;
            while (InterlockedCompareExchangeUL((PULONG)&TargetPrcb->SignalDone,
                                                (ULONG)Prcb,
                                                0) != 0)
            {
                YieldProcessor();
                if (++_spin > 10000000UL)
                {
                    DbgPrint("KiIpiSendPacket: SignalDone of cpu %lu stuck %p, still waiting (target may be dead)\n",
                             (ULONG)i, (PVOID)TargetPrcb->SignalDone);
                    _spin = 0;
                }
            }
        }

        /* Request the rendezvous interrupt */
        InterlockedBitTestAndSet((PLONG)&TargetPrcb->IpiFrozen, IPI_SYNCH_REQUEST);
    }

    /* Deliver all IPIs with a single request */
    HalRequestIpi(TargetMask);
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
    PKPRCB Prcb = KeGetCurrentPrcb();

    UNREFERENCED_PARAMETER(PacketContext);

    /* Tell the packet owner that our part is done. The owner waits for
     * every target bit to clear (see KeIpiGenericCall/KeFlushEntireTb),
     * and KiIpiServiceRoutine clears it again below, which is harmless. */
    if (Prcb->SignalDone != NULL)
    {
        InterlockedBitTestAndReset((PLONG)&Prcb->SignalDone->TargetSet,
                                   KeGetCurrentProcessorNumber());
    }
    KeMemoryBarrierWithoutFence();
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile PULONG ReverseStall)
{
    /* Signal completion first */
    KiIpiSignalPacketDone(PacketContext);

    /* Then stall until released */
    while (*ReverseStall != 0)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }
}

#if 0
VOID
NTAPI
KiIpiSendRequest(IN KAFFINITY TargetSet,
                 IN ULONG IpiRequest)
{
#ifdef CONFIG_SMP
    LONG i;
    PKPRCB Prcb;
    KAFFINITY Current;

    for (i = 0, Current = 1; i < KeNumberProcessors; i++, Current <<= 1)
    {
        if (TargetSet & Current)
        {
            /* Get the PRCB for this CPU */
            Prcb = KiProcessorBlock[i];

            InterlockedBitTestAndSet((PLONG)&Prcb->IpiFrozen, IpiRequest);
            HalRequestIpi(i);
        }
    }
#endif
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetSet,
                IN PKIPI_BROADCAST_WORKER WorkerRoutine,
                IN ULONG_PTR Argument,
                IN ULONG Count,
                IN BOOLEAN Synchronize)
{
#ifdef CONFIG_SMP
    KAFFINITY Processor;
    LONG i;
    PKPRCB Prcb, CurrentPrcb;
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() == SYNCH_LEVEL);

    CurrentPrcb = KeGetCurrentPrcb();
    (void)InterlockedExchangeUL(&CurrentPrcb->TargetSet, TargetSet);
    (void)InterlockedExchangeUL(&CurrentPrcb->WorkerRoutine, (ULONG_PTR)WorkerRoutine);
    (void)InterlockedExchangePointer(&CurrentPrcb->CurrentPacket[0], Argument);
    (void)InterlockedExchangeUL(&CurrentPrcb->CurrentPacket[1], Count);
    (void)InterlockedExchangeUL(&CurrentPrcb->CurrentPacket[2], Synchronize ? 1 : 0);

    for (i = 0, Processor = 1; i < KeNumberProcessors; i++, Processor <<= 1)
    {
        if (TargetSet & Processor)
        {
            Prcb = KiProcessorBlock[i];
            while (0 != InterlockedCompareExchangeUL(&Prcb->SignalDone, (LONG)CurrentPrcb, 0));
            InterlockedBitTestAndSet((PLONG)&Prcb->IpiFrozen, IPI_SYNCH_REQUEST);
            if (Processor != CurrentPrcb->SetMember)
            {
                HalRequestIpi(i);
            }
        }
    }
    if (TargetSet & CurrentPrcb->SetMember)
    {
        KeRaiseIrql(IPI_LEVEL, &oldIrql);
        KiIpiServiceRoutine(NULL, NULL);
        KeLowerIrql(oldIrql);
    }
#endif
}
#endif

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KiIpiServiceRoutine(IN PKTRAP_FRAME TrapFrame,
                    IN PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb;
    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    Prcb = KeGetCurrentPrcb();

    if (InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_APC))
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_DPC))
    {
        Prcb->DpcInterruptRequested = TRUE;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }

    if (InterlockedBitTestAndReset((PLONG)&Prcb->IpiFrozen, IPI_SYNCH_REQUEST))
    {
#if defined(_M_ARM) || defined(_M_AMD64)
        DbgBreakPoint();
#else
        (void)InterlockedDecrementUL(&Prcb->SignalDone->CurrentPacket[1]);
        if (InterlockedCompareExchangeUL(&Prcb->SignalDone->CurrentPacket[2], 0, 0))
        {
            while (0 != InterlockedCompareExchangeUL(&Prcb->SignalDone->CurrentPacket[1], 0, 0));
        }
        /* Run the worker. PKIPI_WORKER takes 4 parameters: the packet
         * context plus 3 optional ones. Passing all of them keeps the
         * stdcall callee/caller stack accounting balanced (calling a
         * 4-parameter stdcall worker with a single argument would
         * corrupt the target's stack on return). */
        Prcb->SignalDone->WorkerRoutine(Prcb->SignalDone->CurrentPacket[0],
                                        NULL,
                                        NULL,
                                        NULL);
        InterlockedBitTestAndReset((PLONG)&Prcb->SignalDone->TargetSet, KeGetCurrentProcessorNumber());
        if (InterlockedCompareExchangeUL(&Prcb->SignalDone->CurrentPacket[2], 0, 0))
        {
            while (0 != InterlockedCompareExchangeUL(&Prcb->SignalDone->TargetSet, 0, 0));
        }
        (void)InterlockedExchangePointer((PVOID*)&Prcb->SignalDone, NULL);
#endif // _M_ARM
    }
#endif
   return TRUE;
}

/*
 * @implemented
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(IN PKIPI_BROADCAST_WORKER Function,
                 IN ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#ifdef CONFIG_SMP
    KIPI_GENERIC_CALL_CONTEXT CallContext;
    KAFFINITY Affinity;
    ULONG Count;
    PKPRCB Prcb = KeGetCurrentPrcb();
#endif

    /* Raise to DPC level if required */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

#ifdef CONFIG_SMP
    /* Get current processor count and affinity */
    Count = KeNumberProcessors;
    Affinity = KeActiveProcessors;

    /* Exclude ourselves */
    Affinity &= ~Prcb->SetMember;
#endif

    /* Acquire the IPI lock */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    /* Make sure this is MP */
    if (Affinity)
    {
        /* The context lives on our stack. This is safe: we do not
         * return until every target has run the function (see the
         * TargetSet wait below). */
        CallContext.Function = Function;
        CallContext.Argument = Argument;

        /* Send an IPI */
        KiIpiSendPacket(Affinity,
                        KiIpiGenericCallTarget,
                        Function,
                        (ULONG_PTR)&CallContext,
                        &Count);

        /* Spin until the other processors have picked up the packet.
         * KiIpiServiceRoutine decrements the arrival counter on receipt. */
        while (Prcb->CurrentPacket[1] != NULL)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }
#endif

    /* Raise to IPI level */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

    /* Call the function */
    Status = Function(Argument);

#ifdef CONFIG_SMP
    /* If this is MP, wait for the other processors to finish */
    if (Affinity)
    {
        /* Sanity check */
        ASSERT(Prcb == KeGetCurrentPrcb());

        /* Every target clears its own bit when done (see
         * KiIpiSignalPacketDone/KiIpiServiceRoutine). Bounded like
         * KeFlushEntireTb: never hang mute on a dead target. */
        {
            ULONG _spin = 0;
            while (Prcb->TargetSet != 0)
            {
                YieldProcessor();
                KeMemoryBarrierWithoutFence();
                if (++_spin > 10000000UL)
                {
                    DbgPrint("KeIpiGenericCall: target(s) %lx never acked, continuing degraded\n",
                             (ULONG)Prcb->TargetSet);
                    Prcb->TargetSet = 0;
                    break;
                }
            }
        }
    }
#endif

    /* Release the lock */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
    return Status;
}

#endif // !_M_AMD64
