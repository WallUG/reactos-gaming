/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK; // Note: expected layout only for 32-bit x86

/* Fase 5 SMP: imagenes TSS del BSP (las rellena Ki386InitializeTss con
 * CR3/Eip/Cs/etc validos). Ver abajo. */
extern UCHAR KiDoubleFaultTSS[];
extern UCHAR KiNMITSS[];

/* Fase5 SMP: APINFO por CPU (para forense RSP y re-SIPI). Se fuga a
 * proposito (ver comentario al final de KeStartAllProcessors). */
PAPINFO KiApInfoBlock[32] = {0};

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack, DPCStack;
    PAPINFO APInfo;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    // TODO: Support processor nodes

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        /* Fase5 SMP: no rearrancar un CPU que ya esta activo (la llamada
         * de Phase1 re-entra con el AP ya corriendo: INIT-SIPI a un CPU
         * en marcha deja IRQL en HIGH (pool a 31 en ObInit2) y duplica
         * PCR/pilas. Saltar los ya activos. */
        if (KeActiveProcessors & (1UL << ProcessorCount))
            continue;

        KernelStack = NULL;
        DPCStack = NULL;

        // Allocate structures for a new CPU.
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        /* Fase5 SMP: inicializar ApcState del thread del AP (el pool viene
         * a ceros: ApcState.Process NULL). Sin esto, la primera APC que se
         * entregue en el AP muere en 0x10 INVALID_PROCESS_ATTACH_ATTEMPT
         * (KiDeliverApc Quickie: Process != ApcState.Process), cazado en
         * vivo matando boots en bring-up del AP. */
        APInfo->Thread.Tcb.ApcState.Process = &KiInitialProcess.Pcb;
        APInfo->Thread.Tcb.ApcStateIndex = OriginalApcEnvironment;

        /* Fase5 SMP: publicar APINFO para forense y re-uso (NMI-kick,
         * re-SIPI). */
        if (ProcessorCount < 32)
            KiApInfoBlock[ProcessorCount] = APInfo;

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
            break;

        // Initalize a new PCR for the specific AP
        KiInitializePcr(ProcessorCount,
                        &APInfo->Pcr,
                        &APInfo->Idt[0],
                        &APInfo->Gdt[0],
                        &APInfo->Tss,
                        (PKTHREAD)&APInfo->Thread,
                        DPCStack);

        // Prepare descriptor tables
        KDESCRIPTOR bspGdt, bspIdt;
        __sgdt(&bspGdt.Limit);
        __sidt(&bspIdt.Limit);
        RtlCopyMemory(&APInfo->Gdt, (PVOID)bspGdt.Base, bspGdt.Limit + 1);
        RtlCopyMemory(&APInfo->Idt, (PVOID)bspIdt.Base, bspIdt.Limit + 1);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_R0_PCR), (ULONG_PTR)&APInfo->Pcr);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_DF_TSS), (ULONG_PTR)&APInfo->TssDoubleFault);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_NMI_TSS), (ULONG_PTR)&APInfo->TssNMI);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS), (ULONG_PTR)&APInfo->Tss);
        // Clear TSS Busy flag (aka set the type to "TSS (Available)")
        KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS)->HighWord.Bits.Type = I386_TSS;

        /* Fase 5 SMP: copia los TSS de DF/NMI del BSP (CR3/Eip/Cs/Fs/IoMap
         * validos) y dales pilas propias del AP. Sin esto quedan a ceros
         * (CR3=0, Eip=0): cualquier DF/NMI en el AP falla el cambio de
         * tarea (#TS) y acaba en triple-fault con reset silencioso en vez
         * del 0x7f visible y diagnosticable. */
        RtlCopyMemory(&APInfo->TssDoubleFault, KiDoubleFaultTSS, sizeof(KTSS));
        RtlCopyMemory(&APInfo->TssNMI, KiNMITSS, sizeof(KTSS));
        /* Fase5 SMP: Esp0/Esp apuntan al TOPE de la pila (crecen hacia
         * abajo). Apuntar al inicio (&NMIStackData) hacia que el primer
         * push cayera fuera (triple-fault silencioso en NMI/DF del AP;
         * cazado: NMI-kick reseteaba la maquina). */
        APInfo->TssDoubleFault.Esp0 = (ULONG_PTR)&APInfo->NMIStackData + sizeof(APInfo->NMIStackData);
        APInfo->TssDoubleFault.Esp = (ULONG_PTR)&APInfo->NMIStackData + sizeof(APInfo->NMIStackData);

        APInfo->TssNMI.Esp0 = (ULONG_PTR)&APInfo->NMIStackData + sizeof(APInfo->NMIStackData);
        APInfo->TssNMI.Esp = (ULONG_PTR)&APInfo->NMIStackData + sizeof(APInfo->NMIStackData);

        /* Fase5 SMP: inicializar TAMBIEN el TSS normal del AP (estaba a
         * ceros de pool: CR3=0, Eip=0). El primer task-switch-back (NMI en
         * KiTrap02Handler, o DF) restaura desde el y triplica en silencio
         * (cazado: NMI-kick a AP#1 reseteaba aun con DF/NMI TSS sanos).
         * Se copia el del BSP (mismo espacio, selectores validos) y se
         * deja Esp0 en la pila del AP. */
        RtlCopyMemory(&APInfo->Tss, KeGetPcr()->TSS, sizeof(KTSS));
        APInfo->Tss.Esp0 = (ULONG_PTR)KernelStack;

        // Fill the processor state
        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb->ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->ContextFrame.SegCs = KGDT_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;

        ProcessorState->SpecialRegisters.Gdtr.Base = (ULONG_PTR)APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = (ULONG_PTR)APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        ProcessorState->SpecialRegisters.Tr = KGDT_TSS;

        ProcessorState->ContextFrame.Esp = (ULONG_PTR)KernelStack;
        ProcessorState->ContextFrame.Eip = (ULONG_PTR)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
        PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
        ApStack->KxLoaderBlock = KeLoaderBlock;
        ApStack->ReturnAddr = NULL;

        // Update the LOADER_PARAMETER_BLOCK structure for the new processor
        // Fase 5 SMP: Thread debe ser el ETHREAD embebido (&APInfo->Thread,
        // el mismo pasado a KiInitializePcr), NO &Prcb->IdleThread (eso es
        // la direccion DEL PUNTERO, y el AP acababa corriendo sobre PRCB+12
        // como si fuera un thread: Header/PRCB/TSS pisados, triple-fault).
        // Prcb apunta al PRCB real (antes a la direccion del puntero).
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.PrcbData;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        // Start the CPU
        DPRINT("Attempting to Start a CPU with number: %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            break;
        }

        // And wait for it to start
        while (KeLoaderBlock->Prcb != 0)
        {
            //TODO: Add a time out so we don't wait forever
            KeMemoryBarrier();
            YieldProcessor();
        }
    }

    // The last CPU didn't start - clean the data
    ProcessorCount--;

    /* Fase 5 SMP: NO liberar APInfo/KernelStack/DPCStack aqui. El AP
     * corre sobre ellos (PCR/PRCB via FS, GDT/IDT cargadas, pila de
     * arranque): liberarlos es use-after-free (el pool los reutiliza y
     * el AP lee/escribe basura: SetMember corrupto -> auto-IPI roto ->
     * cuelgue BSP<->AP). Se fugan a proposito (una vez por AP en boot);
     * el fix propio seria copiar PCR/PRCB a pool permanente y conmutar. */

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu\n", ProcessorCount);
}

/* Fase 5.1: devuelve la IDT del AP (copia tomada al arrancarlo) o NULL.
 * Residente (NO INIT): se usa en caliente al registrar vectores tardios.
 * Idt[256] es el primer miembro de APINFO, que se fuga a proposito. */
PKIDTENTRY
NTAPI
KiGetApIdt(IN ULONG Cpu)
{
    if (Cpu == 0 || Cpu >= 32)
        return NULL;
    if (!KiApInfoBlock[Cpu])
        return NULL;
    return &KiApInfoBlock[Cpu]->Idt[0];
}
