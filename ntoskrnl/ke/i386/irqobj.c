/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/i386/irqobj.c
 * PURPOSE:         Manages the Kernel's IRQ support for external drivers,
 *                  for the purposes of connecting, disconnecting and setting
 *                  up ISRs for drivers. The backend behind the Io* Interrupt
 *                  routines.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

ULONG KiISRTimeout = 55;
USHORT KiISROverflow = 30000;
extern ULONG NTAPI KiChainedDispatch2ndLvl(VOID);

/* TEMP-DIAG install-stamp hunt: KiConnectVectorToInterrupt stamps
 * every NormalConnect install here (memory, immune to serial loss);
 * the storm block dumps b1's stamp. Settles live-install vs stale
 * template once and for all. Revert after test. */
volatile ULONG_PTR KiVecInstallObj[256] = {0};
volatile ULONG_PTR KiVecInstallRet[256] = {0};
volatile ULONG KiVecInstallTick[256] = {0};

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
KiGetVectorDispatch(IN ULONG Vector,
                    IN PDISPATCH_INFO Dispatch)
{
    PKINTERRUPT_ROUTINE Handler;
    PVOID Current;
    UCHAR Type;
    UCHAR Entry;

    /* Check if this is a primary or 2nd-level dispatch */
    Type = HalSystemVectorDispatchEntry(Vector,
                                        &Dispatch->FlatDispatch,
                                        &Dispatch->NoDispatch);
    ASSERT(Type == 0);

    /* Get the IDT entry for this vector */
    Entry = HalVectorToIDTEntry(Vector);

    /* Setup the unhandled dispatch */
    Dispatch->NoDispatch = (PVOID)(((ULONG_PTR)&KiStartUnexpectedRange) +
                                   (Entry - PRIMARY_VECTOR_BASE) *
                                   KiUnexpectedEntrySize);

    /* Setup the handlers */
    Dispatch->InterruptDispatch = (PVOID)KiInterruptDispatch;
    Dispatch->FloatingDispatch = NULL; // Floating Interrupts are not supported
    Dispatch->ChainedDispatch = (PVOID)KiChainedDispatch;
    Dispatch->FlatDispatch = NULL;

    /* Get the current handler */
    Current = KeQueryInterruptHandler(Vector);

    /* Set the interrupt */
    Dispatch->Interrupt = CONTAINING_RECORD(Current,
                                            KINTERRUPT,
                                            DispatchCode);

    /* Check what this interrupt is connected to */
    if ((PKINTERRUPT_ROUTINE)Current == Dispatch->NoDispatch)
    {
        /* Not connected */
        Dispatch->Type = NoConnect;
    }
    else
    {
        /* Get the handler */
        Handler = Dispatch->Interrupt->DispatchAddress;
        if (Handler == Dispatch->ChainedDispatch)
        {
            /* It's a chained interrupt */
            Dispatch->Type = ChainConnect;
        }
        else if ((Handler == Dispatch->InterruptDispatch) ||
                 (Handler == Dispatch->FloatingDispatch))
        {
            /* It's unchained */
            Dispatch->Type = NormalConnect;
        }
        else
        {
            /* Unknown */
            Dispatch->Type = UnknownConnect;
        }
    }
}

VOID
NTAPI
KiConnectVectorToInterrupt(IN PKINTERRUPT Interrupt,
                           IN CONNECT_TYPE Type)
{
    DISPATCH_INFO Dispatch;
    PKINTERRUPT_ROUTINE Handler;

    /* Get vector data */
    KiGetVectorDispatch(Interrupt->Vector, &Dispatch);

    /* Check if we're only disconnecting */
    if (Type == NoConnect)
    {
        /* Set the handler to NoDispatch */
        Handler = Dispatch.NoDispatch;
    }
    else
    {
        /* Get the right handler */
        Handler = (Type == NormalConnect) ?
                  Dispatch.InterruptDispatch:
                  Dispatch.ChainedDispatch;
        ASSERT(Interrupt->FloatingSave == FALSE);

        /* Set the handler */
        Interrupt->DispatchAddress = Handler;

        /* Read note in trap.s -- patching not needed since JMP is static */

        /* Now set the final handler address */
        ASSERT(Dispatch.FlatDispatch == NULL);
        Handler = (PVOID)&Interrupt->DispatchCode;
    }

    /* Register the interrupt (local IDT; mirror to APs below) */
    KeRegisterInterruptHandler(Interrupt->Vector, Handler);

    /* TEMP-DIAG install-stamp: record NormalConnect installs (not
     * disconnects/chains). Revert after test. */
    if (Type == NormalConnect && Interrupt->Vector < 256)
    {
        KiVecInstallObj[Interrupt->Vector] = (ULONG_PTR)Interrupt;
        KiVecInstallRet[Interrupt->Vector] = (ULONG_PTR)_ReturnAddress();
        KiVecInstallTick[Interrupt->Vector] = KeTickCount.LowPart;
        /* TEMP-DIAG red-handed trap DISABLED 2026-10-08 (caught:
         * USBPORT_StartDevice; the reorder below should remove the
         * early window entirely). Revert after test. */
#if 0
        if (Interrupt->Vector == 0xB1 && KeTickCount.LowPart < 1000)
        {
            KeBugCheckEx(0xAAAAAAAA,
                         (ULONG_PTR)Interrupt,
                         (ULONG_PTR)_ReturnAddress(),
                         KeTickCount.LowPart,
                         0);
        }
#endif
    }

#ifdef CONFIG_SMP
    /* IDT-sync: each AP's IDT is a copy taken at its startup; vectors
     * registered later (PnP: IRQ->C1...) stayed only on the BSP and
     * the AP vectored to garbage. Mirror the entry (Offset only, like
     * the inline: Selector/Access/P come from the template). Direct
     * write without IPI: doesn't depend on the packet protocol (which
     * stalls with a deaf AP) and doesn't touch IRQL. */
    {
        UCHAR _entry = HalVectorToIDTEntry(Interrupt->Vector);
        ULONG_PTR _addr = PtrToUlong(Handler);
        ULONG _cpu;
        for (_cpu = 1; _cpu < (ULONG)KeNumberProcessors; _cpu++)
        {
            PKIDTENTRY _idt = KiGetApIdt(_cpu);
            if (_idt)
            {
                /* Halves with local IF=0 (like the inline). */
                BOOLEAN _w = KeDisableInterrupts();
                _idt[_entry].ExtendedOffset = (USHORT)(_addr >> 16);
                _idt[_entry].Offset = (USHORT)_addr;
                KeRestoreInterrupts(_w);
            }
        }
    }
#endif
}

FORCEINLINE
DECLSPEC_NORETURN
VOID
KiExitInterrupt(IN PKTRAP_FRAME TrapFrame,
                IN KIRQL OldIrql,
                IN BOOLEAN Spurious)
{
    /* Check if this was a real interrupt */
    if (!Spurious)
    {
        /* It was, disable interrupts and restore the IRQL */
        _disable();
        HalEndSystemInterrupt(OldIrql, TrapFrame);
    }

    /* Now exit the trap */
    KiEoiHelper(TrapFrame);
}

DECLSPEC_NORETURN
VOID
__cdecl
KiUnexpectedInterrupt(VOID)
{
    /* Crash the machine */
    KeBugCheck(TRAP_CAUSE_UNKNOWN);
}

VOID
FASTCALL
KiUnexpectedInterruptTailHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL OldIrql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

    /* Increase interrupt count */
    KeGetCurrentPrcb()->InterruptCount++;

    /* Start the interrupt */
    if (HalBeginSystemInterrupt(HIGH_LEVEL, TrapFrame->ErrCode, &OldIrql))
    {
        /* Warn user */
        DPRINT1("\n\x7\x7!!! Unexpected Interrupt 0x%02lx !!!\n", TrapFrame->ErrCode);

        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, FALSE);
    }
    else
    {
        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, TRUE);
    }
}

typedef
VOID
(FASTCALL *PKI_INTERRUPT_DISPATCH)(
    IN PKTRAP_FRAME TrapFrame,
    IN PKINTERRUPT Interrupt
);

/* Storm detection (SMP bring-up): a level line with no owner acking
 * it re-fires inside its own ISR thousands of times per tick: nested
 * frames overflow the interrupt stack -> triple-fault reset (seen on
 * real HW right after idx11 was programmed), or 100% ISR livelock
 * (silent hang), or nested same-lock 0x0F (seen with USB storms on
 * QEMU). Count per vector per tick; past the threshold, mask it and
 * shout once. The device goes silent (better than dead machine) and
 * the log names the stormer for a proper per-device fix. */
#define ISR_STORM_THRESHOLD 1000
volatile ULONG KiIsrStormTick[256] = {0};
volatile ULONG KiIsrStormCount[256] = {0};

/* TEMP-DIAG ATA-vs-HW hunt: total ISR entries per vector since boot
 * (distinguishes "ATA completion interrupts never arrive" from
 * "driver programs the channel wrong"). Dumped once from
 * IopMarkBootPartition (0x7B site). Revert after test. */
volatile ULONG KiIsrTotalCount[256] = {0};

/* TEMP-DIAG: set when the storm guard masks a vector (serial can lose
 * the HAL's own prints under load; this software flag survives and is
 * reported from the connect path, which always runs). */
volatile BOOLEAN KiIsrStormMasked[256] = {FALSE};

/* BISECT: once-ever storm reporting (per-tick-once flooded serial). */
volatile LONG KiIsrStormReported[256] = {0};

/* BISECT: last 5s-heartbeat tick per storming vector. */
volatile LONG KiIsrStormHeartbeat[256] = {0};

/* UNUSED (reverted 2026-10-08, kept for the next root-cause attempt). */
volatile LONG KiIsrDepth[256] = {0};

/* Same-CPU ISR/sync nesting (APIC bring-up): while a CPU runs inside
 * an ISR or a KeSynchronizeExecution routine for vector V (lock
 * held), a nested trap on the SAME vector on the SAME CPU must not
 * re-acquire the lock (-> 0x0F SPIN_LOCK_ALREADY_OWNED, seen on real
 * HW when PataEnableInterrupts asserts the shared level line with
 * the lock held). Other vectors and other CPUs are unaffected (no
 * shared state, plain stores, strictly paired set/clear). */
#define KI_ISR_MAX_CPUS 32
volatile LONG KiIsrCpuActive[KI_ISR_MAX_CPUS] = {0}; /* 0 = none, else vector+1 */

FORCEINLINE
BOOLEAN
KiIsrNestedSelf(IN PKINTERRUPT Interrupt)
{
    ULONG Cpu = KeGetCurrentProcessorNumber();
    if (Cpu >= KI_ISR_MAX_CPUS) return FALSE;
    return (KiIsrCpuActive[Cpu] == (LONG)Interrupt->Vector + 1);
}

FORCEINLINE
VOID
KiIsrMarkActive(IN PKINTERRUPT Interrupt)
{
    ULONG Cpu = KeGetCurrentProcessorNumber();
    if (Cpu < KI_ISR_MAX_CPUS) KiIsrCpuActive[Cpu] = (LONG)Interrupt->Vector + 1;
}

FORCEINLINE
VOID
KiIsrMarkIdle(VOID)
{
    ULONG Cpu = KeGetCurrentProcessorNumber();
    if (Cpu < KI_ISR_MAX_CPUS) KiIsrCpuActive[Cpu] = 0;
}

VOID
NTAPI
KiIsrDumpTotals(VOID)
{
    /* Vaciado: los contadores siguen para el guard; el dump por boot
     * se quito en la limpieza (el storm-path ya reporta). */
}

FORCEINLINE
BOOLEAN
KiIsrStormCheck(IN UCHAR Vector)
{
    ULONG Tick = KeTickCount.LowPart;
    ULONG Last = (ULONG)InterlockedExchange((PLONG)&KiIsrStormTick[Vector], (LONG)Tick);
    ULONG Count;

    /* TEMP-DIAG: count every ISR entry per vector (see KiIsrTotalCount) */
    InterlockedIncrement((PLONG)&KiIsrTotalCount[Vector]);

    if (Last != Tick)
    {
        /* New tick: reset the counter, allow one grace ISR that sets it to 1 */
        InterlockedExchange((PLONG)&KiIsrStormCount[Vector], 1);
        return FALSE;
    }

    Count = InterlockedIncrement((PLONG)&KiIsrStormCount[Vector]);
    if (Count == ISR_STORM_THRESHOLD)
    {
        /* Storm confirmed: storm-held mask via HAL (rearm/enable no lo
         * deshacen; una alocacion fresca lo levanta) + skip ISR + report
         * once EVER. */
        HalStormMaskVector(Vector);
        if (InterlockedExchange(&KiIsrStormReported[Vector], 1) == 0)
        {
            DbgPrint("ISRSTORM vector %02x storming (skip, no mask)\n",
                     (unsigned)Vector);
            KiIsrStormHeartbeat[Vector] = (LONG)Tick;
            /* TEMP-DIAG stale-IDT hunt: dump the LIVE IDT gate for
             * this vector (template+object vs Unexpected stub?).
             * Revert after test. */
            {
                KDESCRIPTOR _desc;
                PKIDTENTRY _gate;
                ULONG_PTR _stub;
                __sidt(&_desc.Limit);
                _gate = &((PKIDTENTRY)_desc.Base)[Vector];
                _stub = ((ULONG_PTR)&KiStartUnexpectedRange) +
                        ((ULONG_PTR)HalVectorToIDTEntry(Vector) - PRIMARY_VECTOR_BASE) *
                        KiUnexpectedEntrySize;
                DbgPrint("IDTGATE vector %02x Sel %04X Off %04X Acc %02X Ext %04X stub %p %s IDTLimit %04X\n",
                         (unsigned)Vector,
                         (unsigned)_gate->Selector,
                         (unsigned)_gate->Offset,
                         (unsigned)_gate->Access,
                         (unsigned)_gate->ExtendedOffset,
                         (void *)_stub,
                         ((((ULONG_PTR)_gate->Offset) |
                           (((ULONG_PTR)_gate->ExtendedOffset) << 16)) == _stub) ?
                         "STUB(fresh)" : "NOT-STUB",
                         (unsigned)_desc.Limit);
                /* TEMP-DIAG: dump the install stamp (live vs stale). */
                DbgPrint("VECSTAMP vector %02x obj %p ret %p tick %lu\n",
                         (unsigned)Vector,
                         (void *)KiVecInstallObj[Vector],
                         (void *)KiVecInstallRet[Vector],
                         (unsigned long)KiVecInstallTick[Vector]);
                /* TEMP-114: totales por vector al confirmar storm (quien
                 * mas dispara ademas del stormer). */
                {
                    ULONG _v;
                    for (_v = 0; _v < 256; _v++)
                    {
                        if (KiIsrTotalCount[_v] > 20)
                        {
                            DbgPrint("ISRVEC vector %02x total %lu\n",
                                     (unsigned)_v,
                                     (unsigned long)KiIsrTotalCount[_v]);
                        }
                    }
                }
            }
        }
        else if ((Tick - (ULONG)KiIsrStormHeartbeat[Vector]) >= 500)
        {
            /* Still storming 5s later: heartbeat with running total. */
            KiIsrStormHeartbeat[Vector] = (LONG)Tick;
            DbgPrint("ISRSTORM vector %02x still storming, total %lu\n",
                     (unsigned)Vector, (unsigned long)KiIsrTotalCount[Vector]);
        }
        KiIsrStormMasked[Vector] = TRUE; /* TEMP-DIAG (see decl) */
    }
    return (Count >= ISR_STORM_THRESHOLD);
}

VOID
FASTCALL
KiInterruptDispatch(IN PKTRAP_FRAME TrapFrame,
                    IN PKINTERRUPT Interrupt)
{
    KIRQL OldIrql = 0;

    /* Increase interrupt count */
    KeGetCurrentPrcb()->InterruptCount++;

    /* REVERTED 2026-10-08: same-vector nesting guard swallowed normal
     * entries on SMP (QEMU CD lost all completions -> 0x69;
     * ISRTOT stayed empty). Suspect: depth never returns to 0 on
     * some path, or AP/BSP same-vector concurrency. Root-cause with
     * a working baseline first. */

    /* Nested same-CPU/same-vector entry while the lock is held
     * (ISR or sync routine below): EOI via Begin/End, never ISR. */
    if (KiIsrNestedSelf(Interrupt))
    {
        if (HalBeginSystemInterrupt(Interrupt->SynchronizeIrql,
                                    Interrupt->Vector,
                                    &OldIrql))
        {
            KiExitInterrupt(TrapFrame, OldIrql, FALSE);
        }
        else
        {
            KiExitInterrupt(TrapFrame, OldIrql, TRUE);
        }
        return;
    }

    /* Storm guard: do not even enter a storming vector (its stack is
     * already thousands deep elsewhere or about to be). Still run
     * Begin/End so the LAPIC gets its EOI (no stuck ISR bit blocking
     * the priority class) and IRQL accounting stays exact -- just
     * never call the ISR. The RTE is already masked by the check. */
    if (KiIsrStormCheck(Interrupt->Vector))
    {
        if (HalBeginSystemInterrupt(Interrupt->SynchronizeIrql,
                                    Interrupt->Vector,
                                    &OldIrql))
        {
            KiExitInterrupt(TrapFrame, OldIrql, FALSE);
        }
        else
        {
            KiExitInterrupt(TrapFrame, OldIrql, TRUE);
        }
        return;
    }

    /* Begin the interrupt, making sure it's not spurious */
    if (HalBeginSystemInterrupt(Interrupt->SynchronizeIrql,
                                Interrupt->Vector,
                                &OldIrql))
    {
            /* Acquire interrupt lock */
            KxAcquireSpinLock(Interrupt->ActualLock);

            /* Mark this CPU inside the ISR (same-CPU nesting check) */
            KiIsrMarkActive(Interrupt);

            /* Call the ISR */
            Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext);

            /* Back out */
            KiIsrMarkIdle();

        /* Release interrupt lock */
        KxReleaseSpinLock(Interrupt->ActualLock);

        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, FALSE);
    }
    else
    {
        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, TRUE);
    }
}

VOID
FASTCALL
KiChainedDispatch(IN PKTRAP_FRAME TrapFrame,
                   IN PKINTERRUPT Interrupt)
{
    KIRQL OldIrql = 0, OldInterruptIrql = 0;
    BOOLEAN Handled;
    PLIST_ENTRY NextEntry, ListHead;

    /* Increase interrupt count */
    KeGetCurrentPrcb()->InterruptCount++;

    /* REVERTED 2026-10-08: same as KiInterruptDispatch (see above). */

    /* Nested same-CPU/same-vector entry while the lock is held
     * (ISR or sync routine below): EOI via Begin/End, never ISR. */
    if (KiIsrNestedSelf(Interrupt))
    {
        if (HalBeginSystemInterrupt(Interrupt->Irql,
                                    Interrupt->Vector,
                                    &OldIrql))
        {
            KiExitInterrupt(TrapFrame, OldIrql, FALSE);
        }
        else
        {
            KiExitInterrupt(TrapFrame, OldIrql, TRUE);
        }
        return;
    }

    /* Storm guard (see KiInterruptDispatch): shared level lines that
     * nobody acks re-fire into themselves here. Same Begin/End
     * discipline as above: EOI + exact IRQL restore, no ISR. */
    if (KiIsrStormCheck(Interrupt->Vector))
    {
        if (HalBeginSystemInterrupt(Interrupt->Irql,
                                    Interrupt->Vector,
                                    &OldIrql))
        {
            KiExitInterrupt(TrapFrame, OldIrql, FALSE);
        }
        else
        {
                KiExitInterrupt(TrapFrame, OldIrql, TRUE);
            }
            return;
        }

    /* Begin the interrupt, making sure it's not spurious */
    if (HalBeginSystemInterrupt(Interrupt->Irql,
                                Interrupt->Vector,
                                &OldIrql))
    {
        /* Get list pointers */
        ListHead = &Interrupt->InterruptListEntry;
        NextEntry = ListHead; /* The head is an entry! */
        while (TRUE)
        {
            /* Check if this interrupt's IRQL is higher than the current one */
            if (Interrupt->SynchronizeIrql > Interrupt->Irql)
            {
                /* Raise to higher IRQL */
                OldInterruptIrql = KfRaiseIrql(Interrupt->SynchronizeIrql);
            }

            /* Acquire interrupt lock */
            KxAcquireSpinLock(Interrupt->ActualLock);

            /* Mark this CPU inside the ISR (same-CPU nesting check) */
            KiIsrMarkActive(Interrupt);

            /* Call the ISR */
            Handled = Interrupt->ServiceRoutine(Interrupt,
                                                Interrupt->ServiceContext);

            /* Back out */
            KiIsrMarkIdle();

            /* Release interrupt lock */
            KxReleaseSpinLock(Interrupt->ActualLock);

            /* Check if this interrupt's IRQL is higher than the current one */
            if (Interrupt->SynchronizeIrql > Interrupt->Irql)
            {
                /* Lower the IRQL back */
                ASSERT(OldInterruptIrql == Interrupt->Irql);
                KfLowerIrql(OldInterruptIrql);
            }

            /* Check if the interrupt got handled and it's level */
            if ((Handled) && (Interrupt->Mode == LevelSensitive)) break;

            /* What's next? */
            NextEntry = NextEntry->Flink;

            /* Is this the last one? */
            if (NextEntry == ListHead)
            {
                /* Level should not have gotten here */
                if (Interrupt->Mode == LevelSensitive) break;

                /* As for edge, we can only exit once nobody can handle the interrupt */
                if (!Handled) break;
            }

            /* Get the interrupt object for the next pass */
            Interrupt = CONTAINING_RECORD(NextEntry, KINTERRUPT, InterruptListEntry);
        }

        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, FALSE);
    }
    else
    {
        /* Now call the epilogue code */
        KiExitInterrupt(TrapFrame, OldIrql, TRUE);
    }
}

VOID
FASTCALL
KiInterruptTemplateHandler(IN PKTRAP_FRAME TrapFrame,
                           IN PKINTERRUPT Interrupt)
{
    /* Enter interrupt frame */
    KiEnterInterruptTrap(TrapFrame);

    /* Call the correct dispatcher */
    ((PKI_INTERRUPT_DISPATCH)Interrupt->DispatchAddress)(TrapFrame, Interrupt);
}


/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
VOID
NTAPI
KeInitializeInterrupt(IN PKINTERRUPT Interrupt,
                      IN PKSERVICE_ROUTINE ServiceRoutine,
                      IN PVOID ServiceContext,
                      IN PKSPIN_LOCK SpinLock,
                      IN ULONG Vector,
                      IN KIRQL Irql,
                      IN KIRQL SynchronizeIrql,
                      IN KINTERRUPT_MODE InterruptMode,
                      IN BOOLEAN ShareVector,
                      IN CHAR ProcessorNumber,
                      IN BOOLEAN FloatingSave)
{
    ULONG i;
    PULONG DispatchCode = &Interrupt->DispatchCode[0], Patch = DispatchCode;

    /* Set the Interrupt Header */
    Interrupt->Type = InterruptObject;
    Interrupt->Size = sizeof(KINTERRUPT);

    /* Check if we got a spinlock */
    if (SpinLock)
    {
        Interrupt->ActualLock = SpinLock;
    }
    else
    {
        /* Use the built-in one */
        KeInitializeSpinLock(&Interrupt->SpinLock);
        Interrupt->ActualLock = &Interrupt->SpinLock;
    }

    /* Set the other settings */
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->Vector = Vector;
    Interrupt->Irql = Irql;
    Interrupt->SynchronizeIrql = SynchronizeIrql;
    Interrupt->Mode = InterruptMode;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Number = ProcessorNumber;
    Interrupt->FloatingSave = FloatingSave;
    Interrupt->TickCount = MAXULONG;
    Interrupt->DispatchCount = MAXULONG;

    /* Loop the template in memory */
    for (i = 0; i < DISPATCH_LENGTH; i++)
    {
        /* Copy the dispatch code */
        *DispatchCode++ = ((PULONG)KiInterruptTemplate)[i];
    }

    /* Jump to the last 4 bytes */
    Patch = (PULONG)((ULONG_PTR)Patch +
                     ((ULONG_PTR)&KiInterruptTemplateObject -
                      (ULONG_PTR)KiInterruptTemplate) - 4);

    /* Apply the patch */
    *Patch = PtrToUlong(Interrupt);

    /* Disconnect it at first */
    Interrupt->Connected = FALSE;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeConnectInterrupt(IN PKINTERRUPT Interrupt)
{
    BOOLEAN Connected, Error, Status;
    KIRQL Irql, OldIrql;
    UCHAR Number;
    ULONG Vector;
    DISPATCH_INFO Dispatch;

    /* Get data from interrupt */
    Number = Interrupt->Number;
    Vector = Interrupt->Vector;
    Irql = Interrupt->Irql;

    /* Validate the settings */
    if ((Irql > HIGH_LEVEL) ||
        (Number >= KeNumberProcessors) ||
        (Interrupt->SynchronizeIrql < Irql) ||
        (Interrupt->FloatingSave))
    {
        return FALSE;
    }

    /* Set defaults */
    Connected = FALSE;
    Error = FALSE;

    /* Set the system affinity and acquire the dispatcher lock */
    KeSetSystemAffinityThread(1 << Number);
    OldIrql = KiAcquireDispatcherLock();

    /* Check if it's already been connected */
    if (!Interrupt->Connected)
    {
        /* Get vector dispatching information */
        KiGetVectorDispatch(Vector, &Dispatch);

        /* Check if the vector is already connected */
        if (Dispatch.Type == NoConnect)
        {
            /* Do the connection */
            Interrupt->Connected = Connected = TRUE;

            /* Initialize the list */
            InitializeListHead(&Interrupt->InterruptListEntry);

            /* Connect and enable the interrupt */
            KiConnectVectorToInterrupt(Interrupt, NormalConnect);
            Status = HalEnableSystemInterrupt(Vector, Irql, Interrupt->Mode);
            if (!Status) Error = TRUE;
        }
        else if ((Dispatch.Type != UnknownConnect) &&
                (Interrupt->ShareVector) &&
                (Dispatch.Interrupt->ShareVector) &&
                (Dispatch.Interrupt->Mode == Interrupt->Mode))
        {
            /* The vector is shared and the interrupts are compatible */
            Interrupt->Connected = Connected = TRUE;

            /*
             * Verify the IRQL for chained connect,
             */
#if defined(CONFIG_SMP)
            ASSERT(Irql <= SYNCH_LEVEL);
#else
            ASSERT(Irql <= (IPI_LEVEL - 2));
#endif

            /* Check if this is the first chain */
            if (Dispatch.Type != ChainConnect)
            {
                /* This is not supported */
                ASSERT(Dispatch.Interrupt->Mode != Latched);

                /* Setup the chainned handler */
                KiConnectVectorToInterrupt(Dispatch.Interrupt, ChainConnect);
            }

            /* Insert into the interrupt list */
            InsertTailList(&Dispatch.Interrupt->InterruptListEntry,
                           &Interrupt->InterruptListEntry);
        }
    }

    /* Unlock the dispatcher and revert affinity */
    KiReleaseDispatcherLock(OldIrql);
    KeRevertToUserAffinityThread();

    /* Check if we failed while trying to connect */
    if ((Connected) && (Error))
    {
        DPRINT1("HalEnableSystemInterrupt failed\n");
        KeDisconnectInterrupt(Interrupt);
        Connected = FALSE;
    }

    /* Storm-masked vectors: chained connects never call HalEnable, so
     * unmask explicitly here (no-op if already unmasked; F1: stays held
     * while the line still asserts). */
    if ((Connected) && (Vector < 256) && (KiIsrStormMasked[Vector]))
    {
        KiIsrStormMasked[Vector] = FALSE;
        HalRearmSystemInterrupt(Vector);
    }

    /* Return to caller */
    return Connected;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
{
    KIRQL OldIrql, Irql;
    ULONG Vector;
    DISPATCH_INFO Dispatch;
    PKINTERRUPT NextInterrupt;
    BOOLEAN State;

    /* Set the affinity */
    KeSetSystemAffinityThread(1 << Interrupt->Number);

    /* Lock the dispatcher */
    OldIrql = KiAcquireDispatcherLock();

    /* Check if it's actually connected */
    State = Interrupt->Connected;
    if (State)
    {
        /* Get the vector and IRQL */
        Irql = Interrupt->Irql;
        Vector = Interrupt->Vector;

        /* Get vector dispatch data */
        KiGetVectorDispatch(Vector, &Dispatch);

        /* Check if it was chained */
        if (Dispatch.Type == ChainConnect)
        {
            /* Check if the top-level interrupt is being removed */
#if defined(CONFIG_SMP)
            ASSERT(Irql <= SYNCH_LEVEL);
#else
            ASSERT(Irql <= (IPI_LEVEL - 2));
#endif
            if (Interrupt == Dispatch.Interrupt)
            {
                /* Get the next one */
                Dispatch.Interrupt = CONTAINING_RECORD(Dispatch.Interrupt->
                                                       InterruptListEntry.Flink,
                                                       KINTERRUPT,
                                                       InterruptListEntry);

                /* Reconnect it */
                KiConnectVectorToInterrupt(Dispatch.Interrupt, ChainConnect);
            }

            /* Remove it */
            RemoveEntryList(&Interrupt->InterruptListEntry);

            /* Get the next one */
            NextInterrupt = CONTAINING_RECORD(Dispatch.Interrupt->
                                              InterruptListEntry.Flink,
                                              KINTERRUPT,
                                              InterruptListEntry);

            /* Check if this is the only one left */
            if (Dispatch.Interrupt == NextInterrupt)
            {
                /* Connect it in non-chained mode */
                KiConnectVectorToInterrupt(Dispatch.Interrupt, NormalConnect);
            }
        }
        else
        {
            /* Only one left, disable and remove it */
            HalDisableSystemInterrupt(Interrupt->Vector, Irql);
            KiConnectVectorToInterrupt(Interrupt, NoConnect);
        }

        /* Disconnect it */
        Interrupt->Connected = FALSE;
    }

    /* Unlock the dispatcher and revert affinity */
    KiReleaseDispatcherLock(OldIrql);
    KeRevertToUserAffinityThread();

    /* Return to caller */
    return State;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
KeSynchronizeExecution(IN OUT PKINTERRUPT Interrupt,
                       IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                       IN PVOID SynchronizeContext OPTIONAL)
{
    BOOLEAN Success;
    KIRQL OldIrql;

    /* Raise IRQL */
    KeRaiseIrql(Interrupt->SynchronizeIrql,
                &OldIrql);

    /* Acquire interrupt spinlock */
    KeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);

    /* Mark this CPU inside the sync routine: a nested trap on the
     * same vector (device asserts with the lock held, e.g. enabling
     * ATA interrupts on a shared level line) must EOI-and-return
     * instead of deadlocking on the lock (0x0F on real HW). */
    KiIsrMarkActive(Interrupt);

    /* Call the routine */
    Success = SynchronizeRoutine(SynchronizeContext);

    /* Back out */
    KiIsrMarkIdle();

    /* Release lock */
    KeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);

    /* Lower IRQL */
    KeLowerIrql(OldIrql);

    /* Return status */
    return Success;
}

/* EOF */
