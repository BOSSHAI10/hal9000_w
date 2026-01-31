#pragma once

#include "list.h"
#include "ref_cnt.h"
#include "ex_event.h"
#include "thread.h"

struct _PROCESS;

typedef enum _THREAD_STATE {
    ThreadStateRunning,
    ThreadStateReady,
    ThreadStateBlocked,
    ThreadStateDying,
    ThreadStateReserved = ThreadStateDying + 1
} THREAD_STATE;

typedef DWORD THREAD_FLAGS;
#define THREAD_FLAG_FORCE_TERMINATE_PENDING 0x1
#define THREAD_FLAG_FORCE_TERMINATED        0x2

typedef struct _THREAD {
    REF_COUNT               RefCnt;
    struct _THREAD* Self;
    TID                     Id;
    char* Name;
    THREAD_PRIORITY         Priority;
    THREAD_STATE            State;
    STATUS                  ExitStatus;
    EX_EVENT                TerminationEvt;
    volatile THREAD_FLAGS   Flags;
    LOCK                    BlockLock;
    LIST_ENTRY              AllList;
    LIST_ENTRY              ReadyList;
    LIST_ENTRY              ProcessList;
    QWORD                   TickCountCompleted;
    QWORD                   UninterruptedTicks;
    QWORD                   TickCountEarly;
    PVOID                   InitialStackBase;
    DWORD                   StackSize;
    PVOID                   Stack;
    PVOID                   UserStack;
    struct _PROCESS* Process;

    // --- Proiect ---
    BYTE                    CreationCpuId; 
    DWORD                   ChildThreadsCreated; 
    struct _THREAD* ParentThread; 
    volatile int32_t        NumberOfActiveChildren; 
    DWORD                   TotalTicksRun; 
    DWORD                   CurrentQuantum; 
    DWORD                   QuantaAllocatedCount;
} THREAD, *PTHREAD;

void _No_competing_thread_ ThreadSystemPreinit(void);
STATUS ThreadSystemInitMainForCurrentCPU(void);
STATUS ThreadSystemInitIdleForCurrentCPU(void);
void ThreadSystemGetThreadList(OUT PLOCK* ListLock, OUT PLIST_ENTRY* ListHead);
STATUS ThreadCreateEx(IN_Z char* Name, IN THREAD_PRIORITY Priority, IN PFUNC_ThreadStart Function, IN_OPT PVOID Context, OUT_PTR PTHREAD* Thread, INOUT struct _PROCESS* Process);
void ThreadTick(void);
void ThreadBlock(void);
void ThreadUnblock(IN PTHREAD Thread);
BOOLEAN ThreadYieldOnInterrupt(void);
void ThreadTerminate(INOUT PTHREAD Thread);
void ThreadTakeBlockLock(void);
STATUS ThreadExecuteForEachThreadEntry(IN PFUNC_ListFunction Function, IN_OPT PVOID Context);

#define GetCurrentThread() ((THREAD*) AsmReadFsQword(FIELD_OFFSET(THREAD, Self)))
void SetCurrentThread(IN PTHREAD Thread);
void ThreadSetPriority(IN THREAD_PRIORITY NewPriority);