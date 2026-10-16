// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Xi'an Jiaotong University. All rights reserved.

/* Client library users on Windows need this macro defined here. */
#if !defined(__WIN__) && defined(_WIN32)
#define __WIN__
#endif

typedef char        my_bool; /* Small bool */

/* Define some general constants */
#ifndef TRUE
#define TRUE        (1) /* Logical true */
#define FALSE       (0) /* Logical false */
#endif

#include <malloc.h>
#include <stdarg.h>
#include <errno.h>        /* to define ENOENT */
#include <stdio.h>
#include <ctype.h>


#if defined(__WIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>    // exit
#include <psapi.h>
#define getpid _getpid
#define unlink _unlink
#endif /* __WIN__ */

#if !defined(__WIN__)
#include <pthread.h>
#include <mqueue.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#endif

/***************  helpers  *****************************/

#if defined(__WIN__)
/*++

Copyright (c) 2000  Microsoft Corporation

Module Name:

    pushlock.c

Abstract:

    This module houses routines that do push locking.

    Push locks are capable of being acquired in both shared and exclusive mode.

    Properties include:

    They can not be acquired recursively.
    They are small (the size of a pointer) and can be used when embeded in pagable data.
    Acquire and release is done lock free. On lock contention the waiters are chained
    through the lock and local stack space.

    This is the structure of a push lock:

    
    E  == Exclusive bit
    W  == Waiters present
    SC == Share count
    P  == Pointer to wait block
    +----------+---+---+
    |    SC    | E | W | E, W are single bits W == 0
    +----------+---+---+

    +--------------+---+
    |      P       | W | W == 1. Pointer is the address of a chain of stack local wait blocks
    +--------------+---+

    The non-contented acquires and releases are trivial. Interlocked operations make the following
    transformations.

    (SC=0,E=0,W=0) === Exclusive acquire ===> (SC=0,E=1,W=0)
    (SC=n,E=0,W=0) === Shared acquire    ===> (SC=n+1,E=0,W=0)

    (SC=0,E=1,W=0) === Exclusive release ===> (SC=0,E=0,W=0)
    (SC=n,E=0,W=0) === Shared release    ===> (SC=n-1,E=0,W=0) n > 0

    Contention causes the acquiring thread to produce a local stack based wait block and to
    enqueue it to the front of the list.

    (SC=n,E=e,W=0) === Exclusive acquire ===> (P=LWB(SSC=n,E=e),W=1) LWB = local wait block,
                                                                     SSC = Saved share count,
                                                                     n > 0 or e == 1.

    (SC=0,E=1,W=0) === Shared acquire    ===> (P=LWB(SSC=0,E=0),W=1) LWB = local wait block,
                                                                     SSC = Saved share count.

    After contention has causes one or more threads to queue up wait blocks releases are more
    complicated. This following rights are granted to a releasing thread (shared or exclusive).

    1) Shared release threads are allowed to search the wait list until they hit a wait block
       with a non-zero share count (this will be a wait block marked exclusive). This thread is
       allowed to use an interlocked operation to decrement this value. If this thread
       transitioned the value to zero then it obtains the rights of an exclusive release thread

    2) Exclusive threads are allowed to search the wait list until they find a continuous chain
       of shared wait blocks or they find the last wait block is an exclusive waiter. This thread
       may then break the chain at this point or update the header to show a single exclusive
       owner or multiple shared owners. Breaking the list can be done with normal assignment
       but updating the header requires interlocked exchange compare.


Author:

    Neill Clift (NeillC) 30-Sep-2000


Revision History:

--*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef ASSERT
#undef ASSERT
#endif
#define ASSERT(e)        // get rid of the assert symbol.

typedef UCHAR KIRQL;
#ifdef KeRaiseIrql
#undef KeRaiseIrql
#endif
#define KeRaiseIrql(NewLevel, OldLevel)
#ifdef KeLowerIrql
#undef KeLowerIrql
#endif
#define KeLowerIrql(Level)

//
// Push lock definitions
//
typedef struct _EX_PUSH_LOCK {

#define EX_PUSH_LOCK_WAITING   0x1
#define EX_PUSH_LOCK_EXCLUSIVE 0x2
#define EX_PUSH_LOCK_SHARE_INC 0x4

    union {
        struct {
            ULONG_PTR Waiting : 1;
            ULONG_PTR Exclusive : 1;
            ULONG_PTR Shared : sizeof (ULONG_PTR) * 8 - 2;
        }x;
        ULONG_PTR Value;
        PVOID Ptr;
    };
} EX_PUSH_LOCK, *PEX_PUSH_LOCK;

//
// This is a block held on the local stack of the waiting threads.
//

typedef  struct _EX_PUSH_LOCK_WAIT_BLOCK *PEX_PUSH_LOCK_WAIT_BLOCK;

typedef struct _EX_PUSH_LOCK_WAIT_BLOCK {
    HANDLE WakeEvent;
    PEX_PUSH_LOCK_WAIT_BLOCK Next;
    PEX_PUSH_LOCK_WAIT_BLOCK Last;
    PEX_PUSH_LOCK_WAIT_BLOCK Previous;
    ULONG ShareCount;
    BOOLEAN Exclusive;
} EX_PUSH_LOCK_WAIT_BLOCK;



VOID
ExfAcquirePushLockExclusive (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Acquire a push lock exclusively

Arguments:

    PushLock - Push lock to be acquired

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;
    EX_PUSH_LOCK_WAIT_BLOCK WaitBlock;

    OldValue = *PushLock;
    while (1) {
        //
        // If the lock is already held exclusively/shared or there are waiters then
        // we need to wait.
        //
        if (OldValue.Value == 0) {
            NewValue.Value = OldValue.Value + EX_PUSH_LOCK_EXCLUSIVE;
            NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                              NewValue.Ptr,
                                                              OldValue.Ptr);
            if (NewValue.Ptr == OldValue.Ptr) {
                break;
            }
        } else {
            WaitBlock.WakeEvent = CreateEventW ( NULL, FALSE, FALSE, NULL );
            WaitBlock.Exclusive = TRUE;
            WaitBlock.Last = NULL;
            WaitBlock.Previous = NULL;
            //
            // Move the sharecount to our wait block if need be.
            //
            if (OldValue.x.Waiting) {
                WaitBlock.Next = (PEX_PUSH_LOCK_WAIT_BLOCK)
                                     (OldValue.Value - EX_PUSH_LOCK_WAITING);
                WaitBlock.ShareCount = 0;
            } else {
                WaitBlock.Next = NULL;
                WaitBlock.ShareCount = (ULONG) OldValue.x.Shared;
            }
            NewValue.Ptr = ((PUCHAR) &WaitBlock) + EX_PUSH_LOCK_WAITING;
            ASSERT ((NewValue.Value & EX_PUSH_LOCK_WAITING) != 0);
            NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                              NewValue.Ptr,
                                                              OldValue.Ptr);
            if (NewValue.Ptr == OldValue.Ptr) {
                WaitForSingleObject(WaitBlock.WakeEvent, INFINITE);
                ASSERT ((WaitBlock.ShareCount == 0) && (WaitBlock.Next == NULL));
                CloseHandle(WaitBlock.WakeEvent);
                break;
            }
            CloseHandle(WaitBlock.WakeEvent);

        }
        OldValue = NewValue;
    }
}

VOID
ExfAcquirePushLockShared (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Acquire a push lock shared

Arguments:

    PushLock - Push lock to be acquired

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;
    EX_PUSH_LOCK_WAIT_BLOCK WaitBlock;

    OldValue = *PushLock;
    while (1) {
        //
        // If the lock is already held exclusively or there are waiters then we need to wait
        //
        if (OldValue.x.Exclusive || OldValue.x.Waiting) {
            WaitBlock.WakeEvent = CreateEventW ( NULL, FALSE, FALSE, NULL );
            WaitBlock.Exclusive = 0;
            WaitBlock.ShareCount = 0;
            WaitBlock.Last = NULL;
            WaitBlock.Previous = NULL;
            //
            // Chain the next block to us if there is one.
            //
            if (OldValue.x.Waiting) {
                WaitBlock.Next = (PEX_PUSH_LOCK_WAIT_BLOCK)
                                     (OldValue.Value - EX_PUSH_LOCK_WAITING);
            } else {
                WaitBlock.Next = NULL;
            }
            NewValue.Ptr = ((PUCHAR) &WaitBlock) + EX_PUSH_LOCK_WAITING;
            ASSERT ((NewValue.Value & EX_PUSH_LOCK_WAITING) != 0);
            NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                              NewValue.Ptr,
                                                              OldValue.Ptr);
            if (NewValue.Ptr == OldValue.Ptr) {
                WaitForSingleObject(WaitBlock.WakeEvent, INFINITE);

                ASSERT (WaitBlock.ShareCount == 0);
                CloseHandle(WaitBlock.WakeEvent);
                break;
            }
            CloseHandle(WaitBlock.WakeEvent);

        } else {
            //
            // We only have shared accessors at the moment. We can just update the lock to include this thread.
            //
            NewValue.Value = OldValue.Value + EX_PUSH_LOCK_SHARE_INC;
            ASSERT (!(NewValue.Waiting || NewValue.Exclusive));
            NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                              NewValue.Ptr,
                                                              OldValue.Ptr);
            if (NewValue.Ptr == OldValue.Ptr) {
                break;
            }
        }
        OldValue = NewValue;
    }
}


VOID
ExfReleasePushLock (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Release a push lock that was acquired exclusively or shared

Arguments:

    PushLock - Push lock to be released

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;
    PEX_PUSH_LOCK_WAIT_BLOCK WaitBlock, NextWaitBlock, ReleaseWaitList, Previous;
    PEX_PUSH_LOCK_WAIT_BLOCK LastWaitBlock, FirstWaitBlock;
    ULONG ShareCount;
    KIRQL OldIrql;

    OldValue = *PushLock;
    while (1) {
        if (!OldValue.x.Waiting) {
            //
            // Either we hold the lock exclusive or shared but not both.
            //
            ASSERT (OldValue.Exclusive ^ (OldValue.Shared > 0));

            //
            // We must hold the lock exclusive or shared. We make the assuption that
            // the exclusive bit is just below the share count here.
            //
            NewValue.Value = (OldValue.Value - EX_PUSH_LOCK_EXCLUSIVE) &
                             ~EX_PUSH_LOCK_EXCLUSIVE;
            NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                              NewValue.Ptr,
                                                              OldValue.Ptr);
            if (NewValue.Ptr == OldValue.Ptr) {
                break;
            }
            //
            // Either we gained a new waiter or another shared owner arrived or left
            //
            ASSERT (NewValue.Waiting || (NewValue.Shared > 0 && !NewValue.Exclusive));
            OldValue = NewValue;
        } else {
            //
            // There are waiters chained to the lock. We have to release the share count,
            // last exclusive or last chain of shared waiters.
            //
            WaitBlock = (PEX_PUSH_LOCK_WAIT_BLOCK) 
                           (OldValue.Value - EX_PUSH_LOCK_WAITING);

            FirstWaitBlock = WaitBlock;
            ReleaseWaitList = WaitBlock;
            Previous = NULL;
            LastWaitBlock = NULL;
            ShareCount = 0;
            do {

                if (WaitBlock->Last != NULL) {
                    LastWaitBlock = WaitBlock;
                    WaitBlock = WaitBlock->Last;
                    Previous = WaitBlock->Previous;
                    ReleaseWaitList = WaitBlock;
                    ASSERT (WaitBlock->Next == NULL);
                    ASSERT (Previous != NULL);
                    ShareCount = 0;
                }

                if (WaitBlock->Exclusive) {
                    //
                    // This is an exclusive waiter. If this was the first exclusive waited to a shared acquire
                    // then it will have the saved share count. If we acquired the lock shared then the count
                    // must contain a bias for this thread. Release that and if we are not the last shared
                    // accessor then exit. A later shared release thread will wake the exclusive
                    // waiter.
                    //
                    if (WaitBlock->ShareCount != 0) {
                        if (InterlockedDecrement ((PLONG)&WaitBlock->ShareCount) != 0) {
                            return;
                        }
                    }
                    //
                    // Reset count of share acquires waiting.
                    //
                    ShareCount = 0;
                } else {
                    //
                    // This is a shared waiter. Record the number of these to update the head or the
                    // previous exclusive waiter.
                    //
                    ShareCount++;
                }
                NextWaitBlock = WaitBlock->Next;
                if (NextWaitBlock != NULL) {

                    NextWaitBlock->Previous = WaitBlock;

                    if (NextWaitBlock->Exclusive) {
                        //
                        // The next block is exclusive. This may be the entry to free.
                        //
                        Previous = WaitBlock;
                        ReleaseWaitList = NextWaitBlock;
                    } else {
                        //
                        // The next block is shared. If the chain start is exclusive then skip to this one
                        // as the exclusive isn't the thread we will wake up.
                        //
                        if (ReleaseWaitList->Exclusive) {
                            Previous = WaitBlock;
                            ReleaseWaitList = NextWaitBlock;
                        }
                    }
                }

                WaitBlock = NextWaitBlock;
            } while (WaitBlock != NULL);

            //
            // If our release chain is everything then we have to update the header
            //
            if (Previous == NULL) {
                NewValue.Value = 0;
                NewValue.x.Exclusive = ReleaseWaitList->Exclusive;
                NewValue.x.Shared = ShareCount;
                ASSERT (((ShareCount > 0) ^ (ReleaseWaitList->Exclusive)) && !NewValue.Waiting);

                NewValue.Ptr = InterlockedCompareExchangePointer (&PushLock->Ptr,
                                                                  NewValue.Ptr,
                                                                  OldValue.Ptr);
                if (NewValue.Ptr != OldValue.Ptr) {
                    //
                    // We are releasing so we could have only gained another waiter
                    //
                    ASSERT (NewValue.Waiting);
                    OldValue = NewValue;
                    continue;
                }
            } else {

                if (LastWaitBlock != NULL) {
                    LastWaitBlock->Last = NULL;
                }
                //
                // Truncate the chain at this position and save the share count for all the shared owners to
                // decrement later.
                //
                Previous->Next = NULL;
                ASSERT (Previous->ShareCount == 0);
                Previous->ShareCount = ShareCount;

                //
                // Add a pointer to make future searches faster
                //
                if (Previous->Exclusive && FirstWaitBlock != Previous) {
                    FirstWaitBlock->Last = Previous;
                    ASSERT (Previous->Previous != NULL);
                }
                //
                // We are either releasing multiple share accessors or a single exclusive
                //
                ASSERT ((ShareCount > 0) ^ ReleaseWaitList->Exclusive);
            }

            //
            // If we are waking more than one thread then raise to DPC level to prevent us
            // getting rescheduled part way through the operation
            //

            OldIrql = DISPATCH_LEVEL;
            if (ShareCount > 1) {
                KeRaiseIrql (DISPATCH_LEVEL, &OldIrql);
            }

            //
            //
            // Release the chain of threads we located.
            //
            do {
                NextWaitBlock = ReleaseWaitList->Next;
                //
                // All the chain should have the same type (Exclusive/Shared).
                //
                ASSERT (NextWaitBlock == NULL || (ReleaseWaitList->Exclusive == NextWaitBlock->Exclusive));
                ASSERT (!ReleaseWaitList->Exclusive || (ReleaseWaitList->ShareCount == 0));
                SetEvent (ReleaseWaitList->WakeEvent);
                ReleaseWaitList = NextWaitBlock;
            } while (ReleaseWaitList != NULL);

            if (OldIrql != DISPATCH_LEVEL) {
                KeLowerIrql (OldIrql);
            }


            break;
        }
    }
}

VOID
FORCEINLINE
ExInitializePushLock (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Initialize a push lock structure

Arguments:

    PushLock - Push lock to be initialized

Return Value:

    None

--*/
{
    PushLock->Value = 0;
}

VOID
FORCEINLINE
ExAcquirePushLockExclusive (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Acquire a push lock exclusively

Arguments:

    PushLock - Push lock to be acquired

Return Value:

    None

--*/
{
    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           (PVOID)EX_PUSH_LOCK_EXCLUSIVE,
                                           NULL) != NULL) {
        ExfAcquirePushLockExclusive (PushLock);
    }
}

BOOLEAN
FORCEINLINE
ExTryAcquirePushLockExclusive (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Try and acquire a push lock exclusively

Arguments:

    PushLock - Push lock to be acquired

Return Value:

    BOOLEAN - TRUE: Acquire was successfull, FALSE: Lock was already acquired

--*/
{
    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           (PVOID)EX_PUSH_LOCK_EXCLUSIVE,
                                           NULL) == NULL) {
        return TRUE;
    } else {
        return FALSE;
    }
}

VOID
FORCEINLINE
ExAcquirePushLockShared (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Acquire a push lock shared

Arguments:

    PushLock - Push lock to be acquired

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;

    OldValue = *PushLock;
    OldValue.Value &= ~(EX_PUSH_LOCK_EXCLUSIVE | EX_PUSH_LOCK_WAITING);
    NewValue.Value = OldValue.Value + EX_PUSH_LOCK_SHARE_INC;
    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           NewValue.Ptr,
                                           OldValue.Ptr) != OldValue.Ptr) {
        ExfAcquirePushLockShared (PushLock);
    }
}

VOID
FORCEINLINE
ExReleasePushLock (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Release a push lock that was acquired exclusively or shared

Arguments:

    PushLock - Push lock to be released

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;

    OldValue = *PushLock;
    OldValue.Value &= ~EX_PUSH_LOCK_WAITING;
    NewValue.Value = (OldValue.Value - EX_PUSH_LOCK_EXCLUSIVE) &
                         ~EX_PUSH_LOCK_EXCLUSIVE;
    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           NewValue.Ptr,
                                           OldValue.Ptr) != OldValue.Ptr) {
        ExfReleasePushLock (PushLock);
    }
}

VOID
FORCEINLINE
ExReleasePushLockExclusive (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Release a push lock that was acquired exclusively

Arguments:

    PushLock - Push lock to be released

Return Value:

    None

--*/
{
    ASSERT (PushLock->Value & (EX_PUSH_LOCK_WAITING|EX_PUSH_LOCK_EXCLUSIVE));

    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           NULL,
                                           (PVOID)EX_PUSH_LOCK_EXCLUSIVE) != (PVOID)EX_PUSH_LOCK_EXCLUSIVE) {
        ExfReleasePushLock (PushLock);
    }
}

VOID
FORCEINLINE
ExReleasePushLockShared (
     IN PEX_PUSH_LOCK PushLock
     )
/*++

Routine Description:

    Release a push lock that was acquired shared

Arguments:

    PushLock - Push lock to be released

Return Value:

    None

--*/
{
    EX_PUSH_LOCK OldValue, NewValue;

    OldValue = *PushLock;
    ASSERT (OldValue.Waiting || !OldValue.Exclusive);
    OldValue.Value &= ~EX_PUSH_LOCK_WAITING;
    NewValue.Value = OldValue.Value - EX_PUSH_LOCK_SHARE_INC;
    if (InterlockedCompareExchangePointer (&PushLock->Ptr,
                                           NewValue.Ptr,
                                           OldValue.Ptr) != OldValue.Ptr) {
        ExfReleasePushLock (PushLock);
    }
}

typedef EX_PUSH_LOCK pthread_mutex_t;
typedef intptr_t pthread_cond_t;

static inline int pthread_mutex_init(pthread_mutex_t *const mutex,
                                     const void *const)
{
    ExInitializePushLock(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *const) {
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *const mutex) {
    ExAcquirePushLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *const mutex) {
    ExReleasePushLockExclusive(mutex);
    return 0;
}

typedef HANDLE         pthread_t;
typedef struct thread_attr {
    DWORD dwStackSize ;
    DWORD dwCreatingFlag ;
} pthread_attr_t ;

#define pthread_self() GetCurrentThreadId()
typedef void * (__cdecl *pthread_handler)(void *);

typedef volatile LONG my_pthread_once_t;
#define MY_PTHREAD_ONCE_INIT  0
#define MY_PTHREAD_ONCE_INPROGRESS 1
#define MY_PTHREAD_ONCE_DONE 2

struct thread_start_parameter
{
  pthread_handler func;
  void *arg;
};

static unsigned int __stdcall pthread_start(void *p)
{
  struct thread_start_parameter *par= (struct thread_start_parameter *)p;
  pthread_handler func= par->func;
  void *arg= par->arg;
  free(p);
  (*func)(arg);
  return 0;
}


int pthread_create(pthread_t *thread_id, const pthread_attr_t *attr,
                   pthread_handler func, void *param)
{
  struct thread_start_parameter *par;
  unsigned int  stack_size;
  int error_no;

  par= (struct thread_start_parameter *)malloc(sizeof(*par));
  if (!par)
   goto error_return;

  par->func= func;
  par->arg= param;
  stack_size= attr?attr->dwStackSize:0;

  *thread_id= (HANDLE)_beginthreadex(NULL, stack_size , pthread_start, par, 0, NULL);
  if (!*thread_id)
    goto error_return;

  return(0);

error_return:
  error_no= errno;
  free(par);
  return(error_no);
}


int pthread_join(pthread_t thread, void **value_ptr)
{
    DWORD result;
    (void)value_ptr;

    if (!thread) return 0;

    result = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    switch (result) {
    case WAIT_OBJECT_0:
        return 0;
    case WAIT_ABANDONED:
        return EINVAL;
    default:
        return GetLastError();
    }
}

/*
 One time initialization. For simplicity, we assume initializer thread
 does not exit within init_routine().
*/
int my_pthread_once(my_pthread_once_t *once_control, 
    void (*init_routine)(void))
{
  LONG state;

  /*
    Do "dirty" read to find out if initialization is already done, to
    save an interlocked operation in common case. Memory barriers are ensured by 
    Visual C++ volatile implementation.
  */
  if (*once_control == MY_PTHREAD_ONCE_DONE)
    return 0;

  state= InterlockedCompareExchange(once_control, MY_PTHREAD_ONCE_INPROGRESS,
                                        MY_PTHREAD_ONCE_INIT);

  switch(state)
  {
  case MY_PTHREAD_ONCE_INIT:
    /* This is initializer thread */
    (*init_routine)();
    *once_control= MY_PTHREAD_ONCE_DONE;
    break;

  case MY_PTHREAD_ONCE_INPROGRESS:
    /* init_routine in progress. Wait for its completion */
    while(*once_control == MY_PTHREAD_ONCE_INPROGRESS)
    {
      Sleep(1);
    }
    break;
  case MY_PTHREAD_ONCE_DONE:
    /* Nothing to do */
    break;
  }
  return 0;
}
/*
   Copyright (c) 2011-2016  mingw-w64 project

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

/*
 * Posix Condition Variables for Microsoft Windows.
 * 22-9-2010 Partly based on the ACE framework implementation.
 */


#include <malloc.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WINPTHREAD_COND_DECL WINPTHREAD_API

/*
   Copyright (c) 2011-2016  mingw-w64 project

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

#ifndef WIN_PTHREADS_COND_H
#define WIN_PTHREADS_COND_H

#define CHECK_COND(c)                                                   \
    do {                                                                \
        if (!(c) || !*c || (*c == PTHREAD_COND_INITIALIZER)             \
            || ( ((cond_t *)(*c))->valid != (unsigned int)LIFE_COND ) ) \
            return EINVAL;                                              \
    } while (0)

#define LIFE_COND 0xC0BAB1FD
#define DEAD_COND 0xC0DEADBF

#define STATIC_COND_INITIALIZER(x)                ((pthread_cond_t)(x) == ((pthread_cond_t)PTHREAD_COND_INITIALIZER))

typedef struct cond_t cond_t;
struct cond_t
{
    unsigned int valid;
    int busy;
    LONG waiters_count_; /* Number of waiting threads.  */
    LONG waiters_count_unblock_; /* Number of waiting threads whitch can be unblocked.  */
    LONG waiters_count_gone_; /* Number of waiters which are gone.  */
    CRITICAL_SECTION waiters_count_lock_; /* Serialize access to <waiters_count_>.  */
    CRITICAL_SECTION waiters_q_lock_; /* Serialize access to sema_q.  */
    LONG value_q;
    CRITICAL_SECTION waiters_b_lock_; /* Serialize access to sema_b.  */
    LONG value_b;
    HANDLE sema_q; /* Semaphore used to queue up threads waiting for the condition to
                 become signaled.  */
    HANDLE sema_b; /* Semaphore used to queue up threads waiting for the condition which
                 became signaled.  */
};

#endif

#define __pthread_shallcancel() 0

static int do_sema_b_wait (HANDLE sema, int nointerrupt, DWORD timeout,CRITICAL_SECTION *cs, LONG *val);
static int do_sema_b_release(HANDLE sema, LONG count,CRITICAL_SECTION *cs, LONG *val);
static void cleanup_wait(void *arg);

typedef struct sCondWaitHelper {
    cond_t *c;
    pthread_mutex_t *external_mutex;
    int *r;
} sCondWaitHelper;

int do_sema_b_wait_intern (HANDLE sema, int nointerrupt, DWORD timeout);

static pthread_mutex_t cond_locked = {0};

int
pthread_cond_init (pthread_cond_t *c, const void *)
{
  cond_t *_c;
  int r = 0;

  if (!c)
    return EINVAL;

  if ((_c = (cond_t *)calloc(1, sizeof(*_c))) == NULL)
    return ENOMEM;

  _c->valid  = DEAD_COND;
  _c->busy = 0;
  _c->waiters_count_ = 0;
  _c->waiters_count_gone_ = 0;
  _c->waiters_count_unblock_ = 0;

  _c->sema_q = CreateSemaphoreW (NULL,       /* no security */
      0,          /* initially 0 */
      0x7fffffff, /* max count */
      NULL);      /* unnamed  */
  _c->sema_b =  CreateSemaphoreW (NULL,       /* no security */
      0,          /* initially 0 */
      0x7fffffff, /* max count */
      NULL);
  if (_c->sema_q == NULL || _c->sema_b == NULL) {
      if (_c->sema_q != NULL)
        CloseHandle (_c->sema_q);
      if (_c->sema_b != NULL)
        CloseHandle (_c->sema_b);
      free (_c);
      r = EAGAIN;
  } else {
      InitializeCriticalSection(&_c->waiters_count_lock_);
      InitializeCriticalSection(&_c->waiters_b_lock_);
      InitializeCriticalSection(&_c->waiters_q_lock_);
      _c->value_q = 0;
      _c->value_b = 1;
  }
  if (!r)
    {
      _c->valid = LIFE_COND;
      *c = (pthread_cond_t)_c;
    }
  else
    *c = (pthread_cond_t)NULL;
  return r;
}

static int
cond_static_init (pthread_cond_t *c)
{
  int r = 0;

  pthread_mutex_lock (&cond_locked);
  if (c == NULL)
    r = EINVAL;
  else if (*c == 0)
    r = pthread_cond_init (c, NULL);
  else
    /* We assume someone was faster ... */
    r = 0;
  pthread_mutex_unlock (&cond_locked);
  return r;
}

int
pthread_cond_destroy (pthread_cond_t *c)
{
  cond_t *_c;
  int r;
  if (!c || 0)
    return EINVAL;
  if (*c == 0)
    {
      pthread_mutex_lock (&cond_locked);
      if (*c == 0)
      {
        *c = (pthread_cond_t)NULL;
        r = 0;
      }
      else
        r = EBUSY;
      pthread_mutex_unlock (&cond_locked);
      return r;
    }
  _c = (cond_t *) *c;
  r = do_sema_b_wait(_c->sema_b, 0, INFINITE,&_c->waiters_b_lock_,&_c->value_b);
  if (r != 0)
    return r;
  if (!TryEnterCriticalSection (&_c->waiters_count_lock_))
    {
       do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
       return EBUSY;
    }
  if (_c->waiters_count_ > _c->waiters_count_gone_)
    {
      r = do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
      if (!r) r = EBUSY;
      LeaveCriticalSection(&_c->waiters_count_lock_);
      return r;
    }
  *c = (pthread_cond_t)NULL;
  do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);

  if (!CloseHandle (_c->sema_q) && !r)
    r = EINVAL;
  if (!CloseHandle (_c->sema_b) && !r)
    r = EINVAL;
  LeaveCriticalSection (&_c->waiters_count_lock_);
  DeleteCriticalSection(&_c->waiters_count_lock_);
  DeleteCriticalSection(&_c->waiters_b_lock_);
  DeleteCriticalSection(&_c->waiters_q_lock_);
  _c->valid  = DEAD_COND;
  free(_c);
  return 0;
}

int
pthread_cond_signal (pthread_cond_t *c)
{
  cond_t *_c;
  int r;

  if (!c || !*c)
    return EINVAL;
  _c = (cond_t *)*c;
  if (_c == (cond_t *)0)
    return 0;
  else if (_c->valid != (unsigned int)LIFE_COND)
    return EINVAL;

  EnterCriticalSection (&_c->waiters_count_lock_);
  /* If there aren't any waiters, then this is a no-op.   */
  if (_c->waiters_count_unblock_ != 0)
    {
      if (_c->waiters_count_ == 0)
      {
        LeaveCriticalSection (&_c->waiters_count_lock_);
        /* pthread_testcancel(); */
        return 0;
      }
      _c->waiters_count_ -= 1;
      _c->waiters_count_unblock_ += 1;
    }
  else if (_c->waiters_count_ > _c->waiters_count_gone_)
    {
      r = do_sema_b_wait (_c->sema_b, 1, INFINITE,&_c->waiters_b_lock_,&_c->value_b);
      if (r != 0)
      {
        LeaveCriticalSection (&_c->waiters_count_lock_);
        /* pthread_testcancel(); */
        return r;
      }
      if (_c->waiters_count_gone_ != 0)
      {
        _c->waiters_count_ -= _c->waiters_count_gone_;
        _c->waiters_count_gone_ = 0;
      }
      _c->waiters_count_ -= 1;
      _c->waiters_count_unblock_ = 1;
    }
  else
    {
      LeaveCriticalSection (&_c->waiters_count_lock_);
      /* pthread_testcancel(); */
      return 0;
    }
  LeaveCriticalSection (&_c->waiters_count_lock_);
  r = do_sema_b_release(_c->sema_q, 1,&_c->waiters_q_lock_,&_c->value_q);
  /* pthread_testcancel(); */
  return r;
}

int
pthread_cond_broadcast (pthread_cond_t *c)
{
  cond_t *_c;
  int r;
  int relCnt = 0;

  if (!c || !*c)
    return EINVAL;
  _c = (cond_t *)*c;
  if (_c == (cond_t*)0)
    return 0;
  else if (_c->valid != (unsigned int)LIFE_COND)
    return EINVAL;

  EnterCriticalSection (&_c->waiters_count_lock_);
  /* If there aren't any waiters, then this is a no-op.   */
  if (_c->waiters_count_unblock_ != 0)
    {
      if (_c->waiters_count_ == 0)
      {
        LeaveCriticalSection (&_c->waiters_count_lock_);
        /* pthread_testcancel(); */
        return 0;
      }
      relCnt = _c->waiters_count_;
      _c->waiters_count_ = 0;
      _c->waiters_count_unblock_ += relCnt;
    }
  else if (_c->waiters_count_ > _c->waiters_count_gone_)
    {
      r = do_sema_b_wait (_c->sema_b, 1, INFINITE,&_c->waiters_b_lock_,&_c->value_b);
      if (r != 0)
      {
        LeaveCriticalSection (&_c->waiters_count_lock_);
        /* pthread_testcancel(); */
        return r;
      }
      if (_c->waiters_count_gone_ != 0)
      {
        _c->waiters_count_ -= _c->waiters_count_gone_;
        _c->waiters_count_gone_ = 0;
      }
      relCnt = _c->waiters_count_;
      _c->waiters_count_ = 0;
      _c->waiters_count_unblock_ = relCnt;
    }
  else
    {
      LeaveCriticalSection (&_c->waiters_count_lock_);
      /* pthread_testcancel(); */
      return 0;
    }
  LeaveCriticalSection (&_c->waiters_count_lock_);
  r = do_sema_b_release(_c->sema_q, relCnt,&_c->waiters_q_lock_,&_c->value_q);
  /* pthread_testcancel(); */
  return r;
}

int
pthread_cond_wait (pthread_cond_t *c, pthread_mutex_t *external_mutex)
{
  sCondWaitHelper ch;
  cond_t *_c;
  int r;

  /* pthread_testcancel(); */

  if (!c || *c == (pthread_cond_t)NULL)
    return EINVAL;
  _c = (cond_t *)*c;
  if (*c == 0)
  {
    r = cond_static_init(c);
    if (r != 0 && r != EBUSY)
      return r;
    _c = (cond_t *) *c;
  } else if (_c->valid != (unsigned int)LIFE_COND)
    return EINVAL;

tryagain:
  r = do_sema_b_wait (_c->sema_b, 0, INFINITE,&_c->waiters_b_lock_,&_c->value_b);
  if (r != 0)
    return r;

  if (!TryEnterCriticalSection (&_c->waiters_count_lock_))
  {
    r = do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
    if (r != 0)
      return r;
    Sleep(0);
    goto tryagain;
  }

  _c->waiters_count_++;
  LeaveCriticalSection(&_c->waiters_count_lock_);
  r = do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
  if (r != 0)
    return r;

  ch.c = _c;
  ch.r = &r;
  ch.external_mutex = external_mutex;

  r = pthread_mutex_unlock(external_mutex);
  if (!r)
    r = do_sema_b_wait (_c->sema_q, 0, INFINITE,&_c->waiters_q_lock_,&_c->value_q);

  cleanup_wait( (void *) &ch);
  return r;
}

static void
cleanup_wait (void *arg)
{
  int n, r;
  sCondWaitHelper *ch = (sCondWaitHelper *) arg;
  cond_t *_c;

  _c = ch->c;
  EnterCriticalSection (&_c->waiters_count_lock_);
  n = _c->waiters_count_unblock_;
  if (n != 0)
    _c->waiters_count_unblock_ -= 1;
  else if ((INT_MAX/2) - 1 == _c->waiters_count_gone_)
  {
    _c->waiters_count_gone_ += 1;
    r = do_sema_b_wait (_c->sema_b, 1, INFINITE,&_c->waiters_b_lock_,&_c->value_b);
    if (r != 0)
    {
      LeaveCriticalSection(&_c->waiters_count_lock_);
      ch->r[0] = r;
      return;
    }
    _c->waiters_count_ -= _c->waiters_count_gone_;
    r = do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
    if (r != 0)
    {
      LeaveCriticalSection(&_c->waiters_count_lock_);
      ch->r[0] = r;
      return;
    }
    _c->waiters_count_gone_ = 0;
  }
  else
    _c->waiters_count_gone_ += 1;
  LeaveCriticalSection (&_c->waiters_count_lock_);

  if (n == 1)
  {
    r = do_sema_b_release (_c->sema_b, 1,&_c->waiters_b_lock_,&_c->value_b);
    if (r != 0)
    {
      ch->r[0] = r;
      return;
    }
  }
  r = pthread_mutex_lock(ch->external_mutex);
  if (r != 0)
    ch->r[0] = r;
}

static int
do_sema_b_wait (HANDLE sema, int nointerrupt, DWORD timeout,CRITICAL_SECTION *cs, LONG *val)
{
  int r;
  LONG v;
  EnterCriticalSection(cs);
  InterlockedDecrement(val);
  v = val[0];
  LeaveCriticalSection(cs);
  if (v >= 0)
    return 0;
  r = do_sema_b_wait_intern (sema, nointerrupt, timeout);
  EnterCriticalSection(cs);
  if (r != 0)
    InterlockedIncrement(val);
  LeaveCriticalSection(cs);
  return r;
}

int
do_sema_b_wait_intern (HANDLE sema, int nointerrupt, DWORD timeout)
{
  HANDLE arr[2];
  DWORD maxH = 1;
  int r = 0;
  DWORD res, dt;
  if (nointerrupt == 1)
  {
    res = WaitForSingleObject(sema, timeout);
    switch (res) {
    case WAIT_TIMEOUT:
        r = ETIMEDOUT;
        break;
    case WAIT_ABANDONED:
        r = EPERM;
        break;
    case WAIT_OBJECT_0:
        break;
    default:
        /*We can only return EINVAL though it might not be posix compliant  */
        r = EINVAL;
    }
    if (r != 0 && r != EINVAL && WaitForSingleObject(sema, 0) == WAIT_OBJECT_0)
      r = 0;
    return r;
  }
  arr[0] = sema;
  arr[1] = (HANDLE) 0;
  if (arr[1] != NULL) maxH += 1;
  if (maxH == 2)
  {
redo:
      res = WaitForMultipleObjects(maxH, arr, 0, timeout);
      switch (res) {
      case WAIT_TIMEOUT:
          r = ETIMEDOUT;
          break;
      case (WAIT_OBJECT_0 + 1):
          ResetEvent(arr[1]);
          if (nointerrupt != 2)
            {
            /* pthread_testcancel(); */
            return EINVAL;
            }
          /* pthread_testcancel(); */
          goto redo;
      case WAIT_ABANDONED:
          r = EPERM;
          break;
      case WAIT_OBJECT_0:
          r = 0;
          break;
      default:
          /*We can only return EINVAL though it might not be posix compliant  */
          r = EINVAL;
      }
      if (r != 0 && r != EINVAL && WaitForSingleObject(arr[0], 0) == WAIT_OBJECT_0)
        r = 0;
      if (r != 0 && nointerrupt != 2 && __pthread_shallcancel ())
        return EINVAL;
      return r;
  }
  if (timeout == INFINITE)
  {
    do {
      res = WaitForSingleObject(sema, 40);
      switch (res) {
      case WAIT_TIMEOUT:
          r = ETIMEDOUT;
          break;
      case WAIT_ABANDONED:
          r = EPERM;
          break;
      case WAIT_OBJECT_0:
          r = 0;
          break;
      default:
          /*We can only return EINVAL though it might not be posix compliant  */
          r = EINVAL;
      }
      if (r != 0 && __pthread_shallcancel ())
      {
        /*if (nointerrupt != 2)
           pthread_testcancel(); */;
        return EINVAL;
      }
    } while (r == ETIMEDOUT);
    if (r != 0 && r != EINVAL && WaitForSingleObject(sema, 0) == WAIT_OBJECT_0)
      r = 0;
    return r;
  }
  dt = 20;
  do {
    if (dt > timeout) dt = timeout;
    res = WaitForSingleObject(sema, dt);
    switch (res) {
    case WAIT_TIMEOUT:
        r = ETIMEDOUT;
        break;
    case WAIT_ABANDONED:
        r = EPERM;
        break;
    case WAIT_OBJECT_0:
        r = 0;
        break;
    default:
        /*We can only return EINVAL though it might not be posix compliant  */
        r = EINVAL;
    }
    timeout -= dt;
    if (timeout != 0 && r != 0 && __pthread_shallcancel ())
      return EINVAL;
  } while (r == ETIMEDOUT && timeout != 0);
  if (r != 0 && r == ETIMEDOUT && WaitForSingleObject(sema, 0) == WAIT_OBJECT_0)
    r = 0;
  /*if (r != 0 && nointerrupt != 2)
     pthread_testcancel(); */;
  return r;
}

static int
do_sema_b_release(HANDLE sema, LONG count,CRITICAL_SECTION *cs, LONG *val)
{
  int wc;
  EnterCriticalSection(cs);
  if (((long long) val[0] + (long long) count) > (long long) 0x7fffffffLL)
  {
    LeaveCriticalSection(cs);
    return ERANGE;
  }
  wc = -val[0];
  InterlockedExchangeAdd(val, count);
  if (wc <= 0 || ReleaseSemaphore(sema, (wc < count ? wc : count), NULL))
  {
    LeaveCriticalSection(cs);
    return 0;
  }
  InterlockedExchangeAdd(val, -count);
  LeaveCriticalSection(cs);
  return EINVAL;
}
//////////////////////////////////////////////////////////////////////////////
//
//  Create a process with a DLL (creatwth.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

// #define DETOUR_DEBUG 1
#define DETOURS_INTERNAL
/////////////////////////////////////////////////////////////////////////////
//
//  Core Detours Functionality (detours.h of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

//#pragma once
#ifndef _DETOURS_H_
#define _DETOURS_H_

#define DETOURS_VERSION     0x4c0c1   // 0xMAJORcMINORcPATCH

//////////////////////////////////////////////////////////////////////////////
//

#ifdef DETOURS_INTERNAL

#define _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS 1
#define _ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE 1

#pragma warning(disable:4068) // unknown pragma (suppress)

#if _MSC_VER >= 1900
#pragma warning(push)
#pragma warning(disable:4091) // empty typedef
#endif

// Suppress declspec(dllimport) for the sake of Detours
// users that provide kernel32 functionality themselves.
// This is ok in the mainstream case, it will just cost
// an extra instruction calling some functions, which
// LTCG optimizes away.
//
#define _KERNEL32_ 1
#define _USER32_ 1

#include <windows.h>
#if (_MSC_VER < 1310)
#else
#pragma warning(push)
#if _MSC_VER > 1400
#pragma warning(disable:6102 6103) // /analyze warnings
#endif
#include <strsafe.h>
#include <intsafe.h>
#pragma warning(pop)
#endif
#include <crtdbg.h>

// Allow Detours to cleanly compile with the MingW toolchain.
//
#ifdef __GNUC__
#define __try
#define __except(x) if (0)
#include <strsafe.h>
#include <intsafe.h>
#endif

// From winerror.h, as this error isn't found in some SDKs:
//
// MessageId: ERROR_DYNAMIC_CODE_BLOCKED
//
// MessageText:
//
// The operation was blocked as the process prohibits dynamic code generation.
//
#define ERROR_DYNAMIC_CODE_BLOCKED       1655L

#endif // DETOURS_INTERNAL

//////////////////////////////////////////////////////////////////////////////
//

#undef DETOURS_X64
#undef DETOURS_X86
#undef DETOURS_IA64
#undef DETOURS_ARM
#undef DETOURS_ARM64
#undef DETOURS_BITS
#undef DETOURS_32BIT
#undef DETOURS_64BIT

#ifndef DECLSPEC_HYBRID_PATCHABLE
#define DECLSPEC_HYBRID_PATCHABLE DECLSPEC_CHPE_PATCHABLE
#endif

#if defined(_X86_)
#define DETOURS_X86
#define DETOURS_OPTION_BITS 64

#elif defined(_AMD64_) || defined(_ARM64EC_)
#define DETOURS_X64
#define DETOURS_OPTION_BITS 32

#elif defined(_IA64_)
#define DETOURS_IA64
#define DETOURS_OPTION_BITS 32

#elif defined(_ARM_)
#define DETOURS_ARM

#elif defined(_ARM64_)
#define DETOURS_ARM64

#else
#error Unknown architecture (x86, amd64, ia64, arm, arm64, arm64ec)
#endif

#ifdef _WIN64
#undef DETOURS_32BIT
#define DETOURS_64BIT 1
#define DETOURS_BITS 64
// If all 64bit kernels can run one and only one 32bit architecture.
//#define DETOURS_OPTION_BITS 32
#else
#define DETOURS_32BIT 1
#undef DETOURS_64BIT
#define DETOURS_BITS 32
// If all 64bit kernels can run one and only one 32bit architecture.
//#define DETOURS_OPTION_BITS 32
#endif

/////////////////////////////////////////////////////////////// Helper Macros.
//
#define DETOURS_STRINGIFY_(x)    #x
#define DETOURS_STRINGIFY(x)    DETOURS_STRINGIFY_(x)

#define VER_DETOURS_BITS    DETOURS_STRINGIFY(DETOURS_BITS)

//////////////////////////////////////////////////////////////////////////////
//

#if (_MSC_VER < 1299) && !defined(__MINGW32__)
typedef LONG LONG_PTR;
typedef ULONG ULONG_PTR;
#endif

///////////////////////////////////////////////// SAL 2.0 Annotations w/o SAL.
//
//  These definitions are include so that Detours will build even if the
//  compiler doesn't have full SAL 2.0 support.
//
#ifndef DETOURS_DONT_REMOVE_SAL_20

#ifdef DETOURS_TEST_REMOVE_SAL_20
#undef _Analysis_assume_
#undef _Benign_race_begin_
#undef _Benign_race_end_
#undef _Field_range_
#undef _Field_size_
#undef _In_
#undef _In_bytecount_
#undef _In_count_
#undef __in_ecount
#undef _In_opt_
#undef _In_opt_bytecount_
#undef _In_opt_count_
#undef _In_opt_z_
#undef _In_range_
#undef _In_reads_
#undef _In_reads_bytes_
#undef _In_reads_opt_
#undef _In_reads_opt_bytes_
#undef _In_reads_or_z_
#undef _In_z_
#undef _Inout_
#undef _Inout_opt_
#undef _Inout_z_count_
#undef _Out_
#undef _Out_opt_
#undef _Out_writes_
#undef _Outptr_result_maybenull_
#undef _Readable_bytes_
#undef _Success_
#undef _Writable_bytes_
#undef _Pre_notnull_
#endif

#if defined(_Deref_out_opt_z_) && !defined(_Outptr_result_maybenull_)
#define _Outptr_result_maybenull_ _Deref_out_opt_z_
#endif

#if defined(_In_count_) && !defined(_In_reads_)
#define _In_reads_(x) _In_count_(x)
#endif

#if defined(_In_opt_count_) && !defined(_In_reads_opt_)
#define _In_reads_opt_(x) _In_opt_count_(x)
#endif

#if defined(_In_opt_bytecount_) && !defined(_In_reads_opt_bytes_)
#define _In_reads_opt_bytes_(x) _In_opt_bytecount_(x)
#endif

#if defined(_In_bytecount_) && !defined(_In_reads_bytes_)
#define _In_reads_bytes_(x) _In_bytecount_(x)
#endif

#ifndef _In_
#define _In_
#endif

#ifndef _In_bytecount_
#define _In_bytecount_(x)
#endif

#ifndef _In_count_
#define _In_count_(x)
#endif

#ifndef __in_ecount
#define __in_ecount(x)
#endif

#ifndef _In_opt_
#define _In_opt_
#endif

#ifndef _In_opt_bytecount_
#define _In_opt_bytecount_(x)
#endif

#ifndef _In_opt_count_
#define _In_opt_count_(x)
#endif

#ifndef _In_opt_z_
#define _In_opt_z_
#endif

#ifndef _In_range_
#define _In_range_(x,y)
#endif

#ifndef _In_reads_
#define _In_reads_(x)
#endif

#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif

#ifndef _In_reads_opt_
#define _In_reads_opt_(x)
#endif

#ifndef _In_reads_opt_bytes_
#define _In_reads_opt_bytes_(x)
#endif

#ifndef _In_reads_or_z_
#define _In_reads_or_z_
#endif

#ifndef _In_z_
#define _In_z_
#endif

#ifndef _Inout_
#define _Inout_
#endif

#ifndef _Inout_opt_
#define _Inout_opt_
#endif

#ifndef _Inout_z_count_
#define _Inout_z_count_(x)
#endif

#ifndef _Out_
#define _Out_
#endif

#ifndef _Out_opt_
#define _Out_opt_
#endif

#ifndef _Out_writes_
#define _Out_writes_(x)
#endif

#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif

#ifndef _Writable_bytes_
#define _Writable_bytes_(x)
#endif

#ifndef _Readable_bytes_
#define _Readable_bytes_(x)
#endif

#ifndef _Success_
#define _Success_(x)
#endif

#ifndef _Pre_notnull_
#define _Pre_notnull_
#endif

#ifdef DETOURS_INTERNAL

#pragma warning(disable:4615) // unknown warning type (suppress with older compilers)

#ifndef _Benign_race_begin_
#define _Benign_race_begin_
#endif

#ifndef _Benign_race_end_
#define _Benign_race_end_
#endif

#ifndef _Field_size_
#define _Field_size_(x)
#endif

#ifndef _Field_range_
#define _Field_range_(x,y)
#endif

#ifndef _Analysis_assume_
#define _Analysis_assume_(x)
#endif

#endif // DETOURS_INTERNAL
#endif // DETOURS_DONT_REMOVE_SAL_20

//////////////////////////////////////////////////////////////////////////////
//
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct  _GUID
{
    DWORD Data1;
    WORD Data2;
    WORD Data3;
    BYTE Data4[ 8 ];
} GUID;

#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        const GUID name \
                = { l, w1, w2, { b1, b2,  b3,  b4,  b5,  b6,  b7,  b8 } }
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name
#endif // INITGUID
#endif // !GUID_DEFINED

#if defined(__cplusplus)
#ifndef _REFGUID_DEFINED
#define _REFGUID_DEFINED
#define REFGUID             const GUID &
#endif // !_REFGUID_DEFINED
#else // !__cplusplus
#ifndef _REFGUID_DEFINED
#define _REFGUID_DEFINED
#define REFGUID             const GUID * const
#endif // !_REFGUID_DEFINED
#endif // !__cplusplus

#ifndef ARRAYSIZE
#define ARRAYSIZE(x)    (sizeof(x)/sizeof(x[0]))
#endif

//
//////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/////////////////////////////////////////////////// Instruction Target Macros.
//
#define DETOUR_INSTRUCTION_TARGET_NONE          ((PVOID)0)
#define DETOUR_INSTRUCTION_TARGET_DYNAMIC       ((PVOID)(LONG_PTR)-1)
#define DETOUR_SECTION_HEADER_SIGNATURE         0x00727444   // "Dtr\0"

extern const GUID DETOUR_EXE_RESTORE_GUID;
extern const GUID DETOUR_EXE_HELPER_GUID;

#define DETOUR_TRAMPOLINE_SIGNATURE             0x21727444  // Dtr!
typedef struct _DETOUR_TRAMPOLINE DETOUR_TRAMPOLINE, *PDETOUR_TRAMPOLINE;

#ifndef DETOUR_MAX_SUPPORTED_IMAGE_SECTION_HEADERS
#define DETOUR_MAX_SUPPORTED_IMAGE_SECTION_HEADERS      32
#endif // !DETOUR_MAX_SUPPORTED_IMAGE_SECTION_HEADERS

/////////////////////////////////////////////////////////// Binary Structures.
//
#pragma pack(push, 8)
typedef struct _DETOUR_SECTION_HEADER
{
    DWORD       cbHeaderSize;
    DWORD       nSignature;
    DWORD       nDataOffset;
    DWORD       cbDataSize;

    DWORD       nOriginalImportVirtualAddress;
    DWORD       nOriginalImportSize;
    DWORD       nOriginalBoundImportVirtualAddress;
    DWORD       nOriginalBoundImportSize;

    DWORD       nOriginalIatVirtualAddress;
    DWORD       nOriginalIatSize;
    DWORD       nOriginalSizeOfImage;
    DWORD       cbPrePE;

    DWORD       nOriginalClrFlags;
    DWORD       reserved1;
    DWORD       reserved2;
    DWORD       reserved3;

    // Followed by cbPrePE bytes of data.
} DETOUR_SECTION_HEADER, *PDETOUR_SECTION_HEADER;

typedef struct _DETOUR_SECTION_RECORD
{
    DWORD       cbBytes;
    DWORD       nReserved;
    GUID        guid;
} DETOUR_SECTION_RECORD, *PDETOUR_SECTION_RECORD;

typedef struct _DETOUR_CLR_HEADER
{
    // Header versioning
    ULONG                   cb;
    USHORT                  MajorRuntimeVersion;
    USHORT                  MinorRuntimeVersion;

    // Symbol table and startup information
    IMAGE_DATA_DIRECTORY    MetaData;
    ULONG                   Flags;

    // Followed by the rest of the IMAGE_COR20_HEADER
} DETOUR_CLR_HEADER, *PDETOUR_CLR_HEADER;

typedef struct _DETOUR_EXE_RESTORE
{
    DWORD               cb;
    DWORD               cbidh;
    DWORD               cbinh;
    DWORD               cbclr;

    PBYTE               pidh;
    PBYTE               pinh;
    PBYTE               pclr;

    IMAGE_DOS_HEADER    idh;
    union {
        IMAGE_NT_HEADERS    inh;        // all environments have this
#ifdef IMAGE_NT_OPTIONAL_HDR32_MAGIC    // some environments do not have this
        IMAGE_NT_HEADERS32  inh32;
#endif
#ifdef IMAGE_NT_OPTIONAL_HDR64_MAGIC    // some environments do not have this
        IMAGE_NT_HEADERS64  inh64;
#endif
#ifdef IMAGE_NT_OPTIONAL_HDR64_MAGIC    // some environments do not have this
        BYTE                raw[sizeof(IMAGE_NT_HEADERS64) +
                                sizeof(IMAGE_SECTION_HEADER) * DETOUR_MAX_SUPPORTED_IMAGE_SECTION_HEADERS];
#else
        BYTE                raw[0x108 + sizeof(IMAGE_SECTION_HEADER) * DETOUR_MAX_SUPPORTED_IMAGE_SECTION_HEADERS];
#endif
    };
    DETOUR_CLR_HEADER   clr;

} DETOUR_EXE_RESTORE, *PDETOUR_EXE_RESTORE;

#ifdef IMAGE_NT_OPTIONAL_HDR64_MAGIC
C_ASSERT(sizeof(IMAGE_NT_HEADERS64) == 0x108);
#endif

// The size can change, but assert for clarity due to the muddying #ifdefs.
#ifdef _WIN64
C_ASSERT(sizeof(DETOUR_EXE_RESTORE) == 0x688);
#else
C_ASSERT(sizeof(DETOUR_EXE_RESTORE) == 0x678);
#endif

typedef struct _DETOUR_EXE_HELPER
{
    DWORD               cb;
    DWORD               pid;
    DWORD               nDlls;
    CHAR                rDlls[4];
} DETOUR_EXE_HELPER, *PDETOUR_EXE_HELPER;

#pragma pack(pop)

#define DETOUR_SECTION_HEADER_DECLARE(cbSectionSize) \
{ \
      sizeof(DETOUR_SECTION_HEADER),\
      DETOUR_SECTION_HEADER_SIGNATURE,\
      sizeof(DETOUR_SECTION_HEADER),\
      (cbSectionSize),\
      \
      0,\
      0,\
      0,\
      0,\
      \
      0,\
      0,\
      0,\
      0,\
}

///////////////////////////////////////////////////////////// Binary Typedefs.
//
typedef BOOL (CALLBACK *PF_DETOUR_BINARY_BYWAY_CALLBACK)(
    _In_opt_ PVOID pContext,
    _In_opt_ LPCSTR pszFile,
    _Outptr_result_maybenull_ LPCSTR *ppszOutFile);

typedef BOOL (CALLBACK *PF_DETOUR_BINARY_FILE_CALLBACK)(
    _In_opt_ PVOID pContext,
    _In_ LPCSTR pszOrigFile,
    _In_ LPCSTR pszFile,
    _Outptr_result_maybenull_ LPCSTR *ppszOutFile);

typedef BOOL (CALLBACK *PF_DETOUR_BINARY_SYMBOL_CALLBACK)(
    _In_opt_ PVOID pContext,
    _In_ ULONG nOrigOrdinal,
    _In_ ULONG nOrdinal,
    _Out_ ULONG *pnOutOrdinal,
    _In_opt_ LPCSTR pszOrigSymbol,
    _In_opt_ LPCSTR pszSymbol,
    _Outptr_result_maybenull_ LPCSTR *ppszOutSymbol);

typedef BOOL (CALLBACK *PF_DETOUR_BINARY_COMMIT_CALLBACK)(
    _In_opt_ PVOID pContext);

typedef BOOL (CALLBACK *PF_DETOUR_ENUMERATE_EXPORT_CALLBACK)(_In_opt_ PVOID pContext,
                                                             _In_ ULONG nOrdinal,
                                                             _In_opt_ LPCSTR pszName,
                                                             _In_opt_ PVOID pCode);

typedef BOOL (CALLBACK *PF_DETOUR_IMPORT_FILE_CALLBACK)(_In_opt_ PVOID pContext,
                                                        _In_opt_ HMODULE hModule,
                                                        _In_opt_ LPCSTR pszFile);

typedef BOOL (CALLBACK *PF_DETOUR_IMPORT_FUNC_CALLBACK)(_In_opt_ PVOID pContext,
                                                        _In_ DWORD nOrdinal,
                                                        _In_opt_ LPCSTR pszFunc,
                                                        _In_opt_ PVOID pvFunc);

// Same as PF_DETOUR_IMPORT_FUNC_CALLBACK but extra indirection on last parameter.
typedef BOOL (CALLBACK *PF_DETOUR_IMPORT_FUNC_CALLBACK_EX)(_In_opt_ PVOID pContext,
                                                           _In_ DWORD nOrdinal,
                                                           _In_opt_ LPCSTR pszFunc,
                                                           _In_opt_ PVOID* ppvFunc);

typedef VOID * PDETOUR_BINARY;
typedef VOID * PDETOUR_LOADED_BINARY;

//////////////////////////////////////////////////////////// Transaction APIs.
//
LONG WINAPI DetourTransactionBegin(VOID);
LONG WINAPI DetourTransactionAbort(VOID);
LONG WINAPI DetourTransactionCommit(VOID);
LONG WINAPI DetourTransactionCommitEx(_Out_opt_ PVOID **pppFailedPointer);

LONG WINAPI DetourUpdateThread(_In_ HANDLE hThread);

LONG WINAPI DetourAttach(_Inout_ PVOID *ppPointer,
                         _In_ PVOID pDetour);

LONG WINAPI DetourAttachEx(_Inout_ PVOID *ppPointer,
                           _In_ PVOID pDetour,
                           _Out_opt_ PDETOUR_TRAMPOLINE *ppRealTrampoline,
                           _Out_opt_ PVOID *ppRealTarget,
                           _Out_opt_ PVOID *ppRealDetour);

LONG WINAPI DetourDetach(_Inout_ PVOID *ppPointer,
                         _In_ PVOID pDetour);

BOOL WINAPI DetourSetIgnoreTooSmall(_In_ BOOL fIgnore);
BOOL WINAPI DetourSetRetainRegions(_In_ BOOL fRetain);
PVOID WINAPI DetourSetSystemRegionLowerBound(_In_ PVOID pSystemRegionLowerBound);
PVOID WINAPI DetourSetSystemRegionUpperBound(_In_ PVOID pSystemRegionUpperBound);

////////////////////////////////////////////////////////////// Code Functions.
//
PVOID WINAPI DetourFindFunction(_In_ LPCSTR pszModule,
                                _In_ LPCSTR pszFunction);
PVOID WINAPI DetourCodeFromPointer(_In_ PVOID pPointer,
                                   _Out_opt_ PVOID *ppGlobals);
PVOID WINAPI DetourCopyInstruction(_In_opt_ PVOID pDst,
                                   _Inout_opt_ PVOID *ppDstPool,
                                   _In_ PVOID pSrc,
                                   _Out_opt_ PVOID *ppTarget,
                                   _Out_opt_ LONG *plExtra);
BOOL WINAPI DetourSetCodeModule(_In_ HMODULE hModule,
                                _In_ BOOL fLimitReferencesToModule);
PVOID WINAPI DetourAllocateRegionWithinJumpBounds(_In_ LPCVOID pbTarget,
                                                  _Out_ PDWORD pcbAllocatedSize);
BOOL WINAPI DetourIsFunctionImported(_In_ PBYTE pbCode,
                                     _In_ PBYTE pbAddress);

///////////////////////////////////////////////////// Loaded Binary Functions.
//
HMODULE WINAPI DetourGetContainingModule(_In_ PVOID pvAddr);
HMODULE WINAPI DetourEnumerateModules(_In_opt_ HMODULE hModuleLast);
PVOID WINAPI DetourGetEntryPoint(_In_opt_ HMODULE hModule);
ULONG WINAPI DetourGetModuleSize(_In_opt_ HMODULE hModule);
BOOL WINAPI DetourEnumerateExports(_In_ HMODULE hModule,
                                   _In_opt_ PVOID pContext,
                                   _In_ PF_DETOUR_ENUMERATE_EXPORT_CALLBACK pfExport);
BOOL WINAPI DetourEnumerateImports(_In_opt_ HMODULE hModule,
                                   _In_opt_ PVOID pContext,
                                   _In_opt_ PF_DETOUR_IMPORT_FILE_CALLBACK pfImportFile,
                                   _In_opt_ PF_DETOUR_IMPORT_FUNC_CALLBACK pfImportFunc);

BOOL WINAPI DetourEnumerateImportsEx(_In_opt_ HMODULE hModule,
                                     _In_opt_ PVOID pContext,
                                     _In_opt_ PF_DETOUR_IMPORT_FILE_CALLBACK pfImportFile,
                                     _In_opt_ PF_DETOUR_IMPORT_FUNC_CALLBACK_EX pfImportFuncEx);

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourFindPayload(_In_opt_ HMODULE hModule,
                               _In_ REFGUID rguid,
                               _Out_opt_ DWORD *pcbData);

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourFindPayloadEx(_In_ REFGUID rguid,
                                 _Out_opt_ DWORD *pcbData);

DWORD WINAPI DetourGetSizeOfPayloads(_In_opt_ HMODULE hModule);

BOOL WINAPI DetourFreePayload(_In_ PVOID pvData);
///////////////////////////////////////////////// Persistent Binary Functions.
//

PDETOUR_BINARY WINAPI DetourBinaryOpen(_In_ HANDLE hFile);

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourBinaryEnumeratePayloads(_In_ PDETOUR_BINARY pBinary,
                                           _Out_opt_ GUID *pGuid,
                                           _Out_ DWORD *pcbData,
                                           _Inout_ DWORD *pnIterator);

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourBinaryFindPayload(_In_ PDETOUR_BINARY pBinary,
                                     _In_ REFGUID rguid,
                                     _Out_ DWORD *pcbData);

PVOID WINAPI DetourBinarySetPayload(_In_ PDETOUR_BINARY pBinary,
                                    _In_ REFGUID rguid,
                                    _In_reads_opt_(cbData) PVOID pData,
                                    _In_ DWORD cbData);
BOOL WINAPI DetourBinaryDeletePayload(_In_ PDETOUR_BINARY pBinary, _In_ REFGUID rguid);
BOOL WINAPI DetourBinaryPurgePayloads(_In_ PDETOUR_BINARY pBinary);
BOOL WINAPI DetourBinaryResetImports(_In_ PDETOUR_BINARY pBinary);
BOOL WINAPI DetourBinaryEditImports(_In_ PDETOUR_BINARY pBinary,
                                    _In_opt_ PVOID pContext,
                                    _In_opt_ PF_DETOUR_BINARY_BYWAY_CALLBACK pfByway,
                                    _In_opt_ PF_DETOUR_BINARY_FILE_CALLBACK pfFile,
                                    _In_opt_ PF_DETOUR_BINARY_SYMBOL_CALLBACK pfSymbol,
                                    _In_opt_ PF_DETOUR_BINARY_COMMIT_CALLBACK pfCommit);
BOOL WINAPI DetourBinaryWrite(_In_ PDETOUR_BINARY pBinary, _In_ HANDLE hFile);
BOOL WINAPI DetourBinaryClose(_In_ PDETOUR_BINARY pBinary);

/////////////////////////////////////////////////// Create Process & Load Dll.
//
_Success_(return != NULL)
PVOID WINAPI DetourFindRemotePayload(_In_ HANDLE hProcess,
                                     _In_ REFGUID rguid,
                                     _Out_opt_ DWORD *pcbData);

typedef BOOL (WINAPI *PDETOUR_CREATE_PROCESS_ROUTINEA)(
    _In_opt_ LPCSTR lpApplicationName,
    _Inout_opt_ LPSTR lpCommandLine,
    _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
    _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
    _In_ BOOL bInheritHandles,
    _In_ DWORD dwCreationFlags,
    _In_opt_ LPVOID lpEnvironment,
    _In_opt_ LPCSTR lpCurrentDirectory,
    _In_ LPSTARTUPINFOA lpStartupInfo,
    _Out_ LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL (WINAPI *PDETOUR_CREATE_PROCESS_ROUTINEW)(
    _In_opt_ LPCWSTR lpApplicationName,
    _Inout_opt_ LPWSTR lpCommandLine,
    _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
    _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
    _In_ BOOL bInheritHandles,
    _In_ DWORD dwCreationFlags,
    _In_opt_ LPVOID lpEnvironment,
    _In_opt_ LPCWSTR lpCurrentDirectory,
    _In_ LPSTARTUPINFOW lpStartupInfo,
    _Out_ LPPROCESS_INFORMATION lpProcessInformation);

BOOL WINAPI DetourCreateProcessWithDllA(_In_opt_ LPCSTR lpApplicationName,
                                        _Inout_opt_ LPSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOA lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA);

BOOL WINAPI DetourCreateProcessWithDllW(_In_opt_ LPCWSTR lpApplicationName,
                                        _Inout_opt_ LPWSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCWSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOW lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW);

#ifdef UNICODE
#define DetourCreateProcessWithDll      DetourCreateProcessWithDllW
#define PDETOUR_CREATE_PROCESS_ROUTINE  PDETOUR_CREATE_PROCESS_ROUTINEW
#else
#define DetourCreateProcessWithDll      DetourCreateProcessWithDllA
#define PDETOUR_CREATE_PROCESS_ROUTINE  PDETOUR_CREATE_PROCESS_ROUTINEA
#endif // !UNICODE

BOOL WINAPI DetourCreateProcessWithDllExA(_In_opt_ LPCSTR lpApplicationName,
                                          _Inout_opt_ LPSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOA lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA);

BOOL WINAPI DetourCreateProcessWithDllExW(_In_opt_ LPCWSTR lpApplicationName,
                                          _Inout_opt_  LPWSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCWSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOW lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW);

#ifdef UNICODE
#define DetourCreateProcessWithDllEx    DetourCreateProcessWithDllExW
#else
#define DetourCreateProcessWithDllEx    DetourCreateProcessWithDllExA
#endif // !UNICODE

BOOL WINAPI DetourCreateProcessWithDllsA(_In_opt_ LPCSTR lpApplicationName,
                                         _Inout_opt_ LPSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOA lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA);

BOOL WINAPI DetourCreateProcessWithDllsW(_In_opt_ LPCWSTR lpApplicationName,
                                         _Inout_opt_ LPWSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCWSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOW lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW);

#ifdef UNICODE
#define DetourCreateProcessWithDlls     DetourCreateProcessWithDllsW
#else
#define DetourCreateProcessWithDlls     DetourCreateProcessWithDllsA
#endif // !UNICODE

BOOL WINAPI DetourProcessViaHelperA(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA);

BOOL WINAPI DetourProcessViaHelperW(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW);

#ifdef UNICODE
#define DetourProcessViaHelper          DetourProcessViaHelperW
#else
#define DetourProcessViaHelper          DetourProcessViaHelperA
#endif // !UNICODE

BOOL WINAPI DetourProcessViaHelperDllsA(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA);

BOOL WINAPI DetourProcessViaHelperDllsW(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW);

#ifdef UNICODE
#define DetourProcessViaHelperDlls      DetourProcessViaHelperDllsW
#else
#define DetourProcessViaHelperDlls      DetourProcessViaHelperDllsA
#endif // !UNICODE

BOOL WINAPI DetourUpdateProcessWithDll(_In_ HANDLE hProcess,
                                       _In_reads_(nDlls) LPCSTR *rlpDlls,
                                       _In_ DWORD nDlls);

BOOL WINAPI DetourUpdateProcessWithDllEx(_In_ HANDLE hProcess,
                                         _In_ HMODULE hImage,
                                         _In_ BOOL bIs32Bit,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_ DWORD nDlls);

BOOL WINAPI DetourCopyPayloadToProcess(_In_ HANDLE hProcess,
                                       _In_ REFGUID rguid,
                                       _In_reads_bytes_(cbData) LPCVOID pvData,
                                       _In_ DWORD cbData);
_Success_(return != NULL)
PVOID WINAPI DetourCopyPayloadToProcessEx(_In_ HANDLE hProcess,
                                          _In_ REFGUID rguid,
                                          _In_reads_bytes_(cbData) LPCVOID pvData,
                                          _In_ DWORD cbData);

BOOL WINAPI DetourRestoreAfterWith(VOID);
BOOL WINAPI DetourRestoreAfterWithEx(_In_reads_bytes_(cbData) PVOID pvData,
                                     _In_ DWORD cbData);
BOOL WINAPI DetourIsHelperProcess(VOID);
VOID CALLBACK DetourFinishHelperProcess(_In_ HWND,
                                        _In_ HINSTANCE,
                                        _In_ LPSTR,
                                        _In_ INT);

//
//////////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
}
#endif // __cplusplus

/////////////////////////////////////////////////// Type-safe overloads for C++
//
#if __cplusplus >= 201103L || _MSVC_LANG >= 201103L
#include <type_traits>

template<typename T>
struct DetoursIsFunctionPointer : std::false_type {};

template<typename T>
struct DetoursIsFunctionPointer<T*> : std::is_function<typename std::remove_pointer<T>::type> {};

template<
    typename T,
    typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourAttach(_Inout_ T *ppPointer,
                  _In_ T pDetour) noexcept
{
    return DetourAttach(
        reinterpret_cast<void**>(ppPointer),
        reinterpret_cast<void*>(pDetour));
}

template<
    typename T,
    typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourAttachEx(_Inout_ T *ppPointer,
                    _In_ T pDetour,
                    _Out_opt_ PDETOUR_TRAMPOLINE *ppRealTrampoline,
                    _Out_opt_ T *ppRealTarget,
                    _Out_opt_ T *ppRealDetour) noexcept
{
    return DetourAttachEx(
        reinterpret_cast<void**>(ppPointer),
        reinterpret_cast<void*>(pDetour),
        ppRealTrampoline,
        reinterpret_cast<void**>(ppRealTarget),
        reinterpret_cast<void**>(ppRealDetour));
}

template<
    typename T,
    typename std::enable_if<DetoursIsFunctionPointer<T>::value, int>::type = 0>
LONG DetourDetach(_Inout_ T *ppPointer,
                  _In_ T pDetour) noexcept
{
    return DetourDetach(
        reinterpret_cast<void**>(ppPointer),
        reinterpret_cast<void*>(pDetour));
}

#endif // __cplusplus >= 201103L || _MSVC_LANG >= 201103L
//
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////// Detours Internal Definitions.
//
#ifdef __cplusplus
#ifdef DETOURS_INTERNAL

#define NOTHROW
// #define NOTHROW (nothrow)

//////////////////////////////////////////////////////////////////////////////
//
#if (_MSC_VER < 1299) && !defined(__GNUC__)
#include <imagehlp.h>
typedef IMAGEHLP_MODULE IMAGEHLP_MODULE64;
typedef PIMAGEHLP_MODULE PIMAGEHLP_MODULE64;
typedef IMAGEHLP_SYMBOL SYMBOL_INFO;
typedef PIMAGEHLP_SYMBOL PSYMBOL_INFO;

static inline
LONG InterlockedCompareExchange(_Inout_ LONG *ptr, _In_ LONG nval, _In_ LONG oval)
{
    return (LONG)::InterlockedCompareExchange((PVOID*)ptr, (PVOID)nval, (PVOID)oval);
}
#else
#pragma warning(push)
#pragma warning(disable:4091) // empty typedef
#include <dbghelp.h>
#pragma warning(pop)
#endif

#ifdef IMAGEAPI // defined by DBGHELP.H
typedef LPAPI_VERSION (NTAPI *PF_ImagehlpApiVersionEx)(_In_ LPAPI_VERSION AppVersion);

typedef BOOL (NTAPI *PF_SymInitialize)(_In_ HANDLE hProcess,
                                       _In_opt_ LPCSTR UserSearchPath,
                                       _In_ BOOL fInvadeProcess);
typedef DWORD (NTAPI *PF_SymSetOptions)(_In_ DWORD SymOptions);
typedef DWORD (NTAPI *PF_SymGetOptions)(VOID);
typedef DWORD64 (NTAPI *PF_SymLoadModule64)(_In_ HANDLE hProcess,
                                            _In_opt_ HANDLE hFile,
                                            _In_opt_ LPSTR ImageName,
                                            _In_opt_ LPSTR ModuleName,
                                            _In_ DWORD64 BaseOfDll,
                                            _In_ DWORD SizeOfDll);
typedef BOOL (NTAPI *PF_SymGetModuleInfo64)(_In_ HANDLE hProcess,
                                            _In_ DWORD64 qwAddr,
                                            _Out_ PIMAGEHLP_MODULE64 ModuleInfo);
typedef BOOL (NTAPI *PF_SymFromName)(_In_ HANDLE hProcess,
                                     _In_ LPSTR Name,
                                     _Out_ PSYMBOL_INFO Symbol);

typedef struct _DETOUR_SYM_INFO
{
    HANDLE                  hProcess;
    HMODULE                 hDbgHelp;
    PF_ImagehlpApiVersionEx pfImagehlpApiVersionEx;
    PF_SymInitialize        pfSymInitialize;
    PF_SymSetOptions        pfSymSetOptions;
    PF_SymGetOptions        pfSymGetOptions;
    PF_SymLoadModule64      pfSymLoadModule64;
    PF_SymGetModuleInfo64   pfSymGetModuleInfo64;
    PF_SymFromName          pfSymFromName;
} DETOUR_SYM_INFO, *PDETOUR_SYM_INFO;

PDETOUR_SYM_INFO DetourLoadImageHlp(VOID);

#endif // IMAGEAPI

#if defined(_INC_STDIO) && !defined(_CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS)
#error detours.h must be included before stdio.h (or at least define _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS earlier)
#endif
#define _CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS 1

#ifdef _DEBUG

int Detour_AssertExprWithFunctionName(int reportType, const char* filename, int linenumber, const char* FunctionName, const char* msg);

#define DETOUR_ASSERT_EXPR_WITH_FUNCTION(expr, msg) \
    (void) ((expr) || \
    (1 != Detour_AssertExprWithFunctionName(_CRT_ASSERT, __FILE__, __LINE__,__FUNCTION__, msg)) || \
    (_CrtDbgBreak(), 0))

#define DETOUR_ASSERT(expr) DETOUR_ASSERT_EXPR_WITH_FUNCTION((expr), #expr)

#else// _DEBUG
#define DETOUR_ASSERT(expr)
#endif// _DEBUG

#ifndef DETOUR_TRACE
#if DETOUR_DEBUG
#define DETOUR_TRACE(x) printf x
#define DETOUR_BREAK()  __debugbreak()
#include <stdio.h>
#include <limits.h>
#else
#define DETOUR_TRACE(x)
#define DETOUR_BREAK()
#endif
#endif

#if 1 || defined(DETOURS_IA64)

//
// IA64 instructions are 41 bits, 3 per bundle, plus 5 bit bundle template => 128 bits per bundle.
//

#define DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE (3)

#define DETOUR_IA64_TEMPLATE_OFFSET (0)
#define DETOUR_IA64_TEMPLATE_SIZE   (5)

#define DETOUR_IA64_INSTRUCTION_SIZE (41)
#define DETOUR_IA64_INSTRUCTION0_OFFSET (DETOUR_IA64_TEMPLATE_SIZE)
#define DETOUR_IA64_INSTRUCTION1_OFFSET (DETOUR_IA64_TEMPLATE_SIZE + DETOUR_IA64_INSTRUCTION_SIZE)
#define DETOUR_IA64_INSTRUCTION2_OFFSET (DETOUR_IA64_TEMPLATE_SIZE + DETOUR_IA64_INSTRUCTION_SIZE + DETOUR_IA64_INSTRUCTION_SIZE)

C_ASSERT(DETOUR_IA64_TEMPLATE_SIZE + DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE * DETOUR_IA64_INSTRUCTION_SIZE == 128);

__declspec(align(16)) struct DETOUR_IA64_BUNDLE
{
  public:
    union
    {
        BYTE    data[16];
        UINT64  wide[2];
    };

    enum {
        A_UNIT  = 1u,
        I_UNIT  = 2u,
        M_UNIT  = 3u,
        B_UNIT  = 4u,
        F_UNIT  = 5u,
        L_UNIT  = 6u,
        X_UNIT  = 7u,
    };
    struct DETOUR_IA64_METADATA
    {
        ULONG       nTemplate       : 8;    // Instruction template.
        ULONG       nUnit0          : 4;    // Unit for slot 0
        ULONG       nUnit1          : 4;    // Unit for slot 1
        ULONG       nUnit2          : 4;    // Unit for slot 2
    };

  protected:
    static const DETOUR_IA64_METADATA s_rceCopyTable[33];

    UINT RelocateBundle(_Inout_ DETOUR_IA64_BUNDLE* pDst, _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra) const;

    bool RelocateInstruction(_Inout_ DETOUR_IA64_BUNDLE* pDst,
                             _In_ BYTE slot,
                             _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra) const;

    // 120 112 104 96 88 80 72 64 56 48 40 32 24 16  8  0
    //  f.  e.  d. c. b. a. 9. 8. 7. 6. 5. 4. 3. 2. 1. 0.

    //                                      00
    // f.e. d.c. b.a. 9.8. 7.6. 5.4. 3.2. 1.0.
    // 0000 0000 0000 0000 0000 0000 0000 001f : Template [4..0]
    // 0000 0000 0000 0000 0000 03ff ffff ffe0 : Zero [ 41..  5]
    // 0000 0000 0000 0000 0000 3c00 0000 0000 : Zero [ 45.. 42]
    // 0000 0000 0007 ffff ffff c000 0000 0000 : One  [ 82.. 46]
    // 0000 0000 0078 0000 0000 0000 0000 0000 : One  [ 86.. 83]
    // 0fff ffff ff80 0000 0000 0000 0000 0000 : Two  [123.. 87]
    // f000 0000 0000 0000 0000 0000 0000 0000 : Two  [127..124]
    BYTE    GetTemplate() const;
    // Get 4 bit opcodes.
    BYTE    GetInst0() const;
    BYTE    GetInst1() const;
    BYTE    GetInst2() const;
    BYTE    GetUnit(BYTE slot) const;
    BYTE    GetUnit0() const;
    BYTE    GetUnit1() const;
    BYTE    GetUnit2() const;
    // Get 37 bit data.
    UINT64  GetData0() const;
    UINT64  GetData1() const;
    UINT64  GetData2() const;

    // Get/set the full 41 bit instructions.
    UINT64  GetInstruction(BYTE slot) const;
    UINT64  GetInstruction0() const;
    UINT64  GetInstruction1() const;
    UINT64  GetInstruction2() const;
    void    SetInstruction(BYTE slot, UINT64 instruction);
    void    SetInstruction0(UINT64 instruction);
    void    SetInstruction1(UINT64 instruction);
    void    SetInstruction2(UINT64 instruction);

    // Get/set bitfields.
    static UINT64 GetBits(UINT64 Value, UINT64 Offset, UINT64 Count);
    static UINT64 SetBits(UINT64 Value, UINT64 Offset, UINT64 Count, UINT64 Field);

    // Get specific read-only fields.
    static UINT64 GetOpcode(UINT64 instruction); // 4bit opcode
    static UINT64 GetX(UINT64 instruction); // 1bit opcode extension
    static UINT64 GetX3(UINT64 instruction); // 3bit opcode extension
    static UINT64 GetX6(UINT64 instruction); // 6bit opcode extension

    // Get/set specific fields.
    static UINT64 GetImm7a(UINT64 instruction);
    static UINT64 SetImm7a(UINT64 instruction, UINT64 imm7a);
    static UINT64 GetImm13c(UINT64 instruction);
    static UINT64 SetImm13c(UINT64 instruction, UINT64 imm13c);
    static UINT64 GetSignBit(UINT64 instruction);
    static UINT64 SetSignBit(UINT64 instruction, UINT64 signBit);
    static UINT64 GetImm20a(UINT64 instruction);
    static UINT64 SetImm20a(UINT64 instruction, UINT64 imm20a);
    static UINT64 GetImm20b(UINT64 instruction);
    static UINT64 SetImm20b(UINT64 instruction, UINT64 imm20b);

    static UINT64 SignExtend(UINT64 Value, UINT64 Offset);

    BOOL    IsMovlGp() const;

    VOID    SetInst(BYTE Slot, BYTE nInst);
    VOID    SetInst0(BYTE nInst);
    VOID    SetInst1(BYTE nInst);
    VOID    SetInst2(BYTE nInst);
    VOID    SetData(BYTE Slot, UINT64 nData);
    VOID    SetData0(UINT64 nData);
    VOID    SetData1(UINT64 nData);
    VOID    SetData2(UINT64 nData);
    BOOL    SetNop(BYTE Slot);
    BOOL    SetNop0();
    BOOL    SetNop1();
    BOOL    SetNop2();

  public:
    BOOL    IsBrl() const;
    VOID    SetBrl();
    VOID    SetBrl(UINT64 target);
    UINT64  GetBrlTarget() const;
    VOID    SetBrlTarget(UINT64 target);
    VOID    SetBrlImm(UINT64 imm);
    UINT64  GetBrlImm() const;

    UINT64  GetMovlGp() const;
    VOID    SetMovlGp(UINT64 gp);

    VOID    SetStop();

    UINT    Copy(_Out_ DETOUR_IA64_BUNDLE *pDst, _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra = NULL) const;
};
#endif // DETOURS_IA64

#ifdef DETOURS_ARM

#define DETOURS_PFUNC_TO_PBYTE(p)  ((PBYTE)(((ULONG_PTR)(p)) & ~(ULONG_PTR)1))
#define DETOURS_PBYTE_TO_PFUNC(p)  ((PBYTE)(((ULONG_PTR)(p)) | (ULONG_PTR)1))

#endif // DETOURS_ARM

//////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define DETOUR_OFFLINE_LIBRARY(x)                                       \
PVOID WINAPI DetourCopyInstruction##x(_In_opt_ PVOID pDst,              \
                                      _Inout_opt_ PVOID *ppDstPool,     \
                                      _In_ PVOID pSrc,                  \
                                      _Out_opt_ PVOID *ppTarget,        \
                                      _Out_opt_ LONG *plExtra);         \
                                                                        \
BOOL WINAPI DetourSetCodeModule##x(_In_ HMODULE hModule,                \
                                   _In_ BOOL fLimitReferencesToModule); \

DETOUR_OFFLINE_LIBRARY(X86)
DETOUR_OFFLINE_LIBRARY(X64)
DETOUR_OFFLINE_LIBRARY(ARM)
DETOUR_OFFLINE_LIBRARY(ARM64)
DETOUR_OFFLINE_LIBRARY(IA64)

#undef DETOUR_OFFLINE_LIBRARY

//////////////////////////////////////////////////////////////////////////////
//
// Helpers for manipulating page protection.
//

_Success_(return != FALSE)
BOOL WINAPI DetourVirtualProtectSameExecuteEx(_In_  HANDLE hProcess,
                                              _In_  PVOID pAddress,
                                              _In_  SIZE_T nSize,
                                              _In_  DWORD dwNewProtect,
                                              _Out_ PDWORD pdwOldProtect);

_Success_(return != FALSE)
BOOL WINAPI DetourVirtualProtectSameExecute(_In_  PVOID pAddress,
                                            _In_  SIZE_T nSize,
                                            _In_  DWORD dwNewProtect,
                                            _Out_ PDWORD pdwOldProtect);

// Detours must depend only on kernel32.lib, so we cannot use IsEqualGUID
BOOL WINAPI DetourAreSameGuid(_In_ REFGUID left, _In_ REFGUID right);
#ifdef __cplusplus
}
#endif // __cplusplus

//////////////////////////////////////////////////////////////////////////////

#define MM_ALLOCATION_GRANULARITY 0x10000

//////////////////////////////////////////////////////////////////////////////

#endif // DETOURS_INTERNAL
#endif // __cplusplus

#endif // _DETOURS_H_
//
////////////////////////////////////////////////////////////////  End of File.
#include <stddef.h>

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

#define IMPORT_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
#define BOUND_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT]
#define CLR_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
#define IAT_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT]

//////////////////////////////////////////////////////////////////////////////
//
const GUID DETOUR_EXE_HELPER_GUID = { /* ea0251b9-5cde-41b5-98d0-2af4a26b0fee */
    0xea0251b9, 0x5cde, 0x41b5,
    { 0x98, 0xd0, 0x2a, 0xf4, 0xa2, 0x6b, 0x0f, 0xee }};

//////////////////////////////////////////////////////////////////////////////
//
// Enumerate through modules in the target process.
//
static PVOID LoadNtHeaderFromProcess(_In_ HANDLE hProcess,
                                     _In_ HMODULE hModule,
                                     _Out_ PIMAGE_NT_HEADERS32 pNtHeader)
{
    ZeroMemory(pNtHeader, sizeof(*pNtHeader));
    PBYTE pbModule = (PBYTE)hModule;

    if (pbModule == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    if (VirtualQueryEx(hProcess, hModule, &mbi, sizeof(mbi)) == 0) {
        return NULL;
    }

    IMAGE_DOS_HEADER idh;
    if (!ReadProcessMemory(hProcess, pbModule, &idh, sizeof(idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      pbModule, pbModule + sizeof(idh), GetLastError()));
        return NULL;
    }

    if (idh.e_magic != IMAGE_DOS_SIGNATURE ||
        (DWORD)idh.e_lfanew > mbi.RegionSize ||
        (DWORD)idh.e_lfanew < sizeof(idh)) {

        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    if (!ReadProcessMemory(hProcess, pbModule + idh.e_lfanew,
                           pNtHeader, sizeof(*pNtHeader), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p:%p) failed: %lu\n",
                      pbModule + idh.e_lfanew,
                      pbModule + idh.e_lfanew + sizeof(*pNtHeader),
                      pbModule,
                      GetLastError()));
        return NULL;
    }

    if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    return pbModule + idh.e_lfanew;
}

static HMODULE EnumerateModulesInProcess(_In_ HANDLE hProcess,
                                         _In_opt_ HMODULE hModuleLast,
                                         _Out_ PIMAGE_NT_HEADERS32 pNtHeader,
                                         _Out_opt_ PVOID *pRemoteNtHeader)
{
    ZeroMemory(pNtHeader, sizeof(*pNtHeader));
    if (pRemoteNtHeader) {
        *pRemoteNtHeader = NULL;
    }

    PBYTE pbLast = (PBYTE)hModuleLast + MM_ALLOCATION_GRANULARITY;

    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    // Find the next memory region that contains a mapped PE image.
    //

    for (;; pbLast = (PBYTE)mbi.BaseAddress + mbi.RegionSize) {
        if (VirtualQueryEx(hProcess, (PVOID)pbLast, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        // Usermode address space has such an unaligned region size always at the
        // end and only at the end.
        //
        if ((mbi.RegionSize & 0xfff) == 0xfff) {
            break;
        }
        if (((PBYTE)mbi.BaseAddress + mbi.RegionSize) < pbLast) {
            break;
        }

        // Skip uncommitted regions and guard pages.
        //
        if ((mbi.State != MEM_COMMIT) ||
            ((mbi.Protect & 0xff) == PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            continue;
        }

        PVOID remoteHeader
            = LoadNtHeaderFromProcess(hProcess, (HMODULE)pbLast, pNtHeader);
        if (remoteHeader) {
            if (pRemoteNtHeader) {
                *pRemoteNtHeader = remoteHeader;
            }

            return (HMODULE)pbLast;
        }
    }
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
//
// Find payloads in target process.
//

static PVOID FindDetourSectionInRemoteModule(_In_ HANDLE hProcess,
                                             _In_ HMODULE hModule,
                                             _In_ const IMAGE_NT_HEADERS32 *pNtHeader,
                                             _In_ PVOID pRemoteNtHeader)
{
    if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }

    PIMAGE_SECTION_HEADER pRemoteSectionHeaders
        = (PIMAGE_SECTION_HEADER)((PBYTE)pRemoteNtHeader
                                  + sizeof(pNtHeader->Signature)
                                  + sizeof(pNtHeader->FileHeader)
                                  + pNtHeader->FileHeader.SizeOfOptionalHeader);

    IMAGE_SECTION_HEADER header;
    for (DWORD n = 0; n < pNtHeader->FileHeader.NumberOfSections; ++n) {
        if (!ReadProcessMemory(hProcess, pRemoteSectionHeaders + n, &header, sizeof(header), NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p) failed: %lu\n",
                pRemoteSectionHeaders + n,
                (PBYTE)(pRemoteSectionHeaders + n) + sizeof(header),
                GetLastError()));

            return NULL;
        }

        if (strcmp((PCHAR)header.Name, ".detour") == 0) {
            if (header.VirtualAddress == 0 ||
                header.SizeOfRawData == 0) {

                break;
            }

            SetLastError(NO_ERROR);
            return (PBYTE)hModule + header.VirtualAddress;
        }
    }

    SetLastError(ERROR_EXE_MARKED_INVALID);
    return NULL;
}

static PVOID FindPayloadInRemoteDetourSection(_In_ HANDLE hProcess,
                                               _In_ REFGUID rguid,
                                               _Out_opt_ DWORD *pcbData,
                                               _In_ PVOID pvRemoteDetoursSection)
{
    if (pcbData) {
        *pcbData = 0;
    }

    PBYTE pbData = (PBYTE)pvRemoteDetoursSection;

    DETOUR_SECTION_HEADER header;
    if (!ReadProcessMemory(hProcess, pbData, &header, sizeof(header), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(dsh@%p..%p) failed: %lu\n",
            pbData,
            pbData + sizeof(header),
            GetLastError()));
        return NULL;
    }

    if (header.cbHeaderSize < sizeof(DETOUR_SECTION_HEADER) ||
        header.nSignature != DETOUR_SECTION_HEADER_SIGNATURE) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }

    if (header.nDataOffset == 0) {
        header.nDataOffset = header.cbHeaderSize;
    }

    for (PVOID pvSection = pbData + header.nDataOffset; pvSection < pbData + header.cbDataSize;) {
        DETOUR_SECTION_RECORD section;
        if (!ReadProcessMemory(hProcess, pvSection, &section, sizeof(section), NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(dsr@%p..%p) failed: %lu\n",
                pvSection,
                (PBYTE)pvSection + sizeof(section),
                GetLastError()));
            return NULL;
        }

        if (DetourAreSameGuid(section.guid, rguid)) {
            if (pcbData) {
                *pcbData = section.cbBytes - sizeof(section);
            }
            SetLastError(NO_ERROR);
            return (DETOUR_SECTION_RECORD *)pvSection + 1;
        }

        pvSection = (PBYTE)pvSection + section.cbBytes;
    }

    return NULL;
}

_Success_(return != NULL)
PVOID WINAPI DetourFindRemotePayload(_In_ HANDLE hProcess,
                                     _In_ REFGUID rguid,
                                     _Out_opt_ DWORD *pcbData)
{
    if (hProcess == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    IMAGE_NT_HEADERS32 header;
    PVOID pvRemoteHeader;
    for (HMODULE hMod = NULL; (hMod = EnumerateModulesInProcess(hProcess, hMod, &header, &pvRemoteHeader)) != NULL;) {
        PVOID pvData = FindDetourSectionInRemoteModule(hProcess, hMod, &header, pvRemoteHeader);
        if (pvData != NULL) {
            pvData = FindPayloadInRemoteDetourSection(hProcess, rguid, pcbData, pvData);
            if (pvData != NULL) {
                return pvData;
            }
        }
    }

    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
//
// Find a region of memory in which we can create a replacement import table.
//
static PBYTE FindAndAllocateNearBase(HANDLE hProcess, PBYTE pbModule, PBYTE pbBase, DWORD cbAlloc)
{
    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    PBYTE pbLast = pbBase;
    for (;; pbLast = (PBYTE)mbi.BaseAddress + mbi.RegionSize) {

        ZeroMemory(&mbi, sizeof(mbi));
        if (VirtualQueryEx(hProcess, (PVOID)pbLast, &mbi, sizeof(mbi)) == 0) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) {
                break;
            }
            DETOUR_TRACE(("VirtualQueryEx(%p) failed: %lu\n",
                          pbLast, GetLastError()));
            break;
        }
        // Usermode address space has such an unaligned region size always at the
        // end and only at the end.
        //
        if ((mbi.RegionSize & 0xfff) == 0xfff) {
            break;
        }

        // Skip anything other than a pure free region.
        //
        if (mbi.State != MEM_FREE) {
            continue;
        }

        // Use the max of mbi.BaseAddress and pbBase, in case mbi.BaseAddress < pbBase.
        PBYTE pbAddress = (PBYTE)mbi.BaseAddress > pbBase ? (PBYTE)mbi.BaseAddress : pbBase;

        // Round pbAddress up to the nearest MM allocation boundary.
        const DWORD_PTR mmGranularityMinusOne = (DWORD_PTR)(MM_ALLOCATION_GRANULARITY -1);
        pbAddress = (PBYTE)(((DWORD_PTR)pbAddress + mmGranularityMinusOne) & ~mmGranularityMinusOne);

#ifdef _WIN64
        // The offset from pbModule to any replacement import must fit into 32 bits.
        // For simplicity, we check that the offset to the last byte fits into 32 bits,
        // instead of the largest offset we'll actually use. The values are very similar.
        const size_t GB4 = ((((size_t)1) << 32) - 1);
        if ((size_t)(pbAddress + cbAlloc - 1 - pbModule) > GB4) {
            DETOUR_TRACE(("FindAndAllocateNearBase(1) failing due to distance >4GB %p\n", pbAddress));
            return NULL;
        }
#else
        UNREFERENCED_PARAMETER(pbModule);
#endif

        DETOUR_TRACE(("Free region %p..%p\n",
                      mbi.BaseAddress,
                      (PBYTE)mbi.BaseAddress + mbi.RegionSize));

        for (; pbAddress < (PBYTE)mbi.BaseAddress + mbi.RegionSize; pbAddress += MM_ALLOCATION_GRANULARITY) {
            PBYTE pbAlloc = (PBYTE)VirtualAllocEx(hProcess, pbAddress, cbAlloc,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (pbAlloc == NULL) {
                DETOUR_TRACE(("VirtualAllocEx(%p) failed: %lu\n", pbAddress, GetLastError()));
                continue;
            }
#ifdef _WIN64
            // The offset from pbModule to any replacement import must fit into 32 bits.
            if ((size_t)(pbAddress + cbAlloc - 1 - pbModule) > GB4) {
                DETOUR_TRACE(("FindAndAllocateNearBase(2) failing due to distance >4GB %p\n", pbAddress));
                return NULL;
            }
#endif
            DETOUR_TRACE(("[%p..%p] Allocated for import table.\n",
                          pbAlloc, pbAlloc + cbAlloc));
            return pbAlloc;
        }
    }
    return NULL;
}

static inline DWORD PadToDword(DWORD dw)
{
    return (dw + 3) & ~3u;
}

static inline DWORD PadToDwordPtr(DWORD dw)
{
    return (dw + 7) & ~7u;
}

static inline HRESULT ReplaceOptionalSizeA(_Inout_z_count_(cchDest) LPSTR pszDest,
                                           _In_ size_t cchDest,
                                           _In_z_ LPCSTR pszSize)
{
    if (cchDest == 0 || pszDest == NULL || pszSize == NULL ||
        pszSize[0] == '\0' || pszSize[1] == '\0' || pszSize[2] != '\0') {

        // can not write into empty buffer or with string other than two chars.
        return ERROR_INVALID_PARAMETER;
    }

    for (; cchDest >= 2; cchDest--, pszDest++) {
        if (pszDest[0] == '?' && pszDest[1] == '?') {
            pszDest[0] = pszSize[0];
            pszDest[1] = pszSize[1];
            break;
        }
    }

    return S_OK;
}

static BOOL RecordExeRestore(HANDLE hProcess, HMODULE hModule, DETOUR_EXE_RESTORE& der)
{
    // Save the various headers for DetourRestoreAfterWith.
    ZeroMemory(&der, sizeof(der));
    der.cb = sizeof(der);

    der.pidh = (PBYTE)hModule;
    der.cbidh = sizeof(der.idh);
    if (!ReadProcessMemory(hProcess, der.pidh, &der.idh, sizeof(der.idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      der.pidh, der.pidh + der.cbidh, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("IDH: %p..%p\n", der.pidh, der.pidh + der.cbidh));

    // We read the NT header in two passes to get the full size.
    // First we read just the Signature and FileHeader.
    der.pinh = der.pidh + der.idh.e_lfanew;
    der.cbinh = FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader);
    if (!ReadProcessMemory(hProcess, der.pinh, &der.inh, der.cbinh, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      der.pinh, der.pinh + der.cbinh, GetLastError()));
        return FALSE;
    }

    // Second we read the OptionalHeader and Section headers.
    der.cbinh = (FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                 der.inh.FileHeader.SizeOfOptionalHeader +
                 der.inh.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));

    if (der.cbinh > sizeof(der.raw)) {
        return FALSE;
    }

    if (!ReadProcessMemory(hProcess, der.pinh, &der.inh, der.cbinh, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      der.pinh, der.pinh + der.cbinh, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("INH: %p..%p\n", der.pinh, der.pinh + der.cbinh));

    // Third, we read the CLR header

    if (der.inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (der.inh32.CLR_DIRECTORY.VirtualAddress != 0 &&
            der.inh32.CLR_DIRECTORY.Size != 0) {

            DETOUR_TRACE(("CLR32.VirtAddr=%08lx, CLR.Size=%lu\n",
                          der.inh32.CLR_DIRECTORY.VirtualAddress,
                          der.inh32.CLR_DIRECTORY.Size));

            der.pclr = ((PBYTE)hModule) + der.inh32.CLR_DIRECTORY.VirtualAddress;
        }
    }
    else if (der.inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (der.inh64.CLR_DIRECTORY.VirtualAddress != 0 &&
            der.inh64.CLR_DIRECTORY.Size != 0) {

            DETOUR_TRACE(("CLR64.VirtAddr=%08lx, CLR.Size=%lu\n",
                          der.inh64.CLR_DIRECTORY.VirtualAddress,
                          der.inh64.CLR_DIRECTORY.Size));

            der.pclr = ((PBYTE)hModule) + der.inh64.CLR_DIRECTORY.VirtualAddress;
        }
    }

    if (der.pclr != 0) {
        der.cbclr = sizeof(der.clr);
        if (!ReadProcessMemory(hProcess, der.pclr, &der.clr, der.cbclr, NULL)) {
            DETOUR_TRACE(("ReadProcessMemory(clr@%p..%p) failed: %lu\n",
                          der.pclr, der.pclr + der.cbclr, GetLastError()));
            return FALSE;
        }
        DETOUR_TRACE(("CLR: %p..%p\n", der.pclr, der.pclr + der.cbclr));
    }

    return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
//
#if DETOURS_32BIT
#define DWORD_XX                        DWORD32
#define IMAGE_NT_HEADERS_XX             IMAGE_NT_HEADERS32
#define IMAGE_NT_OPTIONAL_HDR_MAGIC_XX  IMAGE_NT_OPTIONAL_HDR32_MAGIC
#define IMAGE_ORDINAL_FLAG_XX           IMAGE_ORDINAL_FLAG32
#define IMAGE_THUNK_DATAXX              IMAGE_THUNK_DATA32
#define UPDATE_IMPORTS_XX               UpdateImports32
#define DETOURS_BITS_XX                 32
#include "uimports.cpp"
#undef DETOUR_EXE_RESTORE_FIELD_XX
#undef DWORD_XX
#undef IMAGE_NT_HEADERS_XX
#undef IMAGE_NT_OPTIONAL_HDR_MAGIC_XX
#undef IMAGE_ORDINAL_FLAG_XX
#undef UPDATE_IMPORTS_XX
#endif // DETOURS_32BIT

#if DETOURS_64BIT
#define DWORD_XX                        DWORD64
#define IMAGE_NT_HEADERS_XX             IMAGE_NT_HEADERS64
#define IMAGE_NT_OPTIONAL_HDR_MAGIC_XX  IMAGE_NT_OPTIONAL_HDR64_MAGIC
#define IMAGE_ORDINAL_FLAG_XX           IMAGE_ORDINAL_FLAG64
#define IMAGE_THUNK_DATAXX              IMAGE_THUNK_DATA64
#define UPDATE_IMPORTS_XX               UpdateImports64
#define DETOURS_BITS_XX                 64
//////////////////////////////////////////////////////////////////////////////
//
//  Add DLLs to a module import table (uimports.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//
//  Note that this file is included into creatwth.cpp one or more times
//  (once for each supported module format).
//

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

// UpdateImports32 aka UpdateImports64
static BOOL UPDATE_IMPORTS_XX(HANDLE hProcess,
                              HMODULE hModule,
                              __in_ecount(nDlls) LPCSTR *plpDlls,
                              DWORD nDlls)
{
    BOOL fSucceeded = FALSE;
    DWORD cbNew = 0;

    BYTE * pbNew = NULL;
    DWORD i;
    SIZE_T cbRead;
    DWORD n;

    PBYTE pbModule = (PBYTE)hModule;

    IMAGE_DOS_HEADER idh;
    ZeroMemory(&idh, sizeof(idh));
    if (!ReadProcessMemory(hProcess, pbModule, &idh, sizeof(idh), &cbRead)
        || cbRead < sizeof(idh)) {

        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      pbModule, pbModule + sizeof(idh), GetLastError()));

      finish:
        if (pbNew != NULL) {
            delete[] pbNew;
            pbNew = NULL;
        }
        return fSucceeded;
    }

    IMAGE_NT_HEADERS_XX inh;
    ZeroMemory(&inh, sizeof(inh));

    if (!ReadProcessMemory(hProcess, pbModule + idh.e_lfanew, &inh, sizeof(inh), &cbRead)
        || cbRead < sizeof(inh)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      pbModule + idh.e_lfanew,
                      pbModule + idh.e_lfanew + sizeof(inh),
                      GetLastError()));
        goto finish;
    }

    if (inh.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC_XX) {
        DETOUR_TRACE(("Wrong size image (%04x != %04x).\n",
                      inh.OptionalHeader.Magic, IMAGE_NT_OPTIONAL_HDR_MAGIC_XX));
        SetLastError(ERROR_INVALID_BLOCK);
        goto finish;
    }

    // Zero out the bound table so loader doesn't use it instead of our new table.
    inh.BOUND_DIRECTORY.VirtualAddress = 0;
    inh.BOUND_DIRECTORY.Size = 0;

    // Find the size of the mapped file.
    DWORD dwSec = idh.e_lfanew +
        FIELD_OFFSET(IMAGE_NT_HEADERS_XX, OptionalHeader) +
        inh.FileHeader.SizeOfOptionalHeader;

    for (i = 0; i < inh.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER ish;
        ZeroMemory(&ish, sizeof(ish));

        if (!ReadProcessMemory(hProcess, pbModule + dwSec + sizeof(ish) * i, &ish,
                               sizeof(ish), &cbRead)
            || cbRead < sizeof(ish)) {

            DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p) failed: %lu\n",
                          pbModule + dwSec + sizeof(ish) * i,
                          pbModule + dwSec + sizeof(ish) * (i + 1),
                          GetLastError()));
            goto finish;
        }

        DETOUR_TRACE(("ish[%lu] : va=%08lx sr=%lu\n", i, ish.VirtualAddress, ish.SizeOfRawData));
        
        // If the linker didn't suggest an IAT in the data directories, the
        // loader will look for the section of the import directory to be used
        // for this instead. Since we put out new IMPORT_DIRECTORY outside any
        // section boundary, the loader will not find it. So we provide one
        // explicitly to avoid the search.
        //
        if (inh.IAT_DIRECTORY.VirtualAddress == 0 &&
            inh.IMPORT_DIRECTORY.VirtualAddress >= ish.VirtualAddress &&
            inh.IMPORT_DIRECTORY.VirtualAddress < ish.VirtualAddress + ish.SizeOfRawData) {

            inh.IAT_DIRECTORY.VirtualAddress = ish.VirtualAddress;
            inh.IAT_DIRECTORY.Size = ish.SizeOfRawData;
        }
    }

    if (inh.IMPORT_DIRECTORY.VirtualAddress != 0 && inh.IMPORT_DIRECTORY.Size == 0) {

        // Don't worry about changing the PE file, 
        // because the load information of the original PE header has been saved and will be restored. 
        // The change here is just for the following code to work normally

        PIMAGE_IMPORT_DESCRIPTOR pImageImport = (PIMAGE_IMPORT_DESCRIPTOR)(pbModule + inh.IMPORT_DIRECTORY.VirtualAddress);

        do {
            IMAGE_IMPORT_DESCRIPTOR ImageImport;
            if (!ReadProcessMemory(hProcess, pImageImport, &ImageImport, sizeof(ImageImport), NULL)) {
                DETOUR_TRACE(("ReadProcessMemory failed: %lu\n", GetLastError()));
                goto finish;
            }
            inh.IMPORT_DIRECTORY.Size += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            if (!ImageImport.Name) {
                break;
            }
            ++pImageImport;
        } while (TRUE);

        DWORD dwLastError = GetLastError();
        OutputDebugString(TEXT("[This PE file has an import table, but the import table size is marked as 0. This is an error.")
            TEXT("If it is not repaired, the launched program will not work properly, Detours has automatically repaired its import table size for you! ! !]\r\n"));
        if (GetLastError() != dwLastError) {
            SetLastError(dwLastError);
        }
    }

    DETOUR_TRACE(("     Imports: %p..%p\n",
                  pbModule + inh.IMPORT_DIRECTORY.VirtualAddress,
                  pbModule + inh.IMPORT_DIRECTORY.VirtualAddress +
                  inh.IMPORT_DIRECTORY.Size));

    // Calculate new import directory size.  Note that since inh is from another
    // process, inh could have been corrupted. We need to protect against
    // integer overflow in allocation calculations.
    DWORD nOldDlls = inh.IMPORT_DIRECTORY.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    DWORD obRem;
    if (DWordMult(sizeof(IMAGE_IMPORT_DESCRIPTOR), nDlls, &obRem) != S_OK) {
        DETOUR_TRACE(("too many new DLLs.\n"));
        goto finish;
    }
    DWORD obOld;
    if (DWordAdd(obRem, sizeof(IMAGE_IMPORT_DESCRIPTOR) * nOldDlls, &obOld) != S_OK) {
        DETOUR_TRACE(("DLL entries overflow.\n"));
        goto finish;
    }
    DWORD obTab = PadToDwordPtr(obOld);
    // Check for integer overflow.
    if (obTab < obOld) {
        DETOUR_TRACE(("DLL entries padding overflow.\n"));
        goto finish;
    }
    DWORD stSize;
    if (DWordMult(sizeof(DWORD_XX) * 4, nDlls, &stSize) != S_OK) {
        DETOUR_TRACE(("String table overflow.\n"));
        goto finish;
    }
    DWORD obDll;
    if (DWordAdd(obTab, stSize, &obDll) != S_OK) {
        DETOUR_TRACE(("Import table size overflow\n"));
        goto finish;
    }
    DWORD obStr = obDll;
    cbNew = obStr;
    for (n = 0; n < nDlls; n++) {
        if (DWordAdd(cbNew, PadToDword((DWORD)strlen(plpDlls[n]) + 1), &cbNew) != S_OK) {
            DETOUR_TRACE(("Overflow adding string table entry\n"));
            goto finish;
        }
    }
    pbNew = new BYTE [cbNew];
    if (pbNew == NULL) {
        DETOUR_TRACE(("new BYTE [cbNew] failed.\n"));
        goto finish;
    }
    ZeroMemory(pbNew, cbNew);

    PBYTE pbBase = pbModule;
    PBYTE pbNext = pbBase
        + inh.OptionalHeader.BaseOfCode
        + inh.OptionalHeader.SizeOfCode
        + inh.OptionalHeader.SizeOfInitializedData
        + inh.OptionalHeader.SizeOfUninitializedData;
    if (pbBase < pbNext) {
        pbBase = pbNext;
    }
    DETOUR_TRACE(("pbBase = %p\n", pbBase));

    PBYTE pbNewIid = FindAndAllocateNearBase(hProcess, pbModule, pbBase, cbNew);
    if (pbNewIid == NULL) {
        DETOUR_TRACE(("FindAndAllocateNearBase failed.\n"));
        goto finish;
    }

    PIMAGE_IMPORT_DESCRIPTOR piid = (PIMAGE_IMPORT_DESCRIPTOR)pbNew;
    IMAGE_THUNK_DATAXX *pt = NULL;

    DWORD obBase = (DWORD)(pbNewIid - pbModule);
    DWORD dwProtect = 0;

    if (inh.IMPORT_DIRECTORY.VirtualAddress != 0) {
        // Read the old import directory if it exists.
        DETOUR_TRACE(("IMPORT_DIRECTORY perms=%lx\n", dwProtect));

        if (!ReadProcessMemory(hProcess,
                               pbModule + inh.IMPORT_DIRECTORY.VirtualAddress,
                               &piid[nDlls],
                               nOldDlls * sizeof(IMAGE_IMPORT_DESCRIPTOR), &cbRead)
            || cbRead < nOldDlls * sizeof(IMAGE_IMPORT_DESCRIPTOR)) {

            DETOUR_TRACE(("ReadProcessMemory(imports) failed: %lu\n", GetLastError()));
            goto finish;
        }
    }

    for (n = 0; n < nDlls; n++) {
        HRESULT hrRet = StringCchCopyA((char*)pbNew + obStr, cbNew - obStr, plpDlls[n]);
        if (FAILED(hrRet)) {
            DETOUR_TRACE(("StringCchCopyA failed: %08lx\n", hrRet));
            goto finish;
        }

        // After copying the string, we patch up the size "??" bits if any.
        hrRet = ReplaceOptionalSizeA((char*)pbNew + obStr,
                                     cbNew - obStr,
                                     DETOURS_STRINGIFY(DETOURS_BITS_XX));
        if (FAILED(hrRet)) {
            DETOUR_TRACE(("ReplaceOptionalSizeA failed: %08lx\n", hrRet));
            goto finish;
        }

        DWORD nOffset = obTab + (sizeof(IMAGE_THUNK_DATAXX) * (4 * n));
        piid[n].OriginalFirstThunk = obBase + nOffset;
      
        // We need 2 thunks for the import table and 2 thunks for the IAT.
        // One for an ordinal import and one to mark the end of the list.
        pt = ((IMAGE_THUNK_DATAXX*)(pbNew + nOffset));
        pt[0].u1.Ordinal = IMAGE_ORDINAL_FLAG_XX + 1;
        pt[1].u1.Ordinal = 0;

        nOffset = obTab + (sizeof(IMAGE_THUNK_DATAXX) * ((4 * n) + 2));
        piid[n].FirstThunk = obBase + nOffset;
        pt = ((IMAGE_THUNK_DATAXX*)(pbNew + nOffset));
        pt[0].u1.Ordinal = IMAGE_ORDINAL_FLAG_XX + 1;
        pt[1].u1.Ordinal = 0;
        piid[n].TimeDateStamp = 0;
        piid[n].ForwarderChain = 0;
        piid[n].Name = obBase + obStr;

        obStr += PadToDword((DWORD)strlen(plpDlls[n]) + 1);
    }
    _Analysis_assume_(obStr <= cbNew);

#if 0
    for (i = 0; i < nDlls + nOldDlls; i++) {
        DETOUR_TRACE(("%8d. Look=%08x Time=%08x Fore=%08x Name=%08x Addr=%08x\n",
                      i,
                      piid[i].OriginalFirstThunk,
                      piid[i].TimeDateStamp,
                      piid[i].ForwarderChain,
                      piid[i].Name,
                      piid[i].FirstThunk));
        if (piid[i].OriginalFirstThunk == 0 && piid[i].FirstThunk == 0) {
            break;
        }
    }
#endif

    if (!WriteProcessMemory(hProcess, pbNewIid, pbNew, obStr, NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(iid) failed: %lu\n", GetLastError()));
        goto finish;
    }

    DETOUR_TRACE(("obBaseBef = %08lx..%08lx\n",
                  inh.IMPORT_DIRECTORY.VirtualAddress,
                  inh.IMPORT_DIRECTORY.VirtualAddress + inh.IMPORT_DIRECTORY.Size));
    DETOUR_TRACE(("obBaseAft = %08lx..%08lx\n", obBase, obBase + obStr));

    // In this case the file didn't have an import directory in first place,
    // so we couldn't fix the missing IAT above. We still need to explicitly
    // provide an IAT to prevent to loader from looking for one.
    //
    if (inh.IAT_DIRECTORY.VirtualAddress == 0) {
        inh.IAT_DIRECTORY.VirtualAddress = obBase;
        inh.IAT_DIRECTORY.Size = cbNew;
    }

    inh.IMPORT_DIRECTORY.VirtualAddress = obBase;
    inh.IMPORT_DIRECTORY.Size = cbNew;

    /////////////////////// Update the NT header for the new import directory.
    //
    if (!DetourVirtualProtectSameExecuteEx(hProcess, pbModule, inh.OptionalHeader.SizeOfHeaders,
                                           PAGE_EXECUTE_READWRITE, &dwProtect)) {
        DETOUR_TRACE(("VirtualProtectEx(inh) write failed: %lu\n", GetLastError()));
        goto finish;
    }

    inh.OptionalHeader.CheckSum = 0;

    if (!WriteProcessMemory(hProcess, pbModule, &idh, sizeof(idh), NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(idh) failed: %lu\n", GetLastError()));
        goto finish;
    }
    DETOUR_TRACE(("WriteProcessMemory(idh:%p..%p)\n", pbModule, pbModule + sizeof(idh)));

    if (!WriteProcessMemory(hProcess, pbModule + idh.e_lfanew, &inh, sizeof(inh), NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(inh) failed: %lu\n", GetLastError()));
        goto finish;
    }
    DETOUR_TRACE(("WriteProcessMemory(inh:%p..%p)\n",
                  pbModule + idh.e_lfanew,
                  pbModule + idh.e_lfanew + sizeof(inh)));

    if (!VirtualProtectEx(hProcess, pbModule, inh.OptionalHeader.SizeOfHeaders,
                          dwProtect, &dwProtect)) {
        DETOUR_TRACE(("VirtualProtectEx(idh) restore failed: %lu\n", GetLastError()));
        goto finish;
    }

    fSucceeded = TRUE;
    goto finish;
}
#undef DETOUR_EXE_RESTORE_FIELD_XX
#undef DWORD_XX
#undef IMAGE_NT_HEADERS_XX
#undef IMAGE_NT_OPTIONAL_HDR_MAGIC_XX
#undef IMAGE_ORDINAL_FLAG_XX
#undef UPDATE_IMPORTS_XX
#endif // DETOURS_64BIT

//////////////////////////////////////////////////////////////////////////////
//
#if DETOURS_64BIT

C_ASSERT(sizeof(IMAGE_NT_HEADERS64) == sizeof(IMAGE_NT_HEADERS32) + 16);

static BOOL UpdateFrom32To64(HANDLE hProcess, HMODULE hModule, WORD machine,
                             DETOUR_EXE_RESTORE& der)
{
    IMAGE_DOS_HEADER idh;
    IMAGE_NT_HEADERS32 inh32;
    IMAGE_NT_HEADERS64 inh64;
    IMAGE_SECTION_HEADER sects[32];
    PBYTE pbModule = (PBYTE)hModule;
    DWORD n;

    ZeroMemory(&inh32, sizeof(inh32));
    ZeroMemory(&inh64, sizeof(inh64));
    ZeroMemory(sects, sizeof(sects));

    DETOUR_TRACE(("UpdateFrom32To64(%04x)\n", machine));
    //////////////////////////////////////////////////////// Read old headers.
    //
    if (!ReadProcessMemory(hProcess, pbModule, &idh, sizeof(idh), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p) failed: %lu\n",
                      pbModule, pbModule + sizeof(idh), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(idh@%p..%p)\n",
                  pbModule, pbModule + sizeof(idh)));

    PBYTE pnh = pbModule + idh.e_lfanew;
    if (!ReadProcessMemory(hProcess, pnh, &inh32, sizeof(inh32), NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p) failed: %lu\n",
                      pnh, pnh + sizeof(inh32), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(inh@%p..%p)\n", pnh, pnh + sizeof(inh32)));

    if (inh32.FileHeader.NumberOfSections > (sizeof(sects)/sizeof(sects[0]))) {
        return FALSE;
    }

    PBYTE psects = pnh +
        FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
        inh32.FileHeader.SizeOfOptionalHeader;
    ULONG cb = inh32.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!ReadProcessMemory(hProcess, psects, &sects, cb, NULL)) {
        DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p) failed: %lu\n",
                      psects, psects + cb, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("ReadProcessMemory(ish@%p..%p)\n", psects, psects + cb));

    ////////////////////////////////////////////////////////// Convert header.
    //
    inh64.Signature = inh32.Signature;
    inh64.FileHeader = inh32.FileHeader;
    inh64.FileHeader.Machine = machine;
    inh64.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);

    inh64.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    inh64.OptionalHeader.MajorLinkerVersion = inh32.OptionalHeader.MajorLinkerVersion;
    inh64.OptionalHeader.MinorLinkerVersion = inh32.OptionalHeader.MinorLinkerVersion;
    inh64.OptionalHeader.SizeOfCode = inh32.OptionalHeader.SizeOfCode;
    inh64.OptionalHeader.SizeOfInitializedData = inh32.OptionalHeader.SizeOfInitializedData;
    inh64.OptionalHeader.SizeOfUninitializedData = inh32.OptionalHeader.SizeOfUninitializedData;
    inh64.OptionalHeader.AddressOfEntryPoint = inh32.OptionalHeader.AddressOfEntryPoint;
    inh64.OptionalHeader.BaseOfCode = inh32.OptionalHeader.BaseOfCode;
    inh64.OptionalHeader.ImageBase = inh32.OptionalHeader.ImageBase;
    inh64.OptionalHeader.SectionAlignment = inh32.OptionalHeader.SectionAlignment;
    inh64.OptionalHeader.FileAlignment = inh32.OptionalHeader.FileAlignment;
    inh64.OptionalHeader.MajorOperatingSystemVersion
        = inh32.OptionalHeader.MajorOperatingSystemVersion;
    inh64.OptionalHeader.MinorOperatingSystemVersion
        = inh32.OptionalHeader.MinorOperatingSystemVersion;
    inh64.OptionalHeader.MajorImageVersion = inh32.OptionalHeader.MajorImageVersion;
    inh64.OptionalHeader.MinorImageVersion = inh32.OptionalHeader.MinorImageVersion;
    inh64.OptionalHeader.MajorSubsystemVersion = inh32.OptionalHeader.MajorSubsystemVersion;
    inh64.OptionalHeader.MinorSubsystemVersion = inh32.OptionalHeader.MinorSubsystemVersion;
    inh64.OptionalHeader.Win32VersionValue = inh32.OptionalHeader.Win32VersionValue;
    inh64.OptionalHeader.SizeOfImage = inh32.OptionalHeader.SizeOfImage;
    inh64.OptionalHeader.SizeOfHeaders = inh32.OptionalHeader.SizeOfHeaders;
    inh64.OptionalHeader.CheckSum = inh32.OptionalHeader.CheckSum;
    inh64.OptionalHeader.Subsystem = inh32.OptionalHeader.Subsystem;
    inh64.OptionalHeader.DllCharacteristics = inh32.OptionalHeader.DllCharacteristics;
    inh64.OptionalHeader.SizeOfStackReserve = inh32.OptionalHeader.SizeOfStackReserve;
    inh64.OptionalHeader.SizeOfStackCommit = inh32.OptionalHeader.SizeOfStackCommit;
    inh64.OptionalHeader.SizeOfHeapReserve = inh32.OptionalHeader.SizeOfHeapReserve;
    inh64.OptionalHeader.SizeOfHeapCommit = inh32.OptionalHeader.SizeOfHeapCommit;
    inh64.OptionalHeader.LoaderFlags = inh32.OptionalHeader.LoaderFlags;
    inh64.OptionalHeader.NumberOfRvaAndSizes = inh32.OptionalHeader.NumberOfRvaAndSizes;
    for (n = 0; n < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; n++) {
        inh64.OptionalHeader.DataDirectory[n] = inh32.OptionalHeader.DataDirectory[n];
    }

    /////////////////////////////////////////////////////// Write new headers.
    //
    DWORD dwProtect = 0;
    if (!DetourVirtualProtectSameExecuteEx(hProcess, pbModule, inh64.OptionalHeader.SizeOfHeaders,
                                           PAGE_EXECUTE_READWRITE, &dwProtect)) {
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pnh, &inh64, sizeof(inh64), NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p) failed: %lu\n",
                      pnh, pnh + sizeof(inh64), GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p)\n", pnh, pnh + sizeof(inh64)));

    psects = pnh +
        FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
        inh64.FileHeader.SizeOfOptionalHeader;
    cb = inh64.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!WriteProcessMemory(hProcess, psects, &sects, cb, NULL)) {
        DETOUR_TRACE(("WriteProcessMemory(ish@%p..%p) failed: %lu\n",
                      psects, psects + cb, GetLastError()));
        return FALSE;
    }
    DETOUR_TRACE(("WriteProcessMemory(ish@%p..%p)\n", psects, psects + cb));

    // Record the updated headers.
    if (!RecordExeRestore(hProcess, hModule, der)) {
        return FALSE;
    }

    // Remove the import table.
    if (der.pclr != NULL && (der.clr.Flags & COMIMAGE_FLAGS_ILONLY)) {
        inh64.IMPORT_DIRECTORY.VirtualAddress = 0;
        inh64.IMPORT_DIRECTORY.Size = 0;

        if (!WriteProcessMemory(hProcess, pnh, &inh64, sizeof(inh64), NULL)) {
            DETOUR_TRACE(("WriteProcessMemory(inh@%p..%p) failed: %lu\n",
                          pnh, pnh + sizeof(inh64), GetLastError()));
            return FALSE;
        }
    }

    DWORD dwOld = 0;
    if (!VirtualProtectEx(hProcess, pbModule, inh64.OptionalHeader.SizeOfHeaders,
                          dwProtect, &dwOld)) {
        return FALSE;
    }

    return TRUE;
}
#endif // DETOURS_64BIT

typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);

static BOOL IsWow64ProcessHelper(HANDLE hProcess,
                                 PBOOL Wow64Process)
{
#ifdef _X86_
    if (Wow64Process == NULL) {
        return FALSE;
    }

    // IsWow64Process is not available on all supported versions of Windows.
    //
    HMODULE hKernel32 = LoadLibraryW(L"KERNEL32.DLL");
    if (hKernel32 == NULL) {
        DETOUR_TRACE(("LoadLibraryW failed: %lu\n", GetLastError()));
        return FALSE;
    }

    LPFN_ISWOW64PROCESS pfnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
        hKernel32, "IsWow64Process");

    if (pfnIsWow64Process == NULL) {
        DETOUR_TRACE(("GetProcAddress failed: %lu\n", GetLastError()));
        return FALSE;
    }
    return pfnIsWow64Process(hProcess, Wow64Process);
#else
    return IsWow64Process(hProcess, Wow64Process);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
BOOL WINAPI DetourUpdateProcessWithDll(_In_ HANDLE hProcess,
                                       _In_reads_(nDlls) LPCSTR *rlpDlls,
                                       _In_ DWORD nDlls)
{
    // Find the next memory region that contains a mapped PE image.
    //
    BOOL bIs32BitProcess;
    BOOL bIs64BitOS = FALSE;
    HMODULE hModule = NULL;
    HMODULE hLast = NULL;

    DETOUR_TRACE(("DetourUpdateProcessWithDll(%p,dlls=%lu)\n", hProcess, nDlls));

    for (;;) {
        IMAGE_NT_HEADERS32 inh;

        if ((hLast = EnumerateModulesInProcess(hProcess, hLast, &inh, NULL)) == NULL) {
            break;
        }

        DETOUR_TRACE(("%p  machine=%04x magic=%04x\n",
                      hLast, inh.FileHeader.Machine, inh.OptionalHeader.Magic));

        if ((inh.FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) {
            hModule = hLast;
            DETOUR_TRACE(("%p  Found EXE\n", hLast));
        }
    }

    if (hModule == NULL) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    // Determine if the target process is 32bit or 64bit. This is a two-stop process:
    //
    // 1. First, determine if we're running on a 64bit operating system.
    //   - If we're running 64bit code (i.e. _WIN64 is defined), this is trivially true.
    //   - If we're running 32bit code (i.e. _WIN64 is not defined), test if
    //   we're running under Wow64. If so, it implies that the operating system
    //   is 64bit.
    //
#ifdef _WIN64
    bIs64BitOS = TRUE;
#else
    if (!IsWow64ProcessHelper(GetCurrentProcess(), &bIs64BitOS)) {
        return FALSE;
    }
#endif

    // 2. With the operating system bitness known, we can now consider the target process:
    //   - If we're running on a 64bit OS, the target process is 32bit in case
    //   it is running under Wow64. Otherwise, it's 64bit, running natively
    //   (without Wow64).
    //   - If we're running on a 32bit OS, the target process must be 32bit, too.
    //
    if (bIs64BitOS) {
        if (!IsWow64ProcessHelper(hProcess, &bIs32BitProcess)) {
            return FALSE;
        }
    } else {
        bIs32BitProcess = TRUE;
    }

    DETOUR_TRACE(("    32BitProcess=%d\n", bIs32BitProcess));

    return DetourUpdateProcessWithDllEx(hProcess,
                                        hModule,
                                        bIs32BitProcess,
                                        rlpDlls,
                                        nDlls);
}

BOOL WINAPI DetourUpdateProcessWithDllEx(_In_ HANDLE hProcess,
                                         _In_ HMODULE hModule,
                                         _In_ BOOL bIs32BitProcess,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_ DWORD nDlls)
{
    // Find the next memory region that contains a mapped PE image.
    //
    BOOL bIs32BitExe = FALSE;

    DETOUR_TRACE(("DetourUpdateProcessWithDllEx(%p,%p,dlls=%lu)\n", hProcess, hModule, nDlls));

    IMAGE_NT_HEADERS32 inh;

    if (hModule == NULL || !LoadNtHeaderFromProcess(hProcess, hModule, &inh)) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    if (inh.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
        && inh.FileHeader.Machine != 0) {

        bIs32BitExe = TRUE;
    }

    DETOUR_TRACE(("    32BitExe=%d\n", bIs32BitExe));

    if (hModule == NULL) {
        SetLastError(ERROR_INVALID_OPERATION);
        return FALSE;
    }

    // Save the various headers for DetourRestoreAfterWith.
    //
    DETOUR_EXE_RESTORE der;

    if (!RecordExeRestore(hProcess, hModule, der)) {
        return FALSE;
    }

#if defined(DETOURS_64BIT)
    // Try to convert a neutral 32-bit managed binary to a 64-bit managed binary.
    if (bIs32BitExe && !bIs32BitProcess) {
        if (!der.pclr                       // Native binary
            || (der.clr.Flags & COMIMAGE_FLAGS_ILONLY) == 0     // Or mixed-mode MSIL
            || (der.clr.Flags & COMIMAGE_FLAGS_32BITREQUIRED) != 0) {  // Or 32BIT Required MSIL

            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        if (!UpdateFrom32To64(hProcess, hModule,
#if defined(DETOURS_X64)
                              IMAGE_FILE_MACHINE_AMD64,
#elif defined(DETOURS_IA64)
                              IMAGE_FILE_MACHINE_IA64,
#elif defined(DETOURS_ARM64)
                              IMAGE_FILE_MACHINE_ARM64,
#else
#error Must define one of DETOURS_X64 or DETOURS_IA64 or DETOURS_ARM64 on 64-bit.
#endif
                              der)) {
            return FALSE;
        }
        bIs32BitExe = FALSE;
    }
#endif // DETOURS_64BIT

    // Now decide if we can insert the detour.

#if defined(DETOURS_32BIT)
    if (bIs32BitProcess) {
        // 32-bit native or 32-bit managed process on any platform.
        if (!UpdateImports32(hProcess, hModule, rlpDlls, nDlls)) {
            return FALSE;
        }
    }
    else {
        // 64-bit native or 64-bit managed process.
        //
        // Can't detour a 64-bit process with 32-bit code.
        // Note: This happens for 32-bit PE binaries containing only
        // manage code that have been marked as 64-bit ready.
        //
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
#elif defined(DETOURS_64BIT)
    if (bIs32BitProcess || bIs32BitExe) {
        // Can't detour a 32-bit process with 64-bit code.
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    else {
        // 64-bit native or 64-bit managed process on any platform.
        if (!UpdateImports64(hProcess, hModule, rlpDlls, nDlls)) {
            return FALSE;
        }
    }
#else
#pragma Must define one of DETOURS_32BIT or DETOURS_64BIT.
#endif // DETOURS_64BIT

    /////////////////////////////////////////////////// Update the CLR header.
    //
    if (der.pclr != NULL) {
        DETOUR_CLR_HEADER clr;
        CopyMemory(&clr, &der.clr, sizeof(clr));
        clr.Flags &= ~COMIMAGE_FLAGS_ILONLY;    // Clear the IL_ONLY flag.

        DWORD dwProtect;
        if (!DetourVirtualProtectSameExecuteEx(hProcess, der.pclr, sizeof(clr), PAGE_READWRITE, &dwProtect)) {
            DETOUR_TRACE(("VirtualProtectEx(clr) write failed: %lu\n", GetLastError()));
            return FALSE;
        }

        if (!WriteProcessMemory(hProcess, der.pclr, &clr, sizeof(clr), NULL)) {
            DETOUR_TRACE(("WriteProcessMemory(clr) failed: %lu\n", GetLastError()));
            return FALSE;
        }

        if (!VirtualProtectEx(hProcess, der.pclr, sizeof(clr), dwProtect, &dwProtect)) {
            DETOUR_TRACE(("VirtualProtectEx(clr) restore failed: %lu\n", GetLastError()));
            return FALSE;
        }
        DETOUR_TRACE(("CLR: %p..%p\n", der.pclr, der.pclr + der.cbclr));

#if DETOURS_64BIT
        if (der.clr.Flags & COMIMAGE_FLAGS_32BITREQUIRED) { // Is the 32BIT Required Flag set?
            // X64 never gets here because the process appears as a WOW64 process.
            // However, on IA64, it doesn't appear to be a WOW process.
            DETOUR_TRACE(("CLR Requires 32-bit\n"));
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
#endif // DETOURS_64BIT
    }

    //////////////////////////////// Save the undo data to the target process.
    //
    if (!DetourCopyPayloadToProcess(hProcess, DETOUR_EXE_RESTORE_GUID, &der, sizeof(der))) {
        DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
        return FALSE;
    }
    return TRUE;
}

//////////////////////////////////////////////////////////////////////////////
//
BOOL WINAPI DetourCreateProcessWithDllA(_In_opt_ LPCSTR lpApplicationName,
                                        _Inout_opt_ LPSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOA lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    DWORD dwMyCreationFlags = (dwCreationFlags | CREATE_SUSPENDED);
    PROCESS_INFORMATION pi;
    BOOL fResult = FALSE;

    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    fResult = pfCreateProcessA(lpApplicationName,
                               lpCommandLine,
                               lpProcessAttributes,
                               lpThreadAttributes,
                               bInheritHandles,
                               dwMyCreationFlags,
                               lpEnvironment,
                               lpCurrentDirectory,
                               lpStartupInfo,
                               &pi);

    if (lpProcessInformation != NULL) {
        CopyMemory(lpProcessInformation, &pi, sizeof(pi));
    }

    if (!fResult) {
        return FALSE;
    }

    LPCSTR rlpDlls[2];
    DWORD nDlls = 0;
    if (lpDllName != NULL) {
        rlpDlls[nDlls++] = lpDllName;
    }

    if (!DetourUpdateProcessWithDll(pi.hProcess, rlpDlls, nDlls)) {
        TerminateProcess(pi.hProcess, ~0u);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(pi.hThread);
    }
    return TRUE;
}


BOOL WINAPI DetourCreateProcessWithDllW(_In_opt_ LPCWSTR lpApplicationName,
                                        _Inout_opt_ LPWSTR lpCommandLine,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                        _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                        _In_ BOOL bInheritHandles,
                                        _In_ DWORD dwCreationFlags,
                                        _In_opt_ LPVOID lpEnvironment,
                                        _In_opt_ LPCWSTR lpCurrentDirectory,
                                        _In_ LPSTARTUPINFOW lpStartupInfo,
                                        _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                        _In_ LPCSTR lpDllName,
                                        _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    DWORD dwMyCreationFlags = (dwCreationFlags | CREATE_SUSPENDED);
    PROCESS_INFORMATION pi;

    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    BOOL fResult = pfCreateProcessW(lpApplicationName,
                                    lpCommandLine,
                                    lpProcessAttributes,
                                    lpThreadAttributes,
                                    bInheritHandles,
                                    dwMyCreationFlags,
                                    lpEnvironment,
                                    lpCurrentDirectory,
                                    lpStartupInfo,
                                    &pi);

    if (lpProcessInformation) {
        CopyMemory(lpProcessInformation, &pi, sizeof(pi));
    }

    if (!fResult) {
        return FALSE;
    }

    LPCSTR rlpDlls[2];
    DWORD nDlls = 0;
    if (lpDllName != NULL) {
        rlpDlls[nDlls++] = lpDllName;
    }

    if (!DetourUpdateProcessWithDll(pi.hProcess, rlpDlls, nDlls)) {
        TerminateProcess(pi.hProcess, ~0u);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(pi.hThread);
    }
    return TRUE;
}

BOOL WINAPI DetourCopyPayloadToProcess(_In_ HANDLE hProcess,
                                       _In_ REFGUID rguid,
                                       _In_reads_bytes_(cbData) LPCVOID pvData,
                                       _In_ DWORD cbData)
{
    return DetourCopyPayloadToProcessEx(hProcess, rguid, pvData, cbData) != NULL;
}

_Success_(return != NULL)
PVOID WINAPI DetourCopyPayloadToProcessEx(_In_ HANDLE hProcess,
                                          _In_ REFGUID rguid,
                                          _In_reads_bytes_(cbData) LPCVOID pvData,
                                          _In_ DWORD cbData)
{
    if (hProcess == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    DWORD cbTotal = (sizeof(IMAGE_DOS_HEADER) +
                     sizeof(IMAGE_NT_HEADERS) +
                     sizeof(IMAGE_SECTION_HEADER) +
                     sizeof(DETOUR_SECTION_HEADER) +
                     sizeof(DETOUR_SECTION_RECORD) +
                     cbData);

    PBYTE pbBase = (PBYTE)VirtualAllocEx(hProcess, NULL, cbTotal,
                                         MEM_COMMIT, PAGE_READWRITE);
    if (pbBase == NULL) {
        DETOUR_TRACE(("VirtualAllocEx(%lu) failed: %lu\n", cbTotal, GetLastError()));
        return NULL;
    }

    // As you can see in the following code,
    // the memory layout of the payload range "[pbBase, pbBase+cbTotal]" is a PE executable file,
    // so DetourFreePayload can use "DetourGetContainingModule(Payload pointer)" to get the above "pbBase" pointer,
    // pbBase: the memory block allocated by VirtualAllocEx will be released in DetourFreePayload by VirtualFree.

    PBYTE pbTarget = pbBase;
    IMAGE_DOS_HEADER idh;
    IMAGE_NT_HEADERS inh;
    IMAGE_SECTION_HEADER ish;
    DETOUR_SECTION_HEADER dsh;
    DETOUR_SECTION_RECORD dsr;
    SIZE_T cbWrote = 0;

    ZeroMemory(&idh, sizeof(idh));
    idh.e_magic = IMAGE_DOS_SIGNATURE;
    idh.e_lfanew = sizeof(idh);
    if (!WriteProcessMemory(hProcess, pbTarget, &idh, sizeof(idh), &cbWrote) ||
        cbWrote != sizeof(idh)) {
        DETOUR_TRACE(("WriteProcessMemory(idh) failed: %lu\n", GetLastError()));
        return NULL;
    }
    pbTarget += sizeof(idh);

    ZeroMemory(&inh, sizeof(inh));
    inh.Signature = IMAGE_NT_SIGNATURE;
    inh.FileHeader.SizeOfOptionalHeader = sizeof(inh.OptionalHeader);
    inh.FileHeader.Characteristics = IMAGE_FILE_DLL;
    inh.FileHeader.NumberOfSections = 1;
    inh.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
    if (!WriteProcessMemory(hProcess, pbTarget, &inh, sizeof(inh), &cbWrote) ||
        cbWrote != sizeof(inh)) {
        return NULL;
    }
    pbTarget += sizeof(inh);

    ZeroMemory(&ish, sizeof(ish));
    memcpy(ish.Name, ".detour", sizeof(ish.Name));
    ish.VirtualAddress = (DWORD)((pbTarget + sizeof(ish)) - pbBase);
    ish.SizeOfRawData = (sizeof(DETOUR_SECTION_HEADER) +
                         sizeof(DETOUR_SECTION_RECORD) +
                         cbData);
    if (!WriteProcessMemory(hProcess, pbTarget, &ish, sizeof(ish), &cbWrote) ||
        cbWrote != sizeof(ish)) {
        return NULL;
    }
    pbTarget += sizeof(ish);

    ZeroMemory(&dsh, sizeof(dsh));
    dsh.cbHeaderSize = sizeof(dsh);
    dsh.nSignature = DETOUR_SECTION_HEADER_SIGNATURE;
    dsh.nDataOffset = sizeof(DETOUR_SECTION_HEADER);
    dsh.cbDataSize = (sizeof(DETOUR_SECTION_HEADER) +
                      sizeof(DETOUR_SECTION_RECORD) +
                      cbData);
    if (!WriteProcessMemory(hProcess, pbTarget, &dsh, sizeof(dsh), &cbWrote) ||
        cbWrote != sizeof(dsh)) {
        return NULL;
    }
    pbTarget += sizeof(dsh);

    ZeroMemory(&dsr, sizeof(dsr));
    dsr.cbBytes = cbData + sizeof(DETOUR_SECTION_RECORD);
    dsr.nReserved = 0;
    dsr.guid = rguid;
    if (!WriteProcessMemory(hProcess, pbTarget, &dsr, sizeof(dsr), &cbWrote) ||
        cbWrote != sizeof(dsr)) {
        return NULL;
    }
    pbTarget += sizeof(dsr);

    if (!WriteProcessMemory(hProcess, pbTarget, pvData, cbData, &cbWrote) ||
        cbWrote != cbData) {
        return NULL;
    }

    DETOUR_TRACE(("Copied %lu byte payload into target process at %p\n",
                  cbData, pbTarget));
    
    SetLastError(NO_ERROR);
    return pbTarget;
}

static BOOL s_fSearchedForHelper = FALSE;
static PDETOUR_EXE_HELPER s_pHelper = NULL;

VOID CALLBACK DetourFinishHelperProcess(_In_ HWND,
                                        _In_ HINSTANCE,
                                        _In_ LPSTR,
                                        _In_ INT)
{
    LPCSTR * rlpDlls = NULL;
    DWORD Result = 9900;
    DWORD cOffset = 0;
    DWORD cSize = 0;
    HANDLE hProcess = NULL;

    if (s_pHelper == NULL) {
        DETOUR_TRACE(("DetourFinishHelperProcess called with s_pHelper = NULL.\n"));
        Result = 9905;
        goto Cleanup;
    }

    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, s_pHelper->pid);
    if (hProcess == NULL) {
        DETOUR_TRACE(("OpenProcess(pid=%lu) failed: %lu\n",
                      s_pHelper->pid, GetLastError()));
        Result = 9901;
        goto Cleanup;
    }

    rlpDlls = new NOTHROW LPCSTR [s_pHelper->nDlls];
    cSize = s_pHelper->cb - sizeof(DETOUR_EXE_HELPER);
    for (DWORD n = 0; n < s_pHelper->nDlls; n++) {
        size_t cchDest = 0;
        HRESULT hr = StringCchLengthA(&s_pHelper->rDlls[cOffset], cSize - cOffset, &cchDest);
        if (!SUCCEEDED(hr)) {
            Result = 9902;
            goto Cleanup;
        }

        rlpDlls[n] = &s_pHelper->rDlls[cOffset];
        cOffset += (DWORD)cchDest + 1;
    }

    if (!DetourUpdateProcessWithDll(hProcess, rlpDlls, s_pHelper->nDlls)) {
        DETOUR_TRACE(("DetourUpdateProcessWithDll(pid=%lu) failed: %lu\n",
                      s_pHelper->pid, GetLastError()));
        Result = 9903;
        goto Cleanup;
    }
    Result = 0;

  Cleanup:
    if (rlpDlls != NULL) {
        delete[] rlpDlls;
        rlpDlls = NULL;
    }

    // Note: s_pHelper is allocated as part of injecting the payload in DetourCopyPayloadToProcess(..),
    // it's a fake section and not data allocated by the system PE loader.

    // Delete the payload after execution to release the memory occupied by it
    if (s_pHelper != NULL) {
        DetourFreePayload(s_pHelper);
        s_pHelper = NULL;
    }

    ExitProcess(Result);
}

BOOL WINAPI DetourIsHelperProcess(VOID)
{
    PVOID pvData;
    DWORD cbData;

    if (s_fSearchedForHelper) {
        return (s_pHelper != NULL);
    }

    s_fSearchedForHelper = TRUE;
    pvData = DetourFindPayloadEx(DETOUR_EXE_HELPER_GUID, &cbData);

    if (pvData == NULL || cbData < sizeof(DETOUR_EXE_HELPER)) {
        return FALSE;
    }

    s_pHelper = (PDETOUR_EXE_HELPER)pvData;
    if (s_pHelper->cb < sizeof(*s_pHelper)) {
        s_pHelper = NULL;
        return FALSE;
    }

    return TRUE;
}

static
BOOL WINAPI AllocExeHelper(_Out_ PDETOUR_EXE_HELPER *pHelper,
                           _In_ DWORD dwTargetPid,
                           _In_ DWORD nDlls,
                           _In_reads_(nDlls) LPCSTR *rlpDlls)
{
    PDETOUR_EXE_HELPER Helper = NULL;
    BOOL Result = FALSE;
    _Field_range_(0, cSize - 4) DWORD cOffset = 0;
    DWORD cSize = 4;

    if (pHelper == NULL) {
        goto Cleanup;
    }
    *pHelper = NULL;

    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }

    for (DWORD n = 0; n < nDlls; n++) {
        HRESULT hr;
        size_t cchDest = 0;

        hr = StringCchLengthA(rlpDlls[n], 4096, &cchDest);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

        cSize += (DWORD)cchDest + 1;
    }

    Helper = (PDETOUR_EXE_HELPER) new NOTHROW BYTE[sizeof(DETOUR_EXE_HELPER) + cSize];
    if (Helper == NULL) {
        goto Cleanup;
    }

    Helper->cb = sizeof(DETOUR_EXE_HELPER) + cSize;
    Helper->pid = dwTargetPid;
    Helper->nDlls = nDlls;

    for (DWORD n = 0; n < nDlls; n++) {
        HRESULT hr;
        size_t cchDest = 0;

        if (cOffset > 0x10000 || cSize > 0x10000 || cOffset + 2 >= cSize) {
            goto Cleanup;
        }

        if (cOffset + 2 >= cSize || cOffset + 65536 < cSize) {
            goto Cleanup;
        }

        _Analysis_assume_(cOffset + 1 < cSize);
        _Analysis_assume_(cOffset < 0x10000);
        _Analysis_assume_(cSize < 0x10000);

        PCHAR psz = &Helper->rDlls[cOffset];

        hr = StringCchCopyA(psz, cSize - cOffset, rlpDlls[n]);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

// REVIEW 28020 The expression '1<=_Param_(2)& &_Param_(2)<=2147483647' is not true at this call.
// REVIEW 28313 Analysis will not proceed past this point because of annotation evaluation. The annotation expression *_Param_(3)<_Param_(2)&&*_Param_(3)<=stringLength$(_Param_(1)) cannot be true under any assumptions at this point in the program.
#pragma warning(suppress:28020 28313)
        hr = StringCchLengthA(psz, cSize - cOffset, &cchDest);
        if (!SUCCEEDED(hr)) {
            goto Cleanup;
        }

        // Replace "32." with "64." or "64." with "32."

        for (DWORD c = (DWORD)cchDest + 1; c > 3; c--) {
#if DETOURS_32BIT
            if (psz[c - 3] == '3' && psz[c - 2] == '2' && psz[c - 1] == '.') {
                psz[c - 3] = '6'; psz[c - 2] = '4';
                break;
            }
#else
            if (psz[c - 3] == '6' && psz[c - 2] == '4' && psz[c - 1] == '.') {
                psz[c - 3] = '3'; psz[c - 2] = '2';
                break;
            }
#endif
        }

        cOffset += (DWORD)cchDest + 1;
    }

    *pHelper = Helper;
    Helper = NULL;
    Result = TRUE;

  Cleanup:
    if (Helper != NULL) {
        delete[] (PBYTE)Helper;
        Helper = NULL;
    }
    return Result;
}

static
VOID WINAPI FreeExeHelper(PDETOUR_EXE_HELPER *pHelper)
{
    if (*pHelper != NULL) {
        delete[] (PBYTE)*pHelper;
        *pHelper = NULL;
    }
}

BOOL WINAPI DetourProcessViaHelperA(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    return DetourProcessViaHelperDllsA(dwTargetPid, 1, &lpDllName, pfCreateProcessA);
}


BOOL WINAPI DetourProcessViaHelperDllsA(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    BOOL Result = FALSE;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    CHAR szExe[MAX_PATH];
    CHAR szCommand[MAX_PATH];
    PDETOUR_EXE_HELPER helper = NULL;
    HRESULT hr;
    DWORD nLen = GetEnvironmentVariableA("WINDIR", szExe, ARRAYSIZE(szExe));

    DETOUR_TRACE(("DetourProcessViaHelperDlls(pid=%lu,dlls=%lu)\n", dwTargetPid, nDlls));
    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }
    if (!AllocExeHelper(&helper, dwTargetPid, nDlls, rlpDlls)) {
        goto Cleanup;
    }

    if (nLen == 0 || nLen >= ARRAYSIZE(szExe)) {
        goto Cleanup;
    }

#if DETOURS_OPTION_BITS
#if DETOURS_32BIT
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\sysnative\\rundll32.exe");
#else // !DETOURS_32BIT
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\syswow64\\rundll32.exe");
#endif // !DETOURS_32BIT
#else // DETOURS_OPTIONS_BITS
    hr = StringCchCatA(szExe, ARRAYSIZE(szExe), "\\system32\\rundll32.exe");
#endif // DETOURS_OPTIONS_BITS
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    //for East Asia languages and so on, like Chinese, print format with "%hs" can not work fine before user call _tsetlocale(LC_ALL,_T(".ACP"));
    //so we can't use "%hs" in format string, because the dll that contain this code would inject to any process, even not call _tsetlocale(LC_ALL,_T(".ACP")) before
    hr = StringCchPrintfA(szCommand, ARRAYSIZE(szCommand),
                          "rundll32.exe \"%s\",#1", &helper->rDlls[0]);
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    DETOUR_TRACE(("DetourProcessViaHelperDlls(\"%hs\", \"%hs\")\n", szExe, szCommand));
    if (pfCreateProcessA(szExe, szCommand, NULL, NULL, FALSE, CREATE_SUSPENDED,
                         NULL, NULL, &si, &pi)) {

        if (!DetourCopyPayloadToProcess(pi.hProcess,
                                        DETOUR_EXE_HELPER_GUID,
                                        helper, helper->cb)) {
            DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
            TerminateProcess(pi.hProcess, ~0u);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            goto Cleanup;
        }

        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD dwResult = 500;
        GetExitCodeProcess(pi.hProcess, &dwResult);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (dwResult != 0) {
            DETOUR_TRACE(("Rundll32.exe failed: result=%lu\n", dwResult));
            goto Cleanup;
        }
        Result = TRUE;
    }
    else {
        DETOUR_TRACE(("CreateProcess failed: %lu\n", GetLastError()));
        goto Cleanup;
    }

  Cleanup:
    FreeExeHelper(&helper);
    return Result;
}

BOOL WINAPI DetourProcessViaHelperW(_In_ DWORD dwTargetPid,
                                    _In_ LPCSTR lpDllName,
                                    _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    return DetourProcessViaHelperDllsW(dwTargetPid, 1, &lpDllName, pfCreateProcessW);
}

BOOL WINAPI DetourProcessViaHelperDllsW(_In_ DWORD dwTargetPid,
                                        _In_ DWORD nDlls,
                                        _In_reads_(nDlls) LPCSTR *rlpDlls,
                                        _In_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    BOOL Result = FALSE;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    WCHAR szExe[MAX_PATH];
    WCHAR szCommand[MAX_PATH];
    PDETOUR_EXE_HELPER helper = NULL;
    HRESULT hr;
    WCHAR szDllName[MAX_PATH];
    int cchWrittenWideChar;
    DWORD nLen = GetEnvironmentVariableW(L"WINDIR", szExe, ARRAYSIZE(szExe));

    DETOUR_TRACE(("DetourProcessViaHelperDlls(pid=%lu,dlls=%lu)\n", dwTargetPid, nDlls));
    if (nDlls < 1 || nDlls > 4096) {
        SetLastError(ERROR_INVALID_PARAMETER);
        goto Cleanup;
    }
    if (!AllocExeHelper(&helper, dwTargetPid, nDlls, rlpDlls)) {
        goto Cleanup;
    }

    if (nLen == 0 || nLen >= ARRAYSIZE(szExe)) {
        goto Cleanup;
    }

#if DETOURS_OPTION_BITS
#if DETOURS_32BIT
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\sysnative\\rundll32.exe");
#else // !DETOURS_32BIT
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\syswow64\\rundll32.exe");
#endif // !DETOURS_32BIT
#else // DETOURS_OPTIONS_BITS
    hr = StringCchCatW(szExe, ARRAYSIZE(szExe), L"\\system32\\rundll32.exe");
#endif // DETOURS_OPTIONS_BITS
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    //for East Asia languages and so on, like Chinese, print format with "%hs" can not work fine before user call _tsetlocale(LC_ALL,_T(".ACP"));
    //so we can't use "%hs" in format string, because the dll that contain this code would inject to any process, even not call _tsetlocale(LC_ALL,_T(".ACP")) before
    
    cchWrittenWideChar = MultiByteToWideChar(CP_ACP, 0, &helper->rDlls[0], -1, szDllName, ARRAYSIZE(szDllName));
    if (cchWrittenWideChar >= ARRAYSIZE(szDllName) || cchWrittenWideChar <= 0) {
        goto Cleanup;
    }
    hr = StringCchPrintfW(szCommand, ARRAYSIZE(szCommand),
        L"rundll32.exe \"%s\",#1", szDllName);
    if (!SUCCEEDED(hr)) {
        goto Cleanup;
    }

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    DETOUR_TRACE(("DetourProcessViaHelperDlls(\"%ls\", \"%ls\")\n", szExe, szCommand));
    if (pfCreateProcessW(szExe, szCommand, NULL, NULL, FALSE, CREATE_SUSPENDED,
                         NULL, NULL, &si, &pi)) {

        if (!DetourCopyPayloadToProcess(pi.hProcess,
                                        DETOUR_EXE_HELPER_GUID,
                                        helper, helper->cb)) {
            DETOUR_TRACE(("DetourCopyPayloadToProcess failed: %lu\n", GetLastError()));
            TerminateProcess(pi.hProcess, ~0u);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            goto Cleanup;
        }

        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD dwResult = 500;
        GetExitCodeProcess(pi.hProcess, &dwResult);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (dwResult != 0) {
            DETOUR_TRACE(("Rundll32.exe failed: result=%lu\n", dwResult));
            goto Cleanup;
        }
        Result = TRUE;
    }
    else {
        DETOUR_TRACE(("CreateProcess failed: %lu\n", GetLastError()));
        goto Cleanup;
    }

  Cleanup:
    FreeExeHelper(&helper);
    return Result;
}

BOOL WINAPI DetourCreateProcessWithDllExA(_In_opt_ LPCSTR lpApplicationName,
                                          _Inout_opt_ LPSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOA lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessA(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }

    LPCSTR szDll = lpDllName;

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, &szDll, 1) &&
        !DetourProcessViaHelperA(lpProcessInformation->dwProcessId,
                                 lpDllName,
                                 pfCreateProcessA)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }

    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllExW(_In_opt_ LPCWSTR lpApplicationName,
                                          _Inout_opt_  LPWSTR lpCommandLine,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                          _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                          _In_ BOOL bInheritHandles,
                                          _In_ DWORD dwCreationFlags,
                                          _In_opt_ LPVOID lpEnvironment,
                                          _In_opt_ LPCWSTR lpCurrentDirectory,
                                          _In_ LPSTARTUPINFOW lpStartupInfo,
                                          _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                          _In_ LPCSTR lpDllName,
                                          _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessW(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }


    LPCSTR sz = lpDllName;

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, &sz, 1) &&
        !DetourProcessViaHelperW(lpProcessInformation->dwProcessId,
                                 lpDllName,
                                 pfCreateProcessW)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }
    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllsA(_In_opt_ LPCSTR lpApplicationName,
                                         _Inout_opt_ LPSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOA lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEA pfCreateProcessA)
{
    if (pfCreateProcessA == NULL) {
        pfCreateProcessA = CreateProcessA;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessA(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }

    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, rlpDlls, nDlls) &&
        !DetourProcessViaHelperDllsA(lpProcessInformation->dwProcessId,
                                     nDlls,
                                     rlpDlls,
                                     pfCreateProcessA)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }

    return TRUE;
}

BOOL WINAPI DetourCreateProcessWithDllsW(_In_opt_ LPCWSTR lpApplicationName,
                                         _Inout_opt_ LPWSTR lpCommandLine,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         _In_ BOOL bInheritHandles,
                                         _In_ DWORD dwCreationFlags,
                                         _In_opt_ LPVOID lpEnvironment,
                                         _In_opt_ LPCWSTR lpCurrentDirectory,
                                         _In_ LPSTARTUPINFOW lpStartupInfo,
                                         _Out_ LPPROCESS_INFORMATION lpProcessInformation,
                                         _In_ DWORD nDlls,
                                         _In_reads_(nDlls) LPCSTR *rlpDlls,
                                         _In_opt_ PDETOUR_CREATE_PROCESS_ROUTINEW pfCreateProcessW)
{
    if (pfCreateProcessW == NULL) {
        pfCreateProcessW = CreateProcessW;
    }

    PROCESS_INFORMATION backup;
    if (lpProcessInformation == NULL) {
        lpProcessInformation = &backup;
        ZeroMemory(&backup, sizeof(backup));
    }

    if (!pfCreateProcessW(lpApplicationName,
                          lpCommandLine,
                          lpProcessAttributes,
                          lpThreadAttributes,
                          bInheritHandles,
                          dwCreationFlags | CREATE_SUSPENDED,
                          lpEnvironment,
                          lpCurrentDirectory,
                          lpStartupInfo,
                          lpProcessInformation)) {
        return FALSE;
    }


    if (!DetourUpdateProcessWithDll(lpProcessInformation->hProcess, rlpDlls, nDlls) &&
        !DetourProcessViaHelperDllsW(lpProcessInformation->dwProcessId,
                                     nDlls,
                                     rlpDlls,
                                     pfCreateProcessW)) {

        TerminateProcess(lpProcessInformation->hProcess, ~0u);
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
        return FALSE;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        ResumeThread(lpProcessInformation->hThread);
    }

    if (lpProcessInformation == &backup) {
        CloseHandle(lpProcessInformation->hProcess);
        CloseHandle(lpProcessInformation->hThread);
    }
    return TRUE;
}

//
///////////////////////////////////////////////////////////////// End of File.
//////////////////////////////////////////////////////////////////////////////
//
//  Module Enumeration Functions (modules.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//
//  Module enumeration functions.
//

// #define DETOUR_DEBUG 1
#define DETOURS_INTERNAL

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

#define CLR_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
#define IAT_DIRECTORY OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT]

//////////////////////////////////////////////////////////////////////////////
//
const GUID DETOUR_EXE_RESTORE_GUID = {
    0xbda26f34, 0xbc82, 0x4829,
    { 0x9e, 0x64, 0x74, 0x2c, 0x4, 0xc8, 0x4f, 0xa0 } };

//////////////////////////////////////////////////////////////////////////////
//
PDETOUR_SYM_INFO DetourLoadImageHlp(VOID)
{
    static DETOUR_SYM_INFO symInfo;
    static PDETOUR_SYM_INFO pSymInfo = NULL;
    static BOOL failed = false;

    if (failed) {
        return NULL;
    }
    if (pSymInfo != NULL) {
        return pSymInfo;
    }

    ZeroMemory(&symInfo, sizeof(symInfo));
    // Create a real handle to the process.
#if 0
    DuplicateHandle(GetCurrentProcess(),
                    GetCurrentProcess(),
                    GetCurrentProcess(),
                    &symInfo.hProcess,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS);
#else
    symInfo.hProcess = GetCurrentProcess();
#endif

    symInfo.hDbgHelp = LoadLibraryExW(L"dbghelp.dll", NULL, 0);
    if (symInfo.hDbgHelp == NULL) {
      abort:
        failed = true;
        if (symInfo.hDbgHelp != NULL) {
            FreeLibrary(symInfo.hDbgHelp);
        }
        symInfo.pfImagehlpApiVersionEx = NULL;
        symInfo.pfSymInitialize = NULL;
        symInfo.pfSymSetOptions = NULL;
        symInfo.pfSymGetOptions = NULL;
        symInfo.pfSymLoadModule64 = NULL;
        symInfo.pfSymGetModuleInfo64 = NULL;
        symInfo.pfSymFromName = NULL;
        return NULL;
    }

    symInfo.pfImagehlpApiVersionEx
        = (PF_ImagehlpApiVersionEx)GetProcAddress(symInfo.hDbgHelp,
                                                  "ImagehlpApiVersionEx");
    symInfo.pfSymInitialize
        = (PF_SymInitialize)GetProcAddress(symInfo.hDbgHelp, "SymInitialize");
    symInfo.pfSymSetOptions
        = (PF_SymSetOptions)GetProcAddress(symInfo.hDbgHelp, "SymSetOptions");
    symInfo.pfSymGetOptions
        = (PF_SymGetOptions)GetProcAddress(symInfo.hDbgHelp, "SymGetOptions");
    symInfo.pfSymLoadModule64
        = (PF_SymLoadModule64)GetProcAddress(symInfo.hDbgHelp, "SymLoadModule64");
    symInfo.pfSymGetModuleInfo64
        = (PF_SymGetModuleInfo64)GetProcAddress(symInfo.hDbgHelp, "SymGetModuleInfo64");
    symInfo.pfSymFromName
        = (PF_SymFromName)GetProcAddress(symInfo.hDbgHelp, "SymFromName");

    API_VERSION av;
    ZeroMemory(&av, sizeof(av));
    av.MajorVersion = API_VERSION_NUMBER;

    if (symInfo.pfImagehlpApiVersionEx == NULL ||
        symInfo.pfSymInitialize == NULL ||
        symInfo.pfSymLoadModule64 == NULL ||
        symInfo.pfSymGetModuleInfo64 == NULL ||
        symInfo.pfSymFromName == NULL) {
        goto abort;
    }

    symInfo.pfImagehlpApiVersionEx(&av);
    if (av.MajorVersion < API_VERSION_NUMBER) {
        goto abort;
    }

    if (!symInfo.pfSymInitialize(symInfo.hProcess, NULL, FALSE)) {
        // We won't retry the initialize if it fails.
        goto abort;
    }

    if (symInfo.pfSymGetOptions != NULL && symInfo.pfSymSetOptions != NULL) {
        DWORD dw = symInfo.pfSymGetOptions();

        dw &= ~(SYMOPT_CASE_INSENSITIVE |
                SYMOPT_UNDNAME |
                SYMOPT_DEFERRED_LOADS |
                0);
        dw |= (
#if defined(SYMOPT_EXACT_SYMBOLS)
               SYMOPT_EXACT_SYMBOLS |
#endif
#if defined(SYMOPT_NO_UNQUALIFIED_LOADS)
               SYMOPT_NO_UNQUALIFIED_LOADS |
#endif
               SYMOPT_DEFERRED_LOADS |
#if defined(SYMOPT_FAIL_CRITICAL_ERRORS)
               SYMOPT_FAIL_CRITICAL_ERRORS |
#endif
#if defined(SYMOPT_INCLUDE_32BIT_MODULES)
               SYMOPT_INCLUDE_32BIT_MODULES |
#endif
               0);
        symInfo.pfSymSetOptions(dw);
    }

    pSymInfo = &symInfo;
    return pSymInfo;
}

PVOID WINAPI DetourFindFunction(_In_ LPCSTR pszModule,
                                _In_ LPCSTR pszFunction)
{
    if (pszFunction == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    /////////////////////////////////////////////// First, try GetProcAddress.
    //
#pragma prefast(suppress:28752, "We don't do the unicode conversion for LoadLibraryExA.")
    HMODULE hModule = LoadLibraryExA(pszModule, NULL, 0);
    if (hModule == NULL) {
        return NULL;
    }

    PBYTE pbCode = (PBYTE)GetProcAddress(hModule, pszFunction);
    if (pbCode) {
        return pbCode;
    }

    ////////////////////////////////////////////////////// Then try ImageHelp.
    //
    DETOUR_TRACE(("DetourFindFunction(%hs, %hs)\n", pszModule, pszFunction));
    PDETOUR_SYM_INFO pSymInfo = DetourLoadImageHlp();
    if (pSymInfo == NULL) {
        DETOUR_TRACE(("DetourLoadImageHlp failed: %lu\n",
                      GetLastError()));
        return NULL;
    }

    if (pSymInfo->pfSymLoadModule64(pSymInfo->hProcess, NULL,
                                    (PCHAR)pszModule, NULL,
                                    (DWORD64)hModule, 0) == 0) {
        if (ERROR_SUCCESS != GetLastError()) {
            DETOUR_TRACE(("SymLoadModule64(%p) failed: %lu\n",
                          pSymInfo->hProcess, GetLastError()));
            return NULL;
        }
    }

    HRESULT hrRet;
    CHAR szFullName[512];
    IMAGEHLP_MODULE64 modinfo;
    ZeroMemory(&modinfo, sizeof(modinfo));
    modinfo.SizeOfStruct = sizeof(modinfo);
    if (!pSymInfo->pfSymGetModuleInfo64(pSymInfo->hProcess, (DWORD64)hModule, &modinfo)) {
        DETOUR_TRACE(("SymGetModuleInfo64(%p, %p) failed: %lu\n",
                      pSymInfo->hProcess, hModule, GetLastError()));
        return NULL;
    }

    hrRet = StringCchCopyA(szFullName, sizeof(szFullName)/sizeof(CHAR), modinfo.ModuleName);
    if (FAILED(hrRet)) {
        DETOUR_TRACE(("StringCchCopyA failed: %08lx\n", hrRet));
        return NULL;
    }
    hrRet = StringCchCatA(szFullName, sizeof(szFullName)/sizeof(CHAR), "!");
    if (FAILED(hrRet)) {
        DETOUR_TRACE(("StringCchCatA failed: %08lx\n", hrRet));
        return NULL;
    }
    hrRet = StringCchCatA(szFullName, sizeof(szFullName)/sizeof(CHAR), pszFunction);
    if (FAILED(hrRet)) {
        DETOUR_TRACE(("StringCchCatA failed: %08lx\n", hrRet));
        return NULL;
    }

    struct CFullSymbol : SYMBOL_INFO {
        CHAR szRestOfName[512];
    } symbol;
    ZeroMemory(&symbol, sizeof(symbol));
    //symbol.ModBase = (ULONG64)hModule;
    symbol.SizeOfStruct = sizeof(SYMBOL_INFO);
#ifdef DBHLPAPI
    symbol.MaxNameLen = sizeof(symbol.szRestOfName)/sizeof(symbol.szRestOfName[0]);
#else
    symbol.MaxNameLength = sizeof(symbol.szRestOfName)/sizeof(symbol.szRestOfName[0]);
#endif

    if (!pSymInfo->pfSymFromName(pSymInfo->hProcess, szFullName, &symbol)) {
        DETOUR_TRACE(("SymFromName(%hs) failed: %lu\n", szFullName, GetLastError()));
        return NULL;
    }

#if defined(DETOURS_IA64)
    // On the IA64, we get a raw code pointer from the symbol engine
    // and have to convert it to a wrapped [code pointer, global pointer].
    //
    PPLABEL_DESCRIPTOR pldEntry = (PPLABEL_DESCRIPTOR)DetourGetEntryPoint(hModule);
    PPLABEL_DESCRIPTOR pldSymbol = new PLABEL_DESCRIPTOR;

    pldSymbol->EntryPoint = symbol.Address;
    pldSymbol->GlobalPointer = pldEntry->GlobalPointer;
    return (PBYTE)pldSymbol;
#elif defined(DETOURS_ARM)
    // On the ARM, we get a raw code pointer, which we must convert into a
    // valied Thumb2 function pointer.
    return DETOURS_PBYTE_TO_PFUNC(symbol.Address);
#else
    return (PBYTE)symbol.Address;
#endif
}

//////////////////////////////////////////////////// Module Image Functions.
//

HMODULE WINAPI DetourEnumerateModules(_In_opt_ HMODULE hModuleLast)
{
    PBYTE pbLast = (PBYTE)hModuleLast + MM_ALLOCATION_GRANULARITY;

    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    // Find the next memory region that contains a mapped PE image.
    //
    for (;; pbLast = (PBYTE)mbi.BaseAddress + mbi.RegionSize) {
        if (VirtualQuery(pbLast, &mbi, sizeof(mbi)) <= 0) {
            break;
        }

        // Skip uncommitted regions and guard pages.
        //
        if ((mbi.State != MEM_COMMIT) ||
            ((mbi.Protect & 0xff) == PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            continue;
        }

        __try {
            PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pbLast;
            if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
                (DWORD)pDosHeader->e_lfanew > mbi.RegionSize ||
                (DWORD)pDosHeader->e_lfanew < sizeof(*pDosHeader)) {
                continue;
            }

            PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                              pDosHeader->e_lfanew);
            if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
                continue;
            }

            SetLastError(NO_ERROR);
            return (HMODULE)pDosHeader;
        }
#pragma prefast(suppress:28940, "A bad pointer means this probably isn't a PE header.")
        __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                 EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            continue;
        }
    }
    return NULL;
}

PVOID WINAPI DetourGetEntryPoint(_In_opt_ HMODULE hModule)
{
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (hModule == NULL) {
        pDosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    }

    __try {
#pragma warning(suppress:6011) // GetModuleHandleW(NULL) never returns NULL.
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return NULL;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return NULL;
        }

        PDETOUR_CLR_HEADER pClrHeader = NULL;
        if (pNtHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            if (((PIMAGE_NT_HEADERS32)pNtHeader)->CLR_DIRECTORY.VirtualAddress != 0 &&
                ((PIMAGE_NT_HEADERS32)pNtHeader)->CLR_DIRECTORY.Size != 0) {
                pClrHeader = (PDETOUR_CLR_HEADER)
                    (((PBYTE)pDosHeader)
                     + ((PIMAGE_NT_HEADERS32)pNtHeader)->CLR_DIRECTORY.VirtualAddress);
            }
        }
        else if (pNtHeader->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            if (((PIMAGE_NT_HEADERS64)pNtHeader)->CLR_DIRECTORY.VirtualAddress != 0 &&
                ((PIMAGE_NT_HEADERS64)pNtHeader)->CLR_DIRECTORY.Size != 0) {
                pClrHeader = (PDETOUR_CLR_HEADER)
                    (((PBYTE)pDosHeader)
                     + ((PIMAGE_NT_HEADERS64)pNtHeader)->CLR_DIRECTORY.VirtualAddress);
            }
        }

        if (pClrHeader != NULL) {
            // For MSIL assemblies, we want to use the _Cor entry points.

            HMODULE hClr = GetModuleHandleW(L"MSCOREE.DLL");
            if (hClr == NULL) {
                return NULL;
            }

            SetLastError(NO_ERROR);
            return (PVOID)GetProcAddress(hClr, "_CorExeMain");
        }

        SetLastError(NO_ERROR);

        // Pure resource DLLs have neither an entry point nor CLR information
        // so handle them by returning NULL (LastError is NO_ERROR)
        if (pNtHeader->OptionalHeader.AddressOfEntryPoint == 0) {
            return NULL;
        }

        return ((PBYTE)pDosHeader) +
            pNtHeader->OptionalHeader.AddressOfEntryPoint;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }
}

ULONG WINAPI DetourGetModuleSize(_In_opt_ HMODULE hModule)
{
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (hModule == NULL) {
        pDosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    }

    __try {
#pragma warning(suppress:6011) // GetModuleHandleW(NULL) never returns NULL.
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return NULL;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return NULL;
        }
        SetLastError(NO_ERROR);

        return (pNtHeader->OptionalHeader.SizeOfImage);
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }
}

HMODULE WINAPI DetourGetContainingModule(_In_ PVOID pvAddr)
{
    MEMORY_BASIC_INFORMATION mbi;
    ZeroMemory(&mbi, sizeof(mbi));

    __try {
        if (VirtualQuery(pvAddr, &mbi, sizeof(mbi)) <= 0) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        // Skip uncommitted regions and guard pages.
        //
        if ((mbi.State != MEM_COMMIT) ||
            ((mbi.Protect & 0xff) == PAGE_NOACCESS) ||
            (mbi.Protect & PAGE_GUARD)) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)mbi.AllocationBase;
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return NULL;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return NULL;
        }
        SetLastError(NO_ERROR);

        return (HMODULE)pDosHeader;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_INVALID_EXE_SIGNATURE);
        return NULL;
    }
}


static inline PBYTE RvaAdjust(_Pre_notnull_ PIMAGE_DOS_HEADER pDosHeader, _In_ DWORD raddr)
{
    if (raddr != NULL) {
        return ((PBYTE)pDosHeader) + raddr;
    }
    return NULL;
}

BOOL WINAPI DetourEnumerateExports(_In_ HMODULE hModule,
                                   _In_opt_ PVOID pContext,
                                   _In_ PF_DETOUR_ENUMERATE_EXPORT_CALLBACK pfExport)
{
    if (pfExport == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (hModule == NULL) {
        pDosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    }

    __try {
#pragma warning(suppress:6011) // GetModuleHandleW(NULL) never returns NULL.
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return FALSE;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return FALSE;
        }

        PIMAGE_EXPORT_DIRECTORY pExportDir
            = (PIMAGE_EXPORT_DIRECTORY)
            RvaAdjust(pDosHeader,
                      pNtHeader->OptionalHeader
                      .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

        if (pExportDir == NULL) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return FALSE;
        }

        PBYTE pExportDirEnd = (PBYTE)pExportDir + pNtHeader->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        PDWORD pdwFunctions = (PDWORD)RvaAdjust(pDosHeader, pExportDir->AddressOfFunctions);
        PDWORD pdwNames = (PDWORD)RvaAdjust(pDosHeader, pExportDir->AddressOfNames);
        PWORD pwOrdinals = (PWORD)RvaAdjust(pDosHeader, pExportDir->AddressOfNameOrdinals);

        for (DWORD nFunc = 0; nFunc < pExportDir->NumberOfFunctions; nFunc++) {
            PBYTE pbCode = (pdwFunctions != NULL)
                ? (PBYTE)RvaAdjust(pDosHeader, pdwFunctions[nFunc]) : NULL;
            PCHAR pszName = NULL;

            // if the pointer is in the export region, then it is a forwarder.
            if (pbCode > (PBYTE)pExportDir && pbCode < pExportDirEnd) {
                pbCode = NULL;
            }

            for (DWORD n = 0; n < pExportDir->NumberOfNames; n++) {
                if (pwOrdinals[n] == nFunc) {
                    pszName = (pdwNames != NULL)
                        ? (PCHAR)RvaAdjust(pDosHeader, pdwNames[n]) : NULL;
                    break;
                }
            }
            ULONG nOrdinal = pExportDir->Base + nFunc;

            if (!pfExport(pContext, nOrdinal, pszName, pbCode)) {
                break;
            }
        }
        SetLastError(NO_ERROR);
        return TRUE;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }
}

BOOL WINAPI DetourEnumerateImportsEx(_In_opt_ HMODULE hModule,
                                     _In_opt_ PVOID pContext,
                                     _In_opt_ PF_DETOUR_IMPORT_FILE_CALLBACK pfImportFile,
                                     _In_opt_ PF_DETOUR_IMPORT_FUNC_CALLBACK_EX pfImportFunc)
{
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (hModule == NULL) {
        pDosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    }

    __try {
#pragma warning(suppress:6011) // GetModuleHandleW(NULL) never returns NULL.
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return FALSE;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return FALSE;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return FALSE;
        }

        PIMAGE_IMPORT_DESCRIPTOR iidp
            = (PIMAGE_IMPORT_DESCRIPTOR)
            RvaAdjust(pDosHeader,
                      pNtHeader->OptionalHeader
                      .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        if (iidp == NULL) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return FALSE;
        }

        for (; iidp->OriginalFirstThunk != 0; iidp++) {

            PCSTR pszName = (PCHAR)RvaAdjust(pDosHeader, iidp->Name);
            if (pszName == NULL) {
                SetLastError(ERROR_EXE_MARKED_INVALID);
                return FALSE;
            }

            PIMAGE_THUNK_DATA pThunks = (PIMAGE_THUNK_DATA)
                RvaAdjust(pDosHeader, iidp->OriginalFirstThunk);
            PVOID * pAddrs = (PVOID *)
                RvaAdjust(pDosHeader, iidp->FirstThunk);

            HMODULE hFile = DetourGetContainingModule(pAddrs[0]);

            if (pfImportFile != NULL) {
                if (!pfImportFile(pContext, hFile, pszName)) {
                    break;
                }
            }

            DWORD nNames = 0;
            if (pThunks) {
                for (; pThunks[nNames].u1.Ordinal; nNames++) {
                    DWORD nOrdinal = 0;
                    PCSTR pszFunc = NULL;

                    if (IMAGE_SNAP_BY_ORDINAL(pThunks[nNames].u1.Ordinal)) {
                        nOrdinal = (DWORD)IMAGE_ORDINAL(pThunks[nNames].u1.Ordinal);
                    }
                    else {
                        pszFunc = (PCSTR)RvaAdjust(pDosHeader,
                                                   (DWORD)pThunks[nNames].u1.AddressOfData + 2);
                    }

                    if (pfImportFunc != NULL) {
                        if (!pfImportFunc(pContext,
                                          nOrdinal,
                                          pszFunc,
                                          &pAddrs[nNames])) {
                            break;
                        }
                    }
                }
                if (pfImportFunc != NULL) {
                    pfImportFunc(pContext, 0, NULL, NULL);
                }
            }
        }
        if (pfImportFile != NULL) {
            pfImportFile(pContext, NULL, NULL);
        }
        SetLastError(NO_ERROR);
        return TRUE;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return FALSE;
    }
}

// Context for DetourEnumerateImportsThunk, which adapts "regular" callbacks for use with "Ex".
struct _DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT
{
    PVOID pContext;
    PF_DETOUR_IMPORT_FILE_CALLBACK pfImportFile;
    PF_DETOUR_IMPORT_FUNC_CALLBACK pfImportFunc;
};

// Callback for DetourEnumerateImportsEx that adapts DetourEnumerateImportsEx
// for use with a DetourEnumerateImports callback -- derefence the IAT and pass the value on.

static
BOOL
CALLBACK
DetourEnumerateImportsThunk(_In_ PVOID VoidContext,
                            _In_ DWORD nOrdinal,
                            _In_opt_ PCSTR pszFunc,
                            _In_opt_ PVOID* ppvFunc)
{
    _DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT const * const
        pContext = (_DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT*)VoidContext;
    return pContext->pfImportFunc(pContext->pContext, nOrdinal, pszFunc, ppvFunc ? *ppvFunc : NULL);
}

static
BOOL
CALLBACK
DetourEnumerateImportsFile(_In_ PVOID VoidContext,
                           _In_opt_ HMODULE hModule,
                           _In_opt_ LPCSTR pszFile)
{
    _DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT const * const
        pContext = (_DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT*)VoidContext;
    return pContext->pfImportFile(pContext->pContext, hModule, pszFile);
}


BOOL WINAPI DetourEnumerateImports(_In_opt_ HMODULE hModule,
                                   _In_opt_ PVOID pContext,
                                   _In_opt_ PF_DETOUR_IMPORT_FILE_CALLBACK pfImportFile,
                                   _In_opt_ PF_DETOUR_IMPORT_FUNC_CALLBACK pfImportFunc)
{
    if (pfImportFile == NULL || pfImportFunc == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    _DETOUR_ENUMERATE_IMPORTS_THUNK_CONTEXT const context = { pContext, pfImportFile, pfImportFunc };
    return DetourEnumerateImportsEx(hModule,
                                    (PVOID)&context,
                                    &DetourEnumerateImportsFile,
                                    &DetourEnumerateImportsThunk);
}

static PDETOUR_LOADED_BINARY WINAPI GetPayloadSectionFromModule(HMODULE hModule)
{
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (hModule == NULL) {
        pDosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    }

    __try {
#pragma warning(suppress:6011) // GetModuleHandleW(NULL) never returns NULL.
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            SetLastError(ERROR_BAD_EXE_FORMAT);
            return NULL;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return NULL;
        }
        if (pNtHeader->FileHeader.SizeOfOptionalHeader == 0) {
            SetLastError(ERROR_EXE_MARKED_INVALID);
            return NULL;
        }

        PIMAGE_SECTION_HEADER pSectionHeaders
            = (PIMAGE_SECTION_HEADER)((PBYTE)pNtHeader
                                      + sizeof(pNtHeader->Signature)
                                      + sizeof(pNtHeader->FileHeader)
                                      + pNtHeader->FileHeader.SizeOfOptionalHeader);

        for (DWORD n = 0; n < pNtHeader->FileHeader.NumberOfSections; n++) {
            if (strcmp((PCHAR)pSectionHeaders[n].Name, ".detour") == 0) {
                if (pSectionHeaders[n].VirtualAddress == 0 ||
                    pSectionHeaders[n].SizeOfRawData == 0) {

                    break;
                }

                PBYTE pbData = (PBYTE)pDosHeader + pSectionHeaders[n].VirtualAddress;
                DETOUR_SECTION_HEADER *pHeader = (DETOUR_SECTION_HEADER *)pbData;
                if (pHeader->cbHeaderSize < sizeof(DETOUR_SECTION_HEADER) ||
                    pHeader->nSignature != DETOUR_SECTION_HEADER_SIGNATURE) {

                    break;
                }

                if (pHeader->nDataOffset == 0) {
                    pHeader->nDataOffset = pHeader->cbHeaderSize;
                }
                SetLastError(NO_ERROR);
                return (PBYTE)pHeader;
            }
        }
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_EXE_MARKED_INVALID);
        return NULL;
    }
}

DWORD WINAPI DetourGetSizeOfPayloads(_In_opt_ HMODULE hModule)
{
    PDETOUR_LOADED_BINARY pBinary = GetPayloadSectionFromModule(hModule);
    if (pBinary == NULL) {
        // Error set by GetPayloadSectionFromModule.
        return 0;
    }

    __try {
        DETOUR_SECTION_HEADER *pHeader = (DETOUR_SECTION_HEADER *)pBinary;
        if (pHeader->cbHeaderSize < sizeof(DETOUR_SECTION_HEADER) ||
            pHeader->nSignature != DETOUR_SECTION_HEADER_SIGNATURE) {

            SetLastError(ERROR_INVALID_HANDLE);
            return 0;
        }
        SetLastError(NO_ERROR);
        return pHeader->cbDataSize;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
}

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourFindPayload(_In_opt_ HMODULE hModule,
                               _In_ REFGUID rguid,
                               _Out_opt_ DWORD *pcbData)
{
    PBYTE pbData = NULL;
    if (pcbData) {
        *pcbData = 0;
    }

    PDETOUR_LOADED_BINARY pBinary = GetPayloadSectionFromModule(hModule);
    if (pBinary == NULL) {
        // Error set by GetPayloadSectionFromModule.
        return NULL;
    }

    __try {
        DETOUR_SECTION_HEADER *pHeader = (DETOUR_SECTION_HEADER *)pBinary;
        if (pHeader->cbHeaderSize < sizeof(DETOUR_SECTION_HEADER) ||
            pHeader->nSignature != DETOUR_SECTION_HEADER_SIGNATURE) {

            SetLastError(ERROR_INVALID_EXE_SIGNATURE);
            return NULL;
        }

        PBYTE pbBeg = ((PBYTE)pHeader) + pHeader->nDataOffset;
        PBYTE pbEnd = ((PBYTE)pHeader) + pHeader->cbDataSize;

        for (pbData = pbBeg; pbData < pbEnd;) {
            DETOUR_SECTION_RECORD *pSection = (DETOUR_SECTION_RECORD *)pbData;

            if (DetourAreSameGuid(pSection->guid, rguid)) {
                if (pcbData) {
                    *pcbData = pSection->cbBytes - sizeof(*pSection);
                }
                SetLastError(NO_ERROR);
                return (PBYTE)(pSection + 1);
            }

            pbData = (PBYTE)pSection + pSection->cbBytes;
        }
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
}

_Writable_bytes_(*pcbData)
_Readable_bytes_(*pcbData)
_Success_(return != NULL)
PVOID WINAPI DetourFindPayloadEx(_In_ REFGUID rguid,
                                 _Out_opt_ DWORD *pcbData)
{
    for (HMODULE hMod = NULL; (hMod = DetourEnumerateModules(hMod)) != NULL;) {
        PVOID pvData;

        pvData = DetourFindPayload(hMod, rguid, pcbData);
        if (pvData != NULL) {
            return pvData;
        }
    }
    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

BOOL WINAPI DetourFreePayload(_In_ PVOID pvData)
{
    BOOL fSucceeded = FALSE;

    // If you have any doubts about the following code, please refer to the comments in DetourCopyPayloadToProcess.
    HMODULE hModule = DetourGetContainingModule(pvData);
    DETOUR_ASSERT(hModule != NULL);
    if (hModule != NULL) {
        fSucceeded = VirtualFree(hModule, 0, MEM_RELEASE);
        DETOUR_ASSERT(fSucceeded);
        if (fSucceeded) {
            hModule = NULL;
        }
    }

    return fSucceeded;
}

BOOL WINAPI DetourRestoreAfterWithEx(_In_reads_bytes_(cbData) PVOID pvData,
                                     _In_ DWORD cbData)
{
    PDETOUR_EXE_RESTORE pder = (PDETOUR_EXE_RESTORE)pvData;

    if (pder->cb != sizeof(*pder) || pder->cb > cbData) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }

    DWORD dwPermIdh = ~0u;
    DWORD dwPermInh = ~0u;
    DWORD dwPermClr = ~0u;
    DWORD dwIgnore;
    BOOL fSucceeded = FALSE;
    BOOL fUpdated32To64 = FALSE;

    if (pder->pclr != NULL && pder->clr.Flags != ((PDETOUR_CLR_HEADER)pder->pclr)->Flags) {
        // If we had to promote the 32/64-bit agnostic IL to 64-bit, we can't restore
        // that.
        fUpdated32To64 = TRUE;
    }

    if (DetourVirtualProtectSameExecute(pder->pidh, pder->cbidh,
                                        PAGE_EXECUTE_READWRITE, &dwPermIdh)) {
        if (DetourVirtualProtectSameExecute(pder->pinh, pder->cbinh,
                                            PAGE_EXECUTE_READWRITE, &dwPermInh)) {

            CopyMemory(pder->pidh, &pder->idh, pder->cbidh);
            CopyMemory(pder->pinh, &pder->inh, pder->cbinh);

            if (pder->pclr != NULL && !fUpdated32To64) {
                if (DetourVirtualProtectSameExecute(pder->pclr, pder->cbclr,
                                                    PAGE_EXECUTE_READWRITE, &dwPermClr)) {
                    CopyMemory(pder->pclr, &pder->clr, pder->cbclr);
                    VirtualProtect(pder->pclr, pder->cbclr, dwPermClr, &dwIgnore);
                    fSucceeded = TRUE;
                }
            }
            else {
                fSucceeded = TRUE;
            }
            VirtualProtect(pder->pinh, pder->cbinh, dwPermInh, &dwIgnore);
        }
        VirtualProtect(pder->pidh, pder->cbidh, dwPermIdh, &dwIgnore);
    }
    // Delete the payload after successful recovery to prevent repeated restore
    if (fSucceeded) {
        DetourFreePayload(pder);
        pder = NULL;
    }
    return fSucceeded;
}

BOOL WINAPI DetourRestoreAfterWith()
{
    PVOID pvData;
    DWORD cbData;

    pvData = DetourFindPayloadEx(DETOUR_EXE_RESTORE_GUID, &cbData);

    if (pvData != NULL && cbData != 0) {
        return DetourRestoreAfterWithEx(pvData, cbData);
    }
    SetLastError(ERROR_MOD_NOT_FOUND);
    return FALSE;
}

//  End of File
//////////////////////////////////////////////////////////////////////////////
//
//  Core Detours Functionality (detours.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//


//#define DETOUR_DEBUG 1
#define DETOURS_INTERNAL

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

#define NOTHROW

//////////////////////////////////////////////////////////////////////////////
//

#ifdef _DEBUG
extern "C" IMAGE_DOS_HEADER __ImageBase;
int Detour_AssertExprWithFunctionName(int reportType, const char* filename, int linenumber, const char* FunctionName, const char* msg)
{
    int nRet = 0;
    DWORD dwLastError = GetLastError();
    CHAR szModuleNameWithFunctionName[MAX_PATH * 2];
    szModuleNameWithFunctionName[0] = 0;
    GetModuleFileNameA((HMODULE)&__ImageBase, szModuleNameWithFunctionName, ARRAYSIZE(szModuleNameWithFunctionName));
    StringCchCatNA(szModuleNameWithFunctionName, ARRAYSIZE(szModuleNameWithFunctionName), ",", ARRAYSIZE(szModuleNameWithFunctionName) - strlen(szModuleNameWithFunctionName) - 1);
    StringCchCatNA(szModuleNameWithFunctionName, ARRAYSIZE(szModuleNameWithFunctionName), FunctionName, ARRAYSIZE(szModuleNameWithFunctionName) - strlen(szModuleNameWithFunctionName) - 1);
    SetLastError(dwLastError);
    nRet = _CrtDbgReport(reportType, filename, linenumber, szModuleNameWithFunctionName, msg);
    SetLastError(dwLastError);
    return nRet;
}
#endif// _DEBUG

//////////////////////////////////////////////////////////////////////////////
//
struct _DETOUR_ALIGN
{
    BYTE    obTarget        : 3;
    BYTE    obTrampoline    : 5;
};

C_ASSERT(sizeof(_DETOUR_ALIGN) == 1);

//////////////////////////////////////////////////////////////////////////////
//
// Region reserved for system DLLs, which cannot be used for trampolines.
//
static PVOID    s_pSystemRegionLowerBound   = (PVOID)(ULONG_PTR)0x70000000;
static PVOID    s_pSystemRegionUpperBound   = (PVOID)(ULONG_PTR)0x80000000;

//////////////////////////////////////////////////////////////////////////////
//
static bool detour_is_imported(PBYTE pbCode, PBYTE pbAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery((PVOID)pbCode, &mbi, sizeof(mbi));
    __try {
        PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)mbi.AllocationBase;
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
                                                          pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        if (pbAddress >= ((PBYTE)pDosHeader +
                          pNtHeader->OptionalHeader
                          .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress) &&
            pbAddress < ((PBYTE)pDosHeader +
                         pNtHeader->OptionalHeader
                         .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress +
                         pNtHeader->OptionalHeader
                         .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size)) {
            return true;
        }
    }
#pragma prefast(suppress:28940, "A bad pointer means this probably isn't a PE header.")
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return false;
}

inline ULONG_PTR detour_2gb_below(ULONG_PTR address)
{
    return (address > (ULONG_PTR)0x7ff80000) ? address - 0x7ff80000 : 0x80000;
}

inline ULONG_PTR detour_2gb_above(ULONG_PTR address)
{
#if defined(DETOURS_64BIT)
    return (address < (ULONG_PTR)0xffffffff80000000) ? address + 0x7ff80000 : (ULONG_PTR)0xfffffffffff80000;
#else
    return (address < (ULONG_PTR)0x80000000) ? address + 0x7ff80000 : (ULONG_PTR)0xfff80000;
#endif
}

///////////////////////////////////////////////////////////////////////// X86.
//
#ifdef DETOURS_X86

struct _DETOUR_TRAMPOLINE
{
    BYTE            rbCode[30];     // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[22];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 72);

enum {
    SIZE_OF_JMP = 5
};

inline PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE pbJmpVal)
{
    PBYTE pbJmpSrc = pbCode + 5;
    *pbCode++ = 0xE9;   // jmp +imm32
    *((INT32*&)pbCode)++ = (INT32)(pbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_jmp_indirect(PBYTE pbCode, PBYTE *ppbJmpVal)
{
    *pbCode++ = 0xff;   // jmp [+imm32]
    *pbCode++ = 0x25;
    *((INT32*&)pbCode)++ = (INT32)((PBYTE)ppbJmpVal);
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit) {
        *pbCode++ = 0xcc;   // brk;
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    PBYTE pbCodeOriginal;

    if (pbCode == NULL) {
        return NULL;
    }
    if (ppGlobals != NULL) {
        *ppGlobals = NULL;
    }

    // First, skip over the import vector if there is one.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25) {   // jmp [imm32]
        // Looks like an import alias jump, then get the code it points to.
        PBYTE pbTarget = *(UNALIGNED PBYTE *)&pbCode[2];
        if (detour_is_imported(pbCode, pbTarget)) {
            PBYTE pbNew = *(UNALIGNED PBYTE *)pbTarget;
            DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }

    // Then, skip over a patch jump
    if (pbCode[0] == 0xeb) {   // jmp +imm8
        PBYTE pbNew = pbCode + 2 + *(CHAR *)&pbCode[1];
        DETOUR_TRACE(("%p->%p: skipped over short jump.\n", pbCode, pbNew));
        pbCode = pbNew;
        pbCodeOriginal = pbCode;

        // First, skip over the import vector if there is one.
        if (pbCode[0] == 0xff && pbCode[1] == 0x25) {   // jmp [imm32]
            // Looks like an import alias jump, then get the code it points to.
            PBYTE pbTarget = *(UNALIGNED PBYTE *)&pbCode[2];
            if (detour_is_imported(pbCode, pbTarget)) {
                pbNew = *(UNALIGNED PBYTE *)pbTarget;
                DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                pbCode = pbNew;
            }
        }
        // Finally, skip over a long jump if it is the target of the patch jump.
        else if (pbCode[0] == 0xe9) {   // jmp +imm32
            pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];
            DETOUR_TRACE(("%p->%p: skipped over long jump.\n", pbCode, pbNew));
            pbCode = pbNew;

            // Patches applied by the OS will jump through an HPAT page to get
            // the target function in the patch image. The jump is always performed
            // to the target function found at the current instruction pointer +
            // PAGE_SIZE - 6 (size of jump).
            // If this is an OS patch, we want to detour at the point of the target function
            // padding in the base image. Ideally, we would detour at the target function, but
            // since it's patched it begins with a short jump (to padding) which isn't long
            // enough to hold the detour code bytes.
            if (pbCode[0] == 0xff &&
                pbCode[1] == 0x25 &&
                *(UNALIGNED INT32 *)&pbCode[2] == (UNALIGNED INT32)(pbCode + 0x1000)) {   // jmp [eip+PAGE_SIZE-6]

                DETOUR_TRACE(("%p->%p: OS patch encountered, reset back to long jump 5 bytes prior to target function.\n", pbCode, pbCodeOriginal));
                pbCode = pbCodeOriginal;
            }

        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
                                   PDETOUR_TRAMPOLINE *ppLower,
                                   PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", (PVOID)lo, pbCode, (PVOID)hi));

    // And, within +/- 2GB of relative jmp targets.
    if (pbCode[0] == 0xe9) {   // jmp +imm32
        PBYTE pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];

        if (pbNew < pbCode) {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%p..%p..%p] +imm32\n", (PVOID)lo, pbCode, (PVOID)hi));
    }

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    if (pbCode[0] == 0xeb ||    // jmp +imm8
        pbCode[0] == 0xe9 ||    // jmp +imm32
        pbCode[0] == 0xe0 ||    // jmp eax
        pbCode[0] == 0xc2 ||    // ret +imm8
        pbCode[0] == 0xc3 ||    // ret
        pbCode[0] == 0xcc) {    // brk
        return TRUE;
    }
    else if (pbCode[0] == 0xf3 && pbCode[1] == 0xc3) {  // rep ret
        return TRUE;
    }
    else if (pbCode[0] == 0xff && pbCode[1] == 0x25) {  // jmp [+imm32]
        return TRUE;
    }
    else if ((pbCode[0] == 0x26 ||      // jmp es:
              pbCode[0] == 0x2e ||      // jmp cs:
              pbCode[0] == 0x36 ||      // jmp ss:
              pbCode[0] == 0x3e ||      // jmp ds:
              pbCode[0] == 0x64 ||      // jmp fs:
              pbCode[0] == 0x65) &&     // jmp gs:
             pbCode[1] == 0xff &&       // jmp [+imm32]
             pbCode[2] == 0x25) {
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // 1-byte through 11-byte NOPs.
    if (pbCode[0] == 0x90) {
        return 1;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x90) {
        return 2;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x00) {
        return 3;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x40 &&
        pbCode[3] == 0x00) {
        return 4;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x44 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00) {
        return 5;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x44 && pbCode[4] == 0x00 && pbCode[5] == 0x00) {
        return 6;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x80 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00) {
        return 7;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x84 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00) {
        return 8;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x84 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00) {
        return 9;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x0F &&
        pbCode[3] == 0x1F && pbCode[4] == 0x84 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00) {
        return 10;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x66 &&
        pbCode[3] == 0x0F && pbCode[4] == 0x1F && pbCode[5] == 0x84 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00 && pbCode[10] == 0x00) {
        return 11;
    }

    // int 3.
    if (pbCode[0] == 0xcc) {
        return 1;
    }
    return 0;
}

#endif // DETOURS_X86

///////////////////////////////////////////////////////////////////////// X64.
//
#ifdef DETOURS_X64

struct _DETOUR_TRAMPOLINE
{
    // An X64 instuction can be 15 bytes long.
    // In practice 11 seems to be the limit.
    BYTE            rbCode[30];     // target code + jmp to pbRemain.
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[30];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
    BYTE            rbCodeIn[8];    // jmp [pbDetour]
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 96);

enum {
    SIZE_OF_JMP = 5
};

inline PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE pbJmpVal)
{
    PBYTE pbJmpSrc = pbCode + 5;
    *pbCode++ = 0xE9;   // jmp +imm32
    *((INT32*&)pbCode)++ = (INT32)(pbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_jmp_indirect(PBYTE pbCode, PBYTE *ppbJmpVal)
{
    PBYTE pbJmpSrc = pbCode + 6;
    *pbCode++ = 0xff;   // jmp [+imm32]
    *pbCode++ = 0x25;
    *((INT32*&)pbCode)++ = (INT32)((PBYTE)ppbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit) {
        *pbCode++ = 0xcc;   // brk;
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    PBYTE pbCodeOriginal;

    if (pbCode == NULL) {
        return NULL;
    }
    if (ppGlobals != NULL) {
        *ppGlobals = NULL;
    }

    // First, skip over the import vector if there is one.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25) {   // jmp [+imm32]
        // Looks like an import alias jump, then get the code it points to.
        PBYTE pbTarget = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];
        if (detour_is_imported(pbCode, pbTarget)) {
            PBYTE pbNew = *(UNALIGNED PBYTE *)pbTarget;
            DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }

    // Then, skip over a patch jump
    if (pbCode[0] == 0xeb) {   // jmp +imm8
        PBYTE pbNew = pbCode + 2 + *(CHAR *)&pbCode[1];
        DETOUR_TRACE(("%p->%p: skipped over short jump.\n", pbCode, pbNew));
        pbCode = pbNew;
        pbCodeOriginal = pbCode;

        // First, skip over the import vector if there is one.
        if (pbCode[0] == 0xff && pbCode[1] == 0x25) {   // jmp [+imm32]
            // Looks like an import alias jump, then get the code it points to.
            PBYTE pbTarget = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];
            if (detour_is_imported(pbCode, pbTarget)) {
                pbNew = *(UNALIGNED PBYTE *)pbTarget;
                DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                pbCode = pbNew;
            }
        }
        // Finally, skip over a long jump if it is the target of the patch jump.
        else if (pbCode[0] == 0xe9) {   // jmp +imm32
            pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];
            DETOUR_TRACE(("%p->%p: skipped over long jump.\n", pbCode, pbNew));
            pbCode = pbNew;

            // Patches applied by the OS will jump through an HPAT page to get
            // the target function in the patch image. The jump is always performed
            // to the target function found at the current instruction pointer +
            // PAGE_SIZE - 6 (size of jump).
            // If this is an OS patch, we want to detour at the point of the target function
            // in the base image. Since we need 5 bytes to perform the jump, detour at the
            // point of the long jump instead of the short jump at the start of the target.
            if (pbCode[0] == 0xff &&
                pbCode[1] == 0x25 &&
                *(UNALIGNED INT32 *)&pbCode[2] == 0xFFA) {   // jmp [rip+PAGE_SIZE-6]

                DETOUR_TRACE(("%p->%p: OS patch encountered, reset back to long jump 5 bytes prior to target function.\n", pbCode, pbCodeOriginal));
                pbCode = pbCodeOriginal;
            }
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
                                   PDETOUR_TRAMPOLINE *ppLower,
                                   PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", (PVOID)lo, pbCode, (PVOID)hi));

    // And, within +/- 2GB of relative jmp vectors.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25) {   // jmp [+imm32]
        PBYTE pbNew = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];

        if (pbNew < pbCode) {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%p..%p..%p] [+imm32]\n", (PVOID)lo, pbCode, (PVOID)hi));
    }
    // And, within +/- 2GB of relative jmp targets.
    else if (pbCode[0] == 0xe9) {   // jmp +imm32
        PBYTE pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];

        if (pbNew < pbCode) {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%p..%p..%p] +imm32\n", (PVOID)lo, pbCode, (PVOID)hi));
    }

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    if (pbCode[0] == 0xeb ||    // jmp +imm8
        pbCode[0] == 0xe9 ||    // jmp +imm32
        pbCode[0] == 0xe0 ||    // jmp eax
        pbCode[0] == 0xc2 ||    // ret +imm8
        pbCode[0] == 0xc3 ||    // ret
        pbCode[0] == 0xcc) {    // brk
        return TRUE;
    }
    else if (pbCode[0] == 0xf3 && pbCode[1] == 0xc3) {  // rep ret
        return TRUE;
    }
    else if (pbCode[0] == 0xff && pbCode[1] == 0x25) {  // jmp [+imm32]
        return TRUE;
    }
    else if ((pbCode[0] == 0x26 ||      // jmp es:
              pbCode[0] == 0x2e ||      // jmp cs:
              pbCode[0] == 0x36 ||      // jmp ss:
              pbCode[0] == 0x3e ||      // jmp ds:
              pbCode[0] == 0x64 ||      // jmp fs:
              pbCode[0] == 0x65) &&     // jmp gs:
             pbCode[1] == 0xff &&       // jmp [+imm32]
             pbCode[2] == 0x25) {
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // 1-byte through 11-byte NOPs.
    if (pbCode[0] == 0x90) {
        return 1;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x90) {
        return 2;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x00) {
        return 3;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x40 &&
        pbCode[3] == 0x00) {
        return 4;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x44 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00) {
        return 5;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x44 && pbCode[4] == 0x00 && pbCode[5] == 0x00) {
        return 6;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x80 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00) {
        return 7;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x84 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00) {
        return 8;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x84 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00) {
        return 9;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x0F &&
        pbCode[3] == 0x1F && pbCode[4] == 0x84 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00) {
        return 10;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x66 &&
        pbCode[3] == 0x0F && pbCode[4] == 0x1F && pbCode[5] == 0x84 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00 && pbCode[10] == 0x00) {
        return 11;
    }

    // int 3.
    if (pbCode[0] == 0xcc) {
        return 1;
    }
    return 0;
}

#endif // DETOURS_X64

//////////////////////////////////////////////////////////////////////// IA64.
//
#ifdef DETOURS_IA64

struct _DETOUR_TRAMPOLINE
{
    // On the IA64, a trampoline is used for both incoming and outgoing calls.
    //
    // The trampoline contains the following bundles for the outgoing call:
    //      movl gp=target_gp;
    //      <relocated target bundle>
    //      brl  target_code;
    //
    // The trampoline contains the following bundles for the incoming call:
    //      alloc  r41=ar.pfs, b, 0, 8, 0
    //      mov    r40=rp
    //
    //      adds   r50=0, r39
    //      adds   r49=0, r38
    //      adds   r48=0, r37 ;;
    //
    //      adds   r47=0, r36
    //      adds   r46=0, r35
    //      adds   r45=0, r34
    //
    //      adds   r44=0, r33
    //      adds   r43=0, r32
    //      adds   r42=0, gp ;;
    //
    //      movl   gp=ffffffff`ffffffff ;;
    //
    //      brl.call.sptk.few rp=disas!TestCodes+20e0 (00000000`00404ea0) ;;
    //
    //      adds   gp=0, r42
    //      mov    rp=r40, +0 ;;
    //      mov.i  ar.pfs=r41
    //
    //      br.ret.sptk.many rp ;;
    //
    // This way, we only have to relocate a single bundle.
    //
    // The complicated incoming trampoline is required because we have to
    // create an additional stack frame so that we save and restore the gp.
    // We must do this because gp is a caller-saved register, but not saved
    // if the caller thinks the target is in the same DLL, which changes
    // when we insert a detour.
    //
    DETOUR_IA64_BUNDLE  bMovlTargetGp;  // Bundle which sets target GP
    BYTE                rbCode[sizeof(DETOUR_IA64_BUNDLE)]; // moved bundle.
    DETOUR_IA64_BUNDLE  bBrlRemainEip;  // Brl to pbRemain
    // This must be adjacent to bBranchIslands.

    // Each instruction in the moved bundle could be a IP-relative chk or branch or call.
    // Any such instructions are changed to point to a brl in bBranchIslands.
    // This must be adjacent to bBrlRemainEip -- see "pbPool".
    DETOUR_IA64_BUNDLE bBranchIslands[DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE];

    // Target of brl inserted in target function
    DETOUR_IA64_BUNDLE  bAllocFrame;    // alloc frame
    DETOUR_IA64_BUNDLE  bSave37to39;    // save r37, r38, r39.
    DETOUR_IA64_BUNDLE  bSave34to36;    // save r34, r35, r36.
    DETOUR_IA64_BUNDLE  bSaveGPto33;    // save gp, r32, r33.
    DETOUR_IA64_BUNDLE  bMovlDetourGp;  // set detour GP.
    DETOUR_IA64_BUNDLE  bCallDetour;    // call detour.
    DETOUR_IA64_BUNDLE  bPopFrameGp;    // pop frame and restore gp.
    DETOUR_IA64_BUNDLE  bReturn;        // return to caller.

    PLABEL_DESCRIPTOR   pldTrampoline;

    BYTE                rbRestore[sizeof(DETOUR_IA64_BUNDLE)]; // original target bundle.
    BYTE                cbRestore;      // size of original target code.
    BYTE                cbCode;         // size of moved target code.
    _DETOUR_ALIGN       rAlign[14];     // instruction alignment array.
    PBYTE               pbRemain;       // first instruction after moved code. [free list]
    PBYTE               pbDetour;       // first instruction of detour function.
    PPLABEL_DESCRIPTOR  ppldDetour;     // [pbDetour,gpDetour]
    PPLABEL_DESCRIPTOR  ppldTarget;     // [pbTarget,gpDetour]
};

C_ASSERT(sizeof(DETOUR_IA64_BUNDLE) == 16);
C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 256 + DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE * 16);

enum {
    SIZE_OF_JMP = sizeof(DETOUR_IA64_BUNDLE)
};

inline PBYTE detour_skip_jmp(PBYTE pPointer, PVOID *ppGlobals)
{
    PBYTE pGlobals = NULL;
    PBYTE pbCode = NULL;

    if (pPointer != NULL) {
        PPLABEL_DESCRIPTOR ppld = (PPLABEL_DESCRIPTOR)pPointer;
        pbCode = (PBYTE)ppld->EntryPoint;
        pGlobals = (PBYTE)ppld->GlobalPointer;
    }
    if (ppGlobals != NULL) {
        *ppGlobals = pGlobals;
    }
    if (pbCode == NULL) {
        return NULL;
    }

    DETOUR_IA64_BUNDLE *pb = (DETOUR_IA64_BUNDLE *)pbCode;

    // IA64 Local Import Jumps look like:
    //      addl   r2=ffffffff`ffe021c0, gp ;;
    //      ld8    r2=[r2]
    //      nop.i  0 ;;
    //
    //      ld8    r3=[r2], 8 ;;
    //      ld8    gp=[r2]
    //      mov    b6=r3, +0
    //
    //      nop.m  0
    //      nop.i  0
    //      br.cond.sptk.few b6
    //

    //                     002024000200100b
    if ((pb[0].wide[0] & 0xfffffc000603ffff) == 0x002024000200100b &&
        pb[0].wide[1] == 0x0004000000203008 &&
        pb[1].wide[0] == 0x001014180420180a &&
        pb[1].wide[1] == 0x07000830c0203008 &&
        pb[2].wide[0] == 0x0000000100000010 &&
        pb[2].wide[1] == 0x0080006000000200) {

        ULONG64 offset =
            ((pb[0].wide[0] & 0x0000000001fc0000) >> 18) |  // imm7b
            ((pb[0].wide[0] & 0x000001ff00000000) >> 25) |  // imm9d
            ((pb[0].wide[0] & 0x00000000f8000000) >> 11);   // imm5c
        if (pb[0].wide[0] & 0x0000020000000000) {           // sign
            offset |= 0xffffffffffe00000;
        }
        PBYTE pbTarget = pGlobals + offset;
        DETOUR_TRACE(("%p: potential import jump, target=%p\n", pb, pbTarget));

        if (detour_is_imported(pbCode, pbTarget) && *(PBYTE*)pbTarget != NULL) {
            DETOUR_TRACE(("%p: is import jump, label=%p\n", pb, *(PBYTE *)pbTarget));

            PPLABEL_DESCRIPTOR ppld = (PPLABEL_DESCRIPTOR)*(PBYTE *)pbTarget;
            pbCode = (PBYTE)ppld->EntryPoint;
            pGlobals = (PBYTE)ppld->GlobalPointer;
            if (ppGlobals != NULL) {
                *ppGlobals = pGlobals;
            }
        }
    }
    return pbCode;
}


inline void detour_find_jmp_bounds(PBYTE pbCode,
                                   PDETOUR_TRAMPOLINE *ppLower,
                                   PDETOUR_TRAMPOLINE *ppUpper)
{
    (void)pbCode;
    *ppLower = (PDETOUR_TRAMPOLINE)(ULONG_PTR)0x0000000000080000;
    *ppUpper = (PDETOUR_TRAMPOLINE)(ULONG_PTR)0xfffffffffff80000;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    // Routine not needed on IA64.
    (void)pbCode;
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // Routine not needed on IA64.
    (void)pbCode;
    return 0;
}

#endif // DETOURS_IA64

#ifdef DETOURS_ARM

struct _DETOUR_TRAMPOLINE
{
    // A Thumb-2 instruction can be 2 or 4 bytes long.
    BYTE            rbCode[62];     // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[22];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 104);

enum {
    SIZE_OF_JMP = 8
};

inline PBYTE align4(PBYTE pValue)
{
    return (PBYTE)(((ULONG)pValue) & ~(ULONG)3u);
}

inline ULONG fetch_thumb_opcode(PBYTE pbCode)
{
    ULONG Opcode = *(UINT16 *)&pbCode[0];
    if (Opcode >= 0xe800) {
        Opcode = (Opcode << 16) | *(UINT16 *)&pbCode[2];
    }
    return Opcode;
}

inline void write_thumb_opcode(PBYTE &pbCode, ULONG Opcode)
{
    if (Opcode >= 0x10000) {
        *((UINT16*&)pbCode)++ = Opcode >> 16;
    }
    *((UINT16*&)pbCode)++ = (UINT16)Opcode;
}

PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE *ppPool, PBYTE pbJmpVal)
{
    PBYTE pbLiteral;
    if (ppPool != NULL) {
        *ppPool = *ppPool - 4;
        pbLiteral = *ppPool;
    }
    else {
        pbLiteral = align4(pbCode + 6);
    }

    *((PBYTE*&)pbLiteral) = DETOURS_PBYTE_TO_PFUNC(pbJmpVal);
    LONG delta = pbLiteral - align4(pbCode + 4);

    write_thumb_opcode(pbCode, 0xf8dff000 | delta);     // LDR PC,[PC+n]

    if (ppPool == NULL) {
        if (((ULONG)pbCode & 2) != 0) {
            write_thumb_opcode(pbCode, 0xdefe);         // BREAK
        }
        pbCode += 4;
    }
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit) {
        write_thumb_opcode(pbCode, 0xdefe);
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == NULL) {
        return NULL;
    }
    if (ppGlobals != NULL) {
        *ppGlobals = NULL;
    }

    // Skip over the import jump if there is one.
    pbCode = (PBYTE)DETOURS_PFUNC_TO_PBYTE(pbCode);
    ULONG Opcode = fetch_thumb_opcode(pbCode);

    if ((Opcode & 0xfbf08f00) == 0xf2400c00) {          // movw r12,#xxxx
        ULONG Opcode2 = fetch_thumb_opcode(pbCode+4);

        if ((Opcode2 & 0xfbf08f00) == 0xf2c00c00) {      // movt r12,#xxxx
            ULONG Opcode3 = fetch_thumb_opcode(pbCode+8);
            if (Opcode3 == 0xf8dcf000) {                 // ldr  pc,[r12]
                PBYTE pbTarget = (PBYTE)(((Opcode2 << 12) & 0xf7000000) |
                                         ((Opcode2 <<  1) & 0x08000000) |
                                         ((Opcode2 << 16) & 0x00ff0000) |
                                         ((Opcode  >>  4) & 0x0000f700) |
                                         ((Opcode  >> 15) & 0x00000800) |
                                         ((Opcode  >>  0) & 0x000000ff));
                if (detour_is_imported(pbCode, pbTarget)) {
                    PBYTE pbNew = *(PBYTE *)pbTarget;
                    pbNew = DETOURS_PFUNC_TO_PBYTE(pbNew);
                    DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                    return pbNew;
                }
            }
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
                                   PDETOUR_TRAMPOLINE *ppLower,
                                   PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", (PVOID)lo, pbCode, (PVOID)hi));

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}


inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    ULONG Opcode = fetch_thumb_opcode(pbCode);
    if ((Opcode & 0xffffff87) == 0x4700 ||          // bx <reg>
        (Opcode & 0xf800d000) == 0xf0009000) {      // b <imm20>
        return TRUE;
    }
    if ((Opcode & 0xffff8000) == 0xe8bd8000) {      // pop {...,pc}
        __debugbreak();
        return TRUE;
    }
    if ((Opcode & 0xffffff00) == 0x0000bd00) {      // pop {...,pc}
        __debugbreak();
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    if (pbCode[0] == 0x00 && pbCode[1] == 0xbf) { // nop.
        return 2;
    }
    if (pbCode[0] == 0x00 && pbCode[1] == 0x00) { // zero-filled padding.
        return 2;
    }
    return 0;
}

#endif // DETOURS_ARM

#ifdef DETOURS_ARM64

struct _DETOUR_TRAMPOLINE
{
    // An ARM64 instruction is 4 bytes long.
    //
    // The overwrite is always composed of 3 instructions (12 bytes) which perform an indirect jump
    // using _DETOUR_TRAMPOLINE::pbDetour as the address holding the target location.
    //
    // Copied instructions can expand.
    //
    // The scheme using MovImmediate can cause an instruction
    // to grow as much as 6 times.
    // That would be Bcc or Tbz with a large address space:
    //   4 instructions to form immediate
    //   inverted tbz/bcc
    //   br
    //
    // An expansion of 4 is not uncommon -- bl/blr and small address space:
    //   3 instructions to form immediate
    //   br or brl
    //
    // A theoretical maximum for rbCode is thefore 4*4*6 + 16 = 112 (another 16 for jmp to pbRemain).
    //
    // With literals, the maximum expansion is 5, including the literals: 4*4*5 + 16 = 96.
    //
    // The number is rounded up to 128. m_rbScratchDst should match this.
    //
    BYTE            rbCode[128];    // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak[3]; // padding to make debugging easier.
    BYTE            rbRestore[24];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak[3]; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 184);

enum {
    SIZE_OF_JMP = 12
};

inline ULONG fetch_opcode(PBYTE pbCode)
{
    return *(ULONG *)pbCode;
}

inline void write_opcode(PBYTE &pbCode, ULONG Opcode)
{
    *(ULONG *)pbCode = Opcode;
    pbCode += 4;
}

struct ARM64_INDIRECT_JMP {
    struct {
        ULONG Rd : 5;
        ULONG immhi : 19;
        ULONG iop : 5;
        ULONG immlo : 2;
        ULONG op : 1;
    } ardp;

    struct {
        ULONG Rt : 5;
        ULONG Rn : 5;
        ULONG imm : 12;
        ULONG opc : 2;
        ULONG iop1 : 2;
        ULONG V : 1;
        ULONG iop2 : 3;
        ULONG size : 2;
    } ldr;

    ULONG br;
};

#pragma warning(push)
#pragma warning(disable:4201)

union ARM64_INDIRECT_IMM {
    struct {
        ULONG64 pad : 12;
        ULONG64 adrp_immlo : 2;
        ULONG64 adrp_immhi : 19;
    };

    LONG64 value;
};

#pragma warning(pop)

PBYTE detour_gen_jmp_indirect(BYTE *pbCode, ULONG64 *pbJmpVal)
{
    // adrp x17, [jmpval]
    // ldr x17, [x17, jmpval]
    // br x17

    struct ARM64_INDIRECT_JMP *pIndJmp;
    union ARM64_INDIRECT_IMM jmpIndAddr;

    jmpIndAddr.value = (((LONG64)pbJmpVal) & 0xFFFFFFFFFFFFF000) - 
                       (((LONG64)pbCode) & 0xFFFFFFFFFFFFF000);

    pIndJmp = (struct ARM64_INDIRECT_JMP *)pbCode;
    pbCode = (BYTE *)(pIndJmp + 1);

    pIndJmp->ardp.Rd = 17;
    pIndJmp->ardp.immhi = jmpIndAddr.adrp_immhi;
    pIndJmp->ardp.iop = 0x10;
    pIndJmp->ardp.immlo = jmpIndAddr.adrp_immlo;
    pIndJmp->ardp.op = 1;

    pIndJmp->ldr.Rt = 17;
    pIndJmp->ldr.Rn = 17;
    pIndJmp->ldr.imm = (((ULONG64)pbJmpVal) & 0xFFF) / 8;
    pIndJmp->ldr.opc = 1;
    pIndJmp->ldr.iop1 = 1;
    pIndJmp->ldr.V = 0;
    pIndJmp->ldr.iop2 = 7;
    pIndJmp->ldr.size = 3;

    pIndJmp->br = 0xD61F0220;

    return pbCode;
}

PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE *ppPool, PBYTE pbJmpVal)
{
    PBYTE pbLiteral;
    if (ppPool != NULL) {
        *ppPool = *ppPool - 8;
        pbLiteral = *ppPool;
    }
    else {
        pbLiteral = pbCode + 8;
    }

    *((PBYTE*&)pbLiteral) = pbJmpVal;
    LONG delta = (LONG)(pbLiteral - pbCode);

    write_opcode(pbCode, 0x58000011 | ((delta / 4) << 5));  // LDR X17,[PC+n]
    write_opcode(pbCode, 0xd61f0000 | (17 << 5));           // BR X17

    if (ppPool == NULL) {
        pbCode += 8;
    }
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit) {
        write_opcode(pbCode, 0xd4100000 | (0xf000 << 5));
    }
    return pbCode;
}

inline INT64 detour_sign_extend(UINT64 value, UINT bits)
{
    const UINT left = 64 - bits;
    const INT64 m1 = -1;
    const INT64 wide = (INT64)(value << left);
    const INT64 sign = (wide < 0) ? (m1 << left) : 0;
    return value | sign;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == NULL) {
        return NULL;
    }
    if (ppGlobals != NULL) {
        *ppGlobals = NULL;
    }

    // Skip over the import jump if there is one.
    pbCode = (PBYTE)pbCode;
    ULONG Opcode = fetch_opcode(pbCode);

    if ((Opcode & 0x9f00001f) == 0x90000010) {           // adrp  x16, IAT
        ULONG Opcode2 = fetch_opcode(pbCode + 4);

        if ((Opcode2 & 0xffe003ff) == 0xf9400210) {      // ldr   x16, [x16, IAT]
            ULONG Opcode3 = fetch_opcode(pbCode + 8);

            if (Opcode3 == 0xd61f0200) {                 // br    x16

/* https://static.docs.arm.com/ddi0487/bb/DDI0487B_b_armv8_arm.pdf
    The ADRP instruction shifts a signed, 21-bit immediate left by 12 bits, adds it to the value of the program counter with
    the bottom 12 bits cleared to zero, and then writes the result to a general-purpose register. This permits the
    calculation of the address at a 4KB aligned memory region. In conjunction with an ADD (immediate) instruction, or
    a Load/Store instruction with a 12-bit immediate offset, this allows for the calculation of, or access to, any address
    within +/- 4GB of the current PC.

PC-rel. addressing
    This section describes the encoding of the PC-rel. addressing instruction class. The encodings in this section are
    decoded from Data Processing -- Immediate on page C4-226.
    Add/subtract (immediate)
    This section describes the encoding of the Add/subtract (immediate) instruction class. The encodings in this section
    are decoded from Data Processing -- Immediate on page C4-226.
    Decode fields
    Instruction page
    op
    0 ADR
    1 ADRP

C6.2.10 ADRP
    Form PC-relative address to 4KB page adds an immediate value that is shifted left by 12 bits, to the PC value to
    form a PC-relative address, with the bottom 12 bits masked out, and writes the result to the destination register.
    ADRP <Xd>, <label>
    imm = SignExtend(immhi:immlo:Zeros(12), 64);

    31  30 29 28 27 26 25 24 23 5    4 0
    1   immlo  1  0  0  0  0  immhi  Rd
         9             0

Rd is hardcoded as 0x10 above.
Immediate is 21 signed bits split into 2 bits and 19 bits, and is scaled by 4K.
*/
                UINT64 const pageLow2 = (Opcode >> 29) & 3;
                UINT64 const pageHigh19 = (Opcode >> 5) & ~(~0ui64 << 19);
                INT64 const page = detour_sign_extend((pageHigh19 << 2) | pageLow2, 21) << 12;

/* https://static.docs.arm.com/ddi0487/bb/DDI0487B_b_armv8_arm.pdf

    C6.2.101 LDR (immediate)
    Load Register (immediate) loads a word or doubleword from memory and writes it to a register. The address that is
    used for the load is calculated from a base register and an immediate offset.
    The Unsigned offset variant scales the immediate offset value by the size of the value accessed before adding it
    to the base register value.

Unsigned offset
64-bit variant Applies when size == 11.
    31 30 29 28  27 26 25 24  23 22  21   10   9 5   4 0
     1  x  1  1   1  0  0  1   0  1  imm12      Rn    Rt
         F             9        4              200    10

That is, two low 5 bit fields are registers, hardcoded as 0x10 and 0x10 << 5 above,
then unsigned size-unscaled (8) 12-bit offset, then opcode bits 0xF94.
*/
                UINT64 const offset = ((Opcode2 >> 10) & ~(~0ui64 << 12)) << 3;

                PBYTE const pbTarget = (PBYTE)((ULONG64)pbCode & 0xfffffffffffff000ULL) + page + offset;

                if (detour_is_imported(pbCode, pbTarget)) {
                    PBYTE pbNew = *(PBYTE *)pbTarget;
                    DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                    return pbNew;
                }
            }
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
                                   PDETOUR_TRAMPOLINE *ppLower,
                                   PDETOUR_TRAMPOLINE *ppUpper)
{
    // The encoding used by detour_gen_jmp_indirect actually enables a
    // displacement of +/- 4GiB. In the future, this could be changed to
    // reflect that. For now, just reuse the x86 logic which is plenty.

    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", (PVOID)lo, pbCode, (PVOID)hi));

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_is_code_os_patched(PBYTE pbCode)
{
    // Identify whether the provided code pointer is a OS patch jump.
    // We can do this by checking if a branch (b <imm26>) is present, and if so,
    // it must be jumping to an HPAT page containing ldr <reg> [PC+PAGE_SIZE-4], br <reg>.
    ULONG Opcode = fetch_opcode(pbCode);

    if ((Opcode & 0xfc000000) != 0x14000000) {
        return FALSE;
    }
    // The branch must be jumping forward if it's going into the HPAT.
    // Check that the sign bit is cleared.
    if ((Opcode & 0x2000000) != 0) {
        return FALSE;
    }
    ULONG Delta = (ULONG)((Opcode & 0x1FFFFFF) * 4);
    PBYTE BranchTarget = pbCode + Delta;

    // Now inspect the opcodes of the code we jumped to in order to determine if it's HPAT.
    ULONG HpatOpcode1 = fetch_opcode(BranchTarget);
    ULONG HpatOpcode2 = fetch_opcode(BranchTarget + 4);

    if (HpatOpcode1 != 0x58008010) {    // ldr <reg> [PC+PAGE_SIZE]
        return FALSE;
    }
    if (HpatOpcode2 != 0xd61f0200) {    // br <reg>
        return FALSE;
    }
    return TRUE;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    ULONG Opcode = fetch_opcode(pbCode);
    // When the OS has patched a function entry point, it will incorrectly
    // appear as though the function is just a single branch instruction.
    if (detour_is_code_os_patched(pbCode)) {
        return FALSE;
    }
    if ((Opcode & 0xffbffc1f) == 0xd61f0000 ||      // ret/br <reg>
        (Opcode & 0xfc000000) == 0x14000000) {      // b <imm26>
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    if (*(ULONG *)pbCode == 0xd503201f) {   // nop.
        return 4;
    }
    if (*(ULONG *)pbCode == 0x00000000) {   // zero-filled padding.
        return 4;
    }
    return 0;
}

#endif // DETOURS_ARM64

//////////////////////////////////////////////// Trampoline Memory Management.
//
struct DETOUR_REGION
{
    ULONG               dwSignature;
    DETOUR_REGION *     pNext;  // Next region in list of regions.
    DETOUR_TRAMPOLINE * pFree;  // List of free trampolines in this region.
};
typedef DETOUR_REGION * PDETOUR_REGION;

const ULONG DETOUR_REGION_SIGNATURE = 'Rrtd';
const ULONG DETOUR_REGION_SIZE = 0x10000;
const ULONG DETOUR_TRAMPOLINES_PER_REGION = (DETOUR_REGION_SIZE
                                             / sizeof(DETOUR_TRAMPOLINE)) - 1;
static PDETOUR_REGION s_pRegions = NULL;            // List of all regions.
static PDETOUR_REGION s_pRegion = NULL;             // Default region.

static DWORD detour_writable_trampoline_regions()
{
    // Mark all of the regions as writable.
    for (PDETOUR_REGION pRegion = s_pRegions; pRegion != NULL; pRegion = pRegion->pNext) {
        DWORD dwOld;
        if (!VirtualProtect(pRegion, DETOUR_REGION_SIZE, PAGE_EXECUTE_READWRITE, &dwOld)) {
            return GetLastError();
        }
    }
    return NO_ERROR;
}

static void detour_runnable_trampoline_regions()
{
    HANDLE hProcess = GetCurrentProcess();

    // Mark all of the regions as executable.
    for (PDETOUR_REGION pRegion = s_pRegions; pRegion != NULL; pRegion = pRegion->pNext) {
        DWORD dwOld;
        VirtualProtect(pRegion, DETOUR_REGION_SIZE, PAGE_EXECUTE_READ, &dwOld);
        FlushInstructionCache(hProcess, pRegion, DETOUR_REGION_SIZE);
    }
}

static PBYTE detour_alloc_round_down_to_region(PBYTE pbTry)
{
    // WinXP64 returns free areas that aren't REGION aligned to 32-bit applications.
    ULONG_PTR extra = ((ULONG_PTR)pbTry) & (DETOUR_REGION_SIZE - 1);
    if (extra != 0) {
        pbTry -= extra;
    }
    return pbTry;
}

static PBYTE detour_alloc_round_up_to_region(PBYTE pbTry)
{
    // WinXP64 returns free areas that aren't REGION aligned to 32-bit applications.
    ULONG_PTR extra = ((ULONG_PTR)pbTry) & (DETOUR_REGION_SIZE - 1);
    if (extra != 0) {
        ULONG_PTR adjust = DETOUR_REGION_SIZE - extra;
        pbTry += adjust;
    }
    return pbTry;
}

// Starting at pbLo, try to allocate a memory region, continue until pbHi.

static PVOID detour_alloc_region_from_lo(PBYTE pbLo, PBYTE pbHi)
{
    PBYTE pbTry = detour_alloc_round_up_to_region(pbLo);

    DETOUR_TRACE((" Looking for free region in %p..%p from %p:\n", pbLo, pbHi, pbTry));

    for (; pbTry < pbHi;) {
        MEMORY_BASIC_INFORMATION mbi;

        if (pbTry >= s_pSystemRegionLowerBound && pbTry <= s_pSystemRegionUpperBound) {
            // Skip region reserved for system DLLs, but preserve address space entropy.
            pbTry += 0x08000000;
            continue;
        }

        ZeroMemory(&mbi, sizeof(mbi));
        if (!VirtualQuery(pbTry, &mbi, sizeof(mbi))) {
            break;
        }

        DETOUR_TRACE(("  Try %p => %p..%p %6lx\n",
                      pbTry,
                      mbi.BaseAddress,
                      (PBYTE)mbi.BaseAddress + mbi.RegionSize - 1,
                      mbi.State));

        if (mbi.State == MEM_FREE && mbi.RegionSize >= DETOUR_REGION_SIZE) {

            PVOID pv = VirtualAlloc(pbTry,
                                    DETOUR_REGION_SIZE,
                                    MEM_COMMIT|MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
            if (pv != NULL) {
                return pv;
            }
            else if (GetLastError() == ERROR_DYNAMIC_CODE_BLOCKED) {
                return NULL;
            }
            pbTry += DETOUR_REGION_SIZE;
        }
        else {
            pbTry = detour_alloc_round_up_to_region((PBYTE)mbi.BaseAddress + mbi.RegionSize);
        }
    }
    return NULL;
}

// Starting at pbHi, try to allocate a memory region, continue until pbLo.

static PVOID detour_alloc_region_from_hi(PBYTE pbLo, PBYTE pbHi)
{
    PBYTE pbTry = detour_alloc_round_down_to_region(pbHi - DETOUR_REGION_SIZE);

    DETOUR_TRACE((" Looking for free region in %p..%p from %p:\n", pbLo, pbHi, pbTry));

    for (; pbTry > pbLo;) {
        MEMORY_BASIC_INFORMATION mbi;

        DETOUR_TRACE(("  Try %p\n", pbTry));
        if (pbTry >= s_pSystemRegionLowerBound && pbTry <= s_pSystemRegionUpperBound) {
            // Skip region reserved for system DLLs, but preserve address space entropy.
            pbTry -= 0x08000000;
            continue;
        }

        ZeroMemory(&mbi, sizeof(mbi));
        if (!VirtualQuery(pbTry, &mbi, sizeof(mbi))) {
            break;
        }

        DETOUR_TRACE(("  Try %p => %p..%p %6lx\n",
                      pbTry,
                      mbi.BaseAddress,
                      (PBYTE)mbi.BaseAddress + mbi.RegionSize - 1,
                      mbi.State));

        if (mbi.State == MEM_FREE && mbi.RegionSize >= DETOUR_REGION_SIZE) {

            PVOID pv = VirtualAlloc(pbTry,
                                    DETOUR_REGION_SIZE,
                                    MEM_COMMIT|MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
            if (pv != NULL) {
                return pv;
            }
            else if (GetLastError() == ERROR_DYNAMIC_CODE_BLOCKED) {
                return NULL;
            }
            pbTry -= DETOUR_REGION_SIZE;
        }
        else {
            pbTry = detour_alloc_round_down_to_region((PBYTE)mbi.AllocationBase
                                                      - DETOUR_REGION_SIZE);
        }
    }
    return NULL;
}

static PVOID detour_alloc_trampoline_allocate_new(PBYTE pbTarget,
                                                  PDETOUR_TRAMPOLINE pLo,
                                                  PDETOUR_TRAMPOLINE pHi)
{
    PVOID pbTry = NULL;

    // NB: We must always also start the search at an offset from pbTarget
    //     in order to maintain ASLR entropy.

#if defined(DETOURS_64BIT)
    // Try looking 1GB below or lower.
    if (pbTry == NULL && pbTarget > (PBYTE)0x40000000) {
        pbTry = detour_alloc_region_from_hi((PBYTE)pLo, pbTarget - 0x40000000);
    }
    // Try looking 1GB above or higher.
    if (pbTry == NULL && pbTarget < (PBYTE)0xffffffff40000000) {
        pbTry = detour_alloc_region_from_lo(pbTarget + 0x40000000, (PBYTE)pHi);
    }
    // Try looking 1GB below or higher.
    if (pbTry == NULL && pbTarget > (PBYTE)0x40000000) {
        pbTry = detour_alloc_region_from_lo(pbTarget - 0x40000000, pbTarget);
    }
    // Try looking 1GB above or lower.
    if (pbTry == NULL && pbTarget < (PBYTE)0xffffffff40000000) {
        pbTry = detour_alloc_region_from_hi(pbTarget, pbTarget + 0x40000000);
    }
#endif

    // Try anything below.
    if (pbTry == NULL) {
        pbTry = detour_alloc_region_from_hi((PBYTE)pLo, pbTarget);
    }
    // try anything above.
    if (pbTry == NULL) {
        pbTry = detour_alloc_region_from_lo(pbTarget, (PBYTE)pHi);
    }

    return pbTry;
}

PVOID WINAPI DetourAllocateRegionWithinJumpBounds(_In_ LPCVOID pbTarget,
                                                  _Out_ PDWORD pcbAllocatedSize)
{
    PDETOUR_TRAMPOLINE pLo;
    PDETOUR_TRAMPOLINE pHi;
    detour_find_jmp_bounds((PBYTE)pbTarget, &pLo, &pHi);

    PVOID pbNewlyAllocated =
        detour_alloc_trampoline_allocate_new((PBYTE)pbTarget, pLo, pHi);
    if (pbNewlyAllocated == NULL) {
        DETOUR_TRACE(("Couldn't find available memory region!\n"));
        *pcbAllocatedSize = 0;
        return NULL;
    }

    *pcbAllocatedSize = DETOUR_REGION_SIZE;
    return pbNewlyAllocated;
}

BOOL WINAPI DetourIsFunctionImported(_In_ PBYTE pbCode,
                                     _In_ PBYTE pbAddress)
{
    return detour_is_imported(pbCode, pbAddress);
}

static PDETOUR_TRAMPOLINE detour_alloc_trampoline(PBYTE pbTarget)
{
    // We have to place trampolines within +/- 2GB of target.

    PDETOUR_TRAMPOLINE pLo;
    PDETOUR_TRAMPOLINE pHi;

    detour_find_jmp_bounds(pbTarget, &pLo, &pHi);

    PDETOUR_TRAMPOLINE pTrampoline = NULL;

    // Insure that there is a default region.
    if (s_pRegion == NULL && s_pRegions != NULL) {
        s_pRegion = s_pRegions;
    }

    // First check the default region for an valid free block.
    if (s_pRegion != NULL && s_pRegion->pFree != NULL &&
        s_pRegion->pFree >= pLo && s_pRegion->pFree <= pHi) {

      found_region:
        pTrampoline = s_pRegion->pFree;
        // do a last sanity check on region.
        if (pTrampoline < pLo || pTrampoline > pHi) {
            return NULL;
        }
        s_pRegion->pFree = (PDETOUR_TRAMPOLINE)pTrampoline->pbRemain;
        memset(pTrampoline, 0xcc, sizeof(*pTrampoline));
        return pTrampoline;
    }

    // Then check the existing regions for a valid free block.
    for (s_pRegion = s_pRegions; s_pRegion != NULL; s_pRegion = s_pRegion->pNext) {
        if (s_pRegion != NULL && s_pRegion->pFree != NULL &&
            s_pRegion->pFree >= pLo && s_pRegion->pFree <= pHi) {
            goto found_region;
        }
    }

    // We need to allocate a new region.

    // Round pbTarget down to 64KB block.
    // /RTCc RuntimeChecks breaks PtrToUlong.
    pbTarget = pbTarget - (ULONG)((ULONG_PTR)pbTarget & 0xffff);

    PVOID pbNewlyAllocated =
        detour_alloc_trampoline_allocate_new(pbTarget, pLo, pHi);
    if (pbNewlyAllocated != NULL) {
        s_pRegion = (DETOUR_REGION*)pbNewlyAllocated;
        s_pRegion->dwSignature = DETOUR_REGION_SIGNATURE;
        s_pRegion->pFree = NULL;
        s_pRegion->pNext = s_pRegions;
        s_pRegions = s_pRegion;
        DETOUR_TRACE(("  Allocated region %p..%p\n\n",
                      s_pRegion, ((PBYTE)s_pRegion) + DETOUR_REGION_SIZE - 1));

        // Put everything but the first trampoline on the free list.
        PBYTE pFree = NULL;
        pTrampoline = ((PDETOUR_TRAMPOLINE)s_pRegion) + 1;
        for (int i = DETOUR_TRAMPOLINES_PER_REGION - 1; i > 1; i--) {
            pTrampoline[i].pbRemain = pFree;
            pFree = (PBYTE)&pTrampoline[i];
        }
        s_pRegion->pFree = (PDETOUR_TRAMPOLINE)pFree;
        goto found_region;
    }

    DETOUR_TRACE(("Couldn't find available memory region!\n"));
    return NULL;
}

static void detour_free_trampoline(PDETOUR_TRAMPOLINE pTrampoline)
{
    PDETOUR_REGION pRegion = (PDETOUR_REGION)
        ((ULONG_PTR)pTrampoline & ~(ULONG_PTR)0xffff);

    memset(pTrampoline, 0, sizeof(*pTrampoline));
    pTrampoline->pbRemain = (PBYTE)pRegion->pFree;
    pRegion->pFree = pTrampoline;
}

static BOOL detour_is_region_empty(PDETOUR_REGION pRegion)
{
    // Stop if the region isn't a region (this would be bad).
    if (pRegion->dwSignature != DETOUR_REGION_SIGNATURE) {
        return FALSE;
    }

    PBYTE pbRegionBeg = (PBYTE)pRegion;
    PBYTE pbRegionLim  = pbRegionBeg + DETOUR_REGION_SIZE;

    // Stop if any of the trampolines aren't free.
    PDETOUR_TRAMPOLINE pTrampoline = ((PDETOUR_TRAMPOLINE)pRegion) + 1;
    for (int i = 0; i < DETOUR_TRAMPOLINES_PER_REGION; i++) {
        if (pTrampoline[i].pbRemain != NULL &&
            (pTrampoline[i].pbRemain < pbRegionBeg ||
             pTrampoline[i].pbRemain >= pbRegionLim)) {
            return FALSE;
        }
    }

    // OK, the region is empty.
    return TRUE;
}

static void detour_free_unused_trampoline_regions()
{
    PDETOUR_REGION *ppRegionBase = &s_pRegions;
    PDETOUR_REGION pRegion = s_pRegions;

    while (pRegion != NULL) {
        if (detour_is_region_empty(pRegion)) {
            *ppRegionBase = pRegion->pNext;

            VirtualFree(pRegion, 0, MEM_RELEASE);
            s_pRegion = NULL;
        }
        else {
            ppRegionBase = &pRegion->pNext;
        }
        pRegion = *ppRegionBase;
    }
}

///////////////////////////////////////////////////////// Transaction Structs.
//
struct DetourThread
{
    DetourThread *      pNext;
    HANDLE              hThread;
};

struct DetourOperation
{
    DetourOperation *   pNext;
    BOOL                fIsRemove;
    PBYTE *             ppbPointer;
    PBYTE               pbTarget;
    PDETOUR_TRAMPOLINE  pTrampoline;
    ULONG               dwPerm;
};

static BOOL                 s_fIgnoreTooSmall       = FALSE;
static BOOL                 s_fRetainRegions        = FALSE;

static LONG                 s_nPendingThreadId      = 0; // Thread owning pending transaction.
static LONG                 s_nPendingError         = NO_ERROR;
static PVOID *              s_ppPendingError        = NULL;
static DetourThread *       s_pPendingThreads       = NULL;
static DetourOperation *    s_pPendingOperations    = NULL;

//////////////////////////////////////////////////////////////////////////////
//
PVOID WINAPI DetourCodeFromPointer(_In_ PVOID pPointer,
                                   _Out_opt_ PVOID *ppGlobals)
{
    return detour_skip_jmp((PBYTE)pPointer, ppGlobals);
}

//////////////////////////////////////////////////////////// Transaction APIs.
//
BOOL WINAPI DetourSetIgnoreTooSmall(_In_ BOOL fIgnore)
{
    BOOL fPrevious = s_fIgnoreTooSmall;
    s_fIgnoreTooSmall = fIgnore;
    return fPrevious;
}

BOOL WINAPI DetourSetRetainRegions(_In_ BOOL fRetain)
{
    BOOL fPrevious = s_fRetainRegions;
    s_fRetainRegions = fRetain;
    return fPrevious;
}

PVOID WINAPI DetourSetSystemRegionLowerBound(_In_ PVOID pSystemRegionLowerBound)
{
    PVOID pPrevious = s_pSystemRegionLowerBound;
    s_pSystemRegionLowerBound = pSystemRegionLowerBound;
    return pPrevious;
}

PVOID WINAPI DetourSetSystemRegionUpperBound(_In_ PVOID pSystemRegionUpperBound)
{
    PVOID pPrevious = s_pSystemRegionUpperBound;
    s_pSystemRegionUpperBound = pSystemRegionUpperBound;
    return pPrevious;
}

LONG WINAPI DetourTransactionBegin()
{
    // Only one transaction is allowed at a time.
_Benign_race_begin_
    if (s_nPendingThreadId != 0) {
        return ERROR_INVALID_OPERATION;
    }
_Benign_race_end_

    // Make sure only one thread can start a transaction.
    if (InterlockedCompareExchange(&s_nPendingThreadId, (LONG)GetCurrentThreadId(), 0) != 0) {
        return ERROR_INVALID_OPERATION;
    }

    s_pPendingOperations = NULL;
    s_pPendingThreads = NULL;
    s_ppPendingError = NULL;

    // Make sure the trampoline pages are writable.
    s_nPendingError = detour_writable_trampoline_regions();

    return s_nPendingError;
}

LONG WINAPI DetourTransactionAbort()
{
    if (s_nPendingThreadId != (LONG)GetCurrentThreadId()) {
        return ERROR_INVALID_OPERATION;
    }

    // Restore all of the page permissions.
    for (DetourOperation *o = s_pPendingOperations; o != NULL;) {
        // We don't care if this fails, because the code is still accessible.
        DWORD dwOld;
        VirtualProtect(o->pbTarget, o->pTrampoline->cbRestore,
                       o->dwPerm, &dwOld);

        if (!o->fIsRemove) {
            if (o->pTrampoline) {
                detour_free_trampoline(o->pTrampoline);
                o->pTrampoline = NULL;
            }
        }

        DetourOperation *n = o->pNext;
        delete o;
        o = n;
    }
    s_pPendingOperations = NULL;

    // Make sure the trampoline pages are no longer writable.
    detour_runnable_trampoline_regions();

    // Resume any suspended threads.
    for (DetourThread *t = s_pPendingThreads; t != NULL;) {
        // There is nothing we can do if this fails.
        ResumeThread(t->hThread);

        DetourThread *n = t->pNext;
        delete t;
        t = n;
    }
    s_pPendingThreads = NULL;
    s_nPendingThreadId = 0;

    return NO_ERROR;
}

LONG WINAPI DetourTransactionCommit()
{
    return DetourTransactionCommitEx(NULL);
}

static BYTE detour_align_from_trampoline(PDETOUR_TRAMPOLINE pTrampoline, BYTE obTrampoline)
{
    for (LONG n = 0; n < ARRAYSIZE(pTrampoline->rAlign); n++) {
        if (pTrampoline->rAlign[n].obTrampoline == obTrampoline) {
            return pTrampoline->rAlign[n].obTarget;
        }
    }
    return 0;
}

static LONG detour_align_from_target(PDETOUR_TRAMPOLINE pTrampoline, LONG obTarget)
{
    for (LONG n = 0; n < ARRAYSIZE(pTrampoline->rAlign); n++) {
        if (pTrampoline->rAlign[n].obTarget == obTarget) {
            return pTrampoline->rAlign[n].obTrampoline;
        }
    }
    return 0;
}

LONG WINAPI DetourTransactionCommitEx(_Out_opt_ PVOID **pppFailedPointer)
{
    if (pppFailedPointer != NULL) {
        // Used to get the last error.
        *pppFailedPointer = s_ppPendingError;
    }
    if (s_nPendingThreadId != (LONG)GetCurrentThreadId()) {
        return ERROR_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we abort the whole transaction.
    if (s_nPendingError != NO_ERROR) {
        DETOUR_BREAK();
        DetourTransactionAbort();
        return s_nPendingError;
    }

    // Common variables.
    DetourOperation *o;
    DetourThread *t;
    BOOL freed = FALSE;

    // Insert or remove each of the detours.
    for (o = s_pPendingOperations; o != NULL; o = o->pNext) {
        if (o->fIsRemove) {
            CopyMemory(o->pbTarget,
                       o->pTrampoline->rbRestore,
                       o->pTrampoline->cbRestore);
#ifdef DETOURS_IA64
            *o->ppbPointer = (PBYTE)o->pTrampoline->ppldTarget;
#endif // DETOURS_IA64

#ifdef DETOURS_X86
            *o->ppbPointer = o->pbTarget;
#endif // DETOURS_X86

#ifdef DETOURS_X64
            *o->ppbPointer = o->pbTarget;
#endif // DETOURS_X64

#ifdef DETOURS_ARM
            *o->ppbPointer = DETOURS_PBYTE_TO_PFUNC(o->pbTarget);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
            *o->ppbPointer = o->pbTarget;
#endif // DETOURS_ARM
        }
        else {
            DETOUR_TRACE(("detours: pbTramp =%p, pbRemain=%p, pbDetour=%p, cbRestore=%u\n",
                          o->pTrampoline,
                          o->pTrampoline->pbRemain,
                          o->pTrampoline->pbDetour,
                          o->pTrampoline->cbRestore));

            DETOUR_TRACE(("detours: pbTarget=%p: "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x [before]\n",
                          o->pbTarget,
                          o->pbTarget[0], o->pbTarget[1], o->pbTarget[2], o->pbTarget[3],
                          o->pbTarget[4], o->pbTarget[5], o->pbTarget[6], o->pbTarget[7],
                          o->pbTarget[8], o->pbTarget[9], o->pbTarget[10], o->pbTarget[11]));

#ifdef DETOURS_IA64
            ((DETOUR_IA64_BUNDLE*)o->pbTarget)
                ->SetBrl((UINT64)&o->pTrampoline->bAllocFrame);
            *o->ppbPointer = (PBYTE)&o->pTrampoline->pldTrampoline;
#endif // DETOURS_IA64

#ifdef DETOURS_X64
            detour_gen_jmp_indirect(o->pTrampoline->rbCodeIn, &o->pTrampoline->pbDetour);
            PBYTE pbCode = detour_gen_jmp_immediate(o->pbTarget, o->pTrampoline->rbCodeIn);
            pbCode = detour_gen_brk(pbCode, o->pTrampoline->pbRemain);
            *o->ppbPointer = o->pTrampoline->rbCode;
            UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_X64

#ifdef DETOURS_X86
            PBYTE pbCode = detour_gen_jmp_immediate(o->pbTarget, o->pTrampoline->pbDetour);
            pbCode = detour_gen_brk(pbCode, o->pTrampoline->pbRemain);
            *o->ppbPointer = o->pTrampoline->rbCode;
            UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_X86

#ifdef DETOURS_ARM
            PBYTE pbCode = detour_gen_jmp_immediate(o->pbTarget, NULL, o->pTrampoline->pbDetour);
            pbCode = detour_gen_brk(pbCode, o->pTrampoline->pbRemain);
            *o->ppbPointer = DETOURS_PBYTE_TO_PFUNC(o->pTrampoline->rbCode);
            UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
            PBYTE pbCode = detour_gen_jmp_indirect(o->pbTarget, (ULONG64*)&(o->pTrampoline->pbDetour));
            pbCode = detour_gen_brk(pbCode, o->pTrampoline->pbRemain);
            *o->ppbPointer = o->pTrampoline->rbCode;
            UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_ARM64

            DETOUR_TRACE(("detours: pbTarget=%p: "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x [after]\n",
                          o->pbTarget,
                          o->pbTarget[0], o->pbTarget[1], o->pbTarget[2], o->pbTarget[3],
                          o->pbTarget[4], o->pbTarget[5], o->pbTarget[6], o->pbTarget[7],
                          o->pbTarget[8], o->pbTarget[9], o->pbTarget[10], o->pbTarget[11]));

            DETOUR_TRACE(("detours: pbTramp =%p: "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x "
                          "%02x %02x %02x %02x\n",
                          o->pTrampoline,
                          o->pTrampoline->rbCode[0], o->pTrampoline->rbCode[1],
                          o->pTrampoline->rbCode[2], o->pTrampoline->rbCode[3],
                          o->pTrampoline->rbCode[4], o->pTrampoline->rbCode[5],
                          o->pTrampoline->rbCode[6], o->pTrampoline->rbCode[7],
                          o->pTrampoline->rbCode[8], o->pTrampoline->rbCode[9],
                          o->pTrampoline->rbCode[10], o->pTrampoline->rbCode[11]));

#ifdef DETOURS_IA64
            DETOUR_TRACE(("\n"));
            DETOUR_TRACE(("detours:  &pldTrampoline  =%p\n",
                          &o->pTrampoline->pldTrampoline));
            DETOUR_TRACE(("detours:  &bMovlTargetGp  =%p [%p]\n",
                          &o->pTrampoline->bMovlTargetGp,
                          o->pTrampoline->bMovlTargetGp.GetMovlGp()));
            DETOUR_TRACE(("detours:  &rbCode         =%p [%p]\n",
                          &o->pTrampoline->rbCode,
                          ((DETOUR_IA64_BUNDLE&)o->pTrampoline->rbCode).GetBrlTarget()));
            DETOUR_TRACE(("detours:  &bBrlRemainEip  =%p [%p]\n",
                          &o->pTrampoline->bBrlRemainEip,
                          o->pTrampoline->bBrlRemainEip.GetBrlTarget()));
            DETOUR_TRACE(("detours:  &bMovlDetourGp  =%p [%p]\n",
                          &o->pTrampoline->bMovlDetourGp,
                          o->pTrampoline->bMovlDetourGp.GetMovlGp()));
            DETOUR_TRACE(("detours:  &bBrlDetourEip  =%p [%p]\n",
                          &o->pTrampoline->bCallDetour,
                          o->pTrampoline->bCallDetour.GetBrlTarget()));
            DETOUR_TRACE(("detours:  pldDetour       =%p [%p]\n",
                          o->pTrampoline->ppldDetour->EntryPoint,
                          o->pTrampoline->ppldDetour->GlobalPointer));
            DETOUR_TRACE(("detours:  pldTarget       =%p [%p]\n",
                          o->pTrampoline->ppldTarget->EntryPoint,
                          o->pTrampoline->ppldTarget->GlobalPointer));
            DETOUR_TRACE(("detours:  pbRemain        =%p\n",
                          o->pTrampoline->pbRemain));
            DETOUR_TRACE(("detours:  pbDetour        =%p\n",
                          o->pTrampoline->pbDetour));
            DETOUR_TRACE(("\n"));
#endif // DETOURS_IA64
        }
    }

#undef DETOURS_EIP
#undef DETOURS_CONTEXT_FLAGS

#ifdef DETOURS_X86
#define DETOURS_EIP         Eip
#define DETOURS_CONTEXT_FLAGS CONTEXT_CONTROL
#endif // DETOURS_X86

#ifdef DETOURS_X64
#define DETOURS_EIP         Rip
#define DETOURS_CONTEXT_FLAGS (CONTEXT_CONTROL | CONTEXT_INTEGER)
#endif // DETOURS_X64

#ifdef DETOURS_IA64
#define DETOURS_EIP         StIIP
#define DETOURS_CONTEXT_FLAGS CONTEXT_CONTROL
#endif // DETOURS_IA64

#ifdef DETOURS_ARM
#define DETOURS_EIP         Pc
#define DETOURS_CONTEXT_FLAGS CONTEXT_CONTROL
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
#define DETOURS_EIP         Pc
#define DETOURS_CONTEXT_FLAGS (CONTEXT_CONTROL | CONTEXT_INTEGER)
#endif // DETOURS_ARM64

typedef ULONG_PTR DETOURS_EIP_TYPE;

    // Update any suspended threads.
    for (t = s_pPendingThreads; t != NULL; t = t->pNext) {
        CONTEXT cxt;
        cxt.ContextFlags = DETOURS_CONTEXT_FLAGS;
        if (GetThreadContext(t->hThread, &cxt)) {
            for (o = s_pPendingOperations; o != NULL; o = o->pNext) {
                if (o->fIsRemove) {
                    if (cxt.DETOURS_EIP >= (DETOURS_EIP_TYPE)(ULONG_PTR)o->pTrampoline &&
                        cxt.DETOURS_EIP < (DETOURS_EIP_TYPE)((ULONG_PTR)o->pTrampoline
                                                             + sizeof(*o->pTrampoline))
                       ) {

                        cxt.DETOURS_EIP = (DETOURS_EIP_TYPE)
                            ((ULONG_PTR)o->pbTarget
                             + detour_align_from_trampoline(o->pTrampoline,
                                                            (BYTE)(cxt.DETOURS_EIP
                                                                   - (DETOURS_EIP_TYPE)(ULONG_PTR)
                                                                   o->pTrampoline)));

                        SetThreadContext(t->hThread, &cxt);
                    }
                }
                else {
                    if (cxt.DETOURS_EIP >= (DETOURS_EIP_TYPE)(ULONG_PTR)o->pbTarget &&
                        cxt.DETOURS_EIP < (DETOURS_EIP_TYPE)((ULONG_PTR)o->pbTarget
                                                             + o->pTrampoline->cbRestore)
                       ) {

                        cxt.DETOURS_EIP = (DETOURS_EIP_TYPE)
                            ((ULONG_PTR)o->pTrampoline
                             + detour_align_from_target(o->pTrampoline,
                                                        (BYTE)(cxt.DETOURS_EIP
                                                               - (DETOURS_EIP_TYPE)(ULONG_PTR)
                                                               o->pbTarget)));

                        SetThreadContext(t->hThread, &cxt);
                    }
                }
            }
        }
#undef DETOURS_EIP
    }

    // Restore all of the page permissions and flush the icache.
    HANDLE hProcess = GetCurrentProcess();
    for (o = s_pPendingOperations; o != NULL;) {
        // We don't care if this fails, because the code is still accessible.
        DWORD dwOld;
        VirtualProtect(o->pbTarget, o->pTrampoline->cbRestore, o->dwPerm, &dwOld);
        FlushInstructionCache(hProcess, o->pbTarget, o->pTrampoline->cbRestore);

        if (o->fIsRemove && o->pTrampoline) {
            detour_free_trampoline(o->pTrampoline);
            o->pTrampoline = NULL;
            freed = true;
        }

        DetourOperation *n = o->pNext;
        delete o;
        o = n;
    }
    s_pPendingOperations = NULL;

    // Free any trampoline regions that are now unused.
    if (freed && !s_fRetainRegions) {
        detour_free_unused_trampoline_regions();
    }

    // Make sure the trampoline pages are no longer writable.
    detour_runnable_trampoline_regions();

    // Resume any suspended threads.
    for (t = s_pPendingThreads; t != NULL;) {
        // There is nothing we can do if this fails.
        ResumeThread(t->hThread);

        DetourThread *n = t->pNext;
        delete t;
        t = n;
    }
    s_pPendingThreads = NULL;
    s_nPendingThreadId = 0;

    if (pppFailedPointer != NULL) {
        *pppFailedPointer = s_ppPendingError;
    }

    return s_nPendingError;
}

LONG WINAPI DetourUpdateThread(_In_ HANDLE hThread)
{
    LONG error;

    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != NO_ERROR) {
        return s_nPendingError;
    }

    // Silently (and safely) drop any attempt to suspend our own thread.
    if (hThread == GetCurrentThread()) {
        return NO_ERROR;
    }

    DetourThread *t = new NOTHROW DetourThread;
    if (t == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
      fail:
        if (t != NULL) {
            delete t;
            t = NULL;
        }
        s_nPendingError = error;
        s_ppPendingError = NULL;
        DETOUR_BREAK();
        return error;
    }

    if (SuspendThread(hThread) == (DWORD)-1) {
        error = GetLastError();
        DETOUR_BREAK();
        goto fail;
    }

    t->hThread = hThread;
    t->pNext = s_pPendingThreads;
    s_pPendingThreads = t;

    return NO_ERROR;
}

///////////////////////////////////////////////////////////// Transacted APIs.
//
LONG WINAPI DetourAttach(_Inout_ PVOID *ppPointer,
                         _In_ PVOID pDetour)
{
    return DetourAttachEx(ppPointer, pDetour, NULL, NULL, NULL);
}

LONG WINAPI DetourAttachEx(_Inout_ PVOID *ppPointer,
                           _In_ PVOID pDetour,
                           _Out_opt_ PDETOUR_TRAMPOLINE *ppRealTrampoline,
                           _Out_opt_ PVOID *ppRealTarget,
                           _Out_opt_ PVOID *ppRealDetour)
{
    LONG error = NO_ERROR;

    if (ppRealTrampoline != NULL) {
        *ppRealTrampoline = NULL;
    }
    if (ppRealTarget != NULL) {
        *ppRealTarget = NULL;
    }
    if (ppRealDetour != NULL) {
        *ppRealDetour = NULL;
    }
    if (pDetour == NULL) {
        DETOUR_TRACE(("empty detour\n"));
        return ERROR_INVALID_PARAMETER;
    }

    if (s_nPendingThreadId != (LONG)GetCurrentThreadId()) {
        DETOUR_TRACE(("transaction conflict with thread id=%ld\n", s_nPendingThreadId));
        return ERROR_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != NO_ERROR) {
        DETOUR_TRACE(("pending transaction error=%ld\n", s_nPendingError));
        return s_nPendingError;
    }

    if (ppPointer == NULL) {
        DETOUR_TRACE(("ppPointer is null\n"));
        return ERROR_INVALID_HANDLE;
    }
    if (*ppPointer == NULL) {
        error = ERROR_INVALID_HANDLE;
        s_nPendingError = error;
        s_ppPendingError = ppPointer;
        DETOUR_TRACE(("*ppPointer is null (ppPointer=%p)\n", ppPointer));
        DETOUR_BREAK();
        return error;
    }

    PBYTE pbTarget = (PBYTE)*ppPointer;
    PDETOUR_TRAMPOLINE pTrampoline = NULL;
    DetourOperation *o = NULL;

#ifdef DETOURS_IA64
    PPLABEL_DESCRIPTOR ppldDetour = (PPLABEL_DESCRIPTOR)pDetour;
    PPLABEL_DESCRIPTOR ppldTarget = (PPLABEL_DESCRIPTOR)pbTarget;
    PVOID pDetourGlobals = NULL;
    PVOID pTargetGlobals = NULL;

    pDetour = (PBYTE)DetourCodeFromPointer(ppldDetour, &pDetourGlobals);
    pbTarget = (PBYTE)DetourCodeFromPointer(ppldTarget, &pTargetGlobals);
    DETOUR_TRACE(("  ppldDetour=%p, code=%p [gp=%p]\n",
                  ppldDetour, pDetour, pDetourGlobals));
    DETOUR_TRACE(("  ppldTarget=%p, code=%p [gp=%p]\n",
                  ppldTarget, pbTarget, pTargetGlobals));
#else // DETOURS_IA64
#if defined(_M_ARM64EC)
    if (RtlIsEcCode(reinterpret_cast<DWORD64>(*ppPointer))) {
        DETOUR_TRACE(("*ppPointer is an Arm64EC address (ppPointer=%p). "
                      "An Arm64EC address cannot be legitimately detoured with an x64 jmp. "
                      "Mark the target function with __declspec(hybrid_patchable) to make it detour-able. "
                      "We still allow an Arm64EC function to be detoured with an x64 jmp to make it easy (crash) to debug.\n", ppPointer));
        DETOUR_BREAK();
    }
#endif
    pbTarget = (PBYTE)DetourCodeFromPointer(pbTarget, NULL);
    pDetour = DetourCodeFromPointer(pDetour, NULL);
#endif // !DETOURS_IA64

    // Don't follow a jump if its destination is the target function.
    // This happens when the detour does nothing other than call the target.
    if (pDetour == (PVOID)pbTarget) {
        if (s_fIgnoreTooSmall) {
            goto stop;
        }
        else {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (ppRealTarget != NULL) {
        *ppRealTarget = pbTarget;
    }
    if (ppRealDetour != NULL) {
        *ppRealDetour = pDetour;
    }

    o = new NOTHROW DetourOperation;
    if (o == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
      fail:
        s_nPendingError = error;
        DETOUR_BREAK();
      stop:
        if (pTrampoline != NULL) {
            detour_free_trampoline(pTrampoline);
            pTrampoline = NULL;
            if (ppRealTrampoline != NULL) {
                *ppRealTrampoline = NULL;
            }
        }
        if (o != NULL) {
            delete o;
            o = NULL;
        }
        if (ppRealDetour != NULL) {
            *ppRealDetour = NULL;
        }
        if (ppRealTarget != NULL) {
            *ppRealTarget = NULL;
        }
        s_ppPendingError = ppPointer;
        return error;
    }

    pTrampoline = detour_alloc_trampoline(pbTarget);
    if (pTrampoline == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        DETOUR_BREAK();
        goto fail;
    }

    if (ppRealTrampoline != NULL) {
        *ppRealTrampoline = pTrampoline;
    }

    DETOUR_TRACE(("detours: pbTramp=%p, pDetour=%p\n", pTrampoline, pDetour));

    memset(pTrampoline->rAlign, 0, sizeof(pTrampoline->rAlign));

    // Determine the number of movable target instructions.
    PBYTE pbSrc = pbTarget;
    PBYTE pbTrampoline = pTrampoline->rbCode;
#ifdef DETOURS_IA64
    PBYTE pbPool = (PBYTE)(&pTrampoline->bBranchIslands + 1);
#else
    PBYTE pbPool = pbTrampoline + sizeof(pTrampoline->rbCode);
#endif
    ULONG cbTarget = 0;
    ULONG cbJump = SIZE_OF_JMP;
    ULONG nAlign = 0;

#ifdef DETOURS_ARM
    // On ARM, we need an extra instruction when the function isn't 32-bit aligned.
    // Check if the existing code is another detour (or at least a similar
    // "ldr pc, [PC+0]" jump.
    if ((ULONG)pbTarget & 2) {
        cbJump += 2;

        ULONG op = fetch_thumb_opcode(pbSrc);
        if (op == 0xbf00) {
            op = fetch_thumb_opcode(pbSrc + 2);
            if (op == 0xf8dff000) { // LDR PC,[PC]
                *((PUSHORT&)pbTrampoline)++ = *((PUSHORT&)pbSrc)++;
                *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
                *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
                cbTarget = (LONG)(pbSrc - pbTarget);
                // We will fall through the "while" because cbTarget is now >= cbJump.
            }
        }
    }
    else {
        ULONG op = fetch_thumb_opcode(pbSrc);
        if (op == 0xf8dff000) { // LDR PC,[PC]
            *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
            *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
            cbTarget = (LONG)(pbSrc - pbTarget);
            // We will fall through the "while" because cbTarget is now >= cbJump.
        }
    }
#endif

    while (cbTarget < cbJump) {
        PBYTE pbOp = pbSrc;
        LONG lExtra = 0;

        DETOUR_TRACE((" DetourCopyInstruction(%p,%p)\n",
                      pbTrampoline, pbSrc));
        pbSrc = (PBYTE)
            DetourCopyInstruction(pbTrampoline, (PVOID*)&pbPool, pbSrc, NULL, &lExtra);
        DETOUR_TRACE((" DetourCopyInstruction() = %p (%d bytes)\n",
                      pbSrc, (int)(pbSrc - pbOp)));
        pbTrampoline += (pbSrc - pbOp) + lExtra;
        cbTarget = (LONG)(pbSrc - pbTarget);
        pTrampoline->rAlign[nAlign].obTarget = cbTarget;
        pTrampoline->rAlign[nAlign].obTrampoline = pbTrampoline - pTrampoline->rbCode;
        nAlign++;

        if (nAlign >= ARRAYSIZE(pTrampoline->rAlign)) {
            break;
        }

        if (detour_does_code_end_function(pbOp)) {
            break;
        }
    }

    // Consume, but don't duplicate padding if it is needed and available.
    while (cbTarget < cbJump) {
        LONG cFiller = detour_is_code_filler(pbSrc);
        if (cFiller == 0) {
            break;
        }

        pbSrc += cFiller;
        cbTarget = (LONG)(pbSrc - pbTarget);
    }

#if DETOUR_DEBUG
    {
        DETOUR_TRACE((" detours: rAlign ["));
        LONG n = 0;
        for (n = 0; n < ARRAYSIZE(pTrampoline->rAlign); n++) {
            if (pTrampoline->rAlign[n].obTarget == 0 &&
                pTrampoline->rAlign[n].obTrampoline == 0) {
                break;
            }
            DETOUR_TRACE((" %u/%u",
                          pTrampoline->rAlign[n].obTarget,
                          pTrampoline->rAlign[n].obTrampoline
                          ));

        }
        DETOUR_TRACE((" ]\n"));
    }
#endif

    if (cbTarget < cbJump || nAlign > ARRAYSIZE(pTrampoline->rAlign)) {
        // Too few instructions.

        error = ERROR_INVALID_BLOCK;
        if (s_fIgnoreTooSmall) {
            goto stop;
        }
        else {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (pbTrampoline > pbPool) {
        __debugbreak();
    }

    pTrampoline->cbCode = (BYTE)(pbTrampoline - pTrampoline->rbCode);
    pTrampoline->cbRestore = (BYTE)cbTarget;
    CopyMemory(pTrampoline->rbRestore, pbTarget, cbTarget);

#if !defined(DETOURS_IA64)
    if (cbTarget > sizeof(pTrampoline->rbCode) - cbJump) {
        // Too many instructions.
        error = ERROR_INVALID_HANDLE;
        DETOUR_BREAK();
        goto fail;
    }
#endif // !DETOURS_IA64

    pTrampoline->pbRemain = pbTarget + cbTarget;
    pTrampoline->pbDetour = (PBYTE)pDetour;

#ifdef DETOURS_IA64
    pTrampoline->ppldDetour = ppldDetour;
    pTrampoline->ppldTarget = ppldTarget;
    pTrampoline->pldTrampoline.EntryPoint = (UINT64)&pTrampoline->bMovlTargetGp;
    pTrampoline->pldTrampoline.GlobalPointer = (UINT64)pDetourGlobals;

    ((DETOUR_IA64_BUNDLE *)pTrampoline->rbCode)->SetStop();

    pTrampoline->bMovlTargetGp.SetMovlGp((UINT64)pTargetGlobals);
    pTrampoline->bBrlRemainEip.SetBrl((UINT64)pTrampoline->pbRemain);

    // Alloc frame:      alloc r41=ar.pfs,11,0,8,0; mov r40=rp
    pTrampoline->bAllocFrame.wide[0] = 0x00000580164d480c;
    pTrampoline->bAllocFrame.wide[1] = 0x00c4000500000200;
    // save r36, r37, r38.
    pTrampoline->bSave37to39.wide[0] = 0x031021004e019001;
    pTrampoline->bSave37to39.wide[1] = 0x8401280600420098;
    // save r34,r35,r36: adds r47=0,r36; adds r46=0,r35; adds r45=0,r34
    pTrampoline->bSave34to36.wide[0] = 0x02e0210048017800;
    pTrampoline->bSave34to36.wide[1] = 0x84011005a042008c;
    // save gp,r32,r33"  adds r44=0,r33; adds r43=0,r32; adds r42=0,gp ;;
    pTrampoline->bSaveGPto33.wide[0] = 0x02b0210042016001;
    pTrampoline->bSaveGPto33.wide[1] = 0x8400080540420080;
    // set detour GP.
    pTrampoline->bMovlDetourGp.SetMovlGp((UINT64)pDetourGlobals);
    // call detour:      brl.call.sptk.few rp=detour ;;
    pTrampoline->bCallDetour.wide[0] = 0x0000000100000005;
    pTrampoline->bCallDetour.wide[1] = 0xd000001000000000;
    pTrampoline->bCallDetour.SetBrlTarget((UINT64)pDetour);
    // pop frame & gp:   adds gp=0,r42; mov rp=r40,+0;; mov.i ar.pfs=r41
    pTrampoline->bPopFrameGp.wide[0] = 0x4000210054000802;
    pTrampoline->bPopFrameGp.wide[1] = 0x00aa029000038005;
    // return to caller: br.ret.sptk.many rp ;;
    pTrampoline->bReturn.wide[0] = 0x0000000100000019;
    pTrampoline->bReturn.wide[1] = 0x0084000880000200;

    DETOUR_TRACE(("detours: &bMovlTargetGp=%p\n", &pTrampoline->bMovlTargetGp));
    DETOUR_TRACE(("detours: &bMovlDetourGp=%p\n", &pTrampoline->bMovlDetourGp));
#endif // DETOURS_IA64

    pbTrampoline = pTrampoline->rbCode + pTrampoline->cbCode;
#ifdef DETOURS_X64
    pbTrampoline = detour_gen_jmp_indirect(pbTrampoline, &pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_X64

#ifdef DETOURS_X86
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_X86

#ifdef DETOURS_ARM
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, &pbPool, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, &pbPool, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_ARM64

    (void)pbTrampoline;

    DWORD dwOld = 0;
    if (!VirtualProtect(pbTarget, cbTarget, PAGE_EXECUTE_READWRITE, &dwOld)) {
        error = GetLastError();
        DETOUR_BREAK();
        goto fail;
    }

    DETOUR_TRACE(("detours: pbTarget=%p: "
                  "%02x %02x %02x %02x "
                  "%02x %02x %02x %02x "
                  "%02x %02x %02x %02x\n",
                  pbTarget,
                  pbTarget[0], pbTarget[1], pbTarget[2], pbTarget[3],
                  pbTarget[4], pbTarget[5], pbTarget[6], pbTarget[7],
                  pbTarget[8], pbTarget[9], pbTarget[10], pbTarget[11]));
    DETOUR_TRACE(("detours: pbTramp =%p: "
                  "%02x %02x %02x %02x "
                  "%02x %02x %02x %02x "
                  "%02x %02x %02x %02x\n",
                  pTrampoline,
                  pTrampoline->rbCode[0], pTrampoline->rbCode[1],
                  pTrampoline->rbCode[2], pTrampoline->rbCode[3],
                  pTrampoline->rbCode[4], pTrampoline->rbCode[5],
                  pTrampoline->rbCode[6], pTrampoline->rbCode[7],
                  pTrampoline->rbCode[8], pTrampoline->rbCode[9],
                  pTrampoline->rbCode[10], pTrampoline->rbCode[11]));

    o->fIsRemove = FALSE;
    o->ppbPointer = (PBYTE*)ppPointer;
    o->pTrampoline = pTrampoline;
    o->pbTarget = pbTarget;
    o->dwPerm = dwOld;
    o->pNext = s_pPendingOperations;
    s_pPendingOperations = o;

    return NO_ERROR;
}

LONG WINAPI DetourDetach(_Inout_ PVOID *ppPointer,
                         _In_ PVOID pDetour)
{
    LONG error = NO_ERROR;

    if (s_nPendingThreadId != (LONG)GetCurrentThreadId()) {
        return ERROR_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != NO_ERROR) {
        return s_nPendingError;
    }

    if (pDetour == NULL) {
        return ERROR_INVALID_PARAMETER;
    }
    if (ppPointer == NULL) {
        return ERROR_INVALID_HANDLE;
    }
    if (*ppPointer == NULL) {
        error = ERROR_INVALID_HANDLE;
        s_nPendingError = error;
        s_ppPendingError = ppPointer;
        DETOUR_BREAK();
        return error;
    }

    DetourOperation *o = new NOTHROW DetourOperation;
    if (o == NULL) {
        error = ERROR_NOT_ENOUGH_MEMORY;
      fail:
        s_nPendingError = error;
        DETOUR_BREAK();
      stop:
        if (o != NULL) {
            delete o;
            o = NULL;
        }
        s_ppPendingError = ppPointer;
        return error;
    }


#ifdef DETOURS_IA64
    PPLABEL_DESCRIPTOR ppldTrampo = (PPLABEL_DESCRIPTOR)*ppPointer;
    PPLABEL_DESCRIPTOR ppldDetour = (PPLABEL_DESCRIPTOR)pDetour;
    PVOID pDetourGlobals = NULL;
    PVOID pTrampoGlobals = NULL;

    pDetour = (PBYTE)DetourCodeFromPointer(ppldDetour, &pDetourGlobals);
    PDETOUR_TRAMPOLINE pTrampoline = (PDETOUR_TRAMPOLINE)
        DetourCodeFromPointer(ppldTrampo, &pTrampoGlobals);
    DETOUR_TRACE(("  ppldDetour=%p, code=%p [gp=%p]\n",
                  ppldDetour, pDetour, pDetourGlobals));
    DETOUR_TRACE(("  ppldTrampo=%p, code=%p [gp=%p]\n",
                  ppldTrampo, pTrampoline, pTrampoGlobals));


    DETOUR_TRACE(("\n"));
    DETOUR_TRACE(("detours:  &pldTrampoline  =%p\n",
                  &pTrampoline->pldTrampoline));
    DETOUR_TRACE(("detours:  &bMovlTargetGp  =%p [%p]\n",
                  &pTrampoline->bMovlTargetGp,
                  pTrampoline->bMovlTargetGp.GetMovlGp()));
    DETOUR_TRACE(("detours:  &rbCode         =%p [%p]\n",
                  &pTrampoline->rbCode,
                  ((DETOUR_IA64_BUNDLE&)pTrampoline->rbCode).GetBrlTarget()));
    DETOUR_TRACE(("detours:  &bBrlRemainEip  =%p [%p]\n",
                  &pTrampoline->bBrlRemainEip,
                  pTrampoline->bBrlRemainEip.GetBrlTarget()));
    DETOUR_TRACE(("detours:  &bMovlDetourGp  =%p [%p]\n",
                  &pTrampoline->bMovlDetourGp,
                  pTrampoline->bMovlDetourGp.GetMovlGp()));
    DETOUR_TRACE(("detours:  &bBrlDetourEip  =%p [%p]\n",
                  &pTrampoline->bCallDetour,
                  pTrampoline->bCallDetour.GetBrlTarget()));
    DETOUR_TRACE(("detours:  pldDetour       =%p [%p]\n",
                  pTrampoline->ppldDetour->EntryPoint,
                  pTrampoline->ppldDetour->GlobalPointer));
    DETOUR_TRACE(("detours:  pldTarget       =%p [%p]\n",
                  pTrampoline->ppldTarget->EntryPoint,
                  pTrampoline->ppldTarget->GlobalPointer));
    DETOUR_TRACE(("detours:  pbRemain        =%p\n",
                  pTrampoline->pbRemain));
    DETOUR_TRACE(("detours:  pbDetour        =%p\n",
                  pTrampoline->pbDetour));
    DETOUR_TRACE(("\n"));
#else // !DETOURS_IA64
    PDETOUR_TRAMPOLINE pTrampoline =
        (PDETOUR_TRAMPOLINE)DetourCodeFromPointer(*ppPointer, NULL);
    pDetour = DetourCodeFromPointer(pDetour, NULL);
#endif // !DETOURS_IA64

    ////////////////////////////////////// Verify that Trampoline is in place.
    //
    LONG cbTarget = pTrampoline->cbRestore;
    PBYTE pbTarget = pTrampoline->pbRemain - cbTarget;
    if (cbTarget == 0 || cbTarget > sizeof(pTrampoline->rbCode)) {
        error = ERROR_INVALID_BLOCK;
        if (s_fIgnoreTooSmall) {
            goto stop;
        }
        else {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (pTrampoline->pbDetour != pDetour) {
        error = ERROR_INVALID_BLOCK;
        if (s_fIgnoreTooSmall) {
            goto stop;
        }
        else {
            DETOUR_BREAK();
            goto fail;
        }
    }

    DWORD dwOld = 0;
    if (!VirtualProtect(pbTarget, cbTarget,
                        PAGE_EXECUTE_READWRITE, &dwOld)) {
        error = GetLastError();
        DETOUR_BREAK();
        goto fail;
    }

    o->fIsRemove = TRUE;
    o->ppbPointer = (PBYTE*)ppPointer;
    o->pTrampoline = pTrampoline;
    o->pbTarget = pbTarget;
    o->dwPerm = dwOld;
    o->pNext = s_pPendingOperations;
    s_pPendingOperations = o;

    return NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////////
//
// Helpers for manipulating page protection.
//

// For reference:
//   PAGE_NOACCESS          0x01
//   PAGE_READONLY          0x02
//   PAGE_READWRITE         0x04
//   PAGE_WRITECOPY         0x08
//   PAGE_EXECUTE           0x10
//   PAGE_EXECUTE_READ      0x20
//   PAGE_EXECUTE_READWRITE 0x40
//   PAGE_EXECUTE_WRITECOPY 0x80
//   PAGE_GUARD             ...
//   PAGE_NOCACHE           ...
//   PAGE_WRITECOMBINE      ...

#define DETOUR_PAGE_EXECUTE_ALL    (PAGE_EXECUTE |              \
                                    PAGE_EXECUTE_READ |         \
                                    PAGE_EXECUTE_READWRITE |    \
                                    PAGE_EXECUTE_WRITECOPY)

#define DETOUR_PAGE_NO_EXECUTE_ALL (PAGE_NOACCESS |             \
                                    PAGE_READONLY |             \
                                    PAGE_READWRITE |            \
                                    PAGE_WRITECOPY)

#define DETOUR_PAGE_ATTRIBUTES     (~(DETOUR_PAGE_EXECUTE_ALL | DETOUR_PAGE_NO_EXECUTE_ALL))

C_ASSERT((DETOUR_PAGE_NO_EXECUTE_ALL << 4) == DETOUR_PAGE_EXECUTE_ALL);

static DWORD DetourPageProtectAdjustExecute(_In_  DWORD dwOldProtect,
                                            _In_  DWORD dwNewProtect)
//  Copy EXECUTE from dwOldProtect to dwNewProtect.
{
    bool const fOldExecute = ((dwOldProtect & DETOUR_PAGE_EXECUTE_ALL) != 0);
    bool const fNewExecute = ((dwNewProtect & DETOUR_PAGE_EXECUTE_ALL) != 0);

    if (fOldExecute && !fNewExecute) {
        dwNewProtect = ((dwNewProtect & DETOUR_PAGE_NO_EXECUTE_ALL) << 4)
            | (dwNewProtect & DETOUR_PAGE_ATTRIBUTES);
    }
    else if (!fOldExecute && fNewExecute) {
        dwNewProtect = ((dwNewProtect & DETOUR_PAGE_EXECUTE_ALL) >> 4)
            | (dwNewProtect & DETOUR_PAGE_ATTRIBUTES);
    }
    return dwNewProtect;
}

_Success_(return != FALSE)
BOOL WINAPI DetourVirtualProtectSameExecuteEx(_In_  HANDLE hProcess,
                                              _In_  PVOID pAddress,
                                              _In_  SIZE_T nSize,
                                              _In_  DWORD dwNewProtect,
                                              _Out_ PDWORD pdwOldProtect)
// Some systems do not allow executability of a page to change. This function applies
// dwNewProtect to [pAddress, nSize), but preserving the previous executability.
// This function is meant to be a drop-in replacement for some uses of VirtualProtectEx.
// When "restoring" page protection, there is no need to use this function.
{
    MEMORY_BASIC_INFORMATION mbi;

    // Query to get existing execute access.

    ZeroMemory(&mbi, sizeof(mbi));

    if (VirtualQueryEx(hProcess, pAddress, &mbi, sizeof(mbi)) == 0) {
        return FALSE;
    }
    return VirtualProtectEx(hProcess, pAddress, nSize,
                            DetourPageProtectAdjustExecute(mbi.Protect, dwNewProtect),
                            pdwOldProtect);
}

_Success_(return != FALSE)
BOOL WINAPI DetourVirtualProtectSameExecute(_In_  PVOID pAddress,
                                            _In_  SIZE_T nSize,
                                            _In_  DWORD dwNewProtect,
                                            _Out_ PDWORD pdwOldProtect)
{
    return DetourVirtualProtectSameExecuteEx(GetCurrentProcess(),
                                             pAddress, nSize, dwNewProtect, pdwOldProtect);
}

BOOL WINAPI DetourAreSameGuid(_In_ REFGUID left, _In_ REFGUID right)
{
    return
        left.Data1 == right.Data1 &&
        left.Data2 == right.Data2 &&
        left.Data3 == right.Data3 &&
        left.Data4[0] == right.Data4[0] &&
        left.Data4[1] == right.Data4[1] &&
        left.Data4[2] == right.Data4[2] &&
        left.Data4[3] == right.Data4[3] &&
        left.Data4[4] == right.Data4[4] &&
        left.Data4[5] == right.Data4[5] &&
        left.Data4[6] == right.Data4[6] &&
        left.Data4[7] == right.Data4[7];
}

//  End of File
//////////////////////////////////////////////////////////////////////////////
//
//  Detours Disassembler (disasm.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

// #define DETOUR_DEBUG 1
#define DETOURS_INTERNAL
#include <limits.h>

#if DETOURS_VERSION != 0x4c0c1   // 0xMAJORcMINORcPATCH
#error detours.h version mismatch
#endif

#undef ASSERT
#define ASSERT(x)

//////////////////////////////////////////////////////////////////////////////
//
//  Special macros to handle the case when we are building disassembler for
//  offline processing.
//


#if defined(DETOURS_X86_OFFLINE_LIBRARY) \
 || defined(DETOURS_X64_OFFLINE_LIBRARY) \
 || defined(DETOURS_ARM_OFFLINE_LIBRARY) \
 || defined(DETOURS_ARM64_OFFLINE_LIBRARY) \
 || defined(DETOURS_IA64_OFFLINE_LIBRARY)

#undef DETOURS_X64
#undef DETOURS_X86
#undef DETOURS_IA64
#undef DETOURS_ARM
#undef DETOURS_ARM64

#if defined(DETOURS_X86_OFFLINE_LIBRARY)

#define DetourCopyInstruction   DetourCopyInstructionX86
#define DetourSetCodeModule     DetourSetCodeModuleX86
#define CDetourDis              CDetourDisX86
#define DETOURS_X86

#elif defined(DETOURS_X64_OFFLINE_LIBRARY)

#if !defined(DETOURS_64BIT)
// Fix this as/if bugs are discovered.
//#error X64 disassembler can only build for 64-bit.
#endif

#define DetourCopyInstruction   DetourCopyInstructionX64
#define DetourSetCodeModule     DetourSetCodeModuleX64
#define CDetourDis              CDetourDisX64
#define DETOURS_X64

#elif defined(DETOURS_ARM_OFFLINE_LIBRARY)

#define DetourCopyInstruction   DetourCopyInstructionARM
#define DetourSetCodeModule     DetourSetCodeModuleARM
#define CDetourDis              CDetourDisARM
#define DETOURS_ARM

#elif defined(DETOURS_ARM64_OFFLINE_LIBRARY)

#define DetourCopyInstruction   DetourCopyInstructionARM64
#define DetourSetCodeModule     DetourSetCodeModuleARM64
#define CDetourDis              CDetourDisARM64
#define DETOURS_ARM64

#elif defined(DETOURS_IA64_OFFLINE_LIBRARY)

#define DetourCopyInstruction   DetourCopyInstructionIA64
#define DetourSetCodeModule     DetourSetCodeModuleIA64
#define DETOURS_IA64

#else

#error

#endif
#endif

//////////////////////////////////////////////////////////////////////////////
//
//  Function:
//      DetourCopyInstruction(PVOID pDst,
//                            PVOID *ppDstPool
//                            PVOID pSrc,
//                            PVOID *ppTarget,
//                            LONG *plExtra)
//  Purpose:
//      Copy a single instruction from pSrc to pDst.
//
//  Arguments:
//      pDst:
//          Destination address for the instruction.  May be NULL in which
//          case DetourCopyInstruction is used to measure an instruction.
//          If not NULL then the source instruction is copied to the
//          destination instruction and any relative arguments are adjusted.
//      ppDstPool:
//          Destination address for the end of the constant pool.  The
//          constant pool works backwards toward pDst.  All memory between
//          pDst and *ppDstPool must be available for use by this function.
//          ppDstPool may be NULL if pDst is NULL.
//      pSrc:
//          Source address of the instruction.
//      ppTarget:
//          Out parameter for any target instruction address pointed to by
//          the instruction.  For example, a branch or a jump insruction has
//          a target, but a load or store instruction doesn't.  A target is
//          another instruction that may be executed as a result of this
//          instruction.  ppTarget may be NULL.
//      plExtra:
//          Out parameter for the number of extra bytes needed by the
//          instruction to reach the target.  For example, lExtra = 3 if the
//          instruction had an 8-bit relative offset, but needs a 32-bit
//          relative offset.
//
//  Returns:
//      Returns the address of the next instruction (following in the source)
//      instruction.  By subtracting pSrc from the return value, the caller
//      can determinte the size of the instruction copied.
//
//  Comments:
//      By following the pTarget, the caller can follow alternate
//      instruction streams.  However, it is not always possible to determine
//      the target based on static analysis.  For example, the destination of
//      a jump relative to a register cannot be determined from just the
//      instruction stream.  The output value, pTarget, can have any of the
//      following outputs:
//          DETOUR_INSTRUCTION_TARGET_NONE:
//              The instruction has no targets.
//          DETOUR_INSTRUCTION_TARGET_DYNAMIC:
//              The instruction has a non-deterministic (dynamic) target.
//              (i.e. the jump is to an address held in a register.)
//          Address:   The instruction has the specified target.
//
//      When copying instructions, DetourCopyInstruction insures that any
//      targets remain constant.  It does so by adjusting any IP relative
//      offsets.
//

#pragma data_seg(".detourd")
#pragma const_seg(".detourc")

//////////////////////////////////////////////////// X86 and X64 Disassembler.
//
//  Includes full support for all x86 chips prior to the Pentium III, and some newer stuff.
//
#if defined(DETOURS_X64) || defined(DETOURS_X86)

class CDetourDis
{
  public:
    CDetourDis(_Out_opt_ PBYTE *ppbTarget,
               _Out_opt_ LONG *plExtra);

    PBYTE   CopyInstruction(PBYTE pbDst, PBYTE pbSrc);
    static BOOL SanityCheckSystem();
    static BOOL SetCodeModule(PBYTE pbBeg, PBYTE pbEnd, BOOL fLimitReferencesToModule);

  public:
    struct COPYENTRY;
    typedef const COPYENTRY * REFCOPYENTRY;

    typedef PBYTE (CDetourDis::* COPYFUNC)(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);

    // nFlagBits flags.
    enum {
        DYNAMIC     = 0x1u,
        ADDRESS     = 0x2u,
        NOENLARGE   = 0x4u,
        RAX         = 0x8u,
    };

    // ModR/M Flags
    enum {
        SIB         = 0x10u,
        RIP         = 0x20u,
        NOTSIB      = 0x0fu,
    };

    struct COPYENTRY
    {
        // Many of these fields are often ignored. See ENTRY_DataIgnored.
        ULONG       nFixedSize      : 4;    // Fixed size of opcode
        ULONG       nFixedSize16    : 4;    // Fixed size when 16 bit operand
        ULONG       nModOffset      : 4;    // Offset to mod/rm byte (0=none)
        ULONG       nRelOffset      : 4;    // Offset to relative target.
        ULONG       nFlagBits       : 4;    // Flags for DYNAMIC, etc.
        COPYFUNC    pfCopy;                 // Function pointer.
    };

  protected:
// These macros define common uses of nFixedSize, nFixedSize16, nModOffset, nRelOffset, nFlagBits, pfCopy.
#define ENTRY_DataIgnored           0, 0, 0, 0, 0,
#define ENTRY_CopyBytes1            { 1, 1, 0, 0, 0, &CDetourDis::CopyBytes }
#ifdef DETOURS_X64
#define ENTRY_CopyBytes1Address     { 9, 5, 0, 0, ADDRESS, &CDetourDis::CopyBytes }
#else
#define ENTRY_CopyBytes1Address     { 5, 3, 0, 0, ADDRESS, &CDetourDis::CopyBytes }
#endif
#define ENTRY_CopyBytes1Dynamic     { 1, 1, 0, 0, DYNAMIC, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2            { 2, 2, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2Jump        { ENTRY_DataIgnored &CDetourDis::CopyBytesJump }
#define ENTRY_CopyBytes2CantJump    { 2, 2, 0, 1, NOENLARGE, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2Dynamic     { 2, 2, 0, 0, DYNAMIC, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3            { 3, 3, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Dynamic     { 3, 3, 0, 0, DYNAMIC, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Or5         { 5, 3, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Or5Dynamic  { 5, 3, 0, 0, DYNAMIC, &CDetourDis::CopyBytes }// x86 only
#ifdef DETOURS_X64
#define ENTRY_CopyBytes3Or5Rax      { 5, 3, 0, 0, RAX, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Or5Target   { 5, 5, 0, 1, 0, &CDetourDis::CopyBytes }
#else
#define ENTRY_CopyBytes3Or5Rax      { 5, 3, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Or5Target   { 5, 3, 0, 1, 0, &CDetourDis::CopyBytes }
#endif
#define ENTRY_CopyBytes4            { 4, 4, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes5            { 5, 5, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes5Or7Dynamic  { 7, 5, 0, 0, DYNAMIC, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes7            { 7, 7, 0, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2Mod         { 2, 2, 1, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2ModDynamic  { 2, 2, 1, 0, DYNAMIC, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2Mod1        { 3, 3, 1, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes2ModOperand  { 6, 4, 1, 0, 0, &CDetourDis::CopyBytes }
#define ENTRY_CopyBytes3Mod         { 3, 3, 2, 0, 0, &CDetourDis::CopyBytes } // SSE3 0F 38 opcode modrm
#define ENTRY_CopyBytes3Mod1        { 4, 4, 2, 0, 0, &CDetourDis::CopyBytes } // SSE3 0F 3A opcode modrm .. imm8
#define ENTRY_CopyBytesPrefix       { ENTRY_DataIgnored &CDetourDis::CopyBytesPrefix }
#define ENTRY_CopyBytesSegment      { ENTRY_DataIgnored &CDetourDis::CopyBytesSegment }
#define ENTRY_CopyBytesRax          { ENTRY_DataIgnored &CDetourDis::CopyBytesRax }
#define ENTRY_CopyF2                { ENTRY_DataIgnored &CDetourDis::CopyF2 }
#define ENTRY_CopyF3                { ENTRY_DataIgnored &CDetourDis::CopyF3 } // 32bit x86 only
#define ENTRY_Copy0F                { ENTRY_DataIgnored &CDetourDis::Copy0F }
#define ENTRY_Copy0F78              { ENTRY_DataIgnored &CDetourDis::Copy0F78 }
#define ENTRY_Copy0F00              { ENTRY_DataIgnored &CDetourDis::Copy0F00 } // 32bit x86 only
#define ENTRY_Copy0FB8              { ENTRY_DataIgnored &CDetourDis::Copy0FB8 } // 32bit x86 only
#define ENTRY_Copy66                { ENTRY_DataIgnored &CDetourDis::Copy66 }
#define ENTRY_Copy67                { ENTRY_DataIgnored &CDetourDis::Copy67 }
#define ENTRY_CopyF6                { ENTRY_DataIgnored &CDetourDis::CopyF6 }
#define ENTRY_CopyF7                { ENTRY_DataIgnored &CDetourDis::CopyF7 }
#define ENTRY_CopyFF                { ENTRY_DataIgnored &CDetourDis::CopyFF }
#define ENTRY_CopyC7                { ENTRY_DataIgnored &CDetourDis::CopyC7 }
#define ENTRY_CopyVex2              { ENTRY_DataIgnored &CDetourDis::CopyVex2 }
#define ENTRY_CopyVex3              { ENTRY_DataIgnored &CDetourDis::CopyVex3 }
#define ENTRY_CopyEvex              { ENTRY_DataIgnored &CDetourDis::CopyEvex } // 62, 3 byte payload, then normal with implied prefixes like vex
#define ENTRY_CopyXop               { ENTRY_DataIgnored &CDetourDis::CopyXop }   // 0x8F ... POP /0 or AMD XOP
#define ENTRY_CopyRex2              { ENTRY_DataIgnored &CDetourDis::CopyRex2 }  // 0xD5 Intel APX REX2 (x64 only)
#define ENTRY_CopyBytesXop          { 5, 5, 4, 0, 0, &CDetourDis::CopyBytes } // 0x8F xop1 xop2 opcode modrm
#define ENTRY_CopyBytesXop1         { 6, 6, 4, 0, 0, &CDetourDis::CopyBytes } // 0x8F xop1 xop2 opcode modrm ... imm8
#define ENTRY_CopyBytesXop4         { 9, 9, 4, 0, 0, &CDetourDis::CopyBytes } // 0x8F xop1 xop2 opcode modrm ... imm32
#define ENTRY_Invalid               { ENTRY_DataIgnored &CDetourDis::Invalid }

    PBYTE CopyBytes(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyBytesPrefix(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyBytesSegment(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyBytesRax(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyBytesJump(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);

    PBYTE Invalid(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);

    PBYTE AdjustTarget(PBYTE pbDst, PBYTE pbSrc, UINT cbOp,
                       UINT cbTargetOffset, UINT cbTargetSize);

  protected:
    PBYTE Copy0F(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE Copy0F00(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc); // x86 only sldt/0 str/1 lldt/2 ltr/3 err/4 verw/5 jmpe/6/dynamic invalid/7
    PBYTE Copy0F78(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc); // vmread, 66/extrq/ib/ib, F2/insertq/ib/ib
    PBYTE Copy0FB8(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc); // jmpe or F3/popcnt
    PBYTE Copy66(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE Copy67(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyF2(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyF3(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc); // x86 only
    PBYTE CopyF6(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyF7(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyFF(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyC7(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyVex2(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyVex3(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyVexCommon(BYTE m, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyVexEvexCommon(BYTE m, PBYTE pbDst, PBYTE pbSrc, BYTE p, BYTE fp16 = 0);
    PBYTE CopyEvex(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyXop(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc);
    PBYTE CopyRex2(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc); // AMD64 only, Intel APX REX2

  protected:
    static const COPYENTRY  s_rceCopyTable[];
    static const COPYENTRY  s_rceCopyTable0F[];
    static const BYTE       s_rbModRm[256];
    static PBYTE            s_pbModuleBeg;
    static PBYTE            s_pbModuleEnd;
    static BOOL             s_fLimitReferencesToModule;

  protected:
    BOOL                m_bOperandOverride;
    BOOL                m_bAddressOverride;
    BOOL                m_bRaxOverride; // AMD64 only
    BOOL                m_bVex;
    BOOL                m_bEvex;
    BOOL                m_bF2;
    BOOL                m_bF3; // x86 only
    BYTE                m_nSegmentOverride;

    PBYTE *             m_ppbTarget;
    LONG *              m_plExtra;

    LONG                m_lScratchExtra;
    PBYTE               m_pbScratchTarget;
    BYTE                m_rbScratchDst[64]; // matches or exceeds rbCode
};

PVOID WINAPI DetourCopyInstruction(_In_opt_ PVOID pDst,
                                   _Inout_opt_ PVOID *ppDstPool,
                                   _In_ PVOID pSrc,
                                   _Out_opt_ PVOID *ppTarget,
                                   _Out_opt_ LONG *plExtra)
{
    UNREFERENCED_PARAMETER(ppDstPool);  // x86 & x64 don't use a constant pool.

    CDetourDis oDetourDisasm((PBYTE*)ppTarget, plExtra);
    return oDetourDisasm.CopyInstruction((PBYTE)pDst, (PBYTE)pSrc);
}

/////////////////////////////////////////////////////////// Disassembler Code.
//
CDetourDis::CDetourDis(_Out_opt_ PBYTE *ppbTarget, _Out_opt_ LONG *plExtra) :
    m_bOperandOverride(FALSE),
    m_bAddressOverride(FALSE),
    m_bRaxOverride(FALSE),
    m_bF2(FALSE),
    m_bF3(FALSE),
    m_bVex(FALSE),
    m_bEvex(FALSE)
{
    m_ppbTarget = ppbTarget ? ppbTarget : &m_pbScratchTarget;
    m_plExtra = plExtra ? plExtra : &m_lScratchExtra;

    *m_ppbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_NONE;
    *m_plExtra = 0;
}

PBYTE CDetourDis::CopyInstruction(PBYTE pbDst, PBYTE pbSrc)
{
    // Configure scratch areas if real areas are not available.
    if (NULL == pbDst) {
        pbDst = m_rbScratchDst;
    }
    if (NULL == pbSrc) {
        // We can't copy a non-existent instruction.
        SetLastError(ERROR_INVALID_DATA);
        return NULL;
    }

    // Figure out how big the instruction is, do the appropriate copy,
    // and figure out what the target of the instruction is if any.
    //
    REFCOPYENTRY pEntry = &s_rceCopyTable[pbSrc[0]];
    return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyBytes(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    UINT nBytesFixed;

    UINT const nModOffset = pEntry->nModOffset;
    UINT const nFlagBits = pEntry->nFlagBits;
    UINT const nFixedSize = pEntry->nFixedSize;
    UINT const nFixedSize16 = pEntry->nFixedSize16;

    if (nFlagBits & ADDRESS) {
        nBytesFixed = m_bAddressOverride ? nFixedSize16 : nFixedSize;
    }
#ifdef DETOURS_X64
    // REX.W trumps 66
    else if (m_bRaxOverride) {
        nBytesFixed = nFixedSize + ((nFlagBits & RAX) ? 4 : 0);
    }
#endif
    else if (m_bVex || m_bEvex) {
        // VEX/EVEX pp field does not shrink immediates to 16-bit.
        // This matters for EVEX MAP4 (APX) where entries like opcode 81
        // (Group 1 imm32) have nFixedSize=6 but nFixedSize16=4.
        nBytesFixed = nFixedSize;
    }
    else {
        nBytesFixed = m_bOperandOverride ? nFixedSize16 : nFixedSize;
    }

    UINT nBytes = nBytesFixed;
    UINT nRelOffset = pEntry->nRelOffset;
    UINT cbTarget = nBytes - nRelOffset;
    if (nModOffset > 0) {
        ASSERT(nRelOffset == 0);
        BYTE const bModRm = pbSrc[nModOffset];
        BYTE const bFlags = s_rbModRm[bModRm];

        nBytes += bFlags & NOTSIB;

        if (bFlags & SIB) {
            BYTE const bSib = pbSrc[nModOffset + 1];

            if ((bSib & 0x07) == 0x05) {
                if ((bModRm & 0xc0) == 0x00) {
                    nBytes += 4;
                }
                else if ((bModRm & 0xc0) == 0x40) {
                    nBytes += 1;
                }
                else if ((bModRm & 0xc0) == 0x80) {
                    nBytes += 4;
                }
            }
            cbTarget = nBytes - nRelOffset;
        }
#ifdef DETOURS_X64
        else if (bFlags & RIP) {
            nRelOffset = nModOffset + 1;
            cbTarget = 4;
        }
#endif
    }
    CopyMemory(pbDst, pbSrc, nBytes);

    if (nRelOffset) {
        *m_ppbTarget = AdjustTarget(pbDst, pbSrc, nBytes, nRelOffset, cbTarget);
#ifdef DETOURS_X64
        if (pEntry->nRelOffset == 0) {
            // This is a data target, not a code target, so we shouldn't return it.
            *m_ppbTarget = NULL;
        }
#endif
    }
    if (nFlagBits & NOENLARGE) {
        *m_plExtra = -*m_plExtra;
    }
    if (nFlagBits & DYNAMIC) {
        *m_ppbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    }
    return pbSrc + nBytes;
}

PBYTE CDetourDis::CopyBytesPrefix(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    pbDst[0] = pbSrc[0];
    pEntry = &s_rceCopyTable[pbSrc[1]];
    return (this->*pEntry->pfCopy)(pEntry, pbDst + 1, pbSrc + 1);
}

PBYTE CDetourDis::CopyBytesSegment(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
{
    m_nSegmentOverride = pbSrc[0];
    return CopyBytesPrefix(0, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyBytesRax(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
{ // AMD64 only
    if (pbSrc[0] & 0x8) {
        m_bRaxOverride = TRUE;
    }
    return CopyBytesPrefix(0, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyBytesJump(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    (void)pEntry;

    PVOID pvSrcAddr = &pbSrc[1];
    PVOID pvDstAddr = NULL;
    LONG_PTR nOldOffset = (LONG_PTR)*(signed char*&)pvSrcAddr;
    LONG_PTR nNewOffset = 0;

    *m_ppbTarget = pbSrc + 2 + nOldOffset;

    if (pbSrc[0] == 0xeb) {
        pbDst[0] = 0xe9;
        pvDstAddr = &pbDst[1];
        nNewOffset = nOldOffset - ((pbDst - pbSrc) + 3);
        *(UNALIGNED LONG*&)pvDstAddr = (LONG)nNewOffset;

        *m_plExtra = 3;
        return pbSrc + 2;
    }

    ASSERT(pbSrc[0] >= 0x70 && pbSrc[0] <= 0x7f);

    pbDst[0] = 0x0f;
    pbDst[1] = 0x80 | (pbSrc[0] & 0xf);
    pvDstAddr = &pbDst[2];
    nNewOffset = nOldOffset - ((pbDst - pbSrc) + 4);
    *(UNALIGNED LONG*&)pvDstAddr = (LONG)nNewOffset;

    *m_plExtra = 4;
    return pbSrc + 2;
}

PBYTE CDetourDis::AdjustTarget(PBYTE pbDst, PBYTE pbSrc, UINT cbOp,
                               UINT cbTargetOffset, UINT cbTargetSize)
{
    PBYTE pbTarget = NULL;
#if 1 // fault injection to test test code
#if defined(DETOURS_X64)
    typedef LONGLONG T;
#else
    typedef LONG T;
#endif
    T nOldOffset;
    T nNewOffset;
    PVOID pvTargetAddr = &pbDst[cbTargetOffset];

    switch (cbTargetSize) {
      case 1:
        nOldOffset = *(signed char*&)pvTargetAddr;
        break;
      case 2:
        nOldOffset = *(UNALIGNED SHORT*&)pvTargetAddr;
        break;
      case 4:
        nOldOffset = *(UNALIGNED LONG*&)pvTargetAddr;
        break;
#if defined(DETOURS_X64)
      case 8:
        nOldOffset = *(UNALIGNED LONGLONG*&)pvTargetAddr;
        break;
#endif
      default:
        ASSERT(!"cbTargetSize is invalid.");
        nOldOffset = 0;
        break;
    }

    pbTarget = pbSrc + cbOp + nOldOffset;
    nNewOffset = nOldOffset - (T)(pbDst - pbSrc);

    switch (cbTargetSize) {
      case 1:
        *(CHAR*&)pvTargetAddr = (CHAR)nNewOffset;
        if (nNewOffset < SCHAR_MIN || nNewOffset > SCHAR_MAX) {
            *m_plExtra = sizeof(ULONG) - 1;
        }
        break;
      case 2:
        *(UNALIGNED SHORT*&)pvTargetAddr = (SHORT)nNewOffset;
        if (nNewOffset < SHRT_MIN || nNewOffset > SHRT_MAX) {
            *m_plExtra = sizeof(ULONG) - 2;
        }
        break;
      case 4:
        *(UNALIGNED LONG*&)pvTargetAddr = (LONG)nNewOffset;
        if (nNewOffset < LONG_MIN || nNewOffset > LONG_MAX) {
            *m_plExtra = sizeof(ULONG) - 4;
        }
        break;
#if defined(DETOURS_X64)
      case 8:
        *(UNALIGNED LONGLONG*&)pvTargetAddr = nNewOffset;
        break;
#endif
    }
#ifdef DETOURS_X64
    // When we are only computing size, source and dest can be
    // far apart, distance not encodable in 32bits. Ok.
    // At least still check the lower 32bits.

    if (pbDst >= m_rbScratchDst && pbDst < (sizeof(m_rbScratchDst) + m_rbScratchDst)) {
        ASSERT((((size_t)pbDst + cbOp + nNewOffset) & 0xFFFFFFFF) == (((size_t)pbTarget) & 0xFFFFFFFF));
    }
    else
#endif
    {
        ASSERT(pbDst + cbOp + nNewOffset == pbTarget);
    }
#endif
    return pbTarget;
}

PBYTE CDetourDis::Invalid(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    (void)pbDst;
    (void)pEntry;
    ASSERT(!"Invalid Instruction");
    return pbSrc + 1;
}

////////////////////////////////////////////////////// Individual Bytes Codes.
//
PBYTE CDetourDis::Copy0F(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    pbDst[0] = pbSrc[0];
    pEntry = &s_rceCopyTable0F[pbSrc[1]];
    return (this->*pEntry->pfCopy)(pEntry, pbDst + 1, pbSrc + 1);
}

PBYTE CDetourDis::Copy0F78(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
{
    // vmread, 66/extrq, F2/insertq

    static const COPYENTRY vmread = /* 78 */ ENTRY_CopyBytes2Mod;
    static const COPYENTRY extrq_insertq = /* 78 */ ENTRY_CopyBytes4;

    ASSERT(!(m_bF2 && m_bOperandOverride));

    // For insertq and presumably despite documentation extrq, mode must be 11, not checked.
    // insertq/extrq/78 are followed by two immediate bytes, and given mode == 11, mod/rm byte is always one byte,
    // and the 0x78 makes 4 bytes (not counting the 66/F2/F which are accounted for elsewhere)

    REFCOPYENTRY const pEntry = ((m_bF2 || m_bOperandOverride) ? &extrq_insertq : &vmread);

    return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::Copy0F00(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
{
    // jmpe is 32bit x86 only
    // Notice that the sizes are the same either way, but jmpe is marked as "dynamic".

    static const COPYENTRY other = /* B8 */ ENTRY_CopyBytes2Mod; // sldt/0 str/1 lldt/2 ltr/3 err/4 verw/5 jmpe/6 invalid/7
    static const COPYENTRY jmpe = /* B8 */ ENTRY_CopyBytes2ModDynamic; // jmpe/6 x86-on-IA64 syscalls

    REFCOPYENTRY const pEntry = (((6 << 3) == ((7 << 3) & pbSrc[1])) ?  &jmpe : &other);
    return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::Copy0FB8(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
{
    // jmpe is 32bit x86 only

    static const COPYENTRY popcnt = /* B8 */ ENTRY_CopyBytes2Mod;
    static const COPYENTRY jmpe = /* B8 */ ENTRY_CopyBytes3Or5Dynamic; // jmpe x86-on-IA64 syscalls
    REFCOPYENTRY const pEntry = m_bF3 ? &popcnt : &jmpe;
    return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::Copy66(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{   // Operand-size override prefix
    m_bOperandOverride = TRUE;
    return CopyBytesPrefix(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::Copy67(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{   // Address size override prefix
    m_bAddressOverride = TRUE;
    return CopyBytesPrefix(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyF2(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    m_bF2 = TRUE;
    return CopyBytesPrefix(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyF3(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{ // x86 only
    m_bF3 = TRUE;
    return CopyBytesPrefix(pEntry, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyF6(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    (void)pEntry;

    // TEST BYTE /0 and /1 (both encodings are TEST per Intel SDM Vol 2A Group 3
    // and AMD APM Vol 3; /1 is an undocumented alias of /0 historically but is
    // now documented in both manuals).
    if (0x00 == (0x30 & pbSrc[1])) {    // reg(bits 543) of ModR/M == 000 or 001
        static const COPYENTRY ce = /* f6 */ ENTRY_CopyBytes2Mod1;
        return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
    }
    // DIV /6
    // IDIV /7
    // IMUL /5
    // MUL /4
    // NEG /3
    // NOT /2

    static const COPYENTRY ce = /* f6 */ ENTRY_CopyBytes2Mod;
    return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyF7(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    (void)pEntry;

    // TEST WORD /0 and /1 (see CopyF6 for /1 rationale).
    if (0x00 == (0x30 & pbSrc[1])) {    // reg(bits 543) of ModR/M == 000 or 001
        static const COPYENTRY ce = /* f7 */ ENTRY_CopyBytes2ModOperand;
        return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
    }

    // DIV /6
    // IDIV /7
    // IMUL /5
    // MUL /4
    // NEG /3
    // NOT /2
    static const COPYENTRY ce = /* f7 */ ENTRY_CopyBytes2Mod;
    return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyFF(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{   // INC /0
    // DEC /1
    // CALL /2
    // CALL /3
    // JMP /4
    // JMP /5
    // PUSH /6
    // invalid/7
    (void)pEntry;

    static const COPYENTRY ce = /* ff */ ENTRY_CopyBytes2Mod;
    PBYTE pbOut = (this->*ce.pfCopy)(&ce, pbDst, pbSrc);

    BYTE const b1 = pbSrc[1];

    if (0x15 == b1 || 0x25 == b1) {         // CALL [], JMP []
#ifdef DETOURS_X64
        // All segments but FS and GS are equivalent.
        if (m_nSegmentOverride != 0x64 && m_nSegmentOverride != 0x65)
#else
        if (m_nSegmentOverride == 0 || m_nSegmentOverride == 0x2E)
#endif
        {
#ifdef DETOURS_X64
            INT32 offset = *(UNALIGNED INT32*)&pbSrc[2];
            PBYTE *ppbTarget = (PBYTE *)(pbSrc + 6 + offset);
#else
            PBYTE *ppbTarget = (PBYTE *)(SIZE_T)*(UNALIGNED ULONG*)&pbSrc[2];
#endif
            if (s_fLimitReferencesToModule &&
                (ppbTarget < (PVOID)s_pbModuleBeg || ppbTarget >= (PVOID)s_pbModuleEnd)) {

                *m_ppbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
            }
            else {
                // This can access violate on random bytes. Use DetourSetCodeModule.
                *m_ppbTarget = *ppbTarget;
            }
        }
        else {
            *m_ppbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
        }
    }
    else if (0x10 == (0x30 & b1) || // CALL /2 or /3  --> reg(bits 543) of ModR/M == 010 or 011
             0x20 == (0x30 & b1)) { // JMP /4 or /5 --> reg(bits 543) of ModR/M == 100 or 101
        *m_ppbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    }
    return pbOut;
}

PBYTE CDetourDis::CopyC7(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
{
    (void)pEntry;

    // C7 /7 is XBEGIN rel32 (or rel16 with 66 prefix).
    // It has a relative displacement that must be relocated like CALL/JMP.
    if (0x38 == (0x38 & pbSrc[1])) {    // reg(bits 543) of ModR/M == 111
        static const COPYENTRY ce = /* c7 /7 */ { 6, 4, 0, 2, 0, &CDetourDis::CopyBytes };
        return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
    }
    // MOV /0 r/m, imm16/32
    static const COPYENTRY ce = /* c7 /0 */ ENTRY_CopyBytes2ModOperand;
    return (this->*ce.pfCopy)(&ce, pbDst, pbSrc);
}

PBYTE CDetourDis::CopyVexEvexCommon(BYTE m, PBYTE pbDst, PBYTE pbSrc, BYTE p, BYTE fp16)
// m is first instead of last in the hopes of pbDst/pbSrc being
// passed along efficiently in the registers they were already in.
{
    static const COPYENTRY ceF38 = /* 38 */ ENTRY_CopyBytes2Mod;
    static const COPYENTRY ceF3A = /* 3A */ ENTRY_CopyBytes2Mod1;
    static const COPYENTRY ceInvalid = /* C4 */ ENTRY_Invalid;

    switch (p & 3) {
    case 0: break;
    case 1: m_bOperandOverride = TRUE; break;
    case 2: m_bF3 = TRUE; break;
    case 3: m_bF2 = TRUE; break;
    }

    REFCOPYENTRY pEntry;

    // see https://software.intel.com/content/www/us/en/develop/download/intel-avx512-fp16-architecture-specification.html
    // EVEX maps (with FP16 and APX mmm-bit extension):
    //   mmm=001 (MAP1) and mmm=101 (MAP5) share the legacy 0F opcode structure,
    //     so per-opcode sizing must come from s_rceCopyTable0F (e.g. MAP5 C2 /r ib = VCMPPH/VCMPSH).
    //   mmm=010 (MAP2) and mmm=110 (MAP6) share the legacy 0F 38 structure (ModR/M, no imm).
    //   mmm=011 (MAP3) has the 0F 3A structure (ModR/M + imm8).
    //   mmm=100 (MAP4, APX) mirrors legacy MAP0 opcode structure (per-opcode sizing).
    switch (m | fp16) {
    default: return Invalid(&ceInvalid, pbDst, pbSrc);
    case 5:  // MAP5 (FP16) - opcode structure mirrors 0F (MAP1).
    case 1:  pEntry = &s_rceCopyTable0F[pbSrc[0]];
             return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
    case 6:  // MAP6 (FP16) - opcode structure mirrors 0F 38 (MAP2).
    case 2:  return CopyBytes(&ceF38, pbDst, pbSrc);
    case 3:  return CopyBytes(&ceF3A, pbDst, pbSrc);
    case 4:  // MAP4 (APX) - promoted legacy MAP0 instructions.
             pEntry = &s_rceCopyTable[pbSrc[0]];
             return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
    }
}

PBYTE CDetourDis::CopyVexCommon(BYTE m, PBYTE pbDst, PBYTE pbSrc)
// m is first instead of last in the hopes of pbDst/pbSrc being
// passed along efficiently in the registers they were already in.
{
    m_bVex = TRUE;
    BYTE const p = (BYTE)(pbSrc[-1] & 3); // p in last byte
    return CopyVexEvexCommon(m, pbDst, pbSrc, p);
}


PBYTE CDetourDis::CopyVex3(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
// 3 byte VEX prefix 0xC4
{
#ifdef DETOURS_X86
    const static COPYENTRY ceLES = /* C4 */ ENTRY_CopyBytes2Mod;
    if ((pbSrc[1] & 0xC0) != 0xC0) {
        REFCOPYENTRY pEntry = &ceLES;
        return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
    }
#endif
    pbDst[0] = pbSrc[0];
    pbDst[1] = pbSrc[1];
    pbDst[2] = pbSrc[2];
#ifdef DETOURS_X64
    m_bRaxOverride |= !!(pbSrc[2] & 0x80); // w in last byte, see CopyBytesRax
#else
    //
    // TODO
    //
    // Usually the VEX.W bit changes the size of a general purpose register and is ignored for 32bit.
    // Sometimes it is an opcode extension.
    // Look in the Intel manual, in the instruction-by-instruction reference, for ".W1",
    // without nearby wording saying it is ignored for 32bit.
    // For example: "VFMADD132PD/VFMADD213PD/VFMADD231PD Fused Multiply-Add of Packed Double-Precision Floating-Point Values".
    //
    // Then, go through each such case and determine if W0 vs. W1 affect the size of the instruction. Probably not.
    // Look for the same encoding but with "W1" changed to "W0".
    // Here is one such pairing:
    // VFMADD132PD/VFMADD213PD/VFMADD231PD Fused Multiply-Add of Packed Double-Precision Floating-Point Values
    //
    // VEX.DDS.128.66.0F38.W1 98 /r A V/V FMA Multiply packed double-precision floating-point values
    // from xmm0 and xmm2/mem, add to xmm1 and
    // put result in xmm0.
    // VFMADD132PD xmm0, xmm1, xmm2/m128
    //
    // VFMADD132PS/VFMADD213PS/VFMADD231PS Fused Multiply-Add of Packed Single-Precision Floating-Point Values
    // VEX.DDS.128.66.0F38.W0 98 /r A V/V FMA Multiply packed single-precision floating-point values
    // from xmm0 and xmm2/mem, add to xmm1 and put
    // result in xmm0.
    // VFMADD132PS xmm0, xmm1, xmm2/m128
    //
#endif
    return CopyVexCommon(pbSrc[1] & 0x1F, pbDst + 3, pbSrc + 3);
}

PBYTE CDetourDis::CopyVex2(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
// 2 byte VEX prefix 0xC5
{
#ifdef DETOURS_X86
    const static COPYENTRY ceLDS = /* C5 */ ENTRY_CopyBytes2Mod;
    if ((pbSrc[1] & 0xC0) != 0xC0) {
        REFCOPYENTRY pEntry = &ceLDS;
        return (this->*pEntry->pfCopy)(pEntry, pbDst, pbSrc);
    }
#endif
    pbDst[0] = pbSrc[0];
    pbDst[1] = pbSrc[1];
    return CopyVexCommon(1, pbDst + 2, pbSrc + 2);
}

PBYTE CDetourDis::CopyEvex(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
// 62, 3 byte payload, x86 with implied prefixes like Vex
// for 32bit, mode 0xC0 else fallback to bound /r
{
    // NOTE: Intel and Wikipedia number these differently.
    // Intel says 0-2, Wikipedia says 1-3.

    BYTE const p0 = pbSrc[1];

#ifdef DETOURS_X86
    const static COPYENTRY ceBound = /* 62 */ ENTRY_CopyBytes2Mod;
    if ((p0 & 0xC0) != 0xC0) {
        return CopyBytes(&ceBound, pbDst, pbSrc);
    }
#endif

    static const COPYENTRY ceInvalid = /* 62 */ ENTRY_Invalid;

    BYTE const p1 = pbSrc[2];

    if ((p1 & 0x04) != 0x04)
        return Invalid(&ceInvalid, pbDst, pbSrc);

    // Copy 4 byte prefix.
    *(UNALIGNED ULONG *)pbDst = *(UNALIGNED ULONG*)pbSrc;

    m_bEvex = TRUE;

#ifdef DETOURS_X64
    m_bRaxOverride |= !!(p1 & 0x80); // w
#endif

    // P0 layout: R'(7) X(6) B3(5) R'4(4) B4(3) m(2) m(1) m(0)
    // Bits [2:0] = map (1-7). Bit 3 = B4 (APX register extension, not a map bit).
    // For FP16: bit 2 extends map (MAP5=101, MAP6=110).
    // For APX:  map=4 (100) uses bit 2 as part of mmm field.
    return CopyVexEvexCommon(p0 & 3u, pbDst + 4, pbSrc + 4, p1 & 3u, p0 & 4u);
}

PBYTE CDetourDis::CopyXop(REFCOPYENTRY, PBYTE pbDst, PBYTE pbSrc)
/* 3 byte AMD XOP prefix 0x8F
byte0: 0x8F
byte1: RXBmmmmm
byte2: WvvvvLpp
byte3: opcode
mmmmm >= 8, else pop
mmmmm only otherwise defined for 8, 9, A.
pp is like VEX but only instructions with 0 are defined
*/
{
    const static COPYENTRY cePop = /* 8F */ ENTRY_CopyBytes2Mod;
    const static COPYENTRY ceXop = /* 8F */ ENTRY_CopyBytesXop;
    const static COPYENTRY ceXop1 = /* 8F */ ENTRY_CopyBytesXop1;
    const static COPYENTRY ceXop4 = /* 8F */ ENTRY_CopyBytesXop4;

    BYTE const m = (BYTE)(pbSrc[1] & 0x1F);
    ASSERT(m <= 10);
    switch (m)
    {
    default:
        return CopyBytes(&cePop, pbDst, pbSrc);

    case 8: // modrm with 8bit immediate
        return CopyBytes(&ceXop1, pbDst, pbSrc);

    case 9: // modrm with no immediate
        return CopyBytes(&ceXop, pbDst, pbSrc);

    case 10: // modrm with 32bit immediate
        return CopyBytes(&ceXop4, pbDst, pbSrc);
    }
}

PBYTE CDetourDis::CopyRex2(REFCOPYENTRY pEntry, PBYTE pbDst, PBYTE pbSrc)
// Intel APX REX2 prefix 0xD5 (64-bit mode only)
// Byte 0: D5
// Byte 1: M(7) R4(6) X4(5) B4(4) W(3) R3(2) X3(1) B3(0)
//   M: 0 = opcode from MAP0, 1 = opcode from MAP1 (no 0F escape needed)
//   W: operand size override to 64-bit (same as REX.W)
{
    (void)pEntry;

    BYTE const payload = pbSrc[1];

    if (payload & 0x08) { // W bit (bit 3)
        m_bRaxOverride = TRUE;
    }

    pbDst[0] = pbSrc[0];
    pbDst[1] = pbSrc[1];

    PBYTE pbOut;
    if (payload & 0x80) { // M bit (bit 7) - MAP1
        REFCOPYENTRY pEntry2 = &s_rceCopyTable0F[pbSrc[2]];
        pbOut = (this->*pEntry2->pfCopy)(pEntry2, pbDst + 2, pbSrc + 2);
    }
    else { // MAP0
        REFCOPYENTRY pEntry2 = &s_rceCopyTable[pbSrc[2]];
        pbOut = (this->*pEntry2->pfCopy)(pEntry2, pbDst + 2, pbSrc + 2);
    }

    // JMPABS: REX2 with payload=0x00 (M=0, W=0, all ext bits 0) and opcode A1.
    // This is an absolute 64-bit jump whose target is the 8-byte immediate.
    if (payload == 0x00 && pbSrc[2] == 0xA1) {
        *m_ppbTarget = *(UNALIGNED PBYTE*)&pbSrc[3];
    }

    return pbOut;
}

//////////////////////////////////////////////////////////////////////////////
//
PBYTE CDetourDis::s_pbModuleBeg = NULL;
PBYTE CDetourDis::s_pbModuleEnd = (PBYTE)~(ULONG_PTR)0;
BOOL CDetourDis::s_fLimitReferencesToModule = FALSE;

BOOL CDetourDis::SetCodeModule(PBYTE pbBeg, PBYTE pbEnd, BOOL fLimitReferencesToModule)
{
    if (pbEnd < pbBeg) {
        return FALSE;
    }

    s_pbModuleBeg = pbBeg;
    s_pbModuleEnd = pbEnd;
    s_fLimitReferencesToModule = fLimitReferencesToModule;

    return TRUE;
}

///////////////////////////////////////////////////////// Disassembler Tables.
//
const BYTE CDetourDis::s_rbModRm[256] = {
    0,0,0,0, SIB|1,RIP|4,0,0, 0,0,0,0, SIB|1,RIP|4,0,0, // 0x
    0,0,0,0, SIB|1,RIP|4,0,0, 0,0,0,0, SIB|1,RIP|4,0,0, // 1x
    0,0,0,0, SIB|1,RIP|4,0,0, 0,0,0,0, SIB|1,RIP|4,0,0, // 2x
    0,0,0,0, SIB|1,RIP|4,0,0, 0,0,0,0, SIB|1,RIP|4,0,0, // 3x
    1,1,1,1, 2,1,1,1, 1,1,1,1, 2,1,1,1,                 // 4x
    1,1,1,1, 2,1,1,1, 1,1,1,1, 2,1,1,1,                 // 5x
    1,1,1,1, 2,1,1,1, 1,1,1,1, 2,1,1,1,                 // 6x
    1,1,1,1, 2,1,1,1, 1,1,1,1, 2,1,1,1,                 // 7x
    4,4,4,4, 5,4,4,4, 4,4,4,4, 5,4,4,4,                 // 8x
    4,4,4,4, 5,4,4,4, 4,4,4,4, 5,4,4,4,                 // 9x
    4,4,4,4, 5,4,4,4, 4,4,4,4, 5,4,4,4,                 // Ax
    4,4,4,4, 5,4,4,4, 4,4,4,4, 5,4,4,4,                 // Bx
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,                 // Cx
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,                 // Dx
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,                 // Ex
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0                  // Fx
};

const CDetourDis::COPYENTRY CDetourDis::s_rceCopyTable[] =
{
    /* 00 */ ENTRY_CopyBytes2Mod,                      // ADD /r
    /* 01 */ ENTRY_CopyBytes2Mod,                      // ADD /r
    /* 02 */ ENTRY_CopyBytes2Mod,                      // ADD /r
    /* 03 */ ENTRY_CopyBytes2Mod,                      // ADD /r
    /* 04 */ ENTRY_CopyBytes2,                         // ADD ib
    /* 05 */ ENTRY_CopyBytes3Or5,                      // ADD iw
#ifdef DETOURS_X64
    /* 06 */ ENTRY_Invalid,                            // Invalid
    /* 07 */ ENTRY_Invalid,                            // Invalid
#else
    /* 06 */ ENTRY_CopyBytes1,                         // PUSH
    /* 07 */ ENTRY_CopyBytes1,                         // POP
#endif
    /* 08 */ ENTRY_CopyBytes2Mod,                      // OR /r
    /* 09 */ ENTRY_CopyBytes2Mod,                      // OR /r
    /* 0A */ ENTRY_CopyBytes2Mod,                      // OR /r
    /* 0B */ ENTRY_CopyBytes2Mod,                      // OR /r
    /* 0C */ ENTRY_CopyBytes2,                         // OR ib
    /* 0D */ ENTRY_CopyBytes3Or5,                      // OR iw
#ifdef DETOURS_X64
    /* 0E */ ENTRY_Invalid,                            // Invalid
#else
    /* 0E */ ENTRY_CopyBytes1,                         // PUSH
#endif
    /* 0F */ ENTRY_Copy0F,                             // Extension Ops
    /* 10 */ ENTRY_CopyBytes2Mod,                      // ADC /r
    /* 11 */ ENTRY_CopyBytes2Mod,                      // ADC /r
    /* 12 */ ENTRY_CopyBytes2Mod,                      // ADC /r
    /* 13 */ ENTRY_CopyBytes2Mod,                      // ADC /r
    /* 14 */ ENTRY_CopyBytes2,                         // ADC ib
    /* 15 */ ENTRY_CopyBytes3Or5,                      // ADC id
#ifdef DETOURS_X64
    /* 16 */ ENTRY_Invalid,                            // Invalid
    /* 17 */ ENTRY_Invalid,                            // Invalid
#else
    /* 16 */ ENTRY_CopyBytes1,                         // PUSH
    /* 17 */ ENTRY_CopyBytes1,                         // POP
#endif
    /* 18 */ ENTRY_CopyBytes2Mod,                      // SBB /r
    /* 19 */ ENTRY_CopyBytes2Mod,                      // SBB /r
    /* 1A */ ENTRY_CopyBytes2Mod,                      // SBB /r
    /* 1B */ ENTRY_CopyBytes2Mod,                      // SBB /r
    /* 1C */ ENTRY_CopyBytes2,                         // SBB ib
    /* 1D */ ENTRY_CopyBytes3Or5,                      // SBB id
#ifdef DETOURS_X64
    /* 1E */ ENTRY_Invalid,                            // Invalid
    /* 1F */ ENTRY_Invalid,                            // Invalid
#else
    /* 1E */ ENTRY_CopyBytes1,                         // PUSH
    /* 1F */ ENTRY_CopyBytes1,                         // POP
#endif
    /* 20 */ ENTRY_CopyBytes2Mod,                      // AND /r
    /* 21 */ ENTRY_CopyBytes2Mod,                      // AND /r
    /* 22 */ ENTRY_CopyBytes2Mod,                      // AND /r
    /* 23 */ ENTRY_CopyBytes2Mod,                      // AND /r
    /* 24 */ ENTRY_CopyBytes2,                         // AND ib
    /* 25 */ ENTRY_CopyBytes3Or5,                      // AND id
    /* 26 */ ENTRY_CopyBytesSegment,                   // ES prefix
#ifdef DETOURS_X64
    /* 27 */ ENTRY_Invalid,                            // Invalid
#else
    /* 27 */ ENTRY_CopyBytes1,                         // DAA
#endif
    /* 28 */ ENTRY_CopyBytes2Mod,                      // SUB /r
    /* 29 */ ENTRY_CopyBytes2Mod,                      // SUB /r
    /* 2A */ ENTRY_CopyBytes2Mod,                      // SUB /r
    /* 2B */ ENTRY_CopyBytes2Mod,                      // SUB /r
    /* 2C */ ENTRY_CopyBytes2,                         // SUB ib
    /* 2D */ ENTRY_CopyBytes3Or5,                      // SUB id
    /* 2E */ ENTRY_CopyBytesSegment,                   // CS prefix
#ifdef DETOURS_X64
    /* 2F */ ENTRY_Invalid,                            // Invalid
#else
    /* 2F */ ENTRY_CopyBytes1,                         // DAS
#endif
    /* 30 */ ENTRY_CopyBytes2Mod,                      // XOR /r
    /* 31 */ ENTRY_CopyBytes2Mod,                      // XOR /r
    /* 32 */ ENTRY_CopyBytes2Mod,                      // XOR /r
    /* 33 */ ENTRY_CopyBytes2Mod,                      // XOR /r
    /* 34 */ ENTRY_CopyBytes2,                         // XOR ib
    /* 35 */ ENTRY_CopyBytes3Or5,                      // XOR id
    /* 36 */ ENTRY_CopyBytesSegment,                   // SS prefix
#ifdef DETOURS_X64
    /* 37 */ ENTRY_Invalid,                            // Invalid
#else
    /* 37 */ ENTRY_CopyBytes1,                         // AAA
#endif
    /* 38 */ ENTRY_CopyBytes2Mod,                      // CMP /r
    /* 39 */ ENTRY_CopyBytes2Mod,                      // CMP /r
    /* 3A */ ENTRY_CopyBytes2Mod,                      // CMP /r
    /* 3B */ ENTRY_CopyBytes2Mod,                      // CMP /r
    /* 3C */ ENTRY_CopyBytes2,                         // CMP ib
    /* 3D */ ENTRY_CopyBytes3Or5,                      // CMP id
    /* 3E */ ENTRY_CopyBytesSegment,                   // DS prefix
#ifdef DETOURS_X64
    /* 3F */ ENTRY_Invalid,                            // Invalid
#else
    /* 3F */ ENTRY_CopyBytes1,                         // AAS
#endif
#ifdef DETOURS_X64 // For Rax Prefix
    /* 40 */ ENTRY_CopyBytesRax,                       // Rax
    /* 41 */ ENTRY_CopyBytesRax,                       // Rax
    /* 42 */ ENTRY_CopyBytesRax,                       // Rax
    /* 43 */ ENTRY_CopyBytesRax,                       // Rax
    /* 44 */ ENTRY_CopyBytesRax,                       // Rax
    /* 45 */ ENTRY_CopyBytesRax,                       // Rax
    /* 46 */ ENTRY_CopyBytesRax,                       // Rax
    /* 47 */ ENTRY_CopyBytesRax,                       // Rax
    /* 48 */ ENTRY_CopyBytesRax,                       // Rax
    /* 49 */ ENTRY_CopyBytesRax,                       // Rax
    /* 4A */ ENTRY_CopyBytesRax,                       // Rax
    /* 4B */ ENTRY_CopyBytesRax,                       // Rax
    /* 4C */ ENTRY_CopyBytesRax,                       // Rax
    /* 4D */ ENTRY_CopyBytesRax,                       // Rax
    /* 4E */ ENTRY_CopyBytesRax,                       // Rax
    /* 4F */ ENTRY_CopyBytesRax,                       // Rax
#else
    /* 40 */ ENTRY_CopyBytes1,                         // INC
    /* 41 */ ENTRY_CopyBytes1,                         // INC
    /* 42 */ ENTRY_CopyBytes1,                         // INC
    /* 43 */ ENTRY_CopyBytes1,                         // INC
    /* 44 */ ENTRY_CopyBytes1,                         // INC
    /* 45 */ ENTRY_CopyBytes1,                         // INC
    /* 46 */ ENTRY_CopyBytes1,                         // INC
    /* 47 */ ENTRY_CopyBytes1,                         // INC
    /* 48 */ ENTRY_CopyBytes1,                         // DEC
    /* 49 */ ENTRY_CopyBytes1,                         // DEC
    /* 4A */ ENTRY_CopyBytes1,                         // DEC
    /* 4B */ ENTRY_CopyBytes1,                         // DEC
    /* 4C */ ENTRY_CopyBytes1,                         // DEC
    /* 4D */ ENTRY_CopyBytes1,                         // DEC
    /* 4E */ ENTRY_CopyBytes1,                         // DEC
    /* 4F */ ENTRY_CopyBytes1,                         // DEC
#endif
    /* 50 */ ENTRY_CopyBytes1,                         // PUSH
    /* 51 */ ENTRY_CopyBytes1,                         // PUSH
    /* 52 */ ENTRY_CopyBytes1,                         // PUSH
    /* 53 */ ENTRY_CopyBytes1,                         // PUSH
    /* 54 */ ENTRY_CopyBytes1,                         // PUSH
    /* 55 */ ENTRY_CopyBytes1,                         // PUSH
    /* 56 */ ENTRY_CopyBytes1,                         // PUSH
    /* 57 */ ENTRY_CopyBytes1,                         // PUSH
    /* 58 */ ENTRY_CopyBytes1,                         // POP
    /* 59 */ ENTRY_CopyBytes1,                         // POP
    /* 5A */ ENTRY_CopyBytes1,                         // POP
    /* 5B */ ENTRY_CopyBytes1,                         // POP
    /* 5C */ ENTRY_CopyBytes1,                         // POP
    /* 5D */ ENTRY_CopyBytes1,                         // POP
    /* 5E */ ENTRY_CopyBytes1,                         // POP
    /* 5F */ ENTRY_CopyBytes1,                         // POP
#ifdef DETOURS_X64
    /* 60 */ ENTRY_Invalid,                            // Invalid
    /* 61 */ ENTRY_Invalid,                            // Invalid
    /* 62 */ ENTRY_CopyEvex,                           // EVEX / AVX512
#else
    /* 60 */ ENTRY_CopyBytes1,                         // PUSHAD
    /* 61 */ ENTRY_CopyBytes1,                         // POPAD
    /* 62 */ ENTRY_CopyEvex,                           // BOUND /r and EVEX / AVX512
#endif
    /* 63 */ ENTRY_CopyBytes2Mod,                      // 32bit ARPL /r, 64bit MOVSXD
    /* 64 */ ENTRY_CopyBytesSegment,                   // FS prefix
    /* 65 */ ENTRY_CopyBytesSegment,                   // GS prefix
    /* 66 */ ENTRY_Copy66,                             // Operand Prefix
    /* 67 */ ENTRY_Copy67,                             // Address Prefix
    /* 68 */ ENTRY_CopyBytes3Or5,                      // PUSH
    /* 69 */ ENTRY_CopyBytes2ModOperand,               // IMUL /r iz
    /* 6A */ ENTRY_CopyBytes2,                         // PUSH
    /* 6B */ ENTRY_CopyBytes2Mod1,                     // IMUL /r ib
    /* 6C */ ENTRY_CopyBytes1,                         // INS
    /* 6D */ ENTRY_CopyBytes1,                         // INS
    /* 6E */ ENTRY_CopyBytes1,                         // OUTS/OUTSB
    /* 6F */ ENTRY_CopyBytes1,                         // OUTS/OUTSW
    /* 70 */ ENTRY_CopyBytes2Jump,                     // JO           // 0f80
    /* 71 */ ENTRY_CopyBytes2Jump,                     // JNO          // 0f81
    /* 72 */ ENTRY_CopyBytes2Jump,                     // JB/JC/JNAE   // 0f82
    /* 73 */ ENTRY_CopyBytes2Jump,                     // JAE/JNB/JNC  // 0f83
    /* 74 */ ENTRY_CopyBytes2Jump,                     // JE/JZ        // 0f84
    /* 75 */ ENTRY_CopyBytes2Jump,                     // JNE/JNZ      // 0f85
    /* 76 */ ENTRY_CopyBytes2Jump,                     // JBE/JNA      // 0f86
    /* 77 */ ENTRY_CopyBytes2Jump,                     // JA/JNBE      // 0f87
    /* 78 */ ENTRY_CopyBytes2Jump,                     // JS           // 0f88
    /* 79 */ ENTRY_CopyBytes2Jump,                     // JNS          // 0f89
    /* 7A */ ENTRY_CopyBytes2Jump,                     // JP/JPE       // 0f8a
    /* 7B */ ENTRY_CopyBytes2Jump,                     // JNP/JPO      // 0f8b
    /* 7C */ ENTRY_CopyBytes2Jump,                     // JL/JNGE      // 0f8c
    /* 7D */ ENTRY_CopyBytes2Jump,                     // JGE/JNL      // 0f8d
    /* 7E */ ENTRY_CopyBytes2Jump,                     // JLE/JNG      // 0f8e
    /* 7F */ ENTRY_CopyBytes2Jump,                     // JG/JNLE      // 0f8f
    /* 80 */ ENTRY_CopyBytes2Mod1,                     // ADD/0 OR/1 ADC/2 SBB/3 AND/4 SUB/5 XOR/6 CMP/7 byte reg, immediate byte
    /* 81 */ ENTRY_CopyBytes2ModOperand,               // ADD/0 OR/1 ADC/2 SBB/3 AND/4 SUB/5 XOR/6 CMP/7 byte reg, immediate word or dword
#ifdef DETOURS_X64
    /* 82 */ ENTRY_Invalid,                            // Invalid
#else
    /* 82 */ ENTRY_CopyBytes2Mod1,                     // MOV al,x
#endif
    /* 83 */ ENTRY_CopyBytes2Mod1,                     // ADD/0 OR/1 ADC/2 SBB/3 AND/4 SUB/5 XOR/6 CMP/7 reg, immediate byte
    /* 84 */ ENTRY_CopyBytes2Mod,                      // TEST /r
    /* 85 */ ENTRY_CopyBytes2Mod,                      // TEST /r
    /* 86 */ ENTRY_CopyBytes2Mod,                      // XCHG /r @todo
    /* 87 */ ENTRY_CopyBytes2Mod,                      // XCHG /r @todo
    /* 88 */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 89 */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 8A */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 8B */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 8C */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 8D */ ENTRY_CopyBytes2Mod,                      // LEA /r
    /* 8E */ ENTRY_CopyBytes2Mod,                      // MOV /r
    /* 8F */ ENTRY_CopyXop,                            // POP /0 or AMD XOP
    /* 90 */ ENTRY_CopyBytes1,                         // NOP
    /* 91 */ ENTRY_CopyBytes1,                         // XCHG
    /* 92 */ ENTRY_CopyBytes1,                         // XCHG
    /* 93 */ ENTRY_CopyBytes1,                         // XCHG
    /* 94 */ ENTRY_CopyBytes1,                         // XCHG
    /* 95 */ ENTRY_CopyBytes1,                         // XCHG
    /* 96 */ ENTRY_CopyBytes1,                         // XCHG
    /* 97 */ ENTRY_CopyBytes1,                         // XCHG
    /* 98 */ ENTRY_CopyBytes1,                         // CWDE
    /* 99 */ ENTRY_CopyBytes1,                         // CDQ
#ifdef DETOURS_X64
    /* 9A */ ENTRY_Invalid,                            // Invalid
#else
    /* 9A */ ENTRY_CopyBytes5Or7Dynamic,               // CALL cp
#endif
    /* 9B */ ENTRY_CopyBytes1,                         // WAIT/FWAIT
    /* 9C */ ENTRY_CopyBytes1,                         // PUSHFD
    /* 9D */ ENTRY_CopyBytes1,                         // POPFD
    /* 9E */ ENTRY_CopyBytes1,                         // SAHF
    /* 9F */ ENTRY_CopyBytes1,                         // LAHF
    /* A0 */ ENTRY_CopyBytes1Address,                  // MOV
    /* A1 */ ENTRY_CopyBytes1Address,                  // MOV
    /* A2 */ ENTRY_CopyBytes1Address,                  // MOV
    /* A3 */ ENTRY_CopyBytes1Address,                  // MOV
    /* A4 */ ENTRY_CopyBytes1,                         // MOVS
    /* A5 */ ENTRY_CopyBytes1,                         // MOVS/MOVSD
    /* A6 */ ENTRY_CopyBytes1,                         // CMPS/CMPSB
    /* A7 */ ENTRY_CopyBytes1,                         // CMPS/CMPSW
    /* A8 */ ENTRY_CopyBytes2,                         // TEST
    /* A9 */ ENTRY_CopyBytes3Or5,                      // TEST
    /* AA */ ENTRY_CopyBytes1,                         // STOS/STOSB
    /* AB */ ENTRY_CopyBytes1,                         // STOS/STOSW
    /* AC */ ENTRY_CopyBytes1,                         // LODS/LODSB
    /* AD */ ENTRY_CopyBytes1,                         // LODS/LODSW
    /* AE */ ENTRY_CopyBytes1,                         // SCAS/SCASB
    /* AF */ ENTRY_CopyBytes1,                         // SCAS/SCASD
    /* B0 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B1 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B2 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B3 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B4 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B5 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B6 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B7 */ ENTRY_CopyBytes2,                         // MOV B0+rb
    /* B8 */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* B9 */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BA */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BB */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BC */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BD */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BE */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* BF */ ENTRY_CopyBytes3Or5Rax,                   // MOV B8+rb
    /* C0 */ ENTRY_CopyBytes2Mod1,                     // RCL/2 ib, etc.
    /* C1 */ ENTRY_CopyBytes2Mod1,                     // RCL/2 ib, etc.
    /* C2 */ ENTRY_CopyBytes3,                         // RET
    /* C3 */ ENTRY_CopyBytes1,                         // RET
    /* C4 */ ENTRY_CopyVex3,                           // LES, VEX 3-byte opcodes.
    /* C5 */ ENTRY_CopyVex2,                           // LDS, VEX 2-byte opcodes.
    /* C6 */ ENTRY_CopyBytes2Mod1,                     // MOV
    /* C7 */ ENTRY_CopyC7,                             // MOV/0 XBEGIN/7
    /* C8 */ ENTRY_CopyBytes4,                         // ENTER
    /* C9 */ ENTRY_CopyBytes1,                         // LEAVE
    /* CA */ ENTRY_CopyBytes3Dynamic,                  // RET
    /* CB */ ENTRY_CopyBytes1Dynamic,                  // RET
    /* CC */ ENTRY_CopyBytes1Dynamic,                  // INT 3
    /* CD */ ENTRY_CopyBytes2Dynamic,                  // INT ib
#ifdef DETOURS_X64
    /* CE */ ENTRY_Invalid,                            // Invalid
#else
    /* CE */ ENTRY_CopyBytes1Dynamic,                  // INTO
#endif
    /* CF */ ENTRY_CopyBytes1Dynamic,                  // IRET
    /* D0 */ ENTRY_CopyBytes2Mod,                      // RCL/2, etc.
    /* D1 */ ENTRY_CopyBytes2Mod,                      // RCL/2, etc.
    /* D2 */ ENTRY_CopyBytes2Mod,                      // RCL/2, etc.
    /* D3 */ ENTRY_CopyBytes2Mod,                      // RCL/2, etc.
#ifdef DETOURS_X64
    /* D4 */ ENTRY_Invalid,                            // Invalid
    /* D5 */ ENTRY_CopyRex2,                           // REX2 (Intel APX)
#else
    /* D4 */ ENTRY_CopyBytes2,                         // AAM
    /* D5 */ ENTRY_CopyBytes2,                         // AAD
#endif
    /* D6 */ ENTRY_Invalid,                            // Invalid
    /* D7 */ ENTRY_CopyBytes1,                         // XLAT/XLATB
    /* D8 */ ENTRY_CopyBytes2Mod,                      // FADD, etc.
    /* D9 */ ENTRY_CopyBytes2Mod,                      // F2XM1, etc.
    /* DA */ ENTRY_CopyBytes2Mod,                      // FLADD, etc.
    /* DB */ ENTRY_CopyBytes2Mod,                      // FCLEX, etc.
    /* DC */ ENTRY_CopyBytes2Mod,                      // FADD/0, etc.
    /* DD */ ENTRY_CopyBytes2Mod,                      // FFREE, etc.
    /* DE */ ENTRY_CopyBytes2Mod,                      // FADDP, etc.
    /* DF */ ENTRY_CopyBytes2Mod,                      // FBLD/4, etc.
    /* E0 */ ENTRY_CopyBytes2CantJump,                 // LOOPNE cb
    /* E1 */ ENTRY_CopyBytes2CantJump,                 // LOOPE cb
    /* E2 */ ENTRY_CopyBytes2CantJump,                 // LOOP cb
    /* E3 */ ENTRY_CopyBytes2CantJump,                 // JCXZ/JECXZ
    /* E4 */ ENTRY_CopyBytes2,                         // IN ib
    /* E5 */ ENTRY_CopyBytes2,                         // IN id
    /* E6 */ ENTRY_CopyBytes2,                         // OUT ib
    /* E7 */ ENTRY_CopyBytes2,                         // OUT ib
    /* E8 */ ENTRY_CopyBytes3Or5Target,                // CALL cd
    /* E9 */ ENTRY_CopyBytes3Or5Target,                // JMP cd
#ifdef DETOURS_X64
    /* EA */ ENTRY_Invalid,                            // Invalid
#else
    /* EA */ ENTRY_CopyBytes5Or7Dynamic,               // JMP cp
#endif
    /* EB */ ENTRY_CopyBytes2Jump,                     // JMP cb
    /* EC */ ENTRY_CopyBytes1,                         // IN ib
    /* ED */ ENTRY_CopyBytes1,                         // IN id
    /* EE */ ENTRY_CopyBytes1,                         // OUT
    /* EF */ ENTRY_CopyBytes1,                         // OUT
    /* F0 */ ENTRY_CopyBytesPrefix,                    // LOCK prefix
    /* F1 */ ENTRY_CopyBytes1Dynamic,                  // INT1 / ICEBP somewhat documented by AMD, not by Intel
    /* F2 */ ENTRY_CopyF2,                             // REPNE prefix
//#ifdef DETOURS_X86
    /* F3 */ ENTRY_CopyF3,                             // REPE prefix
//#else
// This does presently suffice for AMD64 but it requires tracing
// through a bunch of code to verify and seems not worth maintaining.
//  /* F3 */ ENTRY_CopyBytesPrefix,                    // REPE prefix
//#endif
    /* F4 */ ENTRY_CopyBytes1,                         // HLT
    /* F5 */ ENTRY_CopyBytes1,                         // CMC
    /* F6 */ ENTRY_CopyF6,                             // TEST/0, DIV/6
    /* F7 */ ENTRY_CopyF7,                             // TEST/0, DIV/6
    /* F8 */ ENTRY_CopyBytes1,                         // CLC
    /* F9 */ ENTRY_CopyBytes1,                         // STC
    /* FA */ ENTRY_CopyBytes1,                         // CLI
    /* FB */ ENTRY_CopyBytes1,                         // STI
    /* FC */ ENTRY_CopyBytes1,                         // CLD
    /* FD */ ENTRY_CopyBytes1,                         // STD
    /* FE */ ENTRY_CopyBytes2Mod,                      // DEC/1,INC/0
    /* FF */ ENTRY_CopyFF,                             // CALL/2
};

const CDetourDis::COPYENTRY CDetourDis::s_rceCopyTable0F[] =
{
#ifdef DETOURS_X86
    /* 00 */ ENTRY_Copy0F00,                           // sldt/0 str/1 lldt/2 ltr/3 err/4 verw/5 jmpe/6/dynamic invalid/7
#else
    /* 00 */ ENTRY_CopyBytes2Mod,                      // sldt/0 str/1 lldt/2 ltr/3 err/4 verw/5 jmpe/6/dynamic invalid/7
#endif
    /* 01 */ ENTRY_CopyBytes2Mod,                      // INVLPG/7, etc.
    /* 02 */ ENTRY_CopyBytes2Mod,                      // LAR/r
    /* 03 */ ENTRY_CopyBytes2Mod,                      // LSL/r
    /* 04 */ ENTRY_Invalid,                            // _04
    /* 05 */ ENTRY_CopyBytes1,                         // SYSCALL
    /* 06 */ ENTRY_CopyBytes1,                         // CLTS
    /* 07 */ ENTRY_CopyBytes1,                         // SYSRET
    /* 08 */ ENTRY_CopyBytes1,                         // INVD
    /* 09 */ ENTRY_CopyBytes1,                         // WBINVD
    /* 0A */ ENTRY_Invalid,                            // _0A
    /* 0B */ ENTRY_CopyBytes1,                         // UD2
    /* 0C */ ENTRY_Invalid,                            // _0C
    /* 0D */ ENTRY_CopyBytes2Mod,                      // PREFETCH
    /* 0E */ ENTRY_CopyBytes1,                         // FEMMS (3DNow -- not in Intel documentation)
    /* 0F */ ENTRY_CopyBytes2Mod1,                     // 3DNow Opcodes
    /* 10 */ ENTRY_CopyBytes2Mod,                      // MOVSS MOVUPD MOVSD
    /* 11 */ ENTRY_CopyBytes2Mod,                      // MOVSS MOVUPD MOVSD
    /* 12 */ ENTRY_CopyBytes2Mod,                      // MOVLPD
    /* 13 */ ENTRY_CopyBytes2Mod,                      // MOVLPD
    /* 14 */ ENTRY_CopyBytes2Mod,                      // UNPCKLPD
    /* 15 */ ENTRY_CopyBytes2Mod,                      // UNPCKHPD
    /* 16 */ ENTRY_CopyBytes2Mod,                      // MOVHPD
    /* 17 */ ENTRY_CopyBytes2Mod,                      // MOVHPD
    /* 18 */ ENTRY_CopyBytes2Mod,                      // PREFETCHINTA...
    /* 19 */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1A */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1B */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1C */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1D */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1E */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop, not documented by Intel, documented by AMD
    /* 1F */ ENTRY_CopyBytes2Mod,                      // NOP/r multi byte nop
    /* 20 */ ENTRY_CopyBytes2Mod,                      // MOV/r
    /* 21 */ ENTRY_CopyBytes2Mod,                      // MOV/r
    /* 22 */ ENTRY_CopyBytes2Mod,                      // MOV/r
    /* 23 */ ENTRY_CopyBytes2Mod,                      // MOV/r
#ifdef DETOURS_X64
    /* 24 */ ENTRY_Invalid,                            // _24
#else
    /* 24 */ ENTRY_CopyBytes2Mod,                      // MOV/r,TR TR is test register on 80386 and 80486, removed in Pentium
#endif
    /* 25 */ ENTRY_Invalid,                            // _25
#ifdef DETOURS_X64
    /* 26 */ ENTRY_Invalid,                            // _26
#else
    /* 26 */ ENTRY_CopyBytes2Mod,                      // MOV TR/r TR is test register on 80386 and 80486, removed in Pentium
#endif
    /* 27 */ ENTRY_Invalid,                            // _27
    /* 28 */ ENTRY_CopyBytes2Mod,                      // MOVAPS MOVAPD
    /* 29 */ ENTRY_CopyBytes2Mod,                      // MOVAPS MOVAPD
    /* 2A */ ENTRY_CopyBytes2Mod,                      // CVPI2PS &
    /* 2B */ ENTRY_CopyBytes2Mod,                      // MOVNTPS MOVNTPD
    /* 2C */ ENTRY_CopyBytes2Mod,                      // CVTTPS2PI &
    /* 2D */ ENTRY_CopyBytes2Mod,                      // CVTPS2PI &
    /* 2E */ ENTRY_CopyBytes2Mod,                      // UCOMISS UCOMISD
    /* 2F */ ENTRY_CopyBytes2Mod,                      // COMISS COMISD
    /* 30 */ ENTRY_CopyBytes1,                         // WRMSR
    /* 31 */ ENTRY_CopyBytes1,                         // RDTSC
    /* 32 */ ENTRY_CopyBytes1,                         // RDMSR
    /* 33 */ ENTRY_CopyBytes1,                         // RDPMC
    /* 34 */ ENTRY_CopyBytes1,                         // SYSENTER
    /* 35 */ ENTRY_CopyBytes1,                         // SYSEXIT
    /* 36 */ ENTRY_Invalid,                            // _36
    /* 37 */ ENTRY_CopyBytes1,                         // GETSEC
    /* 38 */ ENTRY_CopyBytes3Mod,                      // SSE3 Opcodes
    /* 39 */ ENTRY_Invalid,                            // _39
    /* 3A */ ENTRY_CopyBytes3Mod1,                      // SSE3 Opcodes
    /* 3B */ ENTRY_Invalid,                            // _3B
    /* 3C */ ENTRY_Invalid,                            // _3C
    /* 3D */ ENTRY_Invalid,                            // _3D
    /* 3E */ ENTRY_Invalid,                            // _3E
    /* 3F */ ENTRY_Invalid,                            // _3F
    /* 40 */ ENTRY_CopyBytes2Mod,                      // CMOVO (0F 40)
    /* 41 */ ENTRY_CopyBytes2Mod,                      // CMOVNO (0F 41)
    /* 42 */ ENTRY_CopyBytes2Mod,                      // CMOVB & CMOVNE (0F 42)
    /* 43 */ ENTRY_CopyBytes2Mod,                      // CMOVAE & CMOVNB (0F 43)
    /* 44 */ ENTRY_CopyBytes2Mod,                      // CMOVE & CMOVZ (0F 44)
    /* 45 */ ENTRY_CopyBytes2Mod,                      // CMOVNE & CMOVNZ (0F 45)
    /* 46 */ ENTRY_CopyBytes2Mod,                      // CMOVBE & CMOVNA (0F 46)
    /* 47 */ ENTRY_CopyBytes2Mod,                      // CMOVA & CMOVNBE (0F 47)
    /* 48 */ ENTRY_CopyBytes2Mod,                      // CMOVS (0F 48)
    /* 49 */ ENTRY_CopyBytes2Mod,                      // CMOVNS (0F 49)
    /* 4A */ ENTRY_CopyBytes2Mod,                      // CMOVP & CMOVPE (0F 4A)
    /* 4B */ ENTRY_CopyBytes2Mod,                      // CMOVNP & CMOVPO (0F 4B)
    /* 4C */ ENTRY_CopyBytes2Mod,                      // CMOVL & CMOVNGE (0F 4C)
    /* 4D */ ENTRY_CopyBytes2Mod,                      // CMOVGE & CMOVNL (0F 4D)
    /* 4E */ ENTRY_CopyBytes2Mod,                      // CMOVLE & CMOVNG (0F 4E)
    /* 4F */ ENTRY_CopyBytes2Mod,                      // CMOVG & CMOVNLE (0F 4F)
    /* 50 */ ENTRY_CopyBytes2Mod,                      // MOVMSKPD MOVMSKPD
    /* 51 */ ENTRY_CopyBytes2Mod,                      // SQRTPS &
    /* 52 */ ENTRY_CopyBytes2Mod,                      // RSQRTTS RSQRTPS
    /* 53 */ ENTRY_CopyBytes2Mod,                      // RCPPS RCPSS
    /* 54 */ ENTRY_CopyBytes2Mod,                      // ANDPS ANDPD
    /* 55 */ ENTRY_CopyBytes2Mod,                      // ANDNPS ANDNPD
    /* 56 */ ENTRY_CopyBytes2Mod,                      // ORPS ORPD
    /* 57 */ ENTRY_CopyBytes2Mod,                      // XORPS XORPD
    /* 58 */ ENTRY_CopyBytes2Mod,                      // ADDPS &
    /* 59 */ ENTRY_CopyBytes2Mod,                      // MULPS &
    /* 5A */ ENTRY_CopyBytes2Mod,                      // CVTPS2PD &
    /* 5B */ ENTRY_CopyBytes2Mod,                      // CVTDQ2PS &
    /* 5C */ ENTRY_CopyBytes2Mod,                      // SUBPS &
    /* 5D */ ENTRY_CopyBytes2Mod,                      // MINPS &
    /* 5E */ ENTRY_CopyBytes2Mod,                      // DIVPS &
    /* 5F */ ENTRY_CopyBytes2Mod,                      // MASPS &
    /* 60 */ ENTRY_CopyBytes2Mod,                      // PUNPCKLBW/r
    /* 61 */ ENTRY_CopyBytes2Mod,                      // PUNPCKLWD/r
    /* 62 */ ENTRY_CopyBytes2Mod,                      // PUNPCKLWD/r
    /* 63 */ ENTRY_CopyBytes2Mod,                      // PACKSSWB/r
    /* 64 */ ENTRY_CopyBytes2Mod,                      // PCMPGTB/r
    /* 65 */ ENTRY_CopyBytes2Mod,                      // PCMPGTW/r
    /* 66 */ ENTRY_CopyBytes2Mod,                      // PCMPGTD/r
    /* 67 */ ENTRY_CopyBytes2Mod,                      // PACKUSWB/r
    /* 68 */ ENTRY_CopyBytes2Mod,                      // PUNPCKHBW/r
    /* 69 */ ENTRY_CopyBytes2Mod,                      // PUNPCKHWD/r
    /* 6A */ ENTRY_CopyBytes2Mod,                      // PUNPCKHDQ/r
    /* 6B */ ENTRY_CopyBytes2Mod,                      // PACKSSDW/r
    /* 6C */ ENTRY_CopyBytes2Mod,                      // PUNPCKLQDQ
    /* 6D */ ENTRY_CopyBytes2Mod,                      // PUNPCKHQDQ
    /* 6E */ ENTRY_CopyBytes2Mod,                      // MOVD/r
    /* 6F */ ENTRY_CopyBytes2Mod,                      // MOV/r
    /* 70 */ ENTRY_CopyBytes2Mod1,                     // PSHUFW/r ib
    /* 71 */ ENTRY_CopyBytes2Mod1,                     // PSLLW/6 ib,PSRAW/4 ib,PSRLW/2 ib
    /* 72 */ ENTRY_CopyBytes2Mod1,                     // PSLLD/6 ib,PSRAD/4 ib,PSRLD/2 ib
    /* 73 */ ENTRY_CopyBytes2Mod1,                     // PSLLQ/6 ib,PSRLQ/2 ib
    /* 74 */ ENTRY_CopyBytes2Mod,                      // PCMPEQB/r
    /* 75 */ ENTRY_CopyBytes2Mod,                      // PCMPEQW/r
    /* 76 */ ENTRY_CopyBytes2Mod,                      // PCMPEQD/r
    /* 77 */ ENTRY_CopyBytes1,                         // EMMS
    // extrq/insertq require mode=3 and are followed by two immediate bytes
    /* 78 */ ENTRY_Copy0F78,                           // VMREAD/r, 66/EXTRQ/r/ib/ib, F2/INSERTQ/r/ib/ib
    // extrq/insertq require mod=3, therefore ENTRY_CopyBytes2, but it ends up the same
    /* 79 */ ENTRY_CopyBytes2Mod,                      // VMWRITE/r, 66/EXTRQ/r, F2/INSERTQ/r
    /* 7A */ ENTRY_Invalid,                            // _7A
    /* 7B */ ENTRY_Invalid,                            // _7B
    /* 7C */ ENTRY_CopyBytes2Mod,                      // HADDPS
    /* 7D */ ENTRY_CopyBytes2Mod,                      // HSUBPS
    /* 7E */ ENTRY_CopyBytes2Mod,                      // MOVD/r
    /* 7F */ ENTRY_CopyBytes2Mod,                      // MOV/r
    /* 80 */ ENTRY_CopyBytes3Or5Target,                // JO
    /* 81 */ ENTRY_CopyBytes3Or5Target,                // JNO
    /* 82 */ ENTRY_CopyBytes3Or5Target,                // JB,JC,JNAE
    /* 83 */ ENTRY_CopyBytes3Or5Target,                // JAE,JNB,JNC
    /* 84 */ ENTRY_CopyBytes3Or5Target,                // JE,JZ,JZ
    /* 85 */ ENTRY_CopyBytes3Or5Target,                // JNE,JNZ
    /* 86 */ ENTRY_CopyBytes3Or5Target,                // JBE,JNA
    /* 87 */ ENTRY_CopyBytes3Or5Target,                // JA,JNBE
    /* 88 */ ENTRY_CopyBytes3Or5Target,                // JS
    /* 89 */ ENTRY_CopyBytes3Or5Target,                // JNS
    /* 8A */ ENTRY_CopyBytes3Or5Target,                // JP,JPE
    /* 8B */ ENTRY_CopyBytes3Or5Target,                // JNP,JPO
    /* 8C */ ENTRY_CopyBytes3Or5Target,                // JL,NGE
    /* 8D */ ENTRY_CopyBytes3Or5Target,                // JGE,JNL
    /* 8E */ ENTRY_CopyBytes3Or5Target,                // JLE,JNG
    /* 8F */ ENTRY_CopyBytes3Or5Target,                // JG,JNLE
    /* 90 */ ENTRY_CopyBytes2Mod,                      // CMOVO (0F 40)
    /* 91 */ ENTRY_CopyBytes2Mod,                      // CMOVNO (0F 41)
    /* 92 */ ENTRY_CopyBytes2Mod,                      // CMOVB & CMOVC & CMOVNAE (0F 42)
    /* 93 */ ENTRY_CopyBytes2Mod,                      // CMOVAE & CMOVNB & CMOVNC (0F 43)
    /* 94 */ ENTRY_CopyBytes2Mod,                      // CMOVE & CMOVZ (0F 44)
    /* 95 */ ENTRY_CopyBytes2Mod,                      // CMOVNE & CMOVNZ (0F 45)
    /* 96 */ ENTRY_CopyBytes2Mod,                      // CMOVBE & CMOVNA (0F 46)
    /* 97 */ ENTRY_CopyBytes2Mod,                      // CMOVA & CMOVNBE (0F 47)
    /* 98 */ ENTRY_CopyBytes2Mod,                      // CMOVS (0F 48)
    /* 99 */ ENTRY_CopyBytes2Mod,                      // CMOVNS (0F 49)
    /* 9A */ ENTRY_CopyBytes2Mod,                      // CMOVP & CMOVPE (0F 4A)
    /* 9B */ ENTRY_CopyBytes2Mod,                      // CMOVNP & CMOVPO (0F 4B)
    /* 9C */ ENTRY_CopyBytes2Mod,                      // CMOVL & CMOVNGE (0F 4C)
    /* 9D */ ENTRY_CopyBytes2Mod,                      // CMOVGE & CMOVNL (0F 4D)
    /* 9E */ ENTRY_CopyBytes2Mod,                      // CMOVLE & CMOVNG (0F 4E)
    /* 9F */ ENTRY_CopyBytes2Mod,                      // CMOVG & CMOVNLE (0F 4F)
    /* A0 */ ENTRY_CopyBytes1,                         // PUSH
    /* A1 */ ENTRY_CopyBytes1,                         // POP
    /* A2 */ ENTRY_CopyBytes1,                         // CPUID
    /* A3 */ ENTRY_CopyBytes2Mod,                      // BT  (0F A3)
    /* A4 */ ENTRY_CopyBytes2Mod1,                     // SHLD
    /* A5 */ ENTRY_CopyBytes2Mod,                      // SHLD
    /* A6 */ ENTRY_CopyBytes2Mod,                      // XBTS
    /* A7 */ ENTRY_CopyBytes2Mod,                      // IBTS
    /* A8 */ ENTRY_CopyBytes1,                         // PUSH
    /* A9 */ ENTRY_CopyBytes1,                         // POP
    /* AA */ ENTRY_CopyBytes1,                         // RSM
    /* AB */ ENTRY_CopyBytes2Mod,                      // BTS (0F AB)
    /* AC */ ENTRY_CopyBytes2Mod1,                     // SHRD
    /* AD */ ENTRY_CopyBytes2Mod,                      // SHRD

    // 0F AE mod76=mem mod543=0 fxsave
    // 0F AE mod76=mem mod543=1 fxrstor
    // 0F AE mod76=mem mod543=2 ldmxcsr
    // 0F AE mod76=mem mod543=3 stmxcsr
    // 0F AE mod76=mem mod543=4 xsave
    // 0F AE mod76=mem mod543=5 xrstor
    // 0F AE mod76=mem mod543=6 saveopt
    // 0F AE mod76=mem mod543=7 clflush
    // 0F AE mod76=11b mod543=5 lfence
    // 0F AE mod76=11b mod543=6 mfence
    // 0F AE mod76=11b mod543=7 sfence
    // F3 0F AE mod76=11b mod543=0 rdfsbase
    // F3 0F AE mod76=11b mod543=1 rdgsbase
    // F3 0F AE mod76=11b mod543=2 wrfsbase
    // F3 0F AE mod76=11b mod543=3 wrgsbase
    /* AE */ ENTRY_CopyBytes2Mod,                      // fxsave fxrstor ldmxcsr stmxcsr xsave xrstor saveopt clflush lfence mfence sfence rdfsbase rdgsbase wrfsbase wrgsbase
    /* AF */ ENTRY_CopyBytes2Mod,                      // IMUL (0F AF)
    /* B0 */ ENTRY_CopyBytes2Mod,                      // CMPXCHG (0F B0)
    /* B1 */ ENTRY_CopyBytes2Mod,                      // CMPXCHG (0F B1)
    /* B2 */ ENTRY_CopyBytes2Mod,                      // LSS/r
    /* B3 */ ENTRY_CopyBytes2Mod,                      // BTR (0F B3)
    /* B4 */ ENTRY_CopyBytes2Mod,                      // LFS/r
    /* B5 */ ENTRY_CopyBytes2Mod,                      // LGS/r
    /* B6 */ ENTRY_CopyBytes2Mod,                      // MOVZX/r
    /* B7 */ ENTRY_CopyBytes2Mod,                      // MOVZX/r
#ifdef DETOURS_X86
    /* B8 */ ENTRY_Copy0FB8,                           // jmpe f3/popcnt
#else
    /* B8 */ ENTRY_CopyBytes2Mod,                      // f3/popcnt
#endif
    /* B9 */ ENTRY_Invalid,                            // _B9
    /* BA */ ENTRY_CopyBytes2Mod1,                     // BT & BTC & BTR & BTS (0F BA)
    /* BB */ ENTRY_CopyBytes2Mod,                      // BTC (0F BB)
    /* BC */ ENTRY_CopyBytes2Mod,                      // BSF (0F BC)
    /* BD */ ENTRY_CopyBytes2Mod,                      // BSR (0F BD)
    /* BE */ ENTRY_CopyBytes2Mod,                      // MOVSX/r
    /* BF */ ENTRY_CopyBytes2Mod,                      // MOVSX/r
    /* C0 */ ENTRY_CopyBytes2Mod,                      // XADD/r
    /* C1 */ ENTRY_CopyBytes2Mod,                      // XADD/r
    /* C2 */ ENTRY_CopyBytes2Mod1,                     // CMPPS &
    /* C3 */ ENTRY_CopyBytes2Mod,                      // MOVNTI
    /* C4 */ ENTRY_CopyBytes2Mod1,                     // PINSRW /r ib
    /* C5 */ ENTRY_CopyBytes2Mod1,                     // PEXTRW /r ib
    /* C6 */ ENTRY_CopyBytes2Mod1,                     // SHUFPS & SHUFPD
    /* C7 */ ENTRY_CopyBytes2Mod,                      // CMPXCHG8B (0F C7)
    /* C8 */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* C9 */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* CA */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* CB */ ENTRY_CopyBytes1,                         // CVTPD2PI BSWAP 0F C8 + rd
    /* CC */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* CD */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* CE */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* CF */ ENTRY_CopyBytes1,                         // BSWAP 0F C8 + rd
    /* D0 */ ENTRY_CopyBytes2Mod,                      // ADDSUBPS (untestd)
    /* D1 */ ENTRY_CopyBytes2Mod,                      // PSRLW/r
    /* D2 */ ENTRY_CopyBytes2Mod,                      // PSRLD/r
    /* D3 */ ENTRY_CopyBytes2Mod,                      // PSRLQ/r
    /* D4 */ ENTRY_CopyBytes2Mod,                      // PADDQ
    /* D5 */ ENTRY_CopyBytes2Mod,                      // PMULLW/r
    /* D6 */ ENTRY_CopyBytes2Mod,                      // MOVDQ2Q / MOVQ2DQ
    /* D7 */ ENTRY_CopyBytes2Mod,                      // PMOVMSKB/r
    /* D8 */ ENTRY_CopyBytes2Mod,                      // PSUBUSB/r
    /* D9 */ ENTRY_CopyBytes2Mod,                      // PSUBUSW/r
    /* DA */ ENTRY_CopyBytes2Mod,                      // PMINUB/r
    /* DB */ ENTRY_CopyBytes2Mod,                      // PAND/r
    /* DC */ ENTRY_CopyBytes2Mod,                      // PADDUSB/r
    /* DD */ ENTRY_CopyBytes2Mod,                      // PADDUSW/r
    /* DE */ ENTRY_CopyBytes2Mod,                      // PMAXUB/r
    /* DF */ ENTRY_CopyBytes2Mod,                      // PANDN/r
    /* E0 */ ENTRY_CopyBytes2Mod ,                     // PAVGB
    /* E1 */ ENTRY_CopyBytes2Mod,                      // PSRAW/r
    /* E2 */ ENTRY_CopyBytes2Mod,                      // PSRAD/r
    /* E3 */ ENTRY_CopyBytes2Mod,                      // PAVGW
    /* E4 */ ENTRY_CopyBytes2Mod,                      // PMULHUW/r
    /* E5 */ ENTRY_CopyBytes2Mod,                      // PMULHW/r
    /* E6 */ ENTRY_CopyBytes2Mod,                      // CTDQ2PD &
    /* E7 */ ENTRY_CopyBytes2Mod,                      // MOVNTQ
    /* E8 */ ENTRY_CopyBytes2Mod,                      // PSUBB/r
    /* E9 */ ENTRY_CopyBytes2Mod,                      // PSUBW/r
    /* EA */ ENTRY_CopyBytes2Mod,                      // PMINSW/r
    /* EB */ ENTRY_CopyBytes2Mod,                      // POR/r
    /* EC */ ENTRY_CopyBytes2Mod,                      // PADDSB/r
    /* ED */ ENTRY_CopyBytes2Mod,                      // PADDSW/r
    /* EE */ ENTRY_CopyBytes2Mod,                      // PMAXSW /r
    /* EF */ ENTRY_CopyBytes2Mod,                      // PXOR/r
    /* F0 */ ENTRY_CopyBytes2Mod,                      // LDDQU
    /* F1 */ ENTRY_CopyBytes2Mod,                      // PSLLW/r
    /* F2 */ ENTRY_CopyBytes2Mod,                      // PSLLD/r
    /* F3 */ ENTRY_CopyBytes2Mod,                      // PSLLQ/r
    /* F4 */ ENTRY_CopyBytes2Mod,                      // PMULUDQ/r
    /* F5 */ ENTRY_CopyBytes2Mod,                      // PMADDWD/r
    /* F6 */ ENTRY_CopyBytes2Mod,                      // PSADBW/r
    /* F7 */ ENTRY_CopyBytes2Mod,                      // MASKMOVQ
    /* F8 */ ENTRY_CopyBytes2Mod,                      // PSUBB/r
    /* F9 */ ENTRY_CopyBytes2Mod,                      // PSUBW/r
    /* FA */ ENTRY_CopyBytes2Mod,                      // PSUBD/r
    /* FB */ ENTRY_CopyBytes2Mod,                      // FSUBQ/r
    /* FC */ ENTRY_CopyBytes2Mod,                      // PADDB/r
    /* FD */ ENTRY_CopyBytes2Mod,                      // PADDW/r
    /* FE */ ENTRY_CopyBytes2Mod,                      // PADDD/r
    /* FF */ ENTRY_Invalid,                            // _FF
};

BOOL CDetourDis::SanityCheckSystem()
{
    C_ASSERT(ARRAYSIZE(CDetourDis::s_rceCopyTable) == 256);
    C_ASSERT(ARRAYSIZE(CDetourDis::s_rceCopyTable0F) == 256);
    return TRUE;
}
#endif // defined(DETOURS_X64) || defined(DETOURS_X86)

/////////////////////////////////////////////////////////// IA64 Disassembler.
//
#ifdef DETOURS_IA64

#if defined(_IA64_) != defined(DETOURS_IA64_OFFLINE_LIBRARY)
// Compile DETOUR_IA64_BUNDLE for native IA64 or cross, but not both -- we get duplicates otherwise.
const DETOUR_IA64_BUNDLE::DETOUR_IA64_METADATA DETOUR_IA64_BUNDLE::s_rceCopyTable[33] =
{
    { 0x00, M_UNIT,      I_UNIT,      I_UNIT,   },
    { 0x01, M_UNIT,      I_UNIT,      I_UNIT,   },
    { 0x02, M_UNIT,      I_UNIT,      I_UNIT,   },
    { 0x03, M_UNIT,      I_UNIT,      I_UNIT,   },
    { 0x04, M_UNIT,      L_UNIT,      X_UNIT,   },
    { 0x05, M_UNIT,      L_UNIT,      X_UNIT,   },
    { 0x06, 0,           0,           0,        },
    { 0x07, 0,           0,           0,        },
    { 0x08, M_UNIT,      M_UNIT,      I_UNIT,   },
    { 0x09, M_UNIT,      M_UNIT,      I_UNIT,   },
    { 0x0a, M_UNIT,      M_UNIT,      I_UNIT,   },
    { 0x0b, M_UNIT,      M_UNIT,      I_UNIT,   },
    { 0x0c, M_UNIT,      F_UNIT,      I_UNIT,   },
    { 0x0d, M_UNIT,      F_UNIT,      I_UNIT,   },
    { 0x0e, M_UNIT,      M_UNIT,      F_UNIT,   },
    { 0x0f, M_UNIT,      M_UNIT,      F_UNIT,   },
    { 0x10, M_UNIT,      I_UNIT,      B_UNIT,   },
    { 0x11, M_UNIT,      I_UNIT,      B_UNIT,   },
    { 0x12, M_UNIT,      B_UNIT,      B_UNIT,   },
    { 0x13, M_UNIT,      B_UNIT,      B_UNIT,   },
    { 0x14, 0,           0,           0,        },
    { 0x15, 0,           0,           0,        },
    { 0x16, B_UNIT,      B_UNIT,      B_UNIT,   },
    { 0x17, B_UNIT,      B_UNIT,      B_UNIT,   },
    { 0x18, M_UNIT,      M_UNIT,      B_UNIT,   },
    { 0x19, M_UNIT,      M_UNIT,      B_UNIT,   },
    { 0x1a, 0,           0,           0,        },
    { 0x1b, 0,           0,           0,        },
    { 0x1c, M_UNIT,      F_UNIT,      B_UNIT,   },
    { 0x1d, M_UNIT,      F_UNIT,      B_UNIT,   },
    { 0x1e, 0,           0,           0,        },
    { 0x1f, 0,           0,           0,        },
    { 0x00, 0,           0,           0,        },
};

// 120 112 104 96 88 80 72 64 56 48 40 32 24 16  8  0
//  f.  e.  d. c. b. a. 9. 8. 7. 6. 5. 4. 3. 2. 1. 0.

//                                      00
// f.e. d.c. b.a. 9.8. 7.6. 5.4. 3.2. 1.0.
// 0000 0000 0000 0000 0000 0000 0000 001f : Template [4..0]
// 0000 0000 0000 0000 0000 03ff ffff ffe0 : Zero [ 41..  5]
// 0000 0000 0000 0000 0000 3c00 0000 0000 : Zero [ 45.. 42]
// 0000 0000 0007 ffff ffff c000 0000 0000 : One  [ 82.. 46]
// 0000 0000 0078 0000 0000 0000 0000 0000 : One  [ 86.. 83]
// 0fff ffff ff80 0000 0000 0000 0000 0000 : Two  [123.. 87]
// f000 0000 0000 0000 0000 0000 0000 0000 : Two  [127..124]
BYTE DETOUR_IA64_BUNDLE::GetTemplate() const
{
    return (data[0] & 0x1f);
}

BYTE DETOUR_IA64_BUNDLE::GetInst0() const
{
    return ((data[5] & 0x3c) >> 2);
}

BYTE DETOUR_IA64_BUNDLE::GetInst1() const
{
    return ((data[10] & 0x78) >> 3);
}

BYTE DETOUR_IA64_BUNDLE::GetInst2() const
{
    return ((data[15] & 0xf0) >> 4);
}

BYTE DETOUR_IA64_BUNDLE::GetUnit(BYTE slot) const
{
    switch (slot) {
    case 0: return GetUnit0();
    case 1: return GetUnit1();
    case 2: return GetUnit2();
    }
    __debugbreak();
    return 0;
}

BYTE DETOUR_IA64_BUNDLE::GetUnit0() const
{
    return s_rceCopyTable[data[0] & 0x1f].nUnit0;
}

BYTE DETOUR_IA64_BUNDLE::GetUnit1() const
{
    return s_rceCopyTable[data[0] & 0x1f].nUnit1;
}

BYTE DETOUR_IA64_BUNDLE::GetUnit2() const
{
    return s_rceCopyTable[data[0] & 0x1f].nUnit2;
}

UINT64 DETOUR_IA64_BUNDLE::GetData0() const
{
    return (((wide[0] & 0x000003ffffffffe0) >> 5));
}

UINT64 DETOUR_IA64_BUNDLE::GetData1() const
{
    return (((wide[0] & 0xffffc00000000000) >> 46) |
            ((wide[1] & 0x000000000007ffff) << 18));
}

UINT64 DETOUR_IA64_BUNDLE::GetData2() const
{
    return (((wide[1] & 0x0fffffffff800000) >> 23));
}

VOID DETOUR_IA64_BUNDLE::SetInst(BYTE slot, BYTE nInst)
{
    switch (slot)
    {
    case 0: SetInst0(nInst); return;
    case 1: SetInst1(nInst); return;
    case 2: SetInst2(nInst); return;
    }
    __debugbreak();
}

VOID DETOUR_IA64_BUNDLE::SetInst0(BYTE nInst)
{
    data[5] = (data[5] & ~0x3c) | ((nInst << 2) & 0x3c);
}

VOID DETOUR_IA64_BUNDLE::SetInst1(BYTE nInst)
{
    data[10] = (data[10] & ~0x78) | ((nInst << 3) & 0x78);
}

VOID DETOUR_IA64_BUNDLE::SetInst2(BYTE nInst)
{
    data[15] = (data[15] & ~0xf0) | ((nInst << 4) & 0xf0);
}

VOID DETOUR_IA64_BUNDLE::SetData(BYTE slot, UINT64 nData)
{
    switch (slot)
    {
    case 0: SetData0(nData); return;
    case 1: SetData1(nData); return;
    case 2: SetData2(nData); return;
    }
    __debugbreak();
}

VOID DETOUR_IA64_BUNDLE::SetData0(UINT64 nData)
{
    wide[0] = (wide[0] & ~0x000003ffffffffe0) | (( nData << 5)  & 0x000003ffffffffe0);
}

VOID DETOUR_IA64_BUNDLE::SetData1(UINT64 nData)
{
    wide[0] = (wide[0] & ~0xffffc00000000000) | ((nData << 46) & 0xffffc00000000000);
    wide[1] = (wide[1] & ~0x000000000007ffff) | ((nData >> 18) & 0x000000000007ffff);
}

VOID DETOUR_IA64_BUNDLE::SetData2(UINT64 nData)
{
    wide[1] = (wide[1] & ~0x0fffffffff800000) | ((nData << 23) & 0x0fffffffff800000);
}

UINT64 DETOUR_IA64_BUNDLE::GetInstruction(BYTE slot) const
{
    switch (slot) {
    case 0: return GetInstruction0();
    case 1: return GetInstruction1();
    case 2: return GetInstruction2();
    }
    __debugbreak();
    return 0;
}

UINT64 DETOUR_IA64_BUNDLE::GetInstruction0() const
{
    // 41 bits from wide[0], skipping the 5 bit template.
    return GetBits(wide[0], DETOUR_IA64_INSTRUCTION0_OFFSET, DETOUR_IA64_INSTRUCTION_SIZE);
}

UINT64 DETOUR_IA64_BUNDLE::GetInstruction1() const
{
    // 64-46 bits from wide[0] and the rest from wide[1].
    const UINT count0 = 64 - DETOUR_IA64_INSTRUCTION1_OFFSET;
    const UINT count1 = DETOUR_IA64_INSTRUCTION_SIZE - count0;
    return GetBits(wide[0], DETOUR_IA64_INSTRUCTION1_OFFSET, count0) | (GetBits(wide[1], 0, count1) << count0);
}

UINT64 DETOUR_IA64_BUNDLE::GetInstruction2() const
{
    // Upper 41 bits of wide[1].
    return wide[1] >> (64 - DETOUR_IA64_INSTRUCTION_SIZE);
}

void DETOUR_IA64_BUNDLE::SetInstruction(BYTE slot, UINT64 instruction)
{
    switch (slot) {
    case 0: SetInstruction0(instruction); return;
    case 1: SetInstruction1(instruction); return;
    case 2: SetInstruction2(instruction); return;
    }
    __debugbreak();
}

void DETOUR_IA64_BUNDLE::SetInstruction0(UINT64 instruction)
{
    wide[0] = SetBits(wide[0], DETOUR_IA64_INSTRUCTION0_OFFSET, DETOUR_IA64_INSTRUCTION_SIZE, instruction);
}

void DETOUR_IA64_BUNDLE::SetInstruction1(UINT64 instruction)
{
    UINT const count0 = 64 - DETOUR_IA64_INSTRUCTION1_OFFSET;
    UINT const count1 = DETOUR_IA64_INSTRUCTION_SIZE - count0;
    UINT64 const wide0 = SetBits(wide[0], DETOUR_IA64_INSTRUCTION1_OFFSET, count0, instruction);
    UINT64 const wide1 = SetBits(wide[1], 0, count1, instruction >> count0);
    wide[0] = wide0;
    wide[1] = wide1;
}

void DETOUR_IA64_BUNDLE::SetInstruction2(UINT64 instruction)
{
    // Set upper 41 bits of wide[1].
    wide[1] = SetBits(wide[1], 64 - DETOUR_IA64_INSTRUCTION_SIZE, DETOUR_IA64_INSTRUCTION_SIZE, instruction);
}

UINT64 DETOUR_IA64_BUNDLE::SignExtend(UINT64 Value, UINT64 Offset)
// This definition is from the IA64 manual.
{
    if ((Value & (((UINT64)1) << (Offset - 1))) == 0)
        return Value;
    UINT64 const new_value = Value | ((~(UINT64)0) << Offset);
    return new_value;
}

UINT64 DETOUR_IA64_BUNDLE::GetBits(UINT64 Value, UINT64 Offset, UINT64 Count)
{
    UINT64 const new_value = (Value >> Offset) & ~(~((UINT64)0) << Count);
    return new_value;
}

UINT64 DETOUR_IA64_BUNDLE::SetBits(UINT64 Value, UINT64 Offset, UINT64 Count, UINT64 Field)
{
    UINT64 const mask = (~((~(UINT64)0) << Count)) << Offset;
    UINT64 const new_value = (Value & ~mask) | ((Field << Offset) & mask);
    return new_value;
}

UINT64 DETOUR_IA64_BUNDLE::GetOpcode(UINT64 instruction)
// Get 4bit primary opcode.
{
    UINT64 const opcode = GetBits(instruction, DETOUR_IA64_INSTRUCTION_SIZE - 4, 4);
    return opcode;
}

UINT64 DETOUR_IA64_BUNDLE::GetX(UINT64 instruction)
// Get 1bit opcode extension.
{
    UINT64 const x = GetBits(instruction, 33, 1);
    return x;
}

UINT64 DETOUR_IA64_BUNDLE::GetX3(UINT64 instruction)
// Get 3bit opcode extension.
{
    UINT64 const x3 = GetBits(instruction, 33, 3);
    return x3;
}

UINT64 DETOUR_IA64_BUNDLE::GetX6(UINT64 instruction)
// Get 6bit opcode extension.
{
    UINT64 const x6 = GetBits(instruction, 27, 6);
    return x6;
}

UINT64 DETOUR_IA64_BUNDLE::GetImm7a(UINT64 instruction)
{
    UINT64 const imm7a = GetBits(instruction, 6, 7);
    return imm7a;
}

UINT64 DETOUR_IA64_BUNDLE::SetImm7a(UINT64 instruction, UINT64 imm7a)
{
    UINT64 const new_instruction = SetBits(instruction, 6, 7, imm7a);
    return new_instruction;
}

UINT64 DETOUR_IA64_BUNDLE::GetImm13c(UINT64 instruction)
{
    UINT64 const imm13c = GetBits(instruction, 20, 13);
    return imm13c;
}

UINT64 DETOUR_IA64_BUNDLE::SetImm13c(UINT64 instruction, UINT64 imm13c)
{
    UINT64 const new_instruction = SetBits(instruction, 20, 13, imm13c);
    return new_instruction;
}

UINT64 DETOUR_IA64_BUNDLE::GetSignBit(UINT64 instruction)
{
    UINT64 const signBit = GetBits(instruction, 36, 1);
    return signBit;
}

UINT64 DETOUR_IA64_BUNDLE::SetSignBit(UINT64 instruction, UINT64 signBit)
{
    UINT64 const new_instruction = SetBits(instruction, 36, 1, signBit);
    return new_instruction;
}

UINT64 DETOUR_IA64_BUNDLE::GetImm20a(UINT64 instruction)
{
    UINT64 const imm20a = GetBits(instruction, 6, 20);
    return imm20a;
}

UINT64 DETOUR_IA64_BUNDLE::SetImm20a(UINT64 instruction, UINT64 imm20a)
{
    UINT64 const new_instruction = SetBits(instruction, 6, 20, imm20a);
    return new_instruction;
}

UINT64 DETOUR_IA64_BUNDLE::GetImm20b(UINT64 instruction)
{
    UINT64 const imm20b = GetBits(instruction, 13, 20);
    return imm20b;
}

UINT64 DETOUR_IA64_BUNDLE::SetImm20b(UINT64 instruction, UINT64 imm20b)
{
    UINT64 const new_instruction = SetBits(instruction, 13, 20, imm20b);
    return new_instruction;
}

bool DETOUR_IA64_BUNDLE::RelocateInstruction(_Inout_ DETOUR_IA64_BUNDLE* pDst,
                                             _In_ BYTE slot,
                                             _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra) const
/*
    If pBundleExtra is provided and instruction is IP-relative,
    this function relocates instruction to target pBundleExtra,
    pBundleExtra is set to brl the original target, and return true.

    [Not used] If pBundleExtra is not provided and instruction is IP-relative, return true.

    Else return false.

    The following IP-relative forms are recognized:
        br and br.call
        chk.s.m integer and float
        chk.a.nc integer and float
        chk.a.clr integer and float
        chk.s.i
        fchkf

    Brl is handled elsewhere, because the code was previously written.

    Branch prediction hints are not relocated.
*/
{
    UINT64 const instruction = GetInstruction(slot);
    UINT64 const opcode = GetOpcode(instruction);
    size_t const dest = (size_t)pDst;
    size_t const extra = (size_t)pBundleExtra;

    switch (GetUnit(slot)) {
    case F_UNIT:
        // F14 fchkf
        if (opcode == 0 && GetX(instruction) == 0 && GetX6(instruction) == 8) {
            goto imm20a;
        }
        return false;

    case M_UNIT:
        // M20 x3 == 1 integer chk.s.m
        // M21 x3 == 3 floating point chk.s
        if (opcode == 1) {
            UINT64 const x3 = GetX3(instruction);
            if (x3 == 1 || x3 == 3) {
                goto imm13_7;
            }
        }

        // M22 x3 == 4 integer chk.a.nc
        // M22 x3 == 5 integer chk.a.clr
        // M23 x3 == 6 floating point chk.a.nc
        // M23 x3 == 7 floating point chk.a.clr
        if (opcode == 0) {
            UINT64 const x3 = GetX3(instruction);
            if (x3 == 4 || x3 == 5 || x3 == 6 || x3 == 7) {
                goto imm20b;
            }
        }
        return false;
    case I_UNIT:
        // I20
        if (opcode == 0 && GetX3(instruction) == 1) { // chk.s.i
            goto imm13_7;
        }
        return false;
    case B_UNIT:
        // B1 B2 B3
        // 4 br
        // 5 br.call
        if (opcode == 4 || opcode == 5) {
            goto imm20b;
        }
        return false;
    }
    return false;

    UINT64 imm;
    UINT64 new_instruction;

imm13_7:
    imm = SignExtend((GetSignBit(instruction) << 20) | (GetImm13c(instruction) << 7) | GetImm7a(instruction), 21) << 4;
    new_instruction = SetSignBit(SetImm13c(SetImm7a(instruction, (extra - dest) >> 4), (extra - dest) >> 11), extra < dest);
    goto set_brl;

imm20a:
    imm = SignExtend((GetSignBit(instruction) << 20) | GetImm20a(instruction), 21) << 4;
    new_instruction = SetSignBit(SetImm20a(instruction, (extra - dest) >> 4), extra < dest);
    goto set_brl;

imm20b:
    imm = SignExtend((GetSignBit(instruction) << 20) | GetImm20b(instruction), 21) << 4;
    new_instruction = SetSignBit(SetImm20b(instruction, (extra - dest) >> 4), extra < dest);
    goto set_brl;

set_brl:
    if (pBundleExtra != NULL) {
        pDst->SetInstruction(slot, new_instruction);
        pBundleExtra->SetBrl((size_t)this + imm);
    }
    return true;
}

UINT DETOUR_IA64_BUNDLE::RelocateBundle(_Inout_ DETOUR_IA64_BUNDLE* pDst,
                                        _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra) const
/*
    Having already copied the bundle unchanged, then relocate its instructions one at a time.
    Return how many extra bytes are required to relocate the bundle.
*/
{
    UINT nExtraBytes = 0;
    for (BYTE slot = 0; slot < DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE; ++slot) {
        if (!RelocateInstruction(pDst, slot, pBundleExtra)) {
            continue;
        }
        pBundleExtra -= !!pBundleExtra;
        nExtraBytes += sizeof(DETOUR_IA64_BUNDLE);
    }
    return nExtraBytes;
}

BOOL DETOUR_IA64_BUNDLE::IsBrl() const
{
    // f.e. d.c. b.a. 9.8. 7.6. 5. 4. 3. 2. 1. 0.
    // c000 0070 0000 0000 0000 00 01 00 00 00 05 : brl.sptk.few
    // c8ff fff0 007f fff0 ffff 00 01 00 00 00 05 : brl.sptk.few
    // c000 0048 0000 0000 0001 00 00 00 00 00 05 : brl.sptk.many
    return ((wide[0] & 0x000000000000001e) == 0x0000000000000004 && // 4 or 5.
            (wide[1] & 0xe000000000000000) == 0xc000000000000000);  // c or d.
}

VOID DETOUR_IA64_BUNDLE::SetBrl()
{
    wide[0] = 0x0000000100000005;   // few
    //wide[0] = 0x0000000180000005; // many
    wide[1] = 0xc000000800000000;
}

UINT64 DETOUR_IA64_BUNDLE::GetBrlImm() const
{
    return (
            //          0x0000000000fffff0
            ((wide[1] & 0x00fffff000000000) >> 32) |    // all 20 bits of imm20b.
            //          0x000000ffff000000
            ((wide[0] & 0xffff000000000000) >> 24) |    // bottom 16 bits of imm39.
            //          0x7fffff0000000000
            ((wide[1] & 0x00000000007fffff) << 40) |    // top 23 bits of imm39.
            //          0x8000000000000000
            ((wide[1] & 0x0800000000000000) <<  4)      // single bit of i.
           );
}

VOID DETOUR_IA64_BUNDLE::SetBrlImm(UINT64 imm)
{
    wide[0] = ((wide[0] & ~0xffff000000000000) |
               //      0xffff000000000000
               ((imm & 0x000000ffff000000) << 24)       // bottom 16 bits of imm39.
              );
    wide[1] = ((wide[1] & ~0x08fffff0007fffff) |
               //      0x00fffff000000000
               ((imm & 0x0000000000fffff0) << 32) |     // all 20 bits of imm20b.
               //      0x00000000007fffff
               ((imm & 0x7fffff0000000000) >> 40) |     // top 23 bits of imm39.
               //      0x0800000000000000
               ((imm & 0x8000000000000000) >>  4)       // single bit of i.
              );
}

UINT64 DETOUR_IA64_BUNDLE::GetBrlTarget() const
{
    return (UINT64)this + GetBrlImm();
}

VOID DETOUR_IA64_BUNDLE::SetBrl(UINT64 target)
{
    UINT64 imm = target - (UINT64)this;
    SetBrl();
    SetBrlImm(imm);
}

VOID DETOUR_IA64_BUNDLE::SetBrlTarget(UINT64 target)
{
    UINT64 imm = target - (UINT64)this;
    SetBrlImm(imm);
}

BOOL DETOUR_IA64_BUNDLE::IsMovlGp() const
{
    // f.e. d.c. b.a. 9.8. 7.6. 5.4. 3.2. 1.0.
    // 6fff f7f0 207f ffff ffff c001 0000 0004
    // 6000 0000 2000 0000 0000 0001 0000 0004
    return ((wide[0] & 0x00003ffffffffffe) == 0x0000000100000004 &&
            (wide[1] & 0xf000080fff800000) == 0x6000000020000000);
}

UINT64 DETOUR_IA64_BUNDLE::GetMovlGp() const
{
    UINT64 raw = (
                  //          0x0000000000000070
                  ((wide[1] & 0x000007f000000000) >> 36) |
                  //          0x000000000000ff80
                  ((wide[1] & 0x07fc000000000000) >> 43) |
                  //          0x00000000001f0000
                  ((wide[1] & 0x0003e00000000000) >> 29) |
                  //          0x0000000000200000
                  ((wide[1] & 0x0000100000000000) >> 23) |
                  //          0x000000ffffc00000
                  ((wide[0] & 0xffffc00000000000) >> 24) |
                  //          0x7fffff0000000000
                  ((wide[1] & 0x00000000007fffff) << 40) |
                  //          0x8000000000000000
                  ((wide[1] & 0x0800000000000000) <<  4)
                 );

    return (INT64)raw;
}

VOID DETOUR_IA64_BUNDLE::SetMovlGp(UINT64 gp)
{
    UINT64 raw = (UINT64)gp;

    wide[0] = (0x0000000100000005 |
               //      0xffffc00000000000
               ((raw & 0x000000ffffc00000) << 24)
              );
    wide[1] = (
               0x6000000020000000 |
               //      0x0000070000000000
               ((raw & 0x0000000000000070) << 36) |
               //      0x07fc000000000000
               ((raw & 0x000000000000ff80) << 43) |
               //      0x0003e00000000000
               ((raw & 0x00000000001f0000) << 29) |
               //      0x0000100000000000
               ((raw & 0x0000000000200000) << 23) |
               //      0x00000000007fffff
               ((raw & 0x7fffff0000000000) >> 40) |
               //      0x0800000000000000
               ((raw & 0x8000000000000000) >>  4)
              );
}

UINT DETOUR_IA64_BUNDLE::Copy(_Out_ DETOUR_IA64_BUNDLE *pDst,
                              _Inout_opt_ DETOUR_IA64_BUNDLE* pBundleExtra) const
{
    // Copy the bytes unchanged.

#pragma warning(suppress:6001) // using uninitialized *pDst
    pDst->wide[0] = wide[0];
    pDst->wide[1] = wide[1];

    // Relocate if necessary.

    UINT nExtraBytes = RelocateBundle(pDst, pBundleExtra);

    if (GetUnit1() == L_UNIT && IsBrl()) {
        pDst->SetBrlTarget(GetBrlTarget());
    }

    return nExtraBytes;
}

BOOL DETOUR_IA64_BUNDLE::SetNop(BYTE slot)
{
    switch (GetUnit(slot)) {
      case I_UNIT:
      case M_UNIT:
      case F_UNIT:
        SetInst(slot, 0);
        SetData(slot, 0x8000000);
        return true;
      case B_UNIT:
        SetInst(slot, 2);
        SetData(slot, 0);
        return true;
    }
    DebugBreak();
    return false;
}

BOOL DETOUR_IA64_BUNDLE::SetNop0()
{
    return SetNop(0);
}

BOOL DETOUR_IA64_BUNDLE::SetNop1()
{
    return SetNop(1);
}

BOOL DETOUR_IA64_BUNDLE::SetNop2()
{
    return SetNop(2);
}

VOID DETOUR_IA64_BUNDLE::SetStop()
{
    data[0] |= 0x01;
}

#endif // DETOURS_IA64

PVOID WINAPI DetourCopyInstruction(_In_opt_ PVOID pDst,
                                   _Inout_opt_ PVOID *ppDstPool,
                                   _In_ PVOID pSrc,
                                   _Out_opt_ PVOID *ppTarget,
                                   _Out_opt_ LONG *plExtra)
{
    LONG nExtra;
    DETOUR_IA64_BUNDLE bExtra;
    DETOUR_IA64_BUNDLE *pbSrc = (DETOUR_IA64_BUNDLE *)pSrc;
    DETOUR_IA64_BUNDLE *pbDst = pDst ? (DETOUR_IA64_BUNDLE *)pDst : &bExtra;

    plExtra = plExtra ? plExtra : &nExtra;
    *plExtra = 0;

    if (ppTarget != NULL) {
        if (pbSrc->IsBrl()) {
            *ppTarget = (PVOID)pbSrc->GetBrlTarget();
        }
        else {
            *ppTarget = DETOUR_INSTRUCTION_TARGET_NONE;
        }
    }
    *plExtra = (LONG)pbSrc->Copy(pbDst, ppDstPool ? ((DETOUR_IA64_BUNDLE*)*ppDstPool) - 1 : (DETOUR_IA64_BUNDLE*)NULL);
    return pbSrc + 1;
}

#endif // DETOURS_IA64

#ifdef DETOURS_ARM

#define DETOURS_PFUNC_TO_PBYTE(p)  ((PBYTE)(((ULONG_PTR)(p)) & ~(ULONG_PTR)1))
#define DETOURS_PBYTE_TO_PFUNC(p)  ((PBYTE)(((ULONG_PTR)(p)) | (ULONG_PTR)1))

#define c_PCAdjust  4       // The PC value of an instruction is the PC address plus 4.
#define c_PC        15      // The register number for the Program Counter
#define c_LR        14      // The register number for the Link Register
#define c_SP        13      // The register number for the Stack Pointer
#define c_NOP       0xbf00  // A nop instruction
#define c_BREAK     0xdefe  // A nop instruction

class CDetourDis
{
  public:
    CDetourDis();

    PBYTE   CopyInstruction(PBYTE pDst,
                            PBYTE *ppDstPool,
                            PBYTE pSrc,
                            PBYTE *ppTarget,
                            LONG *plExtra);

  public:
    typedef BYTE (CDetourDis::* COPYFUNC)(PBYTE pbDst, PBYTE pbSrc);

    struct COPYENTRY {
        USHORT      nOpcode;
        COPYFUNC    pfCopy;
    };

    typedef const COPYENTRY * REFCOPYENTRY;

    struct Branch5
    {
        DWORD Register : 3;
        DWORD Imm5 : 5;
        DWORD Padding : 1;
        DWORD I : 1;
        DWORD OpCode : 6;
    };

    struct Branch5Target
    {
        DWORD Padding : 1;
        DWORD Imm5 : 5;
        DWORD I : 1;
        DWORD Padding2 : 25;
    };

    struct Branch8
    {
        DWORD Imm8 : 8;
        DWORD Condition : 4;
        DWORD OpCode : 4;
    };

    struct Branch8Target
    {
        DWORD Padding : 1;
        DWORD Imm8 : 8;
        DWORD Padding2 : 23;
    };

    struct Branch11
    {
        DWORD Imm11 : 11;
        DWORD OpCode : 5;
    };

    struct Branch11Target
    {
        DWORD Padding : 1;
        DWORD Imm11 : 11;
        DWORD Padding2 : 20;
    };

    struct Branch20
    {
        DWORD Imm11 : 11;
        DWORD J2 : 1;
        DWORD IT : 1;
        DWORD J1 : 1;
        DWORD Other : 2;
        DWORD Imm6 : 6;
        DWORD Condition : 4;
        DWORD Sign : 1;
        DWORD OpCode : 5;
    };

    struct Branch20Target
    {
        DWORD Padding : 1;
        DWORD Imm11 : 11;
        DWORD Imm6 : 6;
        DWORD J1 : 1;
        DWORD J2 : 1;
        DWORD Sign : 1;
        INT32 Padding2 : 11;
    };

    struct Branch24
    {
        DWORD Imm11             : 11;
        DWORD J2                : 1;
        DWORD InstructionSet    : 1;
        DWORD J1                : 1;
        DWORD Link              : 1;
        DWORD Branch            : 1;
        DWORD Imm10             : 10;
        DWORD Sign              : 1;
        DWORD OpCode            : 5;
    };

    struct Branch24Target
    {
        DWORD Padding : 1;
        DWORD Imm11 : 11;
        DWORD Imm10 : 10;
        DWORD I2 : 1;
        DWORD I1 : 1;
        DWORD Sign : 1;
        INT32 Padding2 : 7;
    };

    struct LiteralLoad8
    {
        DWORD Imm8 : 8;
        DWORD Register : 3;
        DWORD OpCode : 5;
    };

    struct LiteralLoad8Target
    {
        DWORD Padding : 2;
        DWORD Imm8 : 8;
        DWORD Padding2 : 22;
    };

    struct LiteralLoad12
    {
        DWORD Imm12 : 12;
        DWORD Register : 4;
        DWORD OpCodeSuffix : 7;
        DWORD Add : 1;
        DWORD OpCodePrefix : 8;
    };

    struct LiteralLoad12Target
    {
        DWORD Imm12 : 12;
        DWORD Padding : 20;
    };

    struct ImmediateRegisterLoad32
    {
        DWORD Imm12 : 12;
        DWORD DestinationRegister : 4;
        DWORD SourceRegister: 4;
        DWORD OpCode : 12;
    };

    struct ImmediateRegisterLoad16
    {
        DWORD DestinationRegister : 3;
        DWORD SourceRegister: 3;
        DWORD OpCode : 10;
    };

    struct TableBranch
    {
        DWORD IndexRegister : 4;
        DWORD HalfWord : 1;
        DWORD OpCodeSuffix : 11;
        DWORD BaseRegister : 4;
        DWORD OpCodePrefix : 12;
    };

    struct Shift
    {
        DWORD Imm2 : 2;
        DWORD Imm3 : 3;
    };

    struct Add32
    {
        DWORD SecondOperandRegister : 4;
        DWORD Type : 2;
        DWORD Imm2 : 2;
        DWORD DestinationRegister : 4;
        DWORD Imm3 : 3;
        DWORD Padding : 1;
        DWORD FirstOperandRegister : 4;
        DWORD SetFlags : 1;
        DWORD OpCode : 11;
    };

    struct LogicalShiftLeft32
    {
        DWORD SourceRegister : 4;
        DWORD Padding : 2;
        DWORD Imm2 : 2;
        DWORD DestinationRegister : 4;
        DWORD Imm3 : 3;
        DWORD Padding2 : 5;
        DWORD SetFlags : 1;
        DWORD OpCode : 11;
    };

    struct StoreImmediate12
    {
        DWORD Imm12 : 12;
        DWORD SourceRegister : 4;
        DWORD BaseRegister : 4;
        DWORD OpCode : 12;
    };

  protected:
    BYTE    PureCopy16(BYTE* pSource, BYTE* pDest);
    BYTE    PureCopy32(BYTE* pSource, BYTE* pDest);
    BYTE    CopyMiscellaneous16(BYTE* pSource, BYTE* pDest);
    BYTE    CopyConditionalBranchOrOther16(BYTE* pSource, BYTE* pDest);
    BYTE    CopyUnConditionalBranch16(BYTE* pSource, BYTE* pDest);
    BYTE    CopyLiteralLoad16(BYTE* pSource, BYTE* pDest);
    BYTE    CopyBranchExchangeOrDataProcessing16(BYTE* pSource, BYTE* pDest);
    BYTE    CopyBranch24(BYTE* pSource, BYTE* pDest);
    BYTE    CopyBranchOrMiscellaneous32(BYTE* pSource, BYTE* pDest);
    BYTE    CopyLiteralLoad32(BYTE* pSource, BYTE* pDest);
    BYTE    CopyLoadAndStoreSingle(BYTE* pSource, BYTE* pDest);
    BYTE    CopyLoadAndStoreMultipleAndSRS(BYTE* pSource, BYTE* pDest);
    BYTE    CopyTableBranch(BYTE* pSource, BYTE* pDest);
    BYTE    BeginCopy32(BYTE* pSource, BYTE* pDest);

    LONG    DecodeBranch5(ULONG opcode);
    USHORT  EncodeBranch5(ULONG originalOpCode, LONG delta);
    LONG    DecodeBranch8(ULONG opcode);
    USHORT  EncodeBranch8(ULONG originalOpCode, LONG delta);
    LONG    DecodeBranch11(ULONG opcode);
    USHORT  EncodeBranch11(ULONG originalOpCode, LONG delta);
    BYTE    EmitBranch11(PUSHORT& pDest, LONG relativeAddress);
    LONG    DecodeBranch20(ULONG opcode);
    ULONG   EncodeBranch20(ULONG originalOpCode, LONG delta);
    LONG    DecodeBranch24(ULONG opcode, BOOL& fLink);
    ULONG   EncodeBranch24(ULONG originalOpCode, LONG delta, BOOL fLink);
    LONG    DecodeLiteralLoad8(ULONG instruction);
    LONG    DecodeLiteralLoad12(ULONG instruction);
    BYTE    EmitLiteralLoad8(PUSHORT& pDest, BYTE targetRegister, PBYTE pLiteral);
    BYTE    EmitLiteralLoad12(PUSHORT& pDest, BYTE targetRegister, PBYTE pLiteral);
    BYTE    EmitImmediateRegisterLoad32(PUSHORT& pDest, BYTE reg);
    BYTE    EmitImmediateRegisterLoad16(PUSHORT& pDest, BYTE reg);
    BYTE    EmitLongLiteralLoad(PUSHORT& pDest, BYTE reg, PVOID pTarget);
    BYTE    EmitLongBranch(PUSHORT& pDest, PVOID pTarget);
    USHORT  CalculateExtra(BYTE sourceLength, BYTE* pDestStart, BYTE* pDestEnd);

  protected:
    ULONG GetLongInstruction(BYTE* pSource)
    {
        return (((PUSHORT)pSource)[0] << 16) | (((PUSHORT)pSource)[1]);
    }

    BYTE EmitLongInstruction(PUSHORT& pDstInst, ULONG instruction)
    {
        *pDstInst++ = (USHORT)(instruction >> 16);
        *pDstInst++ = (USHORT)instruction;
        return sizeof(ULONG);
    }

    BYTE EmitShortInstruction(PUSHORT& pDstInst, USHORT instruction)
    {
        *pDstInst++ = instruction;
        return sizeof(USHORT);
    }

    PBYTE Align4(PBYTE pValue)
    {
        return (PBYTE)(((size_t)pValue) & ~(ULONG)3u);
    }

    PBYTE CalculateTarget(PBYTE pSource, LONG delta)
    {
        return (pSource + delta + c_PCAdjust);
    }

    LONG CalculateNewDelta(PBYTE pTarget, BYTE* pDest)
    {
        return (LONG)(pTarget - (pDest + c_PCAdjust));
    }

    BYTE    EmitAdd32(PUSHORT& pDstInst, BYTE op1Reg, BYTE op2Reg, BYTE dstReg, BYTE shiftAmount)
    {
        Shift& shift = (Shift&)(shiftAmount);
        const BYTE shiftType = 0x00; // LSL
        Add32 add = { op2Reg, shiftType, shift.Imm2, dstReg, shift.Imm3,
                      0x0, op1Reg, 0x0, 0x758 };
        return EmitLongInstruction(pDstInst, (ULONG&)add);
    }

    BYTE    EmitLogicalShiftLeft32(PUSHORT& pDstInst, BYTE srcReg, BYTE dstReg, BYTE shiftAmount)
    {
        Shift& shift = (Shift&)(shiftAmount);
        LogicalShiftLeft32 shiftLeft = { srcReg, 0x00, shift.Imm2, dstReg, shift.Imm3, 0x1E,
                                         0x00, 0x752 };
        return EmitLongInstruction(pDstInst, (ULONG&)shiftLeft);
    }

    BYTE    EmitStoreImmediate12(PUSHORT& pDstInst, BYTE srcReg, BYTE baseReg, USHORT offset)
    {
        StoreImmediate12 store = { offset, srcReg, baseReg, 0xF8C };
        return EmitLongInstruction(pDstInst, (ULONG&)store);
    }

  protected:
    PBYTE   m_pbTarget;
    PBYTE   m_pbPool;
    LONG    m_lExtra;

    BYTE    m_rbScratchDst[64]; // matches or exceeds rbCode

    static const COPYENTRY s_rceCopyTable[33];
};

LONG CDetourDis::DecodeBranch5(ULONG opcode)
{
    Branch5& branch = (Branch5&)(opcode);

    Branch5Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm5 = branch.Imm5;
    target.I = branch.I;

    // Return zero-extended value
    return (LONG&)target;
}

USHORT CDetourDis::EncodeBranch5(ULONG originalOpCode, LONG delta)
{
    // Too large for a 5 bit branch (5 bit branches can be up to 7 bits due to I and the trailing 0)
    if (delta < 0 || delta > 0x7F) {
        return 0;
    }

    Branch5& branch = (Branch5&)(originalOpCode);
    Branch5Target& target = (Branch5Target&)(delta);

    branch.Imm5 = target.Imm5;
    branch.I = target.I;

    return (USHORT&)branch;
}

LONG CDetourDis::DecodeBranch8(ULONG opcode)
{
    Branch8& branch = (Branch8&)(opcode);

    Branch8Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm8 = branch.Imm8;

    // Return sign extended value
    return (((LONG&)target) << 23) >> 23;
}

USHORT CDetourDis::EncodeBranch8(ULONG originalOpCode, LONG delta)
{
    // Too large for 8 bit branch (8 bit branches can be up to 9 bits due to the trailing 0)
    if (delta < (-(int)0x100) || delta > 0xFF) {
        return 0;
    }

    Branch8& branch = (Branch8&)(originalOpCode);
    Branch8Target& target = (Branch8Target&)(delta);

    branch.Imm8 = target.Imm8;

    return (USHORT&)branch;
}

LONG CDetourDis::DecodeBranch11(ULONG opcode)
{
    Branch11& branch = (Branch11&)(opcode);

    Branch11Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm11 = branch.Imm11;

    // Return sign extended value
    return (((LONG&)target) << 20) >> 20;
}

USHORT CDetourDis::EncodeBranch11(ULONG originalOpCode, LONG delta)
{
    // Too large for an 11 bit branch (11 bit branches can be up to 12 bits due to the trailing 0)
    if (delta < (-(int)0x800) || delta > 0x7FF) {
        return 0;
    }

    Branch11& branch = (Branch11&)(originalOpCode);
    Branch11Target& target = (Branch11Target&)(delta);

    branch.Imm11 = target.Imm11;

    return (USHORT&)branch;
}

BYTE CDetourDis::EmitBranch11(PUSHORT& pDest, LONG relativeAddress)
{
    Branch11Target& target = (Branch11Target&)(relativeAddress);
    Branch11 branch11 = { target.Imm11, 0x1C };

    *pDest++ = (USHORT&)branch11;
    return sizeof(USHORT);
}

LONG CDetourDis::DecodeBranch20(ULONG opcode)
{
    Branch20& branch = (Branch20&)(opcode);

    Branch20Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm11 = branch.Imm11;
    target.Imm6 = branch.Imm6;
    target.Sign = branch.Sign;
    target.J1 = branch.J1;
    target.J2 = branch.J2;

    // Sign extend
    if (target.Sign) {
        target.Padding2 = -1;
    }

    return (LONG&)target;
}

ULONG CDetourDis::EncodeBranch20(ULONG originalOpCode, LONG delta)
{
    // Too large for 20 bit branch (20 bit branches can be up to 21 bits due to the trailing 0)
    if (delta < (-(int)0x100000) || delta > 0xFFFFF) {
        return 0;
    }

    Branch20& branch = (Branch20&)(originalOpCode);
    Branch20Target& target = (Branch20Target&)(delta);

    branch.Imm11 = target.Imm11;
    branch.Imm6 = target.Imm6;
    branch.Sign = target.Sign;
    branch.J1 = target.J1;
    branch.J2 = target.J2;

    return (ULONG&)branch;
}

LONG CDetourDis::DecodeBranch24(ULONG opcode, BOOL& fLink)
{
    Branch24& branch = (Branch24&)(opcode);

    Branch24Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm11 = branch.Imm11;
    target.Imm10 = branch.Imm10;
    target.Sign = branch.Sign;
    target.I1 = ~(branch.J1 ^ target.Sign);
    target.I2 = ~(branch.J2 ^ target.Sign);
    fLink = branch.Link;

    // Sign extend
    if (target.Sign) {
        target.Padding2 = -1;
    }

    return (LONG&)target;
}

ULONG CDetourDis::EncodeBranch24(ULONG originalOpCode, LONG delta, BOOL fLink)
{
    // Too large for 24 bit branch (24 bit branches can be up to 25 bits due to the trailing 0)
    if (delta < static_cast<int>(0xFF000000) || delta > static_cast<int>(0xFFFFFF)) {
        return 0;
    }

    Branch24& branch = (Branch24&)(originalOpCode);
    Branch24Target& target = (Branch24Target&)(delta);

    branch.Imm11 = target.Imm11;
    branch.Imm10 = target.Imm10;
    branch.Link = fLink;
    branch.Sign = target.Sign;
    branch.J1 = ~(target.I1 ^ branch.Sign);
    branch.J2 = ~(target.I2 ^ branch.Sign);

    return (ULONG&)branch;
}

LONG CDetourDis::DecodeLiteralLoad8(ULONG instruction)
{
    LiteralLoad8& load = (LiteralLoad8&)(instruction);

    LiteralLoad8Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm8 = load.Imm8;

    return (LONG&)target;
}

BYTE CDetourDis::EmitLiteralLoad8(PUSHORT& pDest, BYTE targetRegister, PBYTE pLiteral)
{
    // Note: We add 2 (which gets rounded down) because literals must be 32-bit
    //       aligned, but the ldr can be 16-bit aligned.
    LONG newDelta = CalculateNewDelta((PBYTE)pLiteral + 2, (PBYTE)pDest);
    LONG relative = ((newDelta > 0 ? newDelta : -newDelta) & 0x3FF);

    LiteralLoad8Target& target = (LiteralLoad8Target&)(relative);
    LiteralLoad8 load = { target.Imm8, targetRegister, 0x9 };

    return EmitShortInstruction(pDest, (USHORT&)load);
}

LONG CDetourDis::DecodeLiteralLoad12(ULONG instruction)
{
    LiteralLoad12& load = (LiteralLoad12&)(instruction);

    LiteralLoad12Target target;
    ZeroMemory(&target, sizeof(target));
    target.Imm12 = load.Imm12;

    return (LONG&)target;
}

BYTE CDetourDis::EmitLiteralLoad12(PUSHORT& pDest, BYTE targetRegister, PBYTE pLiteral)
{
    // Note: We add 2 (which gets rounded down) because literals must be 32-bit
    //       aligned, but the ldr can be 16-bit aligned.
    LONG newDelta = CalculateNewDelta((PBYTE)pLiteral + 2, (PBYTE)pDest);
    LONG relative = ((newDelta > 0 ? newDelta : -newDelta) & 0xFFF);

    LiteralLoad12Target& target = (LiteralLoad12Target&)(relative);
    target.Imm12 -= target.Imm12 & 3;
    LiteralLoad12 load = { target.Imm12, targetRegister, 0x5F, (DWORD)(newDelta > 0),  0xF8 };

    return EmitLongInstruction(pDest, (ULONG&)load);
}

BYTE CDetourDis::EmitImmediateRegisterLoad32(PUSHORT& pDest, BYTE reg)
{
    ImmediateRegisterLoad32 load = { 0, reg, reg, 0xF8D };
    return EmitLongInstruction(pDest, (ULONG&)load);
}

BYTE CDetourDis::EmitImmediateRegisterLoad16(PUSHORT& pDest, BYTE reg)
{
    ImmediateRegisterLoad16 load = { reg, reg, 0x680 >> 2 };
    return EmitShortInstruction(pDest, (USHORT&)load);
}

BYTE CDetourDis::EmitLongLiteralLoad(PUSHORT& pDest, BYTE targetRegister, PVOID pTarget)
{
    *--((PULONG&)m_pbPool) = (ULONG)(size_t)pTarget;

    // ldr rn, target.
    BYTE size = EmitLiteralLoad12(pDest, targetRegister, m_pbPool);

    // This only makes sense if targetRegister != PC;
    // otherwise, we would have branched with the previous instruction anyway
    if (targetRegister != c_PC) {
        // ldr rn, [rn]
        if (targetRegister <= 7) {
            size = (BYTE)(size + EmitImmediateRegisterLoad16(pDest, targetRegister));
        }
        else {
            size = (BYTE)(size + EmitImmediateRegisterLoad32(pDest, targetRegister));
        }
    }

    return size;
}

BYTE CDetourDis::EmitLongBranch(PUSHORT& pDest, PVOID pTarget)
{
    // Emit a long literal load into PC
    BYTE size = EmitLongLiteralLoad(pDest, c_PC, DETOURS_PBYTE_TO_PFUNC(pTarget));
    return size;
}

BYTE CDetourDis::PureCopy16(BYTE* pSource, BYTE* pDest)
{
    *(USHORT *)pDest = *(USHORT *)pSource;
    return sizeof(USHORT);
}

BYTE CDetourDis::PureCopy32(BYTE* pSource, BYTE* pDest)
{
    *(UNALIGNED ULONG *)pDest = *(UNALIGNED ULONG*)pSource;
    return sizeof(DWORD);
}

USHORT CDetourDis::CalculateExtra(BYTE sourceLength, BYTE* pDestStart, BYTE* pDestEnd)
{
    ULONG destinationLength = (ULONG)(pDestEnd - pDestStart);
    return static_cast<USHORT>((destinationLength > sourceLength) ? (destinationLength - sourceLength) : 0);
}

BYTE CDetourDis::CopyMiscellaneous16(BYTE* pSource, BYTE* pDest)
{
    USHORT instruction = *(PUSHORT)(pSource);

    // Compare and branch imm5 (CBZ, CBNZ)
    if ((instruction & 0x100) && !(instruction & 0x400)) { // (1011x0x1xxxxxxxx)
        LONG oldDelta = DecodeBranch5(instruction);
        PBYTE pTarget = CalculateTarget(pSource, oldDelta);
        m_pbTarget = pTarget;

        LONG newDelta = CalculateNewDelta(pTarget, pDest);
        instruction = EncodeBranch5(instruction, newDelta);

        if (instruction) {
            // Copy the 16 bit instruction over
            *(PUSHORT)(pDest) = instruction;
            return sizeof(USHORT); // The source instruction was 16 bits
        }

        // If that fails, re-encode with 'conditional branch' logic, without using the condition flags
        // For example, cbz r2,+0x56 (0x90432) becomes:
        //
        //  001df73a b92a     cbnz        r2,001df748
        //  001df73c e002     b           001df744
        //  001df73e bf00     nop
        //  001df740 0432     dc.h        0432
        //  001df742 0009     dc.h        0009
        //  001df744 f85ff008 ldr         pc,=0x90432
        //

        // Store where we will be writing our conditional branch, and move past it so we can emit a long branch
        PUSHORT pDstInst = (PUSHORT)(pDest);
        PUSHORT pConditionalBranchInstruction = pDstInst++;

        // Emit the long branch instruction
        BYTE longBranchSize = EmitLongBranch(pDstInst, pTarget);

        // Invert the CBZ/CBNZ instruction to move past our 'long branch' if the inverse comparison succeeds
        // Write the CBZ/CBNZ instruction *before* the long branch we emitted above
        // This had to be done out of order, since the size of a long branch can vary due to alignment restrictions
        instruction = EncodeBranch5(*(PUSHORT)(pSource), longBranchSize - c_PCAdjust + sizeof(USHORT));
        Branch5& branch = (Branch5&)(instruction);
        branch.OpCode = (branch.OpCode & 0x02) ? 0x2C : 0x2E; // Invert the CBZ/CBNZ comparison
        *pConditionalBranchInstruction = instruction;

        // Compute the extra space needed for the branch sequence
        m_lExtra = CalculateExtra(sizeof(USHORT), pDest, (BYTE*)(pDstInst));
        return sizeof(USHORT); // The source instruction was 16 bits
    }

    // If-Then Instruction (IT)
    if ((instruction >> 8 == 0xBF) && (instruction & 0xF)) { //(10111111xxxx(mask != 0b0000))
        // ToDo: Implement IT handler
        ASSERT(false);
        return sizeof(USHORT);
    }

    // ADD/SUB, SXTH, SXTB, UXTH, UXTB, CBZ, CBNZ, PUSH, POP, REV, REV15, REVSH, NOP, YIELD, WFE, WFI, SEV, etc.
    return PureCopy16(pSource, pDest);
}

BYTE CDetourDis::CopyConditionalBranchOrOther16(BYTE* pSource, BYTE* pDest)
{
    USHORT instruction = *(PUSHORT)(pSource);

    // Could be a conditional branch, an Undefined instruction or a Service System Call
    // Only the former needs special logic
    if ((instruction & 0xE00) != 0xE00) { // 1101(!=111x)xxxxxxxx
        LONG oldDelta = DecodeBranch8(instruction);
        PBYTE pTarget = CalculateTarget(pSource, oldDelta);
        m_pbTarget = pTarget;

        LONG newDelta = CalculateNewDelta(pTarget, pDest);
        instruction = EncodeBranch8(instruction, newDelta);
        if (instruction) {
            // Copy the 16 bit instruction over
            *(PUSHORT)(pDest) = instruction;
            return sizeof(USHORT); // The source instruction was 16 bits
        }

        // If that fails, re-encode as a sequence of branches
        // For example, bne +0x6E (0x90452) becomes:
        //
        // 001df758 d100     bne         001df75c
        // 001df75a e005     b           001df768
        // 001df75c e002     b           001df764
        // 001df75e bf00     nop
        // 001df760 0452     dc.h        0452
        // 001df762 0009     dc.h        0009
        // 001df764 f85ff008 ldr         pc,=0x90452
        //

        // First, reuse the existing conditional branch to, if successful, branch down to a 'long branch' that we will emit below
        USHORT newInstruction = EncodeBranch8(*(PUSHORT)(pSource), 0); // Due to the size of c_PCAdjust a zero-length branch moves 4 bytes forward, past the following unconditional branch
        ASSERT(newInstruction);
        PUSHORT pDstInst = (PUSHORT)(pDest);
        *pDstInst++ = newInstruction;

        // Next, prepare to insert an unconditional branch that will be hit if the condition above is not met.  This branch will branch over the following 'long branch'
        // We can't actually encode this branch yet though, because 'long branches' can vary in size
        PUSHORT pUnconditionalBranchInstruction = pDstInst++;

        // Then, emit a 'long branch' that will be hit if the original condition is met
        BYTE longBranchSize = EmitLongBranch(pDstInst, pTarget);

        // Finally, encode and emit the unconditional branch that will be used to branch past the 'long branch' if the initial condition was not met
        Branch11 branch11 = { 0x00, 0x1C };
        newInstruction = EncodeBranch11(*(DWORD*)(&branch11), longBranchSize - c_PCAdjust + sizeof(USHORT));
        ASSERT(newInstruction);
        *pUnconditionalBranchInstruction = newInstruction;

        // Compute the extra space needed for the branch sequence
        m_lExtra = CalculateExtra(sizeof(USHORT), pDest, (BYTE*)(pDstInst));
        return sizeof(USHORT); // The source instruction was 16 bits
    }

    return PureCopy16(pSource, pDest);
}

BYTE CDetourDis::CopyUnConditionalBranch16(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = *(PUSHORT)(pSource);

    LONG oldDelta = DecodeBranch11(instruction);
    PBYTE pTarget = CalculateTarget(pSource, oldDelta);
    m_pbTarget = pTarget;

    LONG newDelta = CalculateNewDelta(pTarget, pDest);
    instruction = EncodeBranch11(instruction, newDelta);
    if (instruction) {
        // Copy the 16 bit instruction over
        *(PUSHORT)(pDest) = (USHORT)instruction;
        return sizeof(USHORT); // The source instruction was 16 bits
    }

    // If that fails, re-encode as 32-bit
    PUSHORT pDstInst = (PUSHORT)(pDest);
    instruction = EncodeBranch24(0xf0009000, newDelta, FALSE);
    if (instruction) {
        // Copy both bytes of the instruction
        EmitLongInstruction(pDstInst, instruction);

        m_lExtra = sizeof(DWORD) - sizeof(USHORT); // The destination instruction was 32 bits
        return sizeof(USHORT); // The source instruction was 16 bits
    }

    // If that fails, emit as a 'long branch'
    if (!instruction) {
        // For example, b +0x7FE (00090be6) becomes:
        // 003f6d02 e001     b           003f6d08
        // 003f6d04 0be6     dc.h        0be6
        // 003f6d06 0009     dc.h        0009
        // 003f6d08 f85ff008 ldr         pc,=0x90BE6
        EmitLongBranch(pDstInst, pTarget);

        // Compute the extra space needed for the branch sequence
        m_lExtra = CalculateExtra(sizeof(USHORT), pDest, (BYTE*)(pDstInst));
        return sizeof(USHORT); // The source instruction was 16 bits
    }

    return sizeof(USHORT); // The source instruction was 16 bits
}

BYTE CDetourDis::CopyLiteralLoad16(BYTE* pSource, BYTE* pDest)
{
    PBYTE pStart = pDest;
    USHORT instruction = *(PUSHORT)(pSource);

    LONG oldDelta = DecodeLiteralLoad8(instruction);
    PBYTE pTarget = CalculateTarget(Align4(pSource), oldDelta);

    // Re-encode as a 'long literal load'
    // For example, ldr r0, [PC + 1E0] (0x905B4) becomes:
    //
    // 001df72c f85f0008 ldr         r0,=0x905B4
    // 001df730 f8d00000 ldr.w       r0,[r0]
    LiteralLoad8& load8 = (LiteralLoad8&)(instruction);
    EmitLongLiteralLoad((PUSHORT&)pDest, load8.Register, pTarget);

    m_lExtra = (LONG)(pDest - pStart - sizeof(USHORT));
    return sizeof(USHORT); // The source instruction was 16 bits
}

BYTE CDetourDis::CopyBranchExchangeOrDataProcessing16(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = *(PUSHORT)(pSource);

    // BX
    if ((instruction & 0xff80) == 0x4700) {
        // The target is stored in a register
        m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    }

    // AND, LSR, TST, ADD, CMP, MOV
    return PureCopy16(pSource, pDest);
}

const CDetourDis::COPYENTRY CDetourDis::s_rceCopyTable[33] =
{
    // Shift by immediate, move register
    // ToDo: Not handling moves from PC
    /* 0b00000 */ { 0x00, &CDetourDis::PureCopy16 },
    /* 0b00001 */ { 0x01, &CDetourDis::PureCopy16 },
    /* 0b00010 */ { 0x02, &CDetourDis::PureCopy16 },

    // Add/subtract register
    // Add/subtract immediate
    /* 0b00011 */ { 0x03, &CDetourDis::PureCopy16},

    // Add/subtract/compare/move immediate
    /* 0b00100 */ { 0x04, &CDetourDis::PureCopy16 },
    /* 0b00101 */ { 0x05, &CDetourDis::PureCopy16 },
    /* 0b00110 */ { 0x06, &CDetourDis::PureCopy16 },
    /* 0b00111 */ { 0x07, &CDetourDis::PureCopy16 },

    // Data-processing register
    // Special data processing
    // Branch/exchange instruction set
    /* 0b01000 */ { 0x08, &CDetourDis::CopyBranchExchangeOrDataProcessing16 },

    // Load from literal pool
    /* 0b01001 */ { 0x09, &CDetourDis::CopyLiteralLoad16 },

    // Load/store register offset
    /* 0b01010 */ { 0x0a, &CDetourDis::PureCopy16 },
    /* 0b01011 */ { 0x0b, &CDetourDis::PureCopy16 },

    //  Load/store word/byte immediate offset.
    /* 0b01100 */ { 0x0c, &CDetourDis::PureCopy16 },
    /* 0b01101 */ { 0x0d, &CDetourDis::PureCopy16 },
    /* 0b01110 */ { 0x0e, &CDetourDis::PureCopy16 },
    /* 0b01111 */ { 0x0f, &CDetourDis::PureCopy16 },

    //  Load/store halfword immediate offset.
    /* 0b10000 */ { 0x10, &CDetourDis::PureCopy16 },
    /* 0b10001 */ { 0x11, &CDetourDis::PureCopy16 },

    // Load from or store to stack
    /* 0b10010 */ { 0x12, &CDetourDis::PureCopy16 },
    /* 0b10011 */ { 0x13, &CDetourDis::PureCopy16 },

    // Add to SP or PC
    /* 0b10100 */ { 0x14, &CDetourDis::PureCopy16 },
    //   ToDo: Is ADR (T1) blitt-able?
    //     It adds a value to PC and stores the result in a register.
    //     Does this count as a 'target' for detours?
    /* 0b10101 */ { 0x15, &CDetourDis::PureCopy16 },

    // Miscellaneous
    /* 0b10110 */ { 0x16, &CDetourDis::CopyMiscellaneous16 },
    /* 0b10111 */ { 0x17, &CDetourDis::CopyMiscellaneous16 },

    // Load/store multiple
    /* 0b11000 */ { 0x18, &CDetourDis::PureCopy16 },
    /* 0b11001 */ { 0x19, &CDetourDis::PureCopy16 },
    //   ToDo: Are we sure these are all safe?
    //     LDMIA, for example, can include an 'embedded' branch.
    //     Does this count as a 'target' for detours?

    // Conditional branch
    /* 0b11010 */ { 0x1a, &CDetourDis::CopyConditionalBranchOrOther16 },

    // Conditional branch
    // Undefined instruction
    // Service (system) call
    /* 0b11011 */ { 0x1b, &CDetourDis::CopyConditionalBranchOrOther16 },

    // Unconditional branch
    /* 0b11100 */ { 0x1c, &CDetourDis::CopyUnConditionalBranch16 },

    // 32-bit instruction
    /* 0b11101 */ { 0x1d, &CDetourDis::BeginCopy32 },
    /* 0b11110 */ { 0x1e, &CDetourDis::BeginCopy32 },
    /* 0b11111 */ { 0x1f, &CDetourDis::BeginCopy32 },
    { 0, NULL }
};

BYTE CDetourDis::CopyBranch24(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = GetLongInstruction(pSource);
    BOOL fLink;
    LONG oldDelta = DecodeBranch24(instruction, fLink);
    PBYTE pTarget = CalculateTarget(pSource, oldDelta);
    m_pbTarget = pTarget;

    // Re-encode as 32-bit
    PUSHORT pDstInst = (PUSHORT)(pDest);
    LONG newDelta = CalculateNewDelta(pTarget, pDest);
    instruction = EncodeBranch24(instruction, newDelta, fLink);
    if (instruction) {
        // Copy both bytes of the instruction
        EmitLongInstruction(pDstInst, instruction);
        return sizeof(DWORD);
    }

    // If that fails, re-encode as a 'long branch'
    EmitLongBranch(pDstInst, pTarget);

    // Compute the extra space needed for the instruction
    m_lExtra = CalculateExtra(sizeof(DWORD), pDest, (BYTE*)(pDstInst));
    return sizeof(DWORD); // The source instruction was 32 bits
}

BYTE CDetourDis::CopyBranchOrMiscellaneous32(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = GetLongInstruction(pSource);
    if ((instruction & 0xf800d000) == 0xf0008000) { // B<c>.W <label>
        LONG oldDelta = DecodeBranch20(instruction);
        PBYTE pTarget = CalculateTarget(pSource, oldDelta);
        m_pbTarget = pTarget;

        // Re-encode as 32-bit
        PUSHORT pDstInst = (PUSHORT)(pDest);
        LONG newDelta = CalculateNewDelta(pTarget, pDest);
        instruction = EncodeBranch20(instruction, newDelta);
        if (instruction) {
            // Copy both bytes of the instruction
            EmitLongInstruction(pDstInst, instruction);
            return sizeof(DWORD);
        }

        // If that fails, re-encode as a sequence of branches
        // For example, bls.w +0x86 (00090480)| becomes:
        //
        // 001df788 f2408001 bls.w       001df78e
        // 001df78c e004     b           001df798
        // 001df78e e001     b           001df794
        // 001df790 0480     dc.h        0480
        // 001df792 0009     dc.h        0009
        // 001df794 f85ff008 ldr         pc,=0x90480
        //

        // First, reuse the existing conditional branch to, if successful,
        // branch down to a 'long branch' that we will emit below
        instruction = EncodeBranch20(GetLongInstruction(pSource), 2);
        // Due to the size of c_PCAdjust a two-length branch moves 6 bytes forward,
        // past the following unconditional branch
        ASSERT(instruction);
        EmitLongInstruction(pDstInst, instruction);

        // Next, prepare to insert an unconditional branch that will be hit
        // if the condition above is not met.  This branch will branch over
        // the following 'long branch'
        // We can't actually encode this branch yet though, because
        // 'long branches' can vary in size
        PUSHORT pUnconditionalBranchInstruction = pDstInst++;

        // Then, emit a 'long branch' that will be hit if the original condition is met
        BYTE longBranchSize = EmitLongBranch(pDstInst, pTarget);

        // Finally, encode and emit the unconditional branch that will be used
        // to branch past the 'long branch' if the initial condition was not met
        Branch11 branch11 = { 0x00, 0x1C };
        instruction = EncodeBranch11(*(DWORD*)(&branch11), longBranchSize - c_PCAdjust + sizeof(USHORT));
        ASSERT(instruction);
        *pUnconditionalBranchInstruction = static_cast<USHORT>(instruction);

        // Compute the extra space needed for the instruction
        m_lExtra = CalculateExtra(sizeof(DWORD), pDest, (BYTE*)(pDstInst));
        return sizeof(DWORD); // The source instruction was 32 bits
    }

    if ((instruction & 0xf800d000) == 0xf0009000) { // B.W <label>
        // B <label>  11110xxxxxxxxxxx10xxxxxxxxxxxxxx
        return CopyBranch24(pSource, pDest);
    }

    if ((instruction & 0xf800d000) == 0xf000d000) { // BL.W <label>
        // B <label>  11110xxxxxxxxxxx10xxxxxxxxxxxxxx

        PUSHORT pDstInst = (PUSHORT)(pDest);
        BOOL fLink;
        LONG oldDelta = DecodeBranch24(instruction, fLink);
        PBYTE pTarget = CalculateTarget(pSource, oldDelta);
        m_pbTarget = pTarget;

        *--((PULONG&)m_pbPool) = (ULONG)(size_t)DETOURS_PBYTE_TO_PFUNC(pTarget);

        // ldr lr, target.
        EmitLiteralLoad12(pDstInst, c_LR, m_pbPool);
        // blx lr
        EmitShortInstruction(pDstInst, 0x47f0);

        // Compute the extra space needed for the instruction
        m_lExtra = CalculateExtra(sizeof(DWORD), pDest, (BYTE*)(pDstInst));
        return sizeof(DWORD); // The source instruction was 32 bits
    }

    if ((instruction & 0xFFF0FFFF) == 0xF3C08F00) {
        // BXJ 111100111100xxxx1000111100000000
        // BXJ switches to Jazelle mode, which is not supported
        ASSERT(false);
    }

    if ((instruction & 0xFFFFFF00) == 0xF3DE8F00) {
        // SUBS PC, LR 111100111101111010001111xxxxxxxx
        m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    }

    // Everything else should be blitt-able
    return PureCopy32(pSource, pDest);
}

BYTE CDetourDis::CopyLiteralLoad32(BYTE* pSource, BYTE* pDest)
{
    BYTE* pStart = pDest;
    ULONG instruction = GetLongInstruction(pSource);

    LONG oldDelta = DecodeLiteralLoad12(instruction);
    PBYTE pTarget = CalculateTarget(Align4(pSource), oldDelta);

    LiteralLoad12& load = (LiteralLoad12&)(instruction);

    EmitLongLiteralLoad((PUSHORT&)pDest, load.Register, pTarget);

    m_lExtra = (LONG)(pDest - pStart - sizeof(DWORD));

    return sizeof(DWORD); // The source instruction was 32 bits
}

BYTE CDetourDis::CopyLoadAndStoreSingle(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = GetLongInstruction(pSource);

    // Note: The following masks only look at the interesting bits
    // (not the opCode prefix, since that check was performed in
    // order to get to this function)
    if (!(instruction & 0x100000)) {
        // 1111 100x xxx0 xxxxxxxxxxxxxxxxxxxx : STR, STRB, STRH, etc.
        return PureCopy32(pSource, pDest);
    }

    if ((instruction & 0xF81F0000) == 0xF81F0000) {
        // 1111100xxxx11111xxxxxxxxxxxxxxxx : PC +/- Imm12
        return CopyLiteralLoad32(pSource, pDest);
    }

    if ((instruction & 0xFE70F000) == 0xF81FF000) {
        // 1111100xx001xxxx1111xxxxxxxxxxxx : PLD, PLI
        // Convert PC-Relative PLD/PLI instructions to noops (1111100Xx00111111111xxxxxxxxxxxx)
        if ((instruction & 0xFE7FF000) == 0xF81FF000) {
            PUSHORT pDstInst = (PUSHORT)(pDest);
            *pDstInst++ = c_NOP;
            *pDstInst++ = c_NOP;
            return sizeof(DWORD);  // The source instruction was 32 bits
        }

        // All other PLD/PLI instructions are blitt-able
        return PureCopy32(pSource, pDest);
    }

    // If the load is writing to PC
    if ((instruction & 0xF950F000) == 0xF850F000) {
        m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    }

    // All other loads LDR (immediate), etc.
    return PureCopy32(pSource, pDest);
}

BYTE CDetourDis::CopyLoadAndStoreMultipleAndSRS(BYTE* pSource, BYTE* pDest)
{
    // Probably all blitt-able, although not positive since some of these can result in a branch (LDMIA, POP, etc.)
    return PureCopy32(pSource, pDest);
}

BYTE CDetourDis::CopyTableBranch(BYTE* pSource, BYTE* pDest)
{
    m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
    ULONG instruction = GetLongInstruction(pSource);
    TableBranch& tableBranch = (TableBranch&)(instruction);

    // If the base register is anything other than PC, we can simply copy the instruction
    if (tableBranch.BaseRegister != c_PC) {
        return PureCopy32(pSource, pDest);
    }

    __debugbreak();

    // If the base register is PC, we need to manually perform the table lookup
    // For example, this:
    //
    //        7ef40000 e8dff002 tbb         [pc,r2]
    //
    // becomes this:
    //
    //        7ef40404 b401     push        {r0}            ; pushed as a placeholder for the target address
    //        7ef40406 e92d0005 push.w      {r0,r2}         ; scratch register and another register are pushed; there's a minimum of two registers in the list for push.w
    //        7ef40410 4820     ldr         r0,=0x7EF40004  ; load the table address from the literal pool
    //        7ef40414 eb000042 add         r0,r0,r2,lsl #1 ; add the index value to the address of the table to get the table entry; lsl only used if it's a TBH instruction
    //        7ef40418 f8d00000 ldr.w       r0,[r0]         ; dereference the table entry to get the value of the target
    //        7ef4041c ea4f0040 lsl         r0,r0,#1        ; multiply the offset by 2 (per the spec)
    //        7ef40420 eb00000f add.w       r0,r0,pc        ; Add the offset to pc to get the target address
    //        7ef40424 f8cd000c str.w       r0,[sp,#0xC]    ; store the target address on the stack (into the first push)
    //        7ef40428 e8bd0005 pop.w       {r0,r2}         ; scratch register and another register are popped; there's a minimum of two registers in the list for pop.w
    //        7ef4042c bd00     pop         {pc}            ; pop the address into pc
    //

    // Push r0 to make room for our jump address on the stack
    PUSHORT pDstInst = (PUSHORT)(pDest);
    *pDstInst++ = 0xb401;

    // Locate a scratch register
    BYTE scrReg = 0;
    while (scrReg == tableBranch.IndexRegister) {
        ++scrReg;
    }

    // Push scrReg and tableBranch.IndexRegister (push.w doesn't support pushing just 1 register)
    DWORD pushInstruction = 0xe92d0000;
    pushInstruction |= 1 << scrReg;
    pushInstruction |= 1 << tableBranch.IndexRegister;
    EmitLongInstruction(pDstInst, pushInstruction);

    // Write the target address out to the 'literal pool';
    // when the base register of a TBB/TBH is PC,
    // the branch table immediately follows the instruction
    BYTE* pTarget = CalculateTarget(pSource, 0);
    *--((PUSHORT&)m_pbPool) = (USHORT)((size_t)pTarget & 0xffff);
    *--((PUSHORT&)m_pbPool) = (USHORT)((size_t)pTarget >> 16);

    // Load the literal pool value into our scratch register (this contains the address of the branch table)
    // ldr rn, target
    EmitLiteralLoad8(pDstInst, scrReg, m_pbPool);

    // Add the index offset to the address of the branch table; the result will be the value within the table that contains the branch offset
    // We need to multiply the index by two if we are using halfword indexing
    // Will shift tableBranch.IndexRegister by 1 (multiply by 2) if using a TBH
    EmitAdd32(pDstInst, scrReg, tableBranch.IndexRegister, scrReg, tableBranch.HalfWord);

    // Dereference rn into rn, to load the value within the table
    // ldr rn, [rn]
    if (scrReg < 0x7) {
        EmitImmediateRegisterLoad16(pDstInst, scrReg);
    }
    else {
        EmitImmediateRegisterLoad32(pDstInst, scrReg);
    }

    // Multiply the offset by two to get the true offset value (as per the spec)
    EmitLogicalShiftLeft32(pDstInst, scrReg, scrReg, 1);

    // Add the offset to PC to get the target
    EmitAdd32(pDstInst, scrReg, c_PC, scrReg, 0);

    // Now write the contents of scrReg to the stack, so we can pop it into PC
    // Write the address of the branch table entry to the stack, so we can pop it into PC
    EmitStoreImmediate12(pDstInst, scrReg, c_SP, sizeof(DWORD) * 3);

    // Pop scrReg and tableBranch.IndexRegister (pop.w doesn't support popping just 1 register)
    DWORD popInstruction = 0xe8bd0000;
    popInstruction |= 1 << scrReg;
    popInstruction |= 1 << tableBranch.IndexRegister;
    EmitLongInstruction(pDstInst, popInstruction);

    // Pop PC
    *pDstInst++ = 0xbd00;

    // Compute the extra space needed for the branch sequence
    m_lExtra = CalculateExtra(sizeof(USHORT), pDest, (BYTE*)(pDstInst));
    return sizeof(DWORD);
}

BYTE CDetourDis::BeginCopy32(BYTE* pSource, BYTE* pDest)
{
    ULONG instruction = GetLongInstruction(pSource);

    // Immediate data processing instructions; ADD, SUB, MOV, MOVN, ADR, MOVT, BFC, SSAT16, etc.
    if ((instruction & 0xF8008000) == 0xF0000000) { // 11110xxxxxxxxxxx0xxxxxxxxxxxxxxx
        // Should all be blitt-able
        // ToDo: What about ADR?  Is it safe to do a straight-copy?
        // ToDo: Not handling moves to or from PC
        return PureCopy32(pSource, pDest);
    }

    // Non-Immediate data processing instructions; ADD, EOR, TST, etc.
    if ((instruction & 0xEE000000) == 0xEA000000) { // 111x101xxxxxxxxxxxxxxxxxxxxxxx
        // Should all be blitt-able
        return PureCopy32(pSource, pDest);
    }

    // Load and store single data item, memory hints
    if ((instruction & 0xFE000000) == 0xF8000000) { // 1111100xxxxxxxxxxxxxxxxxxxxxxxxx
        return CopyLoadAndStoreSingle(pSource, pDest);
    }

    // Load and store, double and exclusive, and table branch
    if ((instruction & 0xFE400000) == 0xE8400000) { // 1110100xx1xxxxxxxxxxxxxxxxxxxxxx
        // Load and store double
        if (instruction & 0x1200000) {
            // LDRD, STRD (immediate) : xxxxxxxPxxWxxxxxxxxxxxxxxxxxxxxx where PW != 0b00
            // The source register is PC
            if ((instruction & 0xF0000) == 0xF0000) {
                // ToDo: If the source register is PC, what should we do?
                ASSERT(false);
            }

            // If either target registers are PC
            if (((instruction & 0xF000) == 0xF000) ||
                ((instruction & 0xF00) == 0xF00)) {
                m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
            }

            return PureCopy32(pSource, pDest);
        }

        // Load and store exclusive
        if (!(instruction & 0x800000)) { // LDREX, STREX : xxxxxxxx0xxxxxxxxxxxxxxxxxxxxxxx
            if ((instruction & 0xF000) == 0xF000) { // xxxxxxxxxxxx1111xxxxxxxxxxxx
                m_pbTarget = (PBYTE)DETOUR_INSTRUCTION_TARGET_DYNAMIC;
            }
            return PureCopy32(pSource, pDest);
        }

        // Table branch
        if ((instruction & 0x1000F0) == 0x100000 ||  // TBB : xxxxxxxxxxx1xxxxxxxxxxxx0000xxxx
            (instruction & 0x1000F0) == 0x100010) { // TBH : xxxxxxxxxxx1xxxxxxxxxxxx0001xxxx
            return CopyTableBranch(pSource, pDest);
        }

        // Load and store exclusive byte, halfword, doubleword (LDREXB, LDREXH, LDREXD, STREXB, STREXH, STREXD, etc.)
        return PureCopy32(pSource, pDest);
    }

    // Load and store multiple, RFE and SRS
    if ((instruction & 0xFE400000) == 0xE8000000) { // 1110100xx0xxxxxxxxxxxxxxxxxxxxxx
        // Return from exception (RFE)
        if ((instruction & 0xE9900000) == 0xE9900000 || // 1110100110x1xxxxxxxxxxxxxxxxxxxx
            (instruction & 0xE8100000) == 0xE8100000) { // 1110100000x1xxxxxxxxxxxxxxxxxxxx
            return PureCopy32(pSource, pDest);
        }

        return CopyLoadAndStoreMultipleAndSRS(pSource, pDest);
    }

    // Branches, miscellaneous control
    if ((instruction & 0xF8008000) == 0xF0008000) { // 11110xxxxxxxxxxx0xxxxxxxxxxxxxxx
        // Branches, miscellaneous control
        return CopyBranchOrMiscellaneous32(pSource, pDest);
    }

    // Coprocessor instructions
    if ((instruction & 0xEC000000) == 0xEC000000) { // 111x11xxxxxxxxxxxxxxxxxxxxxxxxxx
        return PureCopy32(pSource, pDest);
    }

    // Unhandled instruction; should never make it this far
    ASSERT(false);
    return PureCopy32(pSource, pDest);
}

/////////////////////////////////////////////////////////// Disassembler Code.
//
CDetourDis::CDetourDis() :
    m_pbTarget((PBYTE)DETOUR_INSTRUCTION_TARGET_NONE),
    m_pbPool(NULL),
    m_lExtra(0)
{
}

PBYTE CDetourDis::CopyInstruction(PBYTE pDst,
                                  PBYTE *ppDstPool,
                                  PBYTE pSrc,
                                  PBYTE *ppTarget,
                                  LONG *plExtra)
{
    if (pDst && ppDstPool && ppDstPool != NULL) {
        m_pbPool = (PBYTE)*ppDstPool;
    }
    else {
        pDst = m_rbScratchDst;
        m_pbPool = m_rbScratchDst + sizeof(m_rbScratchDst);
    }
    // Make sure the constant pool is 32-bit aligned.
    m_pbPool -= ((ULONG_PTR)m_pbPool) & 3;

    REFCOPYENTRY pEntry = &s_rceCopyTable[pSrc[1] >> 3];
    ULONG size = (this->*pEntry->pfCopy)(pSrc, pDst);

    pSrc += size;

    // If the target is needed, store our target
    if (ppTarget) {
        *ppTarget = m_pbTarget;
    }
    if (plExtra) {
        *plExtra = m_lExtra;
    }
    if (ppDstPool) {
        *ppDstPool = m_pbPool;
    }

    return pSrc;
}


PVOID WINAPI DetourCopyInstruction(_In_opt_ PVOID pDst,
                                   _Inout_opt_ PVOID *ppDstPool,
                                   _In_ PVOID pSrc,
                                   _Out_opt_ PVOID *ppTarget,
                                   _Out_opt_ LONG *plExtra)
{
    CDetourDis state;
    return (PVOID)state.CopyInstruction((PBYTE)pDst,
                                        (PBYTE*)ppDstPool,
                                        (PBYTE)pSrc,
                                        (PBYTE*)ppTarget,
                                        plExtra);
}

#endif // DETOURS_ARM

#ifdef DETOURS_ARM64

#define c_LR        30          // The register number for the Link Register
#define c_SP        31          // The register number for the Stack Pointer
#define c_NOP       0xd503201f  // A nop instruction
#define c_BREAK     (0xd4200000 | (0xf000 << 5)) // A break instruction

//
// Problematic instructions:
//
// ADR     0ll10000 hhhhhhhh hhhhhhhh hhhddddd  & 0x9f000000 == 0x10000000  (l = low, h = high, d = Rd)
// ADRP    1ll10000 hhhhhhhh hhhhhhhh hhhddddd  & 0x9f000000 == 0x90000000  (l = low, h = high, d = Rd)
//
// B.cond  01010100 iiiiiiii iiiiiiii iii0cccc  & 0xff000010 == 0x54000000  (i = delta = SignExtend(imm19:00, 64), c = cond)
//
// B       000101ii iiiiiiii iiiiiiii iiiiiiii  & 0xfc000000 == 0x14000000  (i = delta = SignExtend(imm26:00, 64))
// BL      100101ii iiiiiiii iiiiiiii iiiiiiii  & 0xfc000000 == 0x94000000  (i = delta = SignExtend(imm26:00, 64))
//
// CBNZ    z0110101 iiiiiiii iiiiiiii iiittttt  & 0x7f000000 == 0x35000000  (z = size, i = delta = SignExtend(imm19:00, 64), t = Rt)
// CBZ     z0110100 iiiiiiii iiiiiiii iiittttt  & 0x7f000000 == 0x34000000  (z = size, i = delta = SignExtend(imm19:00, 64), t = Rt)
//
// LDR Wt  00011000 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x18000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDR Xt  01011000 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x58000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDRSW   10011000 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x98000000  (i = SignExtend(imm19:00, 64), t = Rt)
// PRFM    11011000 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0xd8000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDR St  00011100 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x1c000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDR Dt  01011100 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x5c000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDR Qt  10011100 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0x9c000000  (i = SignExtend(imm19:00, 64), t = Rt)
// LDR inv 11011100 iiiiiiii iiiiiiii iiittttt  & 0xff000000 == 0xdc000000  (i = SignExtend(imm19:00, 64), t = Rt)
//
// TBNZ    z0110111 bbbbbiii iiiiiiii iiittttt  & 0x7f000000 == 0x37000000  (z = size, b = bitnum, i = SignExtend(imm14:00, 64), t = Rt)
// TBZ     z0110110 bbbbbiii iiiiiiii iiittttt  & 0x7f000000 == 0x36000000  (z = size, b = bitnum, i = SignExtend(imm14:00, 64), t = Rt)
//

class CDetourDis
{
  public:
    CDetourDis();

    PBYTE   CopyInstruction(PBYTE pDst,
                            PBYTE pSrc,
                            PBYTE *ppTarget,
                            LONG *plExtra);

  public:
    typedef BYTE (CDetourDis::* COPYFUNC)(PBYTE pbDst, PBYTE pbSrc);

    union AddImm12
    {
        DWORD Assembled;
        struct
        {
            DWORD Rd : 5;           // Destination register
            DWORD Rn : 5;           // Source register
            DWORD Imm12 : 12;       // 12-bit immediate
            DWORD Shift : 2;        // shift (must be 0 or 1)
            DWORD Opcode1 : 7;      // Must be 0010001 == 0x11
            DWORD Size : 1;         // 0 = 32-bit, 1 = 64-bit
        } s;
        static DWORD Assemble(DWORD size, DWORD rd, DWORD rn, ULONG imm, DWORD shift)
        {
            AddImm12 temp;
            temp.s.Rd = rd;
            temp.s.Rn = rn;
            temp.s.Imm12 = imm & 0xfff;
            temp.s.Shift = shift;
            temp.s.Opcode1 = 0x11;
            temp.s.Size = size;
            return temp.Assembled;
        }
        static DWORD AssembleAdd32(DWORD rd, DWORD rn, ULONG imm, DWORD shift) { return Assemble(0, rd, rn, imm, shift); }
        static DWORD AssembleAdd64(DWORD rd, DWORD rn, ULONG imm, DWORD shift) { return Assemble(1, rd, rn, imm, shift); }
    };

    union Adr19
    {
        DWORD Assembled;
        struct
        {
            DWORD Rd : 5;           // Destination register
            DWORD Imm19 : 19;       // 19-bit upper immediate
            DWORD Opcode1 : 5;      // Must be 10000 == 0x10
            DWORD Imm2 : 2;         // 2-bit lower immediate
            DWORD Type : 1;         // 0 = ADR, 1 = ADRP
        } s;
        inline LONG Imm() const { DWORD Imm = (s.Imm19 << 2) | s.Imm2; return (LONG)(Imm << 11) >> 11; }
        static DWORD Assemble(DWORD type, DWORD rd, LONG delta)
        {
            Adr19 temp;
            temp.s.Rd = rd;
            temp.s.Imm19 = (delta >> 2) & 0x7ffff;
            temp.s.Opcode1 = 0x10;
            temp.s.Imm2 = delta & 3;
            temp.s.Type = type;
            return temp.Assembled;
        }
        static DWORD AssembleAdr(DWORD rd, LONG delta) { return Assemble(0, rd, delta); }
        static DWORD AssembleAdrp(DWORD rd, LONG delta) { return Assemble(1, rd, delta); }
    };

    union Bcc19
    {
        DWORD Assembled;
        struct
        {
            DWORD Condition : 4;    // Condition
            DWORD Opcode1 : 1;      // Must be 0
            DWORD Imm19 : 19;       // 19-bit immediate
            DWORD Opcode2 : 8;      // Must be 01010100 == 0x54
        } s;
        inline LONG Imm() const { return (LONG)(s.Imm19 << 13) >> 11; }
        static DWORD AssembleBcc(DWORD condition, LONG delta)
        {
            Bcc19 temp;
            temp.s.Condition = condition;
            temp.s.Opcode1 = 0;
            temp.s.Imm19 = delta >> 2;
            temp.s.Opcode2 = 0x54;
            return temp.Assembled;
        }
    };

    union Branch26
    {
        DWORD Assembled;
        struct
        {
            DWORD Imm26 : 26;       // 26-bit immediate
            DWORD Opcode1 : 5;      // Must be 00101 == 0x5
            DWORD Link : 1;         // 0 = B, 1 = BL
        } s;
        inline LONG Imm() const { return (LONG)(s.Imm26 << 6) >> 4; }
        static DWORD Assemble(DWORD link, LONG delta)
        {
            Branch26 temp;
            temp.s.Imm26 = delta >> 2;
            temp.s.Opcode1 = 0x5;
            temp.s.Link = link;
            return temp.Assembled;
        }
        static DWORD AssembleB(LONG delta) { return Assemble(0, delta); }
        static DWORD AssembleBl(LONG delta) { return Assemble(1, delta); }
    };

    union Br
    {
        DWORD Assembled;
        struct
        {
            DWORD Opcode1 : 5;      // Must be 00000 == 0
            DWORD Rn : 5;           // Register number
            DWORD Opcode2 : 22;     // Must be 1101011000011111000000 == 0x3587c0 for Br
                                    //                                   0x358fc0 for Brl
        } s;
        static DWORD Assemble(DWORD rn, bool link)
        {
            Br temp;
            temp.s.Opcode1 = 0;
            temp.s.Rn = rn;
            temp.s.Opcode2 = 0x3587c0;
            if (link)
                temp.Assembled |= 0x00200000;
            return temp.Assembled;
        }
        static DWORD AssembleBr(DWORD rn)
        {
            return Assemble(rn, false);
        }
        static DWORD AssembleBrl(DWORD rn)
        {
            return Assemble(rn, true);
        }
    };

    union Cbz19
    {
        DWORD Assembled;
        struct
        {
            DWORD Rt : 5;           // Register to test
            DWORD Imm19 : 19;       // 19-bit immediate
            DWORD Nz : 1;           // 0 = CBZ, 1 = CBNZ
            DWORD Opcode1 : 6;      // Must be 011010 == 0x1a
            DWORD Size : 1;         // 0 = 32-bit, 1 = 64-bit
        } s;
        inline LONG Imm() const { return (LONG)(s.Imm19 << 13) >> 11; }
        static DWORD Assemble(DWORD size, DWORD nz, DWORD rt, LONG delta)
        {
            Cbz19 temp;
            temp.s.Rt = rt;
            temp.s.Imm19 = delta >> 2;
            temp.s.Nz = nz;
            temp.s.Opcode1 = 0x1a;
            temp.s.Size = size;
            return temp.Assembled;
        }
    };

    union LdrLit19
    {
        DWORD Assembled;
        struct
        {
            DWORD Rt : 5;           // Destination register
            DWORD Imm19 : 19;       // 19-bit immediate
            DWORD Opcode1 : 2;      // Must be 0
            DWORD FpNeon : 1;       // 0 = LDR Wt/LDR Xt/LDRSW/PRFM, 1 = LDR St/LDR Dt/LDR Qt
            DWORD Opcode2 : 3;      // Must be 011 = 3
            DWORD Size : 2;         // 00 = LDR Wt/LDR St, 01 = LDR Xt/LDR Dt, 10 = LDRSW/LDR Qt, 11 = PRFM/invalid
        } s;
        inline LONG Imm() const { return (LONG)(s.Imm19 << 13) >> 11; }
        static DWORD Assemble(DWORD size, DWORD fpneon, DWORD rt, LONG delta)
        {
            LdrLit19 temp;
            temp.s.Rt = rt;
            temp.s.Imm19 = delta >> 2;
            temp.s.Opcode1 = 0;
            temp.s.FpNeon = fpneon;
            temp.s.Opcode2 = 3;
            temp.s.Size = size;
            return temp.Assembled;
        }
    };

    union LdrFpNeonImm9
    {
        DWORD Assembled;
        struct
        {
            DWORD Rt : 5;           // Destination register
            DWORD Rn : 5;           // Base register
            DWORD Imm12 : 12;       // 12-bit immediate
            DWORD Opcode1 : 1;      // Must be 1 == 1
            DWORD Opc : 1;          // Part of size
            DWORD Opcode2 : 6;      // Must be 111101 == 0x3d
            DWORD Size : 2;         // Size (0=8-bit, 1=16-bit, 2=32-bit, 3=64-bit, 4=128-bit)
        } s;
        static DWORD Assemble(DWORD size, DWORD rt, DWORD rn, ULONG imm)
        {
            LdrFpNeonImm9 temp;
            temp.s.Rt = rt;
            temp.s.Rn = rn;
            temp.s.Imm12 = imm;
            temp.s.Opcode1 = 1;
            temp.s.Opc = size >> 2;
            temp.s.Opcode2 = 0x3d;
            temp.s.Size = size & 3;
            return temp.Assembled;
        }
    };

    union Mov16
    {
        DWORD Assembled;
        struct
        {
            DWORD Rd : 5;           // Destination register
            DWORD Imm16 : 16;       // Immediate
            DWORD Shift : 2;        // Shift amount (0=0, 1=16, 2=32, 3=48)
            DWORD Opcode : 6;       // Must be 100101 == 0x25
            DWORD Type : 2;         // 0 = MOVN, 1 = reserved, 2 = MOVZ, 3 = MOVK
            DWORD Size : 1;         // 0 = 32-bit, 1 = 64-bit
        } s;
        static DWORD Assemble(DWORD size, DWORD type, DWORD rd, DWORD imm, DWORD shift)
        {
            Mov16 temp;
            temp.s.Rd = rd;
            temp.s.Imm16 = imm;
            temp.s.Shift = shift;
            temp.s.Opcode = 0x25;
            temp.s.Type = type;
            temp.s.Size = size;
            return temp.Assembled;
        }
        static DWORD AssembleMovn32(DWORD rd, DWORD imm, DWORD shift) { return Assemble(0, 0, rd, imm, shift); }
        static DWORD AssembleMovn64(DWORD rd, DWORD imm, DWORD shift) { return Assemble(1, 0, rd, imm, shift); }
        static DWORD AssembleMovz32(DWORD rd, DWORD imm, DWORD shift) { return Assemble(0, 2, rd, imm, shift); }
        static DWORD AssembleMovz64(DWORD rd, DWORD imm, DWORD shift) { return Assemble(1, 2, rd, imm, shift); }
        static DWORD AssembleMovk32(DWORD rd, DWORD imm, DWORD shift) { return Assemble(0, 3, rd, imm, shift); }
        static DWORD AssembleMovk64(DWORD rd, DWORD imm, DWORD shift) { return Assemble(1, 3, rd, imm, shift); }
    };

    union Tbz14
    {
        DWORD Assembled;
        struct
        {
            DWORD Rt : 5;           // Register to test
            DWORD Imm14 : 14;       // 14-bit immediate
            DWORD Bit : 5;          // 5-bit index
            DWORD Nz : 1;           // 0 = TBZ, 1 = TBNZ
            DWORD Opcode1 : 6;      // Must be 011011 == 0x1b
            DWORD Size : 1;         // 0 = 32-bit, 1 = 64-bit
        } s;
        inline LONG Imm() const { return (LONG)(s.Imm14 << 18) >> 16; }
        static DWORD Assemble(DWORD size, DWORD nz, DWORD rt, DWORD bit, LONG delta)
        {
            Tbz14 temp;
            temp.s.Rt = rt;
            temp.s.Imm14 = delta >> 2;
            temp.s.Bit = bit;
            temp.s.Nz = nz;
            temp.s.Opcode1 = 0x1b;
            temp.s.Size = size;
            return temp.Assembled;
        }
    };


  protected:
    BYTE    PureCopy32(BYTE* pSource, BYTE* pDest);
    BYTE    EmitMovImmediate(PULONG& pDstInst, BYTE rd, UINT64 immediate);
    BYTE    CopyAdr(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyBcc(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyB(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyBl(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyB_or_Bl(BYTE* pSource, BYTE* pDest, ULONG instruction, bool link);
    BYTE    CopyCbz(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyTbz(BYTE* pSource, BYTE* pDest, ULONG instruction);
    BYTE    CopyLdrLiteral(BYTE* pSource, BYTE* pDest, ULONG instruction);

  protected:
    ULONG GetInstruction(BYTE* pSource)
    {
        return ((PULONG)pSource)[0];
    }

    BYTE EmitInstruction(PULONG& pDstInst, ULONG instruction)
    {
        *pDstInst++ = instruction;
        return sizeof(ULONG);
    }

  protected:
    PBYTE   m_pbTarget;
    BYTE    m_rbScratchDst[128]; // matches or exceeds rbCode
};

BYTE CDetourDis::PureCopy32(BYTE* pSource, BYTE* pDest)
{
    *(ULONG *)pDest = *(ULONG*)pSource;
    return sizeof(DWORD);
}

/////////////////////////////////////////////////////////// Disassembler Code.
//
CDetourDis::CDetourDis() :
    m_pbTarget((PBYTE)DETOUR_INSTRUCTION_TARGET_NONE)
{
}

PBYTE CDetourDis::CopyInstruction(PBYTE pDst,
                                  PBYTE pSrc,
                                  PBYTE *ppTarget,
                                  LONG *plExtra)
{
    if (pDst == NULL) {
        pDst = m_rbScratchDst;
    }

    DWORD Instruction = GetInstruction(pSrc);

    ULONG CopiedSize;
    if ((Instruction & 0x1f000000) == 0x10000000) {
        CopiedSize = CopyAdr(pSrc, pDst, Instruction);
    } else if ((Instruction & 0xff000010) == 0x54000000) {
        CopiedSize = CopyBcc(pSrc, pDst, Instruction);
    } else if ((Instruction & 0x7c000000) == 0x14000000) {
        CopiedSize = CopyB_or_Bl(pSrc, pDst, Instruction, (Instruction & 0x80000000) != 0);
    } else if ((Instruction & 0x7e000000) == 0x34000000) {
        CopiedSize = CopyCbz(pSrc, pDst, Instruction);
    } else if ((Instruction & 0x7e000000) == 0x36000000) {
        CopiedSize = CopyTbz(pSrc, pDst, Instruction);
    } else if ((Instruction & 0x3b000000) == 0x18000000) {
        CopiedSize = CopyLdrLiteral(pSrc, pDst, Instruction);
    } else {
        CopiedSize = PureCopy32(pSrc, pDst);
    }

    // If the target is needed, store our target
    if (ppTarget) {
        *ppTarget = m_pbTarget;
    }
    if (plExtra) {
        *plExtra = CopiedSize - sizeof(DWORD);
    }

    return pSrc + 4;
}

BYTE CDetourDis::EmitMovImmediate(PULONG& pDstInst, BYTE rd, UINT64 immediate)
{
    DWORD piece[4];
    piece[3] = (DWORD)((immediate >> 48) & 0xffff);
    piece[2] = (DWORD)((immediate >> 32) & 0xffff);
    piece[1] = (DWORD)((immediate >> 16) & 0xffff);
    piece[0] = (DWORD)((immediate >> 0) & 0xffff);
    int count = 0;

    // special case: MOVN with 32-bit dest
    if (piece[3] == 0 && piece[2] == 0 && piece[1] == 0xffff)
    {
        EmitInstruction(pDstInst, Mov16::AssembleMovn32(rd, piece[0] ^ 0xffff, 0));
        count++;
    }

    // MOVN/MOVZ with 64-bit dest
    else
    {
        int zero_pieces = (piece[3] == 0x0000) + (piece[2] == 0x0000) + (piece[1] == 0x0000) + (piece[0] == 0x0000);
        int ffff_pieces = (piece[3] == 0xffff) + (piece[2] == 0xffff) + (piece[1] == 0xffff) + (piece[0] == 0xffff);
        DWORD defaultPiece = (ffff_pieces > zero_pieces) ? 0xffff : 0x0000;
        bool first = true;
        for (int pieceNum = 3; pieceNum >= 0; pieceNum--)
        {
            DWORD curPiece = piece[pieceNum];
            if (curPiece != defaultPiece || (pieceNum == 0 && first))
            {
                count++;
                if (first)
                {
                    if (defaultPiece == 0xffff)
                    {
                        EmitInstruction(pDstInst, Mov16::AssembleMovn64(rd, curPiece ^ 0xffff, pieceNum));
                    }
                    else
                    {
                        EmitInstruction(pDstInst, Mov16::AssembleMovz64(rd, curPiece, pieceNum));
                    }
                    first = false;
                }
                else
                {
                    EmitInstruction(pDstInst, Mov16::AssembleMovk64(rd, curPiece, pieceNum));
                }
            }
        }
    }
    return (BYTE)(count * sizeof(DWORD));
}

BYTE CDetourDis::CopyAdr(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    Adr19& decoded = (Adr19&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    // ADR case
    if (decoded.s.Type == 0)
    {
        BYTE* pTarget = pSource + decoded.Imm();
        LONG64 delta = pTarget - pDest;
        LONG64 deltaPage = ((ULONG_PTR)pTarget >> 12) - ((ULONG_PTR)pDest >> 12);

        // output as ADR
        if (delta >= -(1 << 20) && delta < (1 << 20))
        {
            EmitInstruction(pDstInst, Adr19::AssembleAdr(decoded.s.Rd, (LONG)delta));
        }

        // output as ADRP; ADD
        else if (deltaPage >= -(1 << 20) && (deltaPage < (1 << 20)))
        {
            EmitInstruction(pDstInst, Adr19::AssembleAdrp(decoded.s.Rd, (LONG)deltaPage));
            EmitInstruction(pDstInst, AddImm12::AssembleAdd32(decoded.s.Rd, decoded.s.Rd, ((ULONG)(ULONG_PTR)pTarget) & 0xfff, 0));
        }

        // output as immediate move
        else
        {
            EmitMovImmediate(pDstInst, decoded.s.Rd, (ULONG_PTR)pTarget);
        }
    }

    // ADRP case
    else
    {
        BYTE* pTarget = (BYTE*)((((ULONG_PTR)pSource >> 12) + decoded.Imm()) << 12);
        LONG64 deltaPage = ((ULONG_PTR)pTarget >> 12) - ((ULONG_PTR)pDest >> 12);

        // output as ADRP
        if (deltaPage >= -(1 << 20) && (deltaPage < (1 << 20)))
        {
            EmitInstruction(pDstInst, Adr19::AssembleAdrp(decoded.s.Rd, (LONG)deltaPage));
        }

        // output as immediate move
        else
        {
            EmitMovImmediate(pDstInst, decoded.s.Rd, (ULONG_PTR)pTarget);
        }
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}

BYTE CDetourDis::CopyBcc(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    Bcc19& decoded = (Bcc19&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    BYTE* pTarget = pSource + decoded.Imm();
    m_pbTarget = pTarget;
    LONG64 delta = pTarget - pDest;
    LONG64 delta4 = pTarget - (pDest + 4);

    // output as BCC
    if (delta >= -(1 << 20) && delta < (1 << 20))
    {
        EmitInstruction(pDstInst, Bcc19::AssembleBcc(decoded.s.Condition, (LONG)delta));
    }

    // output as BCC <skip>; B
    else if (delta4 >= -(1 << 27) && (delta4 < (1 << 27)))
    {
        EmitInstruction(pDstInst, Bcc19::AssembleBcc(decoded.s.Condition ^ 1, 8));
        EmitInstruction(pDstInst, Branch26::AssembleB((LONG)delta4));
    }

    // output as MOV x17, Target; BCC <skip>; BR x17 (BIG assumption that x17 isn't being used for anything!!)
    else
    {
        EmitMovImmediate(pDstInst, 17, (ULONG_PTR)pTarget);
        EmitInstruction(pDstInst, Bcc19::AssembleBcc(decoded.s.Condition ^ 1, 8));
        EmitInstruction(pDstInst, Br::AssembleBr(17));
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}

BYTE CDetourDis::CopyB_or_Bl(BYTE* pSource, BYTE* pDest, ULONG instruction, bool link)
{
    Branch26& decoded = (Branch26&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    BYTE* pTarget = pSource + decoded.Imm();
    m_pbTarget = pTarget;
    LONG64 delta = pTarget - pDest;

    // output as B or BRL
    if (delta >= -(1 << 27) && (delta < (1 << 27)))
    {
        EmitInstruction(pDstInst, Branch26::Assemble(link, (LONG)delta));
    }

    // output as MOV x17, Target; BR or BRL x17 (BIG assumption that x17 isn't being used for anything!!)
    else
    {
        EmitMovImmediate(pDstInst, 17, (ULONG_PTR)pTarget);
        EmitInstruction(pDstInst, Br::Assemble(17, link));
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}

BYTE CDetourDis::CopyB(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    return CopyB_or_Bl(pSource, pDest, instruction, false);
}

BYTE CDetourDis::CopyBl(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    return CopyB_or_Bl(pSource, pDest, instruction, true);
}

BYTE CDetourDis::CopyCbz(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    Cbz19& decoded = (Cbz19&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    BYTE* pTarget = pSource + decoded.Imm();
    m_pbTarget = pTarget;
    LONG64 delta = pTarget - pDest;
    LONG64 delta4 = pTarget - (pDest + 4);

    // output as CBZ/NZ
    if (delta >= -(1 << 20) && delta < (1 << 20))
    {
        EmitInstruction(pDstInst, Cbz19::Assemble(decoded.s.Size, decoded.s.Nz, decoded.s.Rt, (LONG)delta));
    }

    // output as CBNZ/Z <skip>; B
    else if (delta4 >= -(1 << 27) && (delta4 < (1 << 27)))
    {
        EmitInstruction(pDstInst, Cbz19::Assemble(decoded.s.Size, decoded.s.Nz ^ 1, decoded.s.Rt, 8));
        EmitInstruction(pDstInst, Branch26::AssembleB((LONG)delta4));
    }

    // output as MOV x17, Target; CBNZ/Z <skip>; BR x17 (BIG assumption that x17 isn't being used for anything!!)
    else
    {
        EmitMovImmediate(pDstInst, 17, (ULONG_PTR)pTarget);
        EmitInstruction(pDstInst, Cbz19::Assemble(decoded.s.Size, decoded.s.Nz ^ 1, decoded.s.Rt, 8));
        EmitInstruction(pDstInst, Br::AssembleBr(17));
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}

BYTE CDetourDis::CopyTbz(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    Tbz14& decoded = (Tbz14&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    BYTE* pTarget = pSource + decoded.Imm();
    m_pbTarget = pTarget;
    LONG64 delta = pTarget - pDest;
    LONG64 delta4 = pTarget - (pDest + 4);

    // output as TBZ/NZ
    if (delta >= -(1 << 13) && delta < (1 << 13))
    {
        EmitInstruction(pDstInst, Tbz14::Assemble(decoded.s.Size, decoded.s.Nz, decoded.s.Rt, decoded.s.Bit, (LONG)delta));
    }

    // output as TBNZ/Z <skip>; B
    else if (delta4 >= -(1 << 27) && (delta4 < (1 << 27)))
    {
        EmitInstruction(pDstInst, Tbz14::Assemble(decoded.s.Size, decoded.s.Nz ^ 1, decoded.s.Rt, decoded.s.Bit, 8));
        EmitInstruction(pDstInst, Branch26::AssembleB((LONG)delta4));
    }

    // output as MOV x17, Target; TBNZ/Z <skip>; BR x17 (BIG assumption that x17 isn't being used for anything!!)
    else
    {
        EmitMovImmediate(pDstInst, 17, (ULONG_PTR)pTarget);
        EmitInstruction(pDstInst, Tbz14::Assemble(decoded.s.Size, decoded.s.Nz ^ 1, decoded.s.Rt, decoded.s.Bit, 8));
        EmitInstruction(pDstInst, Br::AssembleBr(17));
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}

BYTE CDetourDis::CopyLdrLiteral(BYTE* pSource, BYTE* pDest, ULONG instruction)
{
    LdrLit19& decoded = (LdrLit19&)(instruction);
    PULONG pDstInst = (PULONG)(pDest);

    BYTE* pTarget = pSource + decoded.Imm();
    LONG64 delta = pTarget - pDest;

    // output as LDR
    if (delta >= -(1 << 21) && delta < (1 << 21))
    {
        EmitInstruction(pDstInst, LdrLit19::Assemble(decoded.s.Size, decoded.s.FpNeon, decoded.s.Rt, (LONG)delta));
    }

    // output as move immediate
    else if (decoded.s.FpNeon == 0)
    {
        UINT64 value = 0;
        switch (decoded.s.Size)
        {
            case 0: value = *(ULONG*)pTarget;       break;
            case 1: value = *(UINT64*)pTarget;   break;
            case 2: value = *(LONG*)pTarget;        break;
        }
        EmitMovImmediate(pDstInst, decoded.s.Rt, value);
    }

    // FP/NEON register: compute address in x17 and load from there (BIG assumption that x17 isn't being used for anything!!)
    else
    {
        EmitMovImmediate(pDstInst, 17, (ULONG_PTR)pTarget);
        EmitInstruction(pDstInst, LdrFpNeonImm9::Assemble(2 + decoded.s.Size, decoded.s.Rt, 17, 0));
    }

    return (BYTE)((BYTE*)pDstInst - pDest);
}


PVOID WINAPI DetourCopyInstruction(_In_opt_ PVOID pDst,
                                   _Inout_opt_ PVOID *ppDstPool,
                                   _In_ PVOID pSrc,
                                   _Out_opt_ PVOID *ppTarget,
                                   _Out_opt_ LONG *plExtra)
{
    UNREFERENCED_PARAMETER(ppDstPool);

    CDetourDis state;
    return (PVOID)state.CopyInstruction((PBYTE)pDst,
                                        (PBYTE)pSrc,
                                        (PBYTE*)ppTarget,
                                        plExtra);
}

#endif // DETOURS_ARM64

BOOL WINAPI DetourSetCodeModule(_In_ HMODULE hModule,
                                _In_ BOOL fLimitReferencesToModule)
{
#if defined(DETOURS_X64) || defined(DETOURS_X86)
    PBYTE pbBeg = NULL;
    PBYTE pbEnd = (PBYTE)~(ULONG_PTR)0;

    if (hModule != NULL) {
        ULONG cbModule = DetourGetModuleSize(hModule);

        pbBeg = (PBYTE)hModule;
        pbEnd = (PBYTE)hModule + cbModule;
    }

    return CDetourDis::SetCodeModule(pbBeg, pbEnd, fLimitReferencesToModule);
#elif defined(DETOURS_ARM) || defined(DETOURS_ARM64) || defined(DETOURS_IA64)
    (void)hModule;
    (void)fLimitReferencesToModule;
    return TRUE;
#else
#error unknown architecture (x86, x64, arm, arm64, ia64)
#endif
}

//
///////////////////////////////////////////////////////////////// End of File.

#else /* Normal threads */

#define my_pthread_once_t pthread_once_t
#if defined(PTHREAD_ONCE_INITIALIZER)
#define MY_PTHREAD_ONCE_INIT PTHREAD_ONCE_INITIALIZER
#else
#define MY_PTHREAD_ONCE_INIT PTHREAD_ONCE_INIT
#endif
#define my_pthread_once(C,F) pthread_once(C,F)

#endif /* __WIN__ */

#if defined(_MSC_VER)
#pragma warning(push)
// disable 
//         4710 : function not inlined
//         4512 : assignment operator not generated
//         4201 : nameless struct union
//         4100 : unreferenced formal;
//           4706 : assignment within conditional expression (oould be bad, but common)
//           4127 : conditional expression is constant (if (1))
//           4242 : truncation warning
//         4244 : truncation warning
//         4267 : conversion from 'size_t' to 'int'


#pragma warning (disable : 4710 4512 4201 4100 4127 4706 4242 4244 4267)

 #pragma warning(disable:4334)

#endif
#undef ERROR


/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/// Zstandard educational decoder implementation
/// See https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md

#include <stdint.h>   // uint8_t, etc.
#include <stdlib.h>   // malloc, free, exit
#include <stdio.h>    // fprintf
#include <string.h>   // memset, memcpy
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <stddef.h>   /* size_t */

/******* EXPOSED TYPES ********************************************************/
/*
* Contains the parsed contents of a dictionary
* This includes Huffman and FSE tables used for decoding and data on offsets
*/
typedef struct dictionary_s dictionary_t;
/******* END EXPOSED TYPES ****************************************************/

/******* DECOMPRESSION FUNCTIONS **********************************************/
/// Zstandard decompression functions.
/// `dst` must point to a space at least as large as the reconstructed output.
size_t ZSTD_decompress(void *const dst, const size_t dst_len,
                    const void *const src, const size_t src_len);

/// If `dict != NULL` and `dict_len >= 8`, does the same thing as
/// `ZSTD_decompress` but uses the provided dict
size_t ZSTD_decompress_with_dict(void *const dst, const size_t dst_len,
                              const void *const src, const size_t src_len,
                              dictionary_t* parsed_dict);

/// Get the decompressed size of an input stream so memory can be allocated in
/// advance
/// Returns -1 if the size can't be determined
/// Assumes decompression of a single frame
size_t ZSTD_get_decompressed_size(const void *const src, const size_t src_len);
/******* END DECOMPRESSION FUNCTIONS ******************************************/

/******* DICTIONARY MANAGEMENT ***********************************************/
/*
 * Return a valid dictionary_t pointer for use with dictionary initialization
 * or decompression
 */
dictionary_t* create_dictionary(void);

/*
 * Parse a provided dictionary blob for use in decompression
 * `src` -- must point to memory space representing the dictionary
 * `src_len` -- must provide the dictionary size
 * `dict` -- will contain the parsed contents of the dictionary and
 *        can be used for decompression
 */
void parse_dictionary(dictionary_t *const dict, const void *src,
                             size_t src_len);

/*
 * Free internal Huffman tables, FSE tables, and dictionary content
 */
void free_dictionary(dictionary_t *const dict);
/******* END DICTIONARY MANAGEMENT *******************************************/


/******* IMPORTANT CONSTANTS *********************************************/

// Zstandard frame
// "Magic_Number
// 4 Bytes, little-endian format. Value : 0xFD2FB528"
#define ZSTD_MAGIC_NUMBER 0xFD2FB528U

// The size of `Block_Content` is limited by `Block_Maximum_Size`,
#define ZSTD_BLOCK_SIZE_MAX ((size_t)128 * 1024)

// literal blocks can't be larger than their block
#define MAX_LITERALS_SIZE ZSTD_BLOCK_SIZE_MAX


/******* UTILITY MACROS AND TYPES *********************************************/
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#if defined(ZDEC_NO_MESSAGE)
#define MESSAGE(...)
#else
#define MESSAGE(...)  fprintf(stderr, "" __VA_ARGS__)
#endif

/// This decoder calls exit(1) when it encounters an error, however a production
/// library should propagate error codes
#define ERROR(s)                                                               \
    do {                                                                       \
        MESSAGE("Error: %s\n", s);                                     \
        exit(1);                                                               \
    } while (0)
#define INP_SIZE()                                                             \
    ERROR("Input buffer smaller than it should be or input is "                \
          "corrupted")
#define OUT_SIZE() ERROR("Output buffer too small for output")
#define CORRUPTION() ERROR("Corruption detected while decompressing")
#define BAD_ALLOC() ERROR("Memory allocation error")
#define IMPOSSIBLE() ERROR("An impossibility has occurred")

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
/******* END UTILITY MACROS AND TYPES *****************************************/

/******* IMPLEMENTATION PRIMITIVE PROTOTYPES **********************************/
/// The implementations for these functions can be found at the bottom of this
/// file.  They implement low-level functionality needed for the higher level
/// decompression functions.

/*** IO STREAM OPERATIONS *************/

/// ostream_t/istream_t are used to wrap the pointers/length data passed into
/// ZSTD_decompress, so that all IO operations are safely bounds checked
/// They are written/read forward, and reads are treated as little-endian
/// They should be used opaquely to ensure safety
typedef struct {
    u8 *ptr;
    size_t len;
} ostream_t;

typedef struct {
    const u8 *ptr;
    size_t len;

    // Input often reads a few bits at a time, so maintain an internal offset
    int bit_offset;
} istream_t;

/// The following two functions are the only ones that allow the istream to be
/// non-byte aligned

/// Reads `num` bits from a bitstream, and updates the internal offset
static inline u64 IO_read_bits(istream_t *const in, const int num_bits);
/// Backs-up the stream by `num` bits so they can be read again
static inline void IO_rewind_bits(istream_t *const in, const int num_bits);
/// If the remaining bits in a byte will be unused, advance to the end of the
/// byte
static inline void IO_align_stream(istream_t *const in);

/// Write the given byte into the output stream
static inline void IO_write_byte(ostream_t *const out, u8 symb);

/// Returns the number of bytes left to be read in this stream.  The stream must
/// be byte aligned.
static inline size_t IO_istream_len(const istream_t *const in);

/// Advances the stream by `len` bytes, and returns a pointer to the chunk that
/// was skipped.  The stream must be byte aligned.
static inline const u8 *IO_get_read_ptr(istream_t *const in, size_t len);
/// Advances the stream by `len` bytes, and returns a pointer to the chunk that
/// was skipped so it can be written to.
static inline u8 *IO_get_write_ptr(ostream_t *const out, size_t len);

/// Advance the inner state by `len` bytes.  The stream must be byte aligned.
static inline void IO_advance_input(istream_t *const in, size_t len);

/// Returns an `ostream_t` constructed from the given pointer and length.
static inline ostream_t IO_make_ostream(u8 *out, size_t len);
/// Returns an `istream_t` constructed from the given pointer and length.
static inline istream_t IO_make_istream(const u8 *in, size_t len);

/// Returns an `istream_t` with the same base as `in`, and length `len`.
/// Then, advance `in` to account for the consumed bytes.
/// `in` must be byte aligned.
static inline istream_t IO_make_sub_istream(istream_t *const in, size_t len);
/*** END IO STREAM OPERATIONS *********/

/*** BITSTREAM OPERATIONS *************/
/// Read `num` bits (up to 64) from `src + offset`, where `offset` is in bits,
/// and return them interpreted as a little-endian unsigned integer.
static inline u64 read_bits_LE(const u8 *src, const int num_bits,
                               const size_t offset);

/// Read bits from the end of a HUF or FSE bitstream.  `offset` is in bits, so
/// it updates `offset` to `offset - bits`, and then reads `bits` bits from
/// `src + offset`.  If the offset becomes negative, the extra bits at the
/// bottom are filled in with `0` bits instead of reading from before `src`.
static inline u64 STREAM_read_bits(const u8 *src, const int bits,
                                   i64 *const offset);
/*** END BITSTREAM OPERATIONS *********/

/*** BIT COUNTING OPERATIONS **********/
/// Returns the index of the highest set bit in `num`, or `-1` if `num == 0`
static inline int highest_set_bit(const u64 num);
/*** END BIT COUNTING OPERATIONS ******/

/*** HUFFMAN PRIMITIVES ***************/
// Table decode method uses exponential memory, so we need to limit depth
#define HUF_MAX_BITS (16)

// Limit the maximum number of symbols to 256 so we can store a symbol in a byte
#define HUF_MAX_SYMBS (256)

/// Structure containing all tables necessary for efficient Huffman decoding
typedef struct {
    u8 *symbols;
    u8 *num_bits;
    int max_bits;
} HUF_dtable;

/// Decode a single symbol and read in enough bits to refresh the state
static inline u8 HUF_decode_symbol(const HUF_dtable *const dtable,
                                   u16 *const state, const u8 *const src,
                                   i64 *const offset);
/// Read in a full state's worth of bits to initialize it
static inline void HUF_init_state(const HUF_dtable *const dtable,
                                  u16 *const state, const u8 *const src,
                                  i64 *const offset);

/// Decompresses a single Huffman stream, returns the number of bytes decoded.
/// `src_len` must be the exact length of the Huffman-coded block.
static size_t HUF_decompress_1stream(const HUF_dtable *const dtable,
                                     ostream_t *const out, istream_t *const in);
/// Same as previous but decodes 4 streams, formatted as in the Zstandard
/// specification.
/// `src_len` must be the exact length of the Huffman-coded block.
static size_t HUF_decompress_4stream(const HUF_dtable *const dtable,
                                     ostream_t *const out, istream_t *const in);

/// Initialize a Huffman decoding table using the table of bit counts provided
static void HUF_init_dtable(HUF_dtable *const table, const u8 *const bits,
                            const int num_symbs);
/// Initialize a Huffman decoding table using the table of weights provided
/// Weights follow the definition provided in the Zstandard specification
static void HUF_init_dtable_usingweights(HUF_dtable *const table,
                                         const u8 *const weights,
                                         const int num_symbs);

/// Free the malloc'ed parts of a decoding table
static void HUF_free_dtable(HUF_dtable *const dtable);
/*** END HUFFMAN PRIMITIVES ***********/

/*** FSE PRIMITIVES *******************/
/// For more description of FSE see
/// https://github.com/Cyan4973/FiniteStateEntropy/

// FSE table decoding uses exponential memory, so limit the maximum accuracy
#define FSE_MAX_ACCURACY_LOG (15)
// Limit the maximum number of symbols so they can be stored in a single byte
#define FSE_MAX_SYMBS (256)

/// The tables needed to decode FSE encoded streams
typedef struct {
    u8 *symbols;
    u8 *num_bits;
    u16 *new_state_base;
    int accuracy_log;
} FSE_dtable;

/// Return the symbol for the current state
static inline u8 FSE_peek_symbol(const FSE_dtable *const dtable,
                                 const u16 state);
/// Read the number of bits necessary to update state, update, and shift offset
/// back to reflect the bits read
static inline void FSE_update_state(const FSE_dtable *const dtable,
                                    u16 *const state, const u8 *const src,
                                    i64 *const offset);

/// Combine peek and update: decode a symbol and update the state
static inline u8 FSE_decode_symbol(const FSE_dtable *const dtable,
                                   u16 *const state, const u8 *const src,
                                   i64 *const offset);

/// Read bits from the stream to initialize the state and shift offset back
static inline void FSE_init_state(const FSE_dtable *const dtable,
                                  u16 *const state, const u8 *const src,
                                  i64 *const offset);

/// Decompress two interleaved bitstreams (e.g. compressed Huffman weights)
/// using an FSE decoding table.  `src_len` must be the exact length of the
/// block.
static size_t FSE_decompress_interleaved2(const FSE_dtable *const dtable,
                                          ostream_t *const out,
                                          istream_t *const in);

/// Initialize a decoding table using normalized frequencies.
static void FSE_init_dtable(FSE_dtable *const dtable,
                            const i16 *const norm_freqs, const int num_symbs,
                            const int accuracy_log);

/// Decode an FSE header as defined in the Zstandard format specification and
/// use the decoded frequencies to initialize a decoding table.
static void FSE_decode_header(FSE_dtable *const dtable, istream_t *const in,
                                const int max_accuracy_log);

/// Initialize an FSE table that will always return the same symbol and consume
/// 0 bits per symbol, to be used for RLE mode in sequence commands
static void FSE_init_dtable_rle(FSE_dtable *const dtable, const u8 symb);

/// Free the malloc'ed parts of a decoding table
static void FSE_free_dtable(FSE_dtable *const dtable);
/*** END FSE PRIMITIVES ***************/

/******* END IMPLEMENTATION PRIMITIVE PROTOTYPES ******************************/

/******* ZSTD HELPER STRUCTS AND PROTOTYPES ***********************************/

/// A small structure that can be reused in various places that need to access
/// frame header information
typedef struct {
    // The size of window that we need to be able to contiguously store for
    // references
    size_t window_size;
    // The total output size of this compressed frame
    size_t frame_content_size;

    // The dictionary id if this frame uses one
    u32 dictionary_id;

    // Whether or not the content of this frame has a checksum
    int content_checksum_flag;
    // Whether or not the output for this frame is in a single segment
    int single_segment_flag;
} frame_header_t;

/// The context needed to decode blocks in a frame
typedef struct {
    frame_header_t header;

    // The total amount of data available for backreferences, to determine if an
    // offset too large to be correct
    size_t current_total_output;

    const u8 *dict_content;
    size_t dict_content_len;

    // Entropy encoding tables so they can be repeated by future blocks instead
    // of retransmitting
    HUF_dtable literals_dtable;
    FSE_dtable ll_dtable;
    FSE_dtable ml_dtable;
    FSE_dtable of_dtable;

    // The last 3 offsets for the special "repeat offsets".
    u64 previous_offsets[3];
} frame_context_t;

/// The decoded contents of a dictionary so that it doesn't have to be repeated
/// for each frame that uses it
struct dictionary_s {
    // Entropy tables
    HUF_dtable literals_dtable;
    FSE_dtable ll_dtable;
    FSE_dtable ml_dtable;
    FSE_dtable of_dtable;

    // Raw content for backreferences
    u8 *content;
    size_t content_size;

    // Offset history to prepopulate the frame's history
    u64 previous_offsets[3];

    u32 dictionary_id;
};

/// A tuple containing the parts necessary to decode and execute a ZSTD sequence
/// command
typedef struct {
    u32 literal_length;
    u32 match_length;
    u32 offset;
} sequence_command_t;

/// The decoder works top-down, starting at the high level like Zstd frames, and
/// working down to lower more technical levels such as blocks, literals, and
/// sequences.  The high-level functions roughly follow the outline of the
/// format specification:
/// https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md

/// Before the implementation of each high-level function declared here, the
/// prototypes for their helper functions are defined and explained

/// Decode a single Zstd frame, or error if the input is not a valid frame.
/// Accepts a dict argument, which may be NULL indicating no dictionary.
/// See
/// https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md#frame-concatenation
static void decode_frame(ostream_t *const out, istream_t *const in,
                         const dictionary_t *const dict);

// Decode data in a compressed block
static void decompress_block(frame_context_t *const ctx, ostream_t *const out,
                             istream_t *const in);

// Decode the literals section of a block
static size_t decode_literals(frame_context_t *const ctx, istream_t *const in,
                              u8 **const literals);

// Decode the sequences part of a block
static size_t decode_sequences(frame_context_t *const ctx, istream_t *const in,
                               sequence_command_t **const sequences);

// Execute the decoded sequences on the literals block
static void execute_sequences(frame_context_t *const ctx, ostream_t *const out,
                              const u8 *const literals,
                              const size_t literals_len,
                              const sequence_command_t *const sequences,
                              const size_t num_sequences);

// Copies literals and returns the total literal length that was copied
static u32 copy_literals(const size_t seq, istream_t *litstream,
                         ostream_t *const out);

// Given an offset code from a sequence command (either an actual offset value
// or an index for previous offset), computes the correct offset and updates
// the offset history
static size_t compute_offset(sequence_command_t seq, u64 *const offset_hist);

// Given an offset, match length, and total output, as well as the frame
// context for the dictionary, determines if the dictionary is used and
// executes the copy operation
static void execute_match_copy(frame_context_t *const ctx, size_t offset,
                              size_t match_length, size_t total_output,
                              ostream_t *const out);

/******* END ZSTD HELPER STRUCTS AND PROTOTYPES *******************************/

size_t ZSTD_decompress(void *const dst, const size_t dst_len,
                       const void *const src, const size_t src_len) {
    dictionary_t* const uninit_dict = create_dictionary();
    size_t const decomp_size = ZSTD_decompress_with_dict(dst, dst_len, src,
                                                         src_len, uninit_dict);
    free_dictionary(uninit_dict);
    return decomp_size;
}

size_t ZSTD_decompress_with_dict(void *const dst, const size_t dst_len,
                                 const void *const src, const size_t src_len,
                                 dictionary_t* parsed_dict) {

    istream_t in = IO_make_istream((u8 *)src, src_len);
    ostream_t out = IO_make_ostream((u8 *)dst, dst_len);

    // "A content compressed by Zstandard is transformed into a Zstandard frame.
    // Multiple frames can be appended into a single file or stream. A frame is
    // totally independent, has a defined beginning and end, and a set of
    // parameters which tells the decoder how to decompress it."

    /* this decoder assumes decompression of a single frame */
    decode_frame(&out, &in, parsed_dict);

    return (size_t)(out.ptr - (u8 *)dst);
}

/******* FRAME DECODING ******************************************************/

static void decode_data_frame(ostream_t *const out, istream_t *const in,
                              const dictionary_t *const dict);
static void init_frame_context(frame_context_t *const context,
                               istream_t *const in,
                               const dictionary_t *const dict);
static void free_frame_context(frame_context_t *const context);
static void parse_frame_header(frame_header_t *const header,
                               istream_t *const in);
static void frame_context_apply_dict(frame_context_t *const ctx,
                                     const dictionary_t *const dict);

static void decompress_data(frame_context_t *const ctx, ostream_t *const out,
                            istream_t *const in);

static void decode_frame(ostream_t *const out, istream_t *const in,
                         const dictionary_t *const dict) {
    const u32 magic_number = (u32)IO_read_bits(in, 32);
    if (magic_number == ZSTD_MAGIC_NUMBER) {
        // ZSTD frame
        decode_data_frame(out, in, dict);

        return;
    }

    // not a real frame or a skippable frame
    ERROR("Tried to decode non-ZSTD frame");
}

/// Decode a frame that contains compressed data.  Not all frames do as there
/// are skippable frames.
/// See
/// https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md#general-structure-of-zstandard-frame-format
static void decode_data_frame(ostream_t *const out, istream_t *const in,
                              const dictionary_t *const dict) {
    frame_context_t ctx;

    // Initialize the context that needs to be carried from block to block
    init_frame_context(&ctx, in, dict);

    if (ctx.header.frame_content_size != 0 &&
        ctx.header.frame_content_size > out->len) {
        OUT_SIZE();
    }

    decompress_data(&ctx, out, in);

    free_frame_context(&ctx);
}

/// Takes the information provided in the header and dictionary, and initializes
/// the context for this frame
static void init_frame_context(frame_context_t *const context,
                               istream_t *const in,
                               const dictionary_t *const dict) {
    // Most fields in context are correct when initialized to 0
    memset(context, 0, sizeof(frame_context_t));

    // Parse data from the frame header
    parse_frame_header(&context->header, in);

    // Set up the offset history for the repeat offset commands
    context->previous_offsets[0] = 1;
    context->previous_offsets[1] = 4;
    context->previous_offsets[2] = 8;

    // Apply details from the dict if it exists
    frame_context_apply_dict(context, dict);
}

static void free_frame_context(frame_context_t *const context) {
    HUF_free_dtable(&context->literals_dtable);

    FSE_free_dtable(&context->ll_dtable);
    FSE_free_dtable(&context->ml_dtable);
    FSE_free_dtable(&context->of_dtable);

    memset(context, 0, sizeof(frame_context_t));
}

static void parse_frame_header(frame_header_t *const header,
                               istream_t *const in) {
    // "The first header's byte is called the Frame_Header_Descriptor. It tells
    // which other fields are present. Decoding this byte is enough to tell the
    // size of Frame_Header.
    //
    // Bit number   Field name
    // 7-6  Frame_Content_Size_flag
    // 5    Single_Segment_flag
    // 4    Unused_bit
    // 3    Reserved_bit
    // 2    Content_Checksum_flag
    // 1-0  Dictionary_ID_flag"
    const u8 descriptor = (u8)IO_read_bits(in, 8);

    // decode frame header descriptor into flags
    const u8 frame_content_size_flag = descriptor >> 6;
    const u8 single_segment_flag = (descriptor >> 5) & 1;
    const u8 reserved_bit = (descriptor >> 3) & 1;
    const u8 content_checksum_flag = (descriptor >> 2) & 1;
    const u8 dictionary_id_flag = descriptor & 3;

    if (reserved_bit != 0) {
        CORRUPTION();
    }

    header->single_segment_flag = single_segment_flag;
    header->content_checksum_flag = content_checksum_flag;

    // decode window size
    if (!single_segment_flag) {
        // "Provides guarantees on maximum back-reference distance that will be
        // used within compressed data. This information is important for
        // decoders to allocate enough memory.
        //
        // Bit numbers  7-3         2-0
        // Field name   Exponent    Mantissa"
        u8 window_descriptor = (u8)IO_read_bits(in, 8);
        u8 exponent = window_descriptor >> 3;
        u8 mantissa = window_descriptor & 7;

        // Use the algorithm from the specification to compute window size
        // https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md#window_descriptor
        size_t window_base = (size_t)1 << (10 + exponent);
        size_t window_add = (window_base / 8) * mantissa;
        header->window_size = window_base + window_add;
    }

    // decode dictionary id if it exists
    if (dictionary_id_flag) {
        // "This is a variable size field, which contains the ID of the
        // dictionary required to properly decode the frame. Note that this
        // field is optional. When it's not present, it's up to the caller to
        // make sure it uses the correct dictionary. Format is little-endian."
        const int bytes_array[] = {0, 1, 2, 4};
        const int bytes = bytes_array[dictionary_id_flag];

        header->dictionary_id = (u32)IO_read_bits(in, bytes * 8);
    } else {
        header->dictionary_id = 0;
    }

    // decode frame content size if it exists
    if (single_segment_flag || frame_content_size_flag) {
        // "This is the original (uncompressed) size. This information is
        // optional. The Field_Size is provided according to value of
        // Frame_Content_Size_flag. The Field_Size can be equal to 0 (not
        // present), 1, 2, 4 or 8 bytes. Format is little-endian."
        //
        // if frame_content_size_flag == 0 but single_segment_flag is set, we
        // still have a 1 byte field
        const int bytes_array[] = {1, 2, 4, 8};
        const int bytes = bytes_array[frame_content_size_flag];

        header->frame_content_size = IO_read_bits(in, bytes * 8);
        if (bytes == 2) {
            // "When Field_Size is 2, the offset of 256 is added."
            header->frame_content_size += 256;
        }
    } else {
        header->frame_content_size = 0;
    }

    if (single_segment_flag) {
        // "The Window_Descriptor byte is optional. It is absent when
        // Single_Segment_flag is set. In this case, the maximum back-reference
        // distance is the content size itself, which can be any value from 1 to
        // 2^64-1 bytes (16 EB)."
        header->window_size = header->frame_content_size;
    }
}

/// Decompress the data from a frame block by block
static void decompress_data(frame_context_t *const ctx, ostream_t *const out,
                            istream_t *const in) {
    // "A frame encapsulates one or multiple blocks. Each block can be
    // compressed or not, and has a guaranteed maximum content size, which
    // depends on frame parameters. Unlike frames, each block depends on
    // previous blocks for proper decoding. However, each block can be
    // decompressed without waiting for its successor, allowing streaming
    // operations."
    int last_block = 0;
    do {
        // "Last_Block
        //
        // The lowest bit signals if this block is the last one. Frame ends
        // right after this block.
        //
        // Block_Type and Block_Size
        //
        // The next 2 bits represent the Block_Type, while the remaining 21 bits
        // represent the Block_Size. Format is little-endian."
        last_block = (int)IO_read_bits(in, 1);
        const int block_type = (int)IO_read_bits(in, 2);
        const size_t block_len = IO_read_bits(in, 21);

        switch (block_type) {
        case 0: {
            // "Raw_Block - this is an uncompressed block. Block_Size is the
            // number of bytes to read and copy."
            const u8 *const read_ptr = IO_get_read_ptr(in, block_len);
            u8 *const write_ptr = IO_get_write_ptr(out, block_len);

            // Copy the raw data into the output
            memcpy(write_ptr, read_ptr, block_len);

            ctx->current_total_output += block_len;
            break;
        }
        case 1: {
            // "RLE_Block - this is a single byte, repeated N times. In which
            // case, Block_Size is the size to regenerate, while the
            // "compressed" block is just 1 byte (the byte to repeat)."
            const u8 *const read_ptr = IO_get_read_ptr(in, 1);
            u8 *const write_ptr = IO_get_write_ptr(out, block_len);

            // Copy `block_len` copies of `read_ptr[0]` to the output
            memset(write_ptr, read_ptr[0], block_len);

            ctx->current_total_output += block_len;
            break;
        }
        case 2: {
            // "Compressed_Block - this is a Zstandard compressed block,
            // detailed in another section of this specification. Block_Size is
            // the compressed size.

            // Create a sub-stream for the block
            istream_t block_stream = IO_make_sub_istream(in, block_len);
            decompress_block(ctx, out, &block_stream);
            break;
        }
        case 3:
            // "Reserved - this is not a block. This value cannot be used with
            // current version of this specification."
            CORRUPTION();
            break;
        default:
            IMPOSSIBLE();
        }
    } while (!last_block);

    if (ctx->header.content_checksum_flag) {
        // This program does not support checking the checksum, so skip over it
        // if it's present
        IO_advance_input(in, 4);
    }
}
/******* END FRAME DECODING ***************************************************/

/******* BLOCK DECOMPRESSION **************************************************/
static void decompress_block(frame_context_t *const ctx, ostream_t *const out,
                             istream_t *const in) {
    // "A compressed block consists of 2 sections :
    //
    // Literals_Section
    // Sequences_Section"


    // Part 1: decode the literals block
    u8 *literals = NULL;
    const size_t literals_size = decode_literals(ctx, in, &literals);

    // Part 2: decode the sequences block
    sequence_command_t *sequences = NULL;
    const size_t num_sequences =
        decode_sequences(ctx, in, &sequences);

    // Part 3: combine literals and sequence commands to generate output
    execute_sequences(ctx, out, literals, literals_size, sequences,
                      num_sequences);
    free(literals);
    free(sequences);
}
/******* END BLOCK DECOMPRESSION **********************************************/

/******* LITERALS DECODING ****************************************************/
static size_t decode_literals_simple(istream_t *const in, u8 **const literals,
                                     const int block_type,
                                     const int size_format);
static size_t decode_literals_compressed(frame_context_t *const ctx,
                                         istream_t *const in,
                                         u8 **const literals,
                                         const int block_type,
                                         const int size_format);
static void decode_huf_table(HUF_dtable *const dtable, istream_t *const in);
static void fse_decode_hufweights(ostream_t *weights, istream_t *const in,
                                    int *const num_symbs);

static size_t decode_literals(frame_context_t *const ctx, istream_t *const in,
                              u8 **const literals) {
    // "Literals can be stored uncompressed or compressed using Huffman prefix
    // codes. When compressed, an optional tree description can be present,
    // followed by 1 or 4 streams."
    //
    // "Literals_Section_Header
    //
    // Header is in charge of describing how literals are packed. It's a
    // byte-aligned variable-size bitfield, ranging from 1 to 5 bytes, using
    // little-endian convention."
    //
    // "Literals_Block_Type
    //
    // This field uses 2 lowest bits of first byte, describing 4 different block
    // types"
    //
    // size_format takes between 1 and 2 bits
    int block_type = (int)IO_read_bits(in, 2);
    int size_format = (int)IO_read_bits(in, 2);

    if (block_type <= 1) {
        // Raw or RLE literals block
        return decode_literals_simple(in, literals, block_type,
                                      size_format);
    } else {
        // Huffman compressed literals
        return decode_literals_compressed(ctx, in, literals, block_type,
                                          size_format);
    }
}

/// Decodes literals blocks in raw or RLE form
static size_t decode_literals_simple(istream_t *const in, u8 **const literals,
                                     const int block_type,
                                     const int size_format) {
    size_t size;
    switch (size_format) {
    // These cases are in the form ?0
    // In this case, the ? bit is actually part of the size field
    case 0:
    case 2:
        // "Size_Format uses 1 bit. Regenerated_Size uses 5 bits (0-31)."
        IO_rewind_bits(in, 1);
        size = IO_read_bits(in, 5);
        break;
    case 1:
        // "Size_Format uses 2 bits. Regenerated_Size uses 12 bits (0-4095)."
        size = IO_read_bits(in, 12);
        break;
    case 3:
        // "Size_Format uses 2 bits. Regenerated_Size uses 20 bits (0-1048575)."
        size = IO_read_bits(in, 20);
        break;
    default:
        // Size format is in range 0-3
        IMPOSSIBLE();
    }

    if (size > MAX_LITERALS_SIZE) {
        CORRUPTION();
    }

    *literals = (u8 *)malloc(size);
    if (!*literals) {
        BAD_ALLOC();
    }

    switch (block_type) {
    case 0: {
        // "Raw_Literals_Block - Literals are stored uncompressed."
        const u8 *const read_ptr = IO_get_read_ptr(in, size);
        memcpy(*literals, read_ptr, size);
        break;
    }
    case 1: {
        // "RLE_Literals_Block - Literals consist of a single byte value repeated N times."
        const u8 *const read_ptr = IO_get_read_ptr(in, 1);
        memset(*literals, read_ptr[0], size);
        break;
    }
    default:
        IMPOSSIBLE();
    }

    return size;
}

/// Decodes Huffman compressed literals
static size_t decode_literals_compressed(frame_context_t *const ctx,
                                         istream_t *const in,
                                         u8 **const literals,
                                         const int block_type,
                                         const int size_format) {
    size_t regenerated_size, compressed_size;
    // Only size_format=0 has 1 stream, so default to 4
    int num_streams = 4;
    switch (size_format) {
    case 0:
        // "A single stream. Both Compressed_Size and Regenerated_Size use 10
        // bits (0-1023)."
        num_streams = 1;
    // Fall through as it has the same size format
        /* fallthrough */
    case 1:
        // "4 streams. Both Compressed_Size and Regenerated_Size use 10 bits
        // (0-1023)."
        regenerated_size = IO_read_bits(in, 10);
        compressed_size = IO_read_bits(in, 10);
        break;
    case 2:
        // "4 streams. Both Compressed_Size and Regenerated_Size use 14 bits
        // (0-16383)."
        regenerated_size = IO_read_bits(in, 14);
        compressed_size = IO_read_bits(in, 14);
        break;
    case 3:
        // "4 streams. Both Compressed_Size and Regenerated_Size use 18 bits
        // (0-262143)."
        regenerated_size = IO_read_bits(in, 18);
        compressed_size = IO_read_bits(in, 18);
        break;
    default:
        // Impossible
        IMPOSSIBLE();
    }
    if (regenerated_size > MAX_LITERALS_SIZE) {
        CORRUPTION();
    }

    *literals = (u8 *)malloc(regenerated_size);
    if (!*literals) {
        BAD_ALLOC();
    }

    ostream_t lit_stream = IO_make_ostream(*literals, regenerated_size);
    istream_t huf_stream = IO_make_sub_istream(in, compressed_size);

    if (block_type == 2) {
        // Decode the provided Huffman table
        // "This section is only present when Literals_Block_Type type is
        // Compressed_Literals_Block (2)."

        HUF_free_dtable(&ctx->literals_dtable);
        decode_huf_table(&ctx->literals_dtable, &huf_stream);
    } else {
        // If the previous Huffman table is being repeated, ensure it exists
        if (!ctx->literals_dtable.symbols) {
            CORRUPTION();
        }
    }

    size_t symbols_decoded;
    if (num_streams == 1) {
        symbols_decoded = HUF_decompress_1stream(&ctx->literals_dtable, &lit_stream, &huf_stream);
    } else {
        symbols_decoded = HUF_decompress_4stream(&ctx->literals_dtable, &lit_stream, &huf_stream);
    }

    if (symbols_decoded != regenerated_size) {
        CORRUPTION();
    }

    return regenerated_size;
}

// Decode the Huffman table description
static void decode_huf_table(HUF_dtable *const dtable, istream_t *const in) {
    // "All literal values from zero (included) to last present one (excluded)
    // are represented by Weight with values from 0 to Max_Number_of_Bits."

    // "This is a single byte value (0-255), which describes how to decode the list of weights."
    const u8 header = IO_read_bits(in, 8);

    u8 weights[HUF_MAX_SYMBS];
    memset(weights, 0, sizeof(weights));

    int num_symbs;

    if (header >= 128) {
        // "This is a direct representation, where each Weight is written
        // directly as a 4 bits field (0-15). The full representation occupies
        // ((Number_of_Symbols+1)/2) bytes, meaning it uses a last full byte
        // even if Number_of_Symbols is odd. Number_of_Symbols = headerByte -
        // 127"
        num_symbs = header - 127;
        const size_t bytes = (num_symbs + 1) / 2;

        const u8 *const weight_src = IO_get_read_ptr(in, bytes);

        for (int i = 0; i < num_symbs; i++) {
            // "They are encoded forward, 2
            // weights to a byte with the first weight taking the top four bits
            // and the second taking the bottom four (e.g. the following
            // operations could be used to read the weights: Weight[0] =
            // (Byte[0] >> 4), Weight[1] = (Byte[0] & 0xf), etc.)."
            if (i % 2 == 0) {
                weights[i] = weight_src[i / 2] >> 4;
            } else {
                weights[i] = weight_src[i / 2] & 0xf;
            }
        }
    } else {
        // The weights are FSE encoded, decode them before we can construct the
        // table
        istream_t fse_stream = IO_make_sub_istream(in, header);
        ostream_t weight_stream = IO_make_ostream(weights, HUF_MAX_SYMBS);
        fse_decode_hufweights(&weight_stream, &fse_stream, &num_symbs);
    }

    // Construct the table using the decoded weights
    HUF_init_dtable_usingweights(dtable, weights, num_symbs);
}

static void fse_decode_hufweights(ostream_t *weights, istream_t *const in,
                                    int *const num_symbs) {
    const int MAX_ACCURACY_LOG = 7;

    FSE_dtable dtable;

    // "An FSE bitstream starts by a header, describing probabilities
    // distribution. It will create a Decoding Table. For a list of Huffman
    // weights, maximum accuracy is 7 bits."
    FSE_decode_header(&dtable, in, MAX_ACCURACY_LOG);

    // Decode the weights
    *num_symbs = FSE_decompress_interleaved2(&dtable, weights, in);

    FSE_free_dtable(&dtable);
}
/******* END LITERALS DECODING ************************************************/

/******* SEQUENCE DECODING ****************************************************/
/// The combination of FSE states needed to decode sequences
typedef struct {
    FSE_dtable ll_table;
    FSE_dtable of_table;
    FSE_dtable ml_table;

    u16 ll_state;
    u16 of_state;
    u16 ml_state;
} sequence_states_t;

/// Different modes to signal to decode_seq_tables what to do
typedef enum {
    seq_literal_length = 0,
    seq_offset = 1,
    seq_match_length = 2,
} seq_part_t;

typedef enum {
    seq_predefined = 0,
    seq_rle = 1,
    seq_fse = 2,
    seq_repeat = 3,
} seq_mode_t;

/// The predefined FSE distribution tables for `seq_predefined` mode
static const i16 SEQ_LITERAL_LENGTH_DEFAULT_DIST[36] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
    2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const i16 SEQ_OFFSET_DEFAULT_DIST[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
static const i16 SEQ_MATCH_LENGTH_DEFAULT_DIST[53] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};

/// The sequence decoding baseline and number of additional bits to read/add
/// https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md#the-codes-for-literals-lengths-match-lengths-and-offsets
static const u32 SEQ_LITERAL_LENGTH_BASELINES[36] = {
    0,  1,  2,   3,   4,   5,    6,    7,    8,    9,     10,    11,
    12, 13, 14,  15,  16,  18,   20,   22,   24,   28,    32,    40,
    48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const u8 SEQ_LITERAL_LENGTH_EXTRA_BITS[36] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
    1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static const u32 SEQ_MATCH_LENGTH_BASELINES[53] = {
    3,  4,   5,   6,   7,    8,    9,    10,   11,    12,    13,   14, 15, 16,
    17, 18,  19,  20,  21,   22,   23,   24,   25,    26,    27,   28, 29, 30,
    31, 32,  33,  34,  35,   37,   39,   41,   43,    47,    51,   59, 67, 83,
    99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const u8 SEQ_MATCH_LENGTH_EXTRA_BITS[53] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
    2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/// Offset decoding is simpler so we just need a maximum code value
static const u8 SEQ_MAX_CODES[3] = {35, (u8)-1, 52};

static void decompress_sequences(frame_context_t *const ctx,
                                 istream_t *const in,
                                 sequence_command_t *const sequences,
                                 const size_t num_sequences);
static sequence_command_t decode_sequence(sequence_states_t *const state,
                                          const u8 *const src,
                                          i64 *const offset,
                                          int lastSequence);
static void decode_seq_table(FSE_dtable *const table, istream_t *const in,
                               const seq_part_t type, const seq_mode_t mode);

static size_t decode_sequences(frame_context_t *const ctx, istream_t *in,
                               sequence_command_t **const sequences) {
    // "A compressed block is a succession of sequences . A sequence is a
    // literal copy command, followed by a match copy command. A literal copy
    // command specifies a length. It is the number of bytes to be copied (or
    // extracted) from the literal section. A match copy command specifies an
    // offset and a length. The offset gives the position to copy from, which
    // can be within a previous block."

    size_t num_sequences;

    // "Number_of_Sequences
    //
    // This is a variable size field using between 1 and 3 bytes. Let's call its
    // first byte byte0."
    u8 header = IO_read_bits(in, 8);
    if (header < 128) {
        // "Number_of_Sequences = byte0 . Uses 1 byte."
        num_sequences = header;
    } else if (header < 255) {
        // "Number_of_Sequences = ((byte0-128) << 8) + byte1 . Uses 2 bytes."
        num_sequences = ((header - 128) << 8) + IO_read_bits(in, 8);
    } else {
        // "Number_of_Sequences = byte1 + (byte2<<8) + 0x7F00 . Uses 3 bytes."
        num_sequences = IO_read_bits(in, 16) + 0x7F00;
    }

    if (num_sequences == 0) {
        // "There are no sequences. The sequence section stops there."
        *sequences = NULL;
        return 0;
    }

    *sequences = (sequence_command_t*)malloc(num_sequences * sizeof(sequence_command_t));
    if (!*sequences) {
        BAD_ALLOC();
    }

    decompress_sequences(ctx, in, *sequences, num_sequences);
    return num_sequences;
}

/// Decompress the FSE encoded sequence commands
static void decompress_sequences(frame_context_t *const ctx, istream_t *in,
                                 sequence_command_t *const sequences,
                                 const size_t num_sequences) {
    // "The Sequences_Section regroup all symbols required to decode commands.
    // There are 3 symbol types : literals lengths, offsets and match lengths.
    // They are encoded together, interleaved, in a single bitstream."

    // "Symbol compression modes
    //
    // This is a single byte, defining the compression mode of each symbol
    // type."
    //
    // Bit number : Field name
    // 7-6        : Literals_Lengths_Mode
    // 5-4        : Offsets_Mode
    // 3-2        : Match_Lengths_Mode
    // 1-0        : Reserved
    u8 compression_modes = IO_read_bits(in, 8);

    if ((compression_modes & 3) != 0) {
        // Reserved bits set
        CORRUPTION();
    }

    // "Following the header, up to 3 distribution tables can be described. When
    // present, they are in this order :
    //
    // Literals lengths
    // Offsets
    // Match Lengths"
    // Update the tables we have stored in the context
    decode_seq_table(&ctx->ll_dtable, in, seq_literal_length,
                     (seq_mode_t)((compression_modes >> 6) & 3));

    decode_seq_table(&ctx->of_dtable, in, seq_offset,
                     (seq_mode_t)((compression_modes >> 4) & 3));

    decode_seq_table(&ctx->ml_dtable, in, seq_match_length,
                     (seq_mode_t)((compression_modes >> 2) & 3));


    sequence_states_t states;

    // Initialize the decoding tables
    {
        states.ll_table = ctx->ll_dtable;
        states.of_table = ctx->of_dtable;
        states.ml_table = ctx->ml_dtable;
    }

    const size_t len = IO_istream_len(in);
    const u8 *const src = IO_get_read_ptr(in, len);

    // "After writing the last bit containing information, the compressor writes
    // a single 1-bit and then fills the byte with 0-7 0 bits of padding."
    const int padding = 8 - highest_set_bit(src[len - 1]);
    // The offset starts at the end because FSE streams are read backwards
    i64 bit_offset = (i64)(len * 8 - (size_t)padding);

    // "The bitstream starts with initial state values, each using the required
    // number of bits in their respective accuracy, decoded previously from
    // their normalized distribution.
    //
    // It starts by Literals_Length_State, followed by Offset_State, and finally
    // Match_Length_State."
    FSE_init_state(&states.ll_table, &states.ll_state, src, &bit_offset);
    FSE_init_state(&states.of_table, &states.of_state, src, &bit_offset);
    FSE_init_state(&states.ml_table, &states.ml_state, src, &bit_offset);

    for (size_t i = 0; i < num_sequences; i++) {
        // Decode sequences one by one
        sequences[i] = decode_sequence(&states, src, &bit_offset, i==num_sequences-1);
    }

    if (bit_offset != 0) {
        CORRUPTION();
    }
}

// Decode a single sequence and update the state
static sequence_command_t decode_sequence(sequence_states_t *const states,
                                          const u8 *const src,
                                          i64 *const offset,
                                          int lastSequence) {
    // "Each symbol is a code in its own context, which specifies Baseline and
    // Number_of_Bits to add. Codes are FSE compressed, and interleaved with raw
    // additional bits in the same bitstream."

    // Decode symbols, but don't update states
    const u8 of_code = FSE_peek_symbol(&states->of_table, states->of_state);
    const u8 ll_code = FSE_peek_symbol(&states->ll_table, states->ll_state);
    const u8 ml_code = FSE_peek_symbol(&states->ml_table, states->ml_state);

    // Offset doesn't need a max value as it's not decoded using a table
    if (ll_code > SEQ_MAX_CODES[seq_literal_length] ||
        ml_code > SEQ_MAX_CODES[seq_match_length]) {
        CORRUPTION();
    }

    // Read the interleaved bits
    sequence_command_t seq;
    // "Decoding starts by reading the Number_of_Bits required to decode Offset.
    // It then does the same for Match_Length, and then for Literals_Length."
    seq.offset = ((u32)1 << of_code) + STREAM_read_bits(src, of_code, offset);

    seq.match_length =
        SEQ_MATCH_LENGTH_BASELINES[ml_code] +
        STREAM_read_bits(src, SEQ_MATCH_LENGTH_EXTRA_BITS[ml_code], offset);

    seq.literal_length =
        SEQ_LITERAL_LENGTH_BASELINES[ll_code] +
        STREAM_read_bits(src, SEQ_LITERAL_LENGTH_EXTRA_BITS[ll_code], offset);

    // "If it is not the last sequence in the block, the next operation is to
    // update states. Using the rules pre-calculated in the decoding tables,
    // Literals_Length_State is updated, followed by Match_Length_State, and
    // then Offset_State."
    // If the stream is complete don't read bits to update state
    if (!lastSequence) {
        FSE_update_state(&states->ll_table, &states->ll_state, src, offset);
        FSE_update_state(&states->ml_table, &states->ml_state, src, offset);
        FSE_update_state(&states->of_table, &states->of_state, src, offset);
    }

    return seq;
}

/// Given a sequence part and table mode, decode the FSE distribution
/// Errors if the mode is `seq_repeat` without a pre-existing table in `table`
static void decode_seq_table(FSE_dtable *const table, istream_t *const in,
                             const seq_part_t type, const seq_mode_t mode) {
    // Constant arrays indexed by seq_part_t
    const i16 *const default_distributions[] = {SEQ_LITERAL_LENGTH_DEFAULT_DIST,
                                                SEQ_OFFSET_DEFAULT_DIST,
                                                SEQ_MATCH_LENGTH_DEFAULT_DIST};
    const size_t default_distribution_lengths[] = {36, 29, 53};
    const size_t default_distribution_accuracies[] = {6, 5, 6};

    const size_t max_accuracies[] = {9, 8, 9};

    if (mode != seq_repeat) {
        // Free old one before overwriting
        FSE_free_dtable(table);
    }

    switch (mode) {
    case seq_predefined: {
        // "Predefined_Mode : uses a predefined distribution table."
        const i16 *distribution = default_distributions[type];
        const size_t symbs = default_distribution_lengths[type];
        const size_t accuracy_log = default_distribution_accuracies[type];

        FSE_init_dtable(table, distribution, symbs, accuracy_log);
        break;
    }
    case seq_rle: {
        // "RLE_Mode : it's a single code, repeated Number_of_Sequences times."
        const u8 symb = IO_get_read_ptr(in, 1)[0];
        FSE_init_dtable_rle(table, symb);
        break;
    }
    case seq_fse: {
        // "FSE_Compressed_Mode : standard FSE compression. A distribution table
        // will be present "
        FSE_decode_header(table, in, max_accuracies[type]);
        break;
    }
    case seq_repeat:
        // "Repeat_Mode : reuse distribution table from previous compressed
        // block."
        // Nothing to do here, table will be unchanged
        if (!table->symbols) {
            // This mode is invalid if we don't already have a table
            CORRUPTION();
        }
        break;
    default:
        // Impossible, as mode is from 0-3
        IMPOSSIBLE();
        break;
    }

}
/******* END SEQUENCE DECODING ************************************************/

/******* SEQUENCE EXECUTION ***************************************************/
static void execute_sequences(frame_context_t *const ctx, ostream_t *const out,
                              const u8 *const literals,
                              const size_t literals_len,
                              const sequence_command_t *const sequences,
                              const size_t num_sequences) {
    istream_t litstream = IO_make_istream(literals, literals_len);

    u64 *const offset_hist = ctx->previous_offsets;
    size_t total_output = ctx->current_total_output;

    for (size_t i = 0; i < num_sequences; i++) {
        const sequence_command_t seq = sequences[i];
        {
            const u32 literals_size = copy_literals(seq.literal_length, &litstream, out);
            total_output += literals_size;
        }

        size_t const offset = compute_offset(seq, offset_hist);

        size_t const match_length = seq.match_length;

        execute_match_copy(ctx, offset, match_length, total_output, out);

        total_output += match_length;
    }

    // Copy any leftover literals
    {
        size_t len = IO_istream_len(&litstream);
        copy_literals(len, &litstream, out);
        total_output += len;
    }

    ctx->current_total_output = total_output;
}

static u32 copy_literals(const size_t literal_length, istream_t *litstream,
                         ostream_t *const out) {
    // If the sequence asks for more literals than are left, the
    // sequence must be corrupted
    if (literal_length > IO_istream_len(litstream)) {
        CORRUPTION();
    }

    u8 *const write_ptr = IO_get_write_ptr(out, literal_length);
    const u8 *const read_ptr =
         IO_get_read_ptr(litstream, literal_length);
    // Copy literals to output
    memcpy(write_ptr, read_ptr, literal_length);

    return literal_length;
}

static size_t compute_offset(sequence_command_t seq, u64 *const offset_hist) {
    size_t offset;
    // Offsets are special, we need to handle the repeat offsets
    if (seq.offset <= 3) {
        // "The first 3 values define a repeated offset and we will call
        // them Repeated_Offset1, Repeated_Offset2, and Repeated_Offset3.
        // They are sorted in recency order, with Repeated_Offset1 meaning
        // 'most recent one'".

        // Use 0 indexing for the array
        u32 idx = seq.offset - 1;
        if (seq.literal_length == 0) {
            // "There is an exception though, when current sequence's
            // literals length is 0. In this case, repeated offsets are
            // shifted by one, so Repeated_Offset1 becomes Repeated_Offset2,
            // Repeated_Offset2 becomes Repeated_Offset3, and
            // Repeated_Offset3 becomes Repeated_Offset1 - 1_byte."
            idx++;
        }

        if (idx == 0) {
            offset = offset_hist[0];
        } else {
            // If idx == 3 then literal length was 0 and the offset was 3,
            // as per the exception listed above
            offset = idx < 3 ? offset_hist[idx] : offset_hist[0] - 1;

            // If idx == 1 we don't need to modify offset_hist[2], since
            // we're using the second-most recent code
            if (idx > 1) {
                offset_hist[2] = offset_hist[1];
            }
            offset_hist[1] = offset_hist[0];
            offset_hist[0] = offset;
        }
    } else {
        // When it's not a repeat offset:
        // "if (Offset_Value > 3) offset = Offset_Value - 3;"
        offset = seq.offset - 3;

        // Shift back history
        offset_hist[2] = offset_hist[1];
        offset_hist[1] = offset_hist[0];
        offset_hist[0] = offset;
    }
    return offset;
}

static void execute_match_copy(frame_context_t *const ctx, size_t offset,
                              size_t match_length, size_t total_output,
                              ostream_t *const out) {
    u8 *write_ptr = IO_get_write_ptr(out, match_length);
    if (total_output <= ctx->header.window_size) {
        // In this case offset might go back into the dictionary
        if (offset > total_output + ctx->dict_content_len) {
            // The offset goes beyond even the dictionary
            CORRUPTION();
        }

        if (offset > total_output) {
            // "The rest of the dictionary is its content. The content act
            // as a "past" in front of data to compress or decompress, so it
            // can be referenced in sequence commands."
            const size_t dict_copy =
                MIN(offset - total_output, match_length);
            const size_t dict_offset =
                ctx->dict_content_len - (offset - total_output);

            memcpy(write_ptr, ctx->dict_content + dict_offset, dict_copy);
            write_ptr += dict_copy;
            match_length -= dict_copy;
        }
    } else if (offset > ctx->header.window_size) {
        CORRUPTION();
    }

    // We must copy byte by byte because the match length might be larger
    // than the offset
    // ex: if the output so far was "abc", a command with offset=3 and
    // match_length=6 would produce "abcabcabc" as the new output
    for (size_t j = 0; j < match_length; j++) {
        *write_ptr = *(write_ptr - offset);
        write_ptr++;
    }
}
/******* END SEQUENCE EXECUTION ***********************************************/

/******* OUTPUT SIZE COUNTING *************************************************/
/// Get the decompressed size of an input stream so memory can be allocated in
/// advance.
/// This implementation assumes `src` points to a single ZSTD-compressed frame
size_t ZSTD_get_decompressed_size(const void *src, const size_t src_len) {
    istream_t in = IO_make_istream((const u8 *)src, src_len);

    // get decompressed size from ZSTD frame header
    {
        const u32 magic_number = (u32)IO_read_bits(&in, 32);

        if (magic_number == ZSTD_MAGIC_NUMBER) {
            // ZSTD frame
            frame_header_t header;
            parse_frame_header(&header, &in);

            if (header.frame_content_size == 0 && !header.single_segment_flag) {
                // Content size not provided, we can't tell
                return (size_t)-1;
            }

            return header.frame_content_size;
        } else {
            // not a real frame or skippable frame
            ERROR("ZSTD frame magic number did not match");
        }
    }
}
/******* END OUTPUT SIZE COUNTING *********************************************/

/******* DICTIONARY PARSING ***************************************************/
dictionary_t* create_dictionary(void) {
    dictionary_t* const dict = (dictionary_t *)calloc(1, sizeof(dictionary_t));
    if (!dict) {
        BAD_ALLOC();
    }
    return dict;
}

/// Free an allocated dictionary
void free_dictionary(dictionary_t *const dict) {
    HUF_free_dtable(&dict->literals_dtable);
    FSE_free_dtable(&dict->ll_dtable);
    FSE_free_dtable(&dict->of_dtable);
    FSE_free_dtable(&dict->ml_dtable);

    free(dict->content);

    memset(dict, 0, sizeof(dictionary_t));

    free(dict);
}


#if !defined(ZDEC_NO_DICTIONARY)
#define DICT_SIZE_ERROR() ERROR("Dictionary size cannot be less than 8 bytes")
#define NULL_SRC() ERROR("Tried to create dictionary with pointer to null src");

static void init_dictionary_content(dictionary_t *const dict,
                                    istream_t *const in);

void parse_dictionary(dictionary_t *const dict, const void *src,
                             size_t src_len) {
    const u8 *byte_src = (const u8 *)src;
    memset(dict, 0, sizeof(dictionary_t));
    if (src == NULL) { /* cannot initialize dictionary with null src */
        NULL_SRC();
    }
    if (src_len < 8) {
        DICT_SIZE_ERROR();
    }

    istream_t in = IO_make_istream(byte_src, src_len);

    const u32 magic_number = IO_read_bits(&in, 32);
    if (magic_number != 0xEC30A437) {
        // raw content dict
        IO_rewind_bits(&in, 32);
        init_dictionary_content(dict, &in);
        return;
    }

    dict->dictionary_id = IO_read_bits(&in, 32);

    // "Entropy_Tables : following the same format as the tables in compressed
    // blocks. They are stored in following order : Huffman tables for literals,
    // FSE table for offsets, FSE table for match lengths, and FSE table for
    // literals lengths. It's finally followed by 3 offset values, populating
    // recent offsets (instead of using {1,4,8}), stored in order, 4-bytes
    // little-endian each, for a total of 12 bytes. Each recent offset must have
    // a value < dictionary size."
    decode_huf_table(&dict->literals_dtable, &in);
    decode_seq_table(&dict->of_dtable, &in, seq_offset, seq_fse);
    decode_seq_table(&dict->ml_dtable, &in, seq_match_length, seq_fse);
    decode_seq_table(&dict->ll_dtable, &in, seq_literal_length, seq_fse);

    // Read in the previous offset history
    dict->previous_offsets[0] = IO_read_bits(&in, 32);
    dict->previous_offsets[1] = IO_read_bits(&in, 32);
    dict->previous_offsets[2] = IO_read_bits(&in, 32);

    // Ensure the provided offsets aren't too large
    // "Each recent offset must have a value < dictionary size."
    for (int i = 0; i < 3; i++) {
        if (dict->previous_offsets[i] > src_len) {
            ERROR("Dictionary corrupted");
        }
    }

    // "Content : The rest of the dictionary is its content. The content act as
    // a "past" in front of data to compress or decompress, so it can be
    // referenced in sequence commands."
    init_dictionary_content(dict, &in);
}

static void init_dictionary_content(dictionary_t *const dict,
                                    istream_t *const in) {
    // Copy in the content
    dict->content_size = IO_istream_len(in);
    dict->content = (u8 *)malloc(dict->content_size);
    if (!dict->content) {
        BAD_ALLOC();
    }

    const u8 *const content = IO_get_read_ptr(in, dict->content_size);

    memcpy(dict->content, content, dict->content_size);
}

static void HUF_copy_dtable(HUF_dtable *const dst,
                            const HUF_dtable *const src) {
    if (src->max_bits == 0) {
        memset(dst, 0, sizeof(HUF_dtable));
        return;
    }

    const size_t size = (size_t)1 << src->max_bits;
    dst->max_bits = src->max_bits;

    dst->symbols = (u8 *)malloc(size);
    dst->num_bits = (u8 *)malloc(size);
    if (!dst->symbols || !dst->num_bits) {
        BAD_ALLOC();
    }

    memcpy(dst->symbols, src->symbols, size);
    memcpy(dst->num_bits, src->num_bits, size);
}

static void FSE_copy_dtable(FSE_dtable *const dst, const FSE_dtable *const src) {
    if (src->accuracy_log == 0) {
        memset(dst, 0, sizeof(FSE_dtable));
        return;
    }

    size_t size = (size_t)1 << src->accuracy_log;
    dst->accuracy_log = src->accuracy_log;

    dst->symbols = (u8 *)malloc(size);
    dst->num_bits = (u8 *)malloc(size);
    dst->new_state_base = (u16 *)malloc(size * sizeof(u16));
    if (!dst->symbols || !dst->num_bits || !dst->new_state_base) {
        BAD_ALLOC();
    }

    memcpy(dst->symbols, src->symbols, size);
    memcpy(dst->num_bits, src->num_bits, size);
    memcpy(dst->new_state_base, src->new_state_base, size * sizeof(u16));
}

/// A dictionary acts as initializing values for the frame context before
/// decompression, so we implement it by applying it's predetermined
/// tables and content to the context before beginning decompression
static void frame_context_apply_dict(frame_context_t *const ctx,
                                     const dictionary_t *const dict) {
    // If the content pointer is NULL then it must be an empty dict
    if (!dict || !dict->content)
        return;

    // If the requested dictionary_id is non-zero, the correct dictionary must
    // be present
    if (ctx->header.dictionary_id != 0 &&
        ctx->header.dictionary_id != dict->dictionary_id) {
        ERROR("Wrong dictionary provided");
    }

    // Copy the dict content to the context for references during sequence
    // execution
    ctx->dict_content = dict->content;
    ctx->dict_content_len = dict->content_size;

    // If it's a formatted dict copy the precomputed tables in so they can
    // be used in the table repeat modes
    if (dict->dictionary_id != 0) {
        // Deep copy the entropy tables so they can be freed independently of
        // the dictionary struct
        HUF_copy_dtable(&ctx->literals_dtable, &dict->literals_dtable);
        FSE_copy_dtable(&ctx->ll_dtable, &dict->ll_dtable);
        FSE_copy_dtable(&ctx->of_dtable, &dict->of_dtable);
        FSE_copy_dtable(&ctx->ml_dtable, &dict->ml_dtable);

        // Copy the repeated offsets
        memcpy(ctx->previous_offsets, dict->previous_offsets,
               sizeof(ctx->previous_offsets));
    }
}

#else  // ZDEC_NO_DICTIONARY is defined

static void frame_context_apply_dict(frame_context_t *const ctx,
                                     const dictionary_t *const dict) {
    (void)ctx;
    if (dict && dict->content) ERROR("dictionary not supported");
}

#endif
/******* END DICTIONARY PARSING ***********************************************/

/******* IO STREAM OPERATIONS *************************************************/

/// Reads `num` bits from a bitstream, and updates the internal offset
static inline u64 IO_read_bits(istream_t *const in, const int num_bits) {
    if (num_bits > 64 || num_bits <= 0) {
        ERROR("Attempt to read an invalid number of bits");
    }

    const size_t bytes = (num_bits + in->bit_offset + 7) / 8;
    const size_t full_bytes = (num_bits + in->bit_offset) / 8;
    if (bytes > in->len) {
        INP_SIZE();
    }

    const u64 result = read_bits_LE(in->ptr, num_bits, in->bit_offset);

    in->bit_offset = (num_bits + in->bit_offset) % 8;
    in->ptr += full_bytes;
    in->len -= full_bytes;

    return result;
}

/// If a non-zero number of bits have been read from the current byte, advance
/// the offset to the next byte
static inline void IO_rewind_bits(istream_t *const in, int num_bits) {
    if (num_bits < 0) {
        ERROR("Attempting to rewind stream by a negative number of bits");
    }

    // move the offset back by `num_bits` bits
    const int new_offset = in->bit_offset - num_bits;
    // determine the number of whole bytes we have to rewind, rounding up to an
    // integer number (e.g. if `new_offset == -5`, `bytes == 1`)
    const i64 bytes = -(new_offset - 7) / 8;

    in->ptr -= bytes;
    in->len += bytes;
    // make sure the resulting `bit_offset` is positive, as mod in C does not
    // convert numbers from negative to positive (e.g. -22 % 8 == -6)
    in->bit_offset = ((new_offset % 8) + 8) % 8;
}

/// If the remaining bits in a byte will be unused, advance to the end of the
/// byte
static inline void IO_align_stream(istream_t *const in) {
    if (in->bit_offset != 0) {
        if (in->len == 0) {
            INP_SIZE();
        }
        in->ptr++;
        in->len--;
        in->bit_offset = 0;
    }
}

/// Write the given byte into the output stream
static inline void IO_write_byte(ostream_t *const out, u8 symb) {
    if (out->len == 0) {
        OUT_SIZE();
    }

    out->ptr[0] = symb;
    out->ptr++;
    out->len--;
}

/// Returns the number of bytes left to be read in this stream.  The stream must
/// be byte aligned.
static inline size_t IO_istream_len(const istream_t *const in) {
    return in->len;
}

/// Returns a pointer where `len` bytes can be read, and advances the internal
/// state.  The stream must be byte aligned.
static inline const u8 *IO_get_read_ptr(istream_t *const in, size_t len) {
    if (len > in->len) {
        INP_SIZE();
    }
    if (in->bit_offset != 0) {
        ERROR("Attempting to operate on a non-byte aligned stream");
    }
    const u8 *const ptr = in->ptr;
    in->ptr += len;
    in->len -= len;

    return ptr;
}
/// Returns a pointer to write `len` bytes to, and advances the internal state
static inline u8 *IO_get_write_ptr(ostream_t *const out, size_t len) {
    if (len > out->len) {
        OUT_SIZE();
    }
    u8 *const ptr = out->ptr;
    out->ptr += len;
    out->len -= len;

    return ptr;
}

/// Advance the inner state by `len` bytes
static inline void IO_advance_input(istream_t *const in, size_t len) {
    if (len > in->len) {
         INP_SIZE();
    }
    if (in->bit_offset != 0) {
        ERROR("Attempting to operate on a non-byte aligned stream");
    }

    in->ptr += len;
    in->len -= len;
}

/// Returns an `ostream_t` constructed from the given pointer and length
static inline ostream_t IO_make_ostream(u8 *out, size_t len) {
    ostream_t const b = { out, len };
    return b;
}

/// Returns an `istream_t` constructed from the given pointer and length
static inline istream_t IO_make_istream(const u8 *in, size_t len) {
    istream_t const b = { in, len, 0 };
    return b;
}

/// Returns an `istream_t` with the same base as `in`, and length `len`
/// Then, advance `in` to account for the consumed bytes
/// `in` must be byte aligned
static inline istream_t IO_make_sub_istream(istream_t *const in, size_t len) {
    // Consume `len` bytes of the parent stream
    const u8 *const ptr = IO_get_read_ptr(in, len);

    // Make a substream using the pointer to those `len` bytes
    return IO_make_istream(ptr, len);
}
/******* END IO STREAM OPERATIONS *********************************************/

/******* BITSTREAM OPERATIONS *************************************************/
/// Read `num` bits (up to 64) from `src + offset`, where `offset` is in bits
static inline u64 read_bits_LE(const u8 *src, const int num_bits,
                               const size_t offset) {
    if (num_bits > 64) {
        ERROR("Attempt to read an invalid number of bits");
    }

    // Skip over bytes that aren't in range
    src += offset / 8;
    size_t bit_offset = offset % 8;
    u64 res = 0;

    int shift = 0;
    int left = num_bits;
    while (left > 0) {
        u64 mask = left >= 8 ? 0xff : (((u64)1 << left) - 1);
        // Read the next byte, shift it to account for the offset, and then mask
        // out the top part if we don't need all the bits
        res += (((u64)*src++ >> bit_offset) & mask) << shift;
        shift += 8 - bit_offset;
        left -= 8 - bit_offset;
        bit_offset = 0;
    }

    return res;
}

/// Read bits from the end of a HUF or FSE bitstream.  `offset` is in bits, so
/// it updates `offset` to `offset - bits`, and then reads `bits` bits from
/// `src + offset`.  If the offset becomes negative, the extra bits at the
/// bottom are filled in with `0` bits instead of reading from before `src`.
static inline u64 STREAM_read_bits(const u8 *const src, const int bits,
                                   i64 *const offset) {
    *offset = *offset - bits;
    size_t actual_off = *offset;
    size_t actual_bits = bits;
    // Don't actually read bits from before the start of src, so if `*offset <
    // 0` fix actual_off and actual_bits to reflect the quantity to read
    if (*offset < 0) {
        actual_bits += *offset;
        actual_off = 0;
    }
    u64 res = read_bits_LE(src, actual_bits, actual_off);

    if (*offset < 0) {
        // Fill in the bottom "overflowed" bits with 0's
        res = -*offset >= 64 ? 0 : (res << -*offset);
    }
    return res;
}
/******* END BITSTREAM OPERATIONS *********************************************/

/******* BIT COUNTING OPERATIONS **********************************************/
/// Returns `x`, where `2^x` is the largest power of 2 less than or equal to
/// `num`, or `-1` if `num == 0`.
static inline int highest_set_bit(const u64 num) {
    for (int i = 63; i >= 0; i--) {
        if (((u64)1 << i) <= num) {
            return i;
        }
    }
    return -1;
}
/******* END BIT COUNTING OPERATIONS ******************************************/

/******* HUFFMAN PRIMITIVES ***************************************************/
static inline u8 HUF_decode_symbol(const HUF_dtable *const dtable,
                                   u16 *const state, const u8 *const src,
                                   i64 *const offset) {
    // Look up the symbol and number of bits to read
    const u8 symb = dtable->symbols[*state];
    const u8 bits = dtable->num_bits[*state];
    const u16 rest = STREAM_read_bits(src, bits, offset);
    // Shift `bits` bits out of the state, keeping the low order bits that
    // weren't necessary to determine this symbol.  Then add in the new bits
    // read from the stream.
    *state = ((*state << bits) + rest) & (((u16)1 << dtable->max_bits) - 1);

    return symb;
}

static inline void HUF_init_state(const HUF_dtable *const dtable,
                                  u16 *const state, const u8 *const src,
                                  i64 *const offset) {
    // Read in a full `dtable->max_bits` bits to initialize the state
    const u8 bits = dtable->max_bits;
    *state = STREAM_read_bits(src, bits, offset);
}

static size_t HUF_decompress_1stream(const HUF_dtable *const dtable,
                                     ostream_t *const out,
                                     istream_t *const in) {
    const size_t len = IO_istream_len(in);
    if (len == 0) {
        INP_SIZE();
    }
    const u8 *const src = IO_get_read_ptr(in, len);

    // "Each bitstream must be read backward, that is starting from the end down
    // to the beginning. Therefore it's necessary to know the size of each
    // bitstream.
    //
    // It's also necessary to know exactly which bit is the latest. This is
    // detected by a final bit flag : the highest bit of latest byte is a
    // final-bit-flag. Consequently, a last byte of 0 is not possible. And the
    // final-bit-flag itself is not part of the useful bitstream. Hence, the
    // last byte contains between 0 and 7 useful bits."
    const int padding = 8 - highest_set_bit(src[len - 1]);

    // Offset starts at the end because HUF streams are read backwards
    i64 bit_offset = len * 8 - padding;
    u16 state;

    HUF_init_state(dtable, &state, src, &bit_offset);

    size_t symbols_written = 0;
    while (bit_offset > -dtable->max_bits) {
        // Iterate over the stream, decoding one symbol at a time
        IO_write_byte(out, HUF_decode_symbol(dtable, &state, src, &bit_offset));
        symbols_written++;
    }
    // "The process continues up to reading the required number of symbols per
    // stream. If a bitstream is not entirely and exactly consumed, hence
    // reaching exactly its beginning position with all bits consumed, the
    // decoding process is considered faulty."

    // When all symbols have been decoded, the final state value shouldn't have
    // any data from the stream, so it should have "read" dtable->max_bits from
    // before the start of `src`
    // Therefore `offset`, the edge to start reading new bits at, should be
    // dtable->max_bits before the start of the stream
    if (bit_offset != -dtable->max_bits) {
        CORRUPTION();
    }

    return symbols_written;
}

static size_t HUF_decompress_4stream(const HUF_dtable *const dtable,
                                     ostream_t *const out, istream_t *const in) {
    // "Compressed size is provided explicitly : in the 4-streams variant,
    // bitstreams are preceded by 3 unsigned little-endian 16-bits values. Each
    // value represents the compressed size of one stream, in order. The last
    // stream size is deducted from total compressed size and from previously
    // decoded stream sizes"
    const size_t csize1 = IO_read_bits(in, 16);
    const size_t csize2 = IO_read_bits(in, 16);
    const size_t csize3 = IO_read_bits(in, 16);

    istream_t in1 = IO_make_sub_istream(in, csize1);
    istream_t in2 = IO_make_sub_istream(in, csize2);
    istream_t in3 = IO_make_sub_istream(in, csize3);
    istream_t in4 = IO_make_sub_istream(in, IO_istream_len(in));

    size_t total_output = 0;
    // Decode each stream independently for simplicity
    // If we wanted to we could decode all 4 at the same time for speed,
    // utilizing more execution units
    total_output += HUF_decompress_1stream(dtable, out, &in1);
    total_output += HUF_decompress_1stream(dtable, out, &in2);
    total_output += HUF_decompress_1stream(dtable, out, &in3);
    total_output += HUF_decompress_1stream(dtable, out, &in4);

    return total_output;
}

/// Initializes a Huffman table using canonical Huffman codes
/// For more explanation on canonical Huffman codes see
/// https://www.cs.scranton.edu/~mccloske/courses/cmps340/huff_canonical_dec2015.html
/// Codes within a level are allocated in symbol order (i.e. smaller symbols get
/// earlier codes)
static void HUF_init_dtable(HUF_dtable *const table, const u8 *const bits,
                            const int num_symbs) {
    memset(table, 0, sizeof(HUF_dtable));
    if (num_symbs > HUF_MAX_SYMBS) {
        ERROR("Too many symbols for Huffman");
    }

    u8 max_bits = 0;
    u16 rank_count[HUF_MAX_BITS + 1];
    memset(rank_count, 0, sizeof(rank_count));

    // Count the number of symbols for each number of bits, and determine the
    // depth of the tree
    for (int i = 0; i < num_symbs; i++) {
        if (bits[i] > HUF_MAX_BITS) {
            ERROR("Huffman table depth too large");
        }
        max_bits = MAX(max_bits, bits[i]);
        rank_count[bits[i]]++;
    }

    const size_t table_size = 1 << max_bits;
    table->max_bits = max_bits;
    table->symbols = (u8 *)malloc(table_size);
    table->num_bits = (u8 *)malloc(table_size);

    if (!table->symbols || !table->num_bits) {
        free(table->symbols);
        free(table->num_bits);
        BAD_ALLOC();
    }

    // "Symbols are sorted by Weight. Within same Weight, symbols keep natural
    // order. Symbols with a Weight of zero are removed. Then, starting from
    // lowest weight, prefix codes are distributed in order."

    u32 rank_idx[HUF_MAX_BITS + 1];
    // Initialize the starting codes for each rank (number of bits)
    rank_idx[max_bits] = 0;
    for (int i = max_bits; i >= 1; i--) {
        rank_idx[i - 1] = rank_idx[i] + rank_count[i] * (1 << (max_bits - i));
        // The entire range takes the same number of bits so we can memset it
        memset(&table->num_bits[rank_idx[i]], i, rank_idx[i - 1] - rank_idx[i]);
    }

    if (rank_idx[0] != table_size) {
        CORRUPTION();
    }

    // Allocate codes and fill in the table
    for (int i = 0; i < num_symbs; i++) {
        if (bits[i] != 0) {
            // Allocate a code for this symbol and set its range in the table
            const u16 code = rank_idx[bits[i]];
            // Since the code doesn't care about the bottom `max_bits - bits[i]`
            // bits of state, it gets a range that spans all possible values of
            // the lower bits
            const u16 len = 1 << (max_bits - bits[i]);
            memset(&table->symbols[code], i, len);
            rank_idx[bits[i]] += len;
        }
    }
}

static void HUF_init_dtable_usingweights(HUF_dtable *const table,
                                         const u8 *const weights,
                                         const int num_symbs) {
    // +1 because the last weight is not transmitted in the header
    if (num_symbs + 1 > HUF_MAX_SYMBS) {
        ERROR("Too many symbols for Huffman");
    }

    u8 bits[HUF_MAX_SYMBS];

    u64 weight_sum = 0;
    for (int i = 0; i < num_symbs; i++) {
        // Weights are in the same range as bit count
        if (weights[i] > HUF_MAX_BITS) {
            CORRUPTION();
        }
        weight_sum += weights[i] > 0 ? (u64)1 << (weights[i] - 1) : 0;
    }

    // Find the first power of 2 larger than the sum
    const int max_bits = highest_set_bit(weight_sum) + 1;
    const u64 left_over = ((u64)1 << max_bits) - weight_sum;
    // If the left over isn't a power of 2, the weights are invalid
    if (left_over & (left_over - 1)) {
        CORRUPTION();
    }

    // left_over is used to find the last weight as it's not transmitted
    // by inverting 2^(weight - 1) we can determine the value of last_weight
    const int last_weight = highest_set_bit(left_over) + 1;

    for (int i = 0; i < num_symbs; i++) {
        // "Number_of_Bits = Number_of_Bits ? Max_Number_of_Bits + 1 - Weight : 0"
        bits[i] = weights[i] > 0 ? (max_bits + 1 - weights[i]) : 0;
    }
    bits[num_symbs] =
        max_bits + 1 - last_weight; // Last weight is always non-zero

    HUF_init_dtable(table, bits, num_symbs + 1);
}

static void HUF_free_dtable(HUF_dtable *const dtable) {
    free(dtable->symbols);
    free(dtable->num_bits);
    memset(dtable, 0, sizeof(HUF_dtable));
}
/******* END HUFFMAN PRIMITIVES ***********************************************/

/******* FSE PRIMITIVES *******************************************************/
/// For more description of FSE see
/// https://github.com/Cyan4973/FiniteStateEntropy/

/// Allow a symbol to be decoded without updating state
static inline u8 FSE_peek_symbol(const FSE_dtable *const dtable,
                                 const u16 state) {
    return dtable->symbols[state];
}

/// Consumes bits from the input and uses the current state to determine the
/// next state
static inline void FSE_update_state(const FSE_dtable *const dtable,
                                    u16 *const state, const u8 *const src,
                                    i64 *const offset) {
    const u8 bits = dtable->num_bits[*state];
    const u16 rest = STREAM_read_bits(src, bits, offset);
    *state = dtable->new_state_base[*state] + rest;
}

/// Decodes a single FSE symbol and updates the offset
static inline u8 FSE_decode_symbol(const FSE_dtable *const dtable,
                                   u16 *const state, const u8 *const src,
                                   i64 *const offset) {
    const u8 symb = FSE_peek_symbol(dtable, *state);
    FSE_update_state(dtable, state, src, offset);
    return symb;
}

static inline void FSE_init_state(const FSE_dtable *const dtable,
                                  u16 *const state, const u8 *const src,
                                  i64 *const offset) {
    // Read in a full `accuracy_log` bits to initialize the state
    const u8 bits = dtable->accuracy_log;
    *state = STREAM_read_bits(src, bits, offset);
}

static size_t FSE_decompress_interleaved2(const FSE_dtable *const dtable,
                                          ostream_t *const out,
                                          istream_t *const in) {
    const size_t len = IO_istream_len(in);
    if (len == 0) {
        INP_SIZE();
    }
    const u8 *const src = IO_get_read_ptr(in, len);

    // "Each bitstream must be read backward, that is starting from the end down
    // to the beginning. Therefore it's necessary to know the size of each
    // bitstream.
    //
    // It's also necessary to know exactly which bit is the latest. This is
    // detected by a final bit flag : the highest bit of latest byte is a
    // final-bit-flag. Consequently, a last byte of 0 is not possible. And the
    // final-bit-flag itself is not part of the useful bitstream. Hence, the
    // last byte contains between 0 and 7 useful bits."
    const int padding = 8 - highest_set_bit(src[len - 1]);
    i64 offset = len * 8 - padding;

    u16 state1, state2;
    // "The first state (State1) encodes the even indexed symbols, and the
    // second (State2) encodes the odd indexes. State1 is initialized first, and
    // then State2, and they take turns decoding a single symbol and updating
    // their state."
    FSE_init_state(dtable, &state1, src, &offset);
    FSE_init_state(dtable, &state2, src, &offset);

    // Decode until we overflow the stream
    // Since we decode in reverse order, overflowing the stream is offset going
    // negative
    size_t symbols_written = 0;
    while (1) {
        // "The number of symbols to decode is determined by tracking bitStream
        // overflow condition: If updating state after decoding a symbol would
        // require more bits than remain in the stream, it is assumed the extra
        // bits are 0. Then, the symbols for each of the final states are
        // decoded and the process is complete."
        IO_write_byte(out, FSE_decode_symbol(dtable, &state1, src, &offset));
        symbols_written++;
        if (offset < 0) {
            // There's still a symbol to decode in state2
            IO_write_byte(out, FSE_peek_symbol(dtable, state2));
            symbols_written++;
            break;
        }

        IO_write_byte(out, FSE_decode_symbol(dtable, &state2, src, &offset));
        symbols_written++;
        if (offset < 0) {
            // There's still a symbol to decode in state1
            IO_write_byte(out, FSE_peek_symbol(dtable, state1));
            symbols_written++;
            break;
        }
    }

    return symbols_written;
}

static void FSE_init_dtable(FSE_dtable *const dtable,
                            const i16 *const norm_freqs, const int num_symbs,
                            const int accuracy_log) {
    if (accuracy_log > FSE_MAX_ACCURACY_LOG) {
        ERROR("FSE accuracy too large");
    }
    if (num_symbs > FSE_MAX_SYMBS) {
        ERROR("Too many symbols for FSE");
    }

    dtable->accuracy_log = accuracy_log;

    const size_t size = (size_t)1 << accuracy_log;
    dtable->symbols = (u8 *)malloc(size * sizeof(u8));
    dtable->num_bits = (u8 *)malloc(size * sizeof(u8));
    dtable->new_state_base = (u16 *)malloc(size * sizeof(u16));

    if (!dtable->symbols || !dtable->num_bits || !dtable->new_state_base) {
        BAD_ALLOC();
    }

    // Used to determine how many bits need to be read for each state,
    // and where the destination range should start
    // Needs to be u16 because max value is 2 * max number of symbols,
    // which can be larger than a byte can store
    u16 state_desc[FSE_MAX_SYMBS];

    // "Symbols are scanned in their natural order for "less than 1"
    // probabilities. Symbols with this probability are being attributed a
    // single cell, starting from the end of the table. These symbols define a
    // full state reset, reading Accuracy_Log bits."
    int high_threshold = size;
    for (int s = 0; s < num_symbs; s++) {
        // Scan for low probability symbols to put at the top
        if (norm_freqs[s] == -1) {
            dtable->symbols[--high_threshold] = s;
            state_desc[s] = 1;
        }
    }

    // "All remaining symbols are sorted in their natural order. Starting from
    // symbol 0 and table position 0, each symbol gets attributed as many cells
    // as its probability. Cell allocation is spread, not linear."
    // Place the rest in the table
    const u16 step = (size >> 1) + (size >> 3) + 3;
    const u16 mask = size - 1;
    u16 pos = 0;
    for (int s = 0; s < num_symbs; s++) {
        if (norm_freqs[s] <= 0) {
            continue;
        }

        state_desc[s] = norm_freqs[s];

        for (int i = 0; i < norm_freqs[s]; i++) {
            // Give `norm_freqs[s]` states to symbol s
            dtable->symbols[pos] = s;
            // "A position is skipped if already occupied, typically by a "less
            // than 1" probability symbol."
            do {
                pos = (pos + step) & mask;
            } while (pos >=
                     high_threshold);
            // Note: no other collision checking is necessary as `step` is
            // coprime to `size`, so the cycle will visit each position exactly
            // once
        }
    }
    if (pos != 0) {
        CORRUPTION();
    }

    // Now we can fill baseline and num bits
    for (size_t i = 0; i < size; i++) {
        u8 symbol = dtable->symbols[i];
        u16 next_state_desc = state_desc[symbol]++;
        // Fills in the table appropriately, next_state_desc increases by symbol
        // over time, decreasing number of bits
        dtable->num_bits[i] = (u8)(accuracy_log - highest_set_bit(next_state_desc));
        // Baseline increases until the bit threshold is passed, at which point
        // it resets to 0
        dtable->new_state_base[i] =
            ((u16)next_state_desc << dtable->num_bits[i]) - size;
    }
}

/// Decode an FSE header as defined in the Zstandard format specification and
/// use the decoded frequencies to initialize a decoding table.
static void FSE_decode_header(FSE_dtable *const dtable, istream_t *const in,
                                const int max_accuracy_log) {
    // "An FSE distribution table describes the probabilities of all symbols
    // from 0 to the last present one (included) on a normalized scale of 1 <<
    // Accuracy_Log .
    //
    // It's a bitstream which is read forward, in little-endian fashion. It's
    // not necessary to know its exact size, since it will be discovered and
    // reported by the decoding process.
    if (max_accuracy_log > FSE_MAX_ACCURACY_LOG) {
        ERROR("FSE accuracy too large");
    }

    // The bitstream starts by reporting on which scale it operates.
    // Accuracy_Log = low4bits + 5. Note that maximum Accuracy_Log for literal
    // and match lengths is 9, and for offsets is 8. Higher values are
    // considered errors."
    const int accuracy_log = 5 + IO_read_bits(in, 4);
    if (accuracy_log > max_accuracy_log) {
        ERROR("FSE accuracy too large");
    }

    // "Then follows each symbol value, from 0 to last present one. The number
    // of bits used by each field is variable. It depends on :
    //
    // Remaining probabilities + 1 : example : Presuming an Accuracy_Log of 8,
    // and presuming 100 probabilities points have already been distributed, the
    // decoder may read any value from 0 to 255 - 100 + 1 == 156 (inclusive).
    // Therefore, it must read log2sup(156) == 8 bits.
    //
    // Value decoded : small values use 1 less bit : example : Presuming values
    // from 0 to 156 (inclusive) are possible, 255-156 = 99 values are remaining
    // in an 8-bits field. They are used this way : first 99 values (hence from
    // 0 to 98) use only 7 bits, values from 99 to 156 use 8 bits. "

    i32 remaining = 1 << accuracy_log;
    i16 frequencies[FSE_MAX_SYMBS];

    int symb = 0;
    while (remaining > 0 && symb < FSE_MAX_SYMBS) {
        // Log of the number of possible values we could read
        int bits = highest_set_bit(remaining + 1) + 1;

        u16 val = IO_read_bits(in, bits);

        // Try to mask out the lower bits to see if it qualifies for the "small
        // value" threshold
        const u16 lower_mask = ((u16)1 << (bits - 1)) - 1;
        const u16 threshold = ((u16)1 << bits) - 1 - (remaining + 1);

        if ((val & lower_mask) < threshold) {
            IO_rewind_bits(in, 1);
            val = val & lower_mask;
        } else if (val > lower_mask) {
            val = val - threshold;
        }

        // "Probability is obtained from Value decoded by following formula :
        // Proba = value - 1"
        const i16 proba = (i16)val - 1;

        // "It means value 0 becomes negative probability -1. -1 is a special
        // probability, which means "less than 1". Its effect on distribution
        // table is described in next paragraph. For the purpose of calculating
        // cumulated distribution, it counts as one."
        remaining -= proba < 0 ? -proba : proba;

        frequencies[symb] = proba;
        symb++;

        // "When a symbol has a probability of zero, it is followed by a 2-bits
        // repeat flag. This repeat flag tells how many probabilities of zeroes
        // follow the current one. It provides a number ranging from 0 to 3. If
        // it is a 3, another 2-bits repeat flag follows, and so on."
        if (proba == 0) {
            // Read the next two bits to see how many more 0s
            int repeat = IO_read_bits(in, 2);

            while (1) {
                for (int i = 0; i < repeat && symb < FSE_MAX_SYMBS; i++) {
                    frequencies[symb++] = 0;
                }
                if (repeat == 3) {
                    repeat = IO_read_bits(in, 2);
                } else {
                    break;
                }
            }
        }
    }
    IO_align_stream(in);

    // "When last symbol reaches cumulated total of 1 << Accuracy_Log, decoding
    // is complete. If the last symbol makes cumulated total go above 1 <<
    // Accuracy_Log, distribution is considered corrupted."
    if (remaining != 0 || symb >= FSE_MAX_SYMBS) {
        CORRUPTION();
    }

    // Initialize the decoding table using the determined weights
    FSE_init_dtable(dtable, frequencies, symb, accuracy_log);
}

static void FSE_init_dtable_rle(FSE_dtable *const dtable, const u8 symb) {
    dtable->symbols = (u8 *)malloc(sizeof(u8));
    dtable->num_bits = (u8 *)malloc(sizeof(u8));
    dtable->new_state_base = (u16 *)malloc(sizeof(u16));

    if (!dtable->symbols || !dtable->num_bits || !dtable->new_state_base) {
        BAD_ALLOC();
    }

    // This setup will always have a state of 0, always return symbol `symb`,
    // and never consume any bits
    dtable->symbols[0] = symb;
    dtable->num_bits[0] = 0;
    dtable->new_state_base[0] = 0;
    dtable->accuracy_log = 0;
}

static void FSE_free_dtable(FSE_dtable *const dtable) {
    free(dtable->symbols);
    free(dtable->num_bits);
    free(dtable->new_state_base);
    memset(dtable, 0, sizeof(FSE_dtable));
}
/******* END FSE PRIMITIVES ***************************************************/
#if defined(_MSC_VER)
#pragma warning(pop)

#endif

/* Produced by texiweb from libavl.w. */

/* libavl - library for manipulation of binary trees.
   Copyright (C) 1998, 1999, 2000, 2001, 2002, 2004 Free Software
   Foundation, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Produced by texiweb from libavl.w. */

/* libavl - library for manipulation of binary trees.
   Copyright (C) 1998, 1999, 2000, 2001, 2002, 2004 Free Software
   Foundation, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA.
*/

#ifndef AVL_H
#define AVL_H 1

#include <stddef.h>

/* Function types. */
typedef int avl_comparison_func (const void *avl_a, const void *avl_b,
                                 void *avl_param);
typedef void avl_item_func (void *avl_item, void *avl_param);
typedef void *avl_copy_func (void *avl_item, void *avl_param);

#ifndef LIBAVL_ALLOCATOR
#define LIBAVL_ALLOCATOR
/* Memory allocator. */
struct libavl_allocator
  {
    void *(*libavl_malloc) (struct libavl_allocator *, size_t libavl_size);
    void (*libavl_free) (struct libavl_allocator *, void *libavl_block);
  };
#endif

/* Default memory allocator. */
extern struct libavl_allocator avl_allocator_default;
void *avl_malloc (struct libavl_allocator *, size_t);
void avl_free (struct libavl_allocator *, void *);

/* Maximum AVL tree height. */
#ifndef AVL_MAX_HEIGHT
#define AVL_MAX_HEIGHT 92
#endif

/* Tree data structure. */
struct avl_table
  {
    struct avl_node *avl_root;          /* Tree's root. */
    avl_comparison_func *avl_compare;   /* Comparison function. */
    void *avl_param;                    /* Extra argument to |avl_compare|. */
    struct libavl_allocator *avl_alloc; /* Memory allocator. */
    size_t avl_count;                   /* Number of items in tree. */
    unsigned long avl_generation;       /* Generation number. */
  };

/* An AVL tree node. */
struct avl_node
  {
    struct avl_node *avl_link[2];  /* Subtrees. */
    void *avl_data;                /* Pointer to data. */
    signed char avl_balance;       /* Balance factor. */
  };

/* AVL traverser structure. */
struct avl_traverser
  {
    struct avl_table *avl_table;        /* Tree being traversed. */
    struct avl_node *avl_node;          /* Current node in tree. */
    struct avl_node *avl_stack[AVL_MAX_HEIGHT];
                                        /* All the nodes above |avl_node|. */
    size_t avl_height;                  /* Number of nodes in |avl_parent|. */
    unsigned long avl_generation;       /* Generation number. */
  };

/* Table functions. */
struct avl_table *avl_create (avl_comparison_func *, void *,
                              struct libavl_allocator *);
struct avl_table *avl_copy (const struct avl_table *, avl_copy_func *,
                            avl_item_func *, struct libavl_allocator *);
void avl_destroy (struct avl_table *, avl_item_func *);
void **avl_probe (struct avl_table *, void *);
void *avl_insert (struct avl_table *, void *);
void *avl_replace (struct avl_table *, void *);
void *avl_delete (struct avl_table *, const void *);
void *avl_find (const struct avl_table *, const void *);
void avl_assert_insert (struct avl_table *, void *);
void *avl_assert_delete (struct avl_table *, void *);

#define avl_count(table) ((size_t) (table)->avl_count)

/* Table traverser functions. */
void avl_t_init (struct avl_traverser *, struct avl_table *);
void *avl_t_first (struct avl_traverser *, struct avl_table *);
void *avl_t_last (struct avl_traverser *, struct avl_table *);
void *avl_t_find (struct avl_traverser *, struct avl_table *, void *);
void *avl_t_insert (struct avl_traverser *, struct avl_table *, void *);
void *avl_t_copy (struct avl_traverser *, const struct avl_traverser *);
void *avl_t_next (struct avl_traverser *);
void *avl_t_prev (struct avl_traverser *);
void *avl_t_cur (struct avl_traverser *);
void *avl_t_replace (struct avl_traverser *, void *);

#endif /* avl.h */

/* Creates and returns a new table
   with comparison function |compare| using parameter |param|
   and memory allocator |allocator|.
   Returns |NULL| if memory allocation failed. */
struct avl_table *
avl_create (avl_comparison_func *compare, void *param,
            struct libavl_allocator *allocator)
{
  struct avl_table *tree;

  assert (compare != NULL);

  if (allocator == NULL)
    allocator = &avl_allocator_default;

  tree = (struct avl_table *)allocator->libavl_malloc (allocator, sizeof *tree);
  if (tree == NULL)
    return NULL;

  tree->avl_root = NULL;
  tree->avl_compare = compare;
  tree->avl_param = param;
  tree->avl_alloc = allocator;
  tree->avl_count = 0;
  tree->avl_generation = 0;

  return tree;
}

/* Search |tree| for an item matching |item|, and return it if found.
   Otherwise return |NULL|. */
void *
avl_find (const struct avl_table *tree, const void *item)
{
  const struct avl_node *p;

  assert (tree != NULL && item != NULL);
  for (p = tree->avl_root; p != NULL; )
    {
      int cmp = tree->avl_compare (item, p->avl_data, tree->avl_param);

      if (cmp < 0)
        p = p->avl_link[0];
      else if (cmp > 0)
        p = p->avl_link[1];
      else /* |cmp == 0| */
        return p->avl_data;
    }

  return NULL;
}

/* Inserts |item| into |tree| and returns a pointer to |item|'s address.
   If a duplicate item is found in the tree,
   returns a pointer to the duplicate without inserting |item|.
   Returns |NULL| in case of memory allocation failure. */
void **
avl_probe (struct avl_table *tree, void *item)
{
  struct avl_node *y, *z; /* Top node to update balance factor, and parent. */
  struct avl_node *p, *q; /* Iterator, and parent. */
  struct avl_node *n;     /* Newly inserted node. */
  struct avl_node *w;     /* New root of rebalanced subtree. */
  int dir;                /* Direction to descend. */

  unsigned char da[AVL_MAX_HEIGHT]; /* Cached comparison results. */
  int k = 0;              /* Number of cached results. */

  assert (tree != NULL && item != NULL);

  z = (struct avl_node *) &tree->avl_root;
  y = tree->avl_root;
  dir = 0;
  for (q = z, p = y; p != NULL; q = p, p = p->avl_link[dir])
    {
      int cmp = tree->avl_compare (item, p->avl_data, tree->avl_param);
      if (cmp == 0)
        return &p->avl_data;

      if (p->avl_balance != 0)
        z = q, y = p, k = 0;
      dir = cmp > 0;
      da[k++] = (unsigned char)dir;
    }

  n = q->avl_link[dir] =(struct avl_node *)
    tree->avl_alloc->libavl_malloc (tree->avl_alloc, sizeof *n);
  if (n == NULL)
    return NULL;

  tree->avl_count++;
  n->avl_data = item;
  n->avl_link[0] = n->avl_link[1] = NULL;
  n->avl_balance = 0;
  if (y == NULL)
    return &n->avl_data;

  for (p = y, k = 0; p != n; p = p->avl_link[da[k]], k++)
    if (da[k] == 0)
      p->avl_balance--;
    else
      p->avl_balance++;

  if (y->avl_balance == -2)
    {
      struct avl_node *x = y->avl_link[0];
      if (x->avl_balance == -1)
        {
          w = x;
          y->avl_link[0] = x->avl_link[1];
          x->avl_link[1] = y;
          x->avl_balance = y->avl_balance = 0;
        }
      else
        {
          assert (x->avl_balance == +1);
          w = x->avl_link[1];
          x->avl_link[1] = w->avl_link[0];
          w->avl_link[0] = x;
          y->avl_link[0] = w->avl_link[1];
          w->avl_link[1] = y;
          if (w->avl_balance == -1)
            x->avl_balance = 0, y->avl_balance = +1;
          else if (w->avl_balance == 0)
            x->avl_balance = y->avl_balance = 0;
          else /* |w->avl_balance == +1| */
            x->avl_balance = -1, y->avl_balance = 0;
          w->avl_balance = 0;
        }
    }
  else if (y->avl_balance == +2)
    {
      struct avl_node *x = y->avl_link[1];
      if (x->avl_balance == +1)
        {
          w = x;
          y->avl_link[1] = x->avl_link[0];
          x->avl_link[0] = y;
          x->avl_balance = y->avl_balance = 0;
        }
      else
        {
          assert (x->avl_balance == -1);
          w = x->avl_link[0];
          x->avl_link[0] = w->avl_link[1];
          w->avl_link[1] = x;
          y->avl_link[1] = w->avl_link[0];
          w->avl_link[0] = y;
          if (w->avl_balance == +1)
            x->avl_balance = 0, y->avl_balance = -1;
          else if (w->avl_balance == 0)
            x->avl_balance = y->avl_balance = 0;
          else /* |w->avl_balance == -1| */
            x->avl_balance = +1, y->avl_balance = 0;
          w->avl_balance = 0;
        }
    }
  else
    return &n->avl_data;
  z->avl_link[y != z->avl_link[0]] = w;

  tree->avl_generation++;
  return &n->avl_data;
}

/* Inserts |item| into |table|.
   Returns |NULL| if |item| was successfully inserted
   or if a memory allocation error occurred.
   Otherwise, returns the duplicate item. */
void *
avl_insert (struct avl_table *table, void *item)
{
  void **p = avl_probe (table, item);
  return p == NULL || *p == item ? NULL : *p;
}

/* Inserts |item| into |table|, replacing any duplicate item.
   Returns |NULL| if |item| was inserted without replacing a duplicate,
   or if a memory allocation error occurred.
   Otherwise, returns the item that was replaced. */
void *
avl_replace (struct avl_table *table, void *item)
{
  void **p = avl_probe (table, item);
  if (p == NULL || *p == item)
    return NULL;
  else
    {
      void *r = *p;
      *p = item;
      return r;
    }
}

/* Deletes from |tree| and returns an item matching |item|.
   Returns a null pointer if no matching item found. */
void *
avl_delete (struct avl_table *tree, const void *item)
{
  /* Stack of nodes. */
  struct avl_node *pa[AVL_MAX_HEIGHT]; /* Nodes. */
  unsigned char da[AVL_MAX_HEIGHT];    /* |avl_link[]| indexes. */
  int k;                               /* Stack pointer. */

  struct avl_node *p;   /* Traverses tree to find node to delete. */
  int cmp;              /* Result of comparison between |item| and |p|. */

  assert (tree != NULL && item != NULL);

  k = 0;
  p = (struct avl_node *) &tree->avl_root;
  for (cmp = -1; cmp != 0;
       cmp = tree->avl_compare (item, p->avl_data, tree->avl_param))
    {
      int dir = cmp > 0;

      pa[k] = p;
      da[k++] = (unsigned char)dir;

      p = p->avl_link[dir];
      if (p == NULL)
        return NULL;
    }
  item = p->avl_data;

  if (p->avl_link[1] == NULL)
    pa[k - 1]->avl_link[da[k - 1]] = p->avl_link[0];
  else
    {
      struct avl_node *r = p->avl_link[1];
      if (r->avl_link[0] == NULL)
        {
          r->avl_link[0] = p->avl_link[0];
          r->avl_balance = p->avl_balance;
          pa[k - 1]->avl_link[da[k - 1]] = r;
          da[k] = 1;
          pa[k++] = r;
        }
      else
        {
          struct avl_node *s;
          int j = k++;

          for (;;)
            {
              da[k] = 0;
              pa[k++] = r;
              s = r->avl_link[0];
              if (s->avl_link[0] == NULL)
                break;

              r = s;
            }

          s->avl_link[0] = p->avl_link[0];
          r->avl_link[0] = s->avl_link[1];
          s->avl_link[1] = p->avl_link[1];
          s->avl_balance = p->avl_balance;

          pa[j - 1]->avl_link[da[j - 1]] = s;
          da[j] = 1;
          pa[j] = s;
        }
    }

  tree->avl_alloc->libavl_free (tree->avl_alloc, p);

  assert (k > 0);
  while (--k > 0)
    {
      struct avl_node *y = pa[k];

      if (da[k] == 0)
        {
          y->avl_balance++;
          if (y->avl_balance == +1)
            break;
          else if (y->avl_balance == +2)
            {
              struct avl_node *x = y->avl_link[1];
              if (x->avl_balance == -1)
                {
                  struct avl_node *w;
                  assert (x->avl_balance == -1);
                  w = x->avl_link[0];
                  x->avl_link[0] = w->avl_link[1];
                  w->avl_link[1] = x;
                  y->avl_link[1] = w->avl_link[0];
                  w->avl_link[0] = y;
                  if (w->avl_balance == +1)
                    x->avl_balance = 0, y->avl_balance = -1;
                  else if (w->avl_balance == 0)
                    x->avl_balance = y->avl_balance = 0;
                  else /* |w->avl_balance == -1| */
                    x->avl_balance = +1, y->avl_balance = 0;
                  w->avl_balance = 0;
                  pa[k - 1]->avl_link[da[k - 1]] = w;
                }
              else
                {
                  y->avl_link[1] = x->avl_link[0];
                  x->avl_link[0] = y;
                  pa[k - 1]->avl_link[da[k - 1]] = x;
                  if (x->avl_balance == 0)
                    {
                      x->avl_balance = -1;
                      y->avl_balance = +1;
                      break;
                    }
                  else
                    x->avl_balance = y->avl_balance = 0;
                }
            }
        }
      else
        {
          y->avl_balance--;
          if (y->avl_balance == -1)
            break;
          else if (y->avl_balance == -2)
            {
              struct avl_node *x = y->avl_link[0];
              if (x->avl_balance == +1)
                {
                  struct avl_node *w;
                  assert (x->avl_balance == +1);
                  w = x->avl_link[1];
                  x->avl_link[1] = w->avl_link[0];
                  w->avl_link[0] = x;
                  y->avl_link[0] = w->avl_link[1];
                  w->avl_link[1] = y;
                  if (w->avl_balance == -1)
                    x->avl_balance = 0, y->avl_balance = +1;
                  else if (w->avl_balance == 0)
                    x->avl_balance = y->avl_balance = 0;
                  else /* |w->avl_balance == +1| */
                    x->avl_balance = -1, y->avl_balance = 0;
                  w->avl_balance = 0;
                  pa[k - 1]->avl_link[da[k - 1]] = w;
                }
              else
                {
                  y->avl_link[0] = x->avl_link[1];
                  x->avl_link[1] = y;
                  pa[k - 1]->avl_link[da[k - 1]] = x;
                  if (x->avl_balance == 0)
                    {
                      x->avl_balance = +1;
                      y->avl_balance = -1;
                      break;
                    }
                  else
                    x->avl_balance = y->avl_balance = 0;
                }
            }
        }
    }

  tree->avl_count--;
  tree->avl_generation++;
  return (void *) item;
}

/* Refreshes the stack of parent pointers in |trav|
   and updates its generation number. */
static void
trav_refresh (struct avl_traverser *trav)
{
  assert (trav != NULL);

  trav->avl_generation = trav->avl_table->avl_generation;

  if (trav->avl_node != NULL)
    {
      avl_comparison_func *cmp = trav->avl_table->avl_compare;
      void *param = trav->avl_table->avl_param;
      struct avl_node *node = trav->avl_node;
      struct avl_node *i;

      trav->avl_height = 0;
      for (i = trav->avl_table->avl_root; i != node; )
        {
          assert (trav->avl_height < AVL_MAX_HEIGHT);
          assert (i != NULL);

          trav->avl_stack[trav->avl_height++] = i;
          i = i->avl_link[cmp (node->avl_data, i->avl_data, param) > 0];
        }
    }
}

/* Initializes |trav| for use with |tree|
   and selects the null node. */
void
avl_t_init (struct avl_traverser *trav, struct avl_table *tree)
{
  trav->avl_table = tree;
  trav->avl_node = NULL;
  trav->avl_height = 0;
  trav->avl_generation = tree->avl_generation;
}

/* Initializes |trav| for |tree|
   and selects and returns a pointer to its least-valued item.
   Returns |NULL| if |tree| contains no nodes. */
void *
avl_t_first (struct avl_traverser *trav, struct avl_table *tree)
{
  struct avl_node *x;

  assert (tree != NULL && trav != NULL);

  trav->avl_table = tree;
  trav->avl_height = 0;
  trav->avl_generation = tree->avl_generation;

  x = tree->avl_root;
  if (x != NULL)
    while (x->avl_link[0] != NULL)
      {
        assert (trav->avl_height < AVL_MAX_HEIGHT);
        trav->avl_stack[trav->avl_height++] = x;
        x = x->avl_link[0];
      }
  trav->avl_node = x;

  return x != NULL ? x->avl_data : NULL;
}

/* Initializes |trav| for |tree|
   and selects and returns a pointer to its greatest-valued item.
   Returns |NULL| if |tree| contains no nodes. */
void *
avl_t_last (struct avl_traverser *trav, struct avl_table *tree)
{
  struct avl_node *x;

  assert (tree != NULL && trav != NULL);

  trav->avl_table = tree;
  trav->avl_height = 0;
  trav->avl_generation = tree->avl_generation;

  x = tree->avl_root;
  if (x != NULL)
    while (x->avl_link[1] != NULL)
      {
        assert (trav->avl_height < AVL_MAX_HEIGHT);
        trav->avl_stack[trav->avl_height++] = x;
        x = x->avl_link[1];
      }
  trav->avl_node = x;

  return x != NULL ? x->avl_data : NULL;
}

/* Searches for |item| in |tree|.
   If found, initializes |trav| to the item found and returns the item
   as well.
   If there is no matching item, initializes |trav| to the null item
   and returns |NULL|. */
void *
avl_t_find (struct avl_traverser *trav, struct avl_table *tree, void *item)
{
  struct avl_node *p, *q;

  assert (trav != NULL && tree != NULL && item != NULL);
  trav->avl_table = tree;
  trav->avl_height = 0;
  trav->avl_generation = tree->avl_generation;
  for (p = tree->avl_root; p != NULL; p = q)
    {
      int cmp = tree->avl_compare (item, p->avl_data, tree->avl_param);

      if (cmp < 0)
        q = p->avl_link[0];
      else if (cmp > 0)
        q = p->avl_link[1];
      else /* |cmp == 0| */
        {
          trav->avl_node = p;
          return p->avl_data;
        }

      assert (trav->avl_height < AVL_MAX_HEIGHT);
      trav->avl_stack[trav->avl_height++] = p;
    }

  trav->avl_height = 0;
  trav->avl_node = NULL;
  return NULL;
}

/* Attempts to insert |item| into |tree|.
   If |item| is inserted successfully, it is returned and |trav| is
   initialized to its location.
   If a duplicate is found, it is returned and |trav| is initialized to
   its location.  No replacement of the item occurs.
   If a memory allocation failure occurs, |NULL| is returned and |trav|
   is initialized to the null item. */
void *
avl_t_insert (struct avl_traverser *trav, struct avl_table *tree, void *item)
{
  void **p;

  assert (trav != NULL && tree != NULL && item != NULL);

  p = avl_probe (tree, item);
  if (p != NULL)
    {
      trav->avl_table = tree;
      trav->avl_node =
        ((struct avl_node *)
         ((char *) p - offsetof (struct avl_node, avl_data)));
      trav->avl_generation = tree->avl_generation - 1;
      return *p;
    }
  else
    {
      avl_t_init (trav, tree);
      return NULL;
    }
}

/* Initializes |trav| to have the same current node as |src|. */
void *
avl_t_copy (struct avl_traverser *trav, const struct avl_traverser *src)
{
  assert (trav != NULL && src != NULL);

  if (trav != src)
    {
      trav->avl_table = src->avl_table;
      trav->avl_node = src->avl_node;
      trav->avl_generation = src->avl_generation;
      if (trav->avl_generation == trav->avl_table->avl_generation)
        {
          trav->avl_height = src->avl_height;
          memcpy (trav->avl_stack, (const void *) src->avl_stack,
                  sizeof *trav->avl_stack * trav->avl_height);
        }
    }

  return trav->avl_node != NULL ? trav->avl_node->avl_data : NULL;
}

/* Returns the next data item in inorder
   within the tree being traversed with |trav|,
   or if there are no more data items returns |NULL|. */
void *
avl_t_next (struct avl_traverser *trav)
{
  struct avl_node *x;

  assert (trav != NULL);

  if (trav->avl_generation != trav->avl_table->avl_generation)
    trav_refresh (trav);

  x = trav->avl_node;
  if (x == NULL)
    {
      return avl_t_first (trav, trav->avl_table);
    }
  else if (x->avl_link[1] != NULL)
    {
      assert (trav->avl_height < AVL_MAX_HEIGHT);
      trav->avl_stack[trav->avl_height++] = x;
      x = x->avl_link[1];

      while (x->avl_link[0] != NULL)
        {
          assert (trav->avl_height < AVL_MAX_HEIGHT);
          trav->avl_stack[trav->avl_height++] = x;
          x = x->avl_link[0];
        }
    }
  else
    {
      struct avl_node *y;

      do
        {
          if (trav->avl_height == 0)
            {
              trav->avl_node = NULL;
              return NULL;
            }

          y = x;
          x = trav->avl_stack[--trav->avl_height];
        }
      while (y == x->avl_link[1]);
    }
  trav->avl_node = x;

  return x->avl_data;
}

/* Returns the previous data item in inorder
   within the tree being traversed with |trav|,
   or if there are no more data items returns |NULL|. */
void *
avl_t_prev (struct avl_traverser *trav)
{
  struct avl_node *x;

  assert (trav != NULL);

  if (trav->avl_generation != trav->avl_table->avl_generation)
    trav_refresh (trav);

  x = trav->avl_node;
  if (x == NULL)
    {
      return avl_t_last (trav, trav->avl_table);
    }
  else if (x->avl_link[0] != NULL)
    {
      assert (trav->avl_height < AVL_MAX_HEIGHT);
      trav->avl_stack[trav->avl_height++] = x;
      x = x->avl_link[0];

      while (x->avl_link[1] != NULL)
        {
          assert (trav->avl_height < AVL_MAX_HEIGHT);
          trav->avl_stack[trav->avl_height++] = x;
          x = x->avl_link[1];
        }
    }
  else
    {
      struct avl_node *y;

      do
        {
          if (trav->avl_height == 0)
            {
              trav->avl_node = NULL;
              return NULL;
            }

          y = x;
          x = trav->avl_stack[--trav->avl_height];
        }
      while (y == x->avl_link[0]);
    }
  trav->avl_node = x;

  return x->avl_data;
}

/* Returns |trav|'s current item. */
void *
avl_t_cur (struct avl_traverser *trav)
{
  assert (trav != NULL);

  return trav->avl_node != NULL ? trav->avl_node->avl_data : NULL;
}

/* Replaces the current item in |trav| by |new| and returns the item replaced.
   |trav| must not have the null item selected.
   The new item must not upset the ordering of the tree. */
void *
avl_t_replace (struct avl_traverser *trav, void *c_new)
{
  void *old;

  assert (trav != NULL && trav->avl_node != NULL && c_new != NULL);
  old = trav->avl_node->avl_data;
  trav->avl_node->avl_data = c_new;
  return old;
}

/* Destroys |new| with |avl_destroy (new, destroy)|,
   first setting right links of nodes in |stack| within |new|
   to null pointers to avoid touching uninitialized data. */
static void
copy_error_recovery (struct avl_node **stack, int height,
                     struct avl_table *c_new, avl_item_func *destroy)
{
  assert (stack != NULL && height >= 0 && c_new != NULL);

  for (; height > 2; height -= 2)
    stack[height - 1]->avl_link[1] = NULL;
  avl_destroy (c_new, destroy);
}

/* Copies |org| to a newly created tree, which is returned.
   If |copy != NULL|, each data item in |org| is first passed to |copy|,
   and the return values are inserted into the tree,
   with |NULL| return values taken as indications of failure.
   On failure, destroys the partially created new tree,
   applying |destroy|, if non-null, to each item in the new tree so far,
   and returns |NULL|.
   If |allocator != NULL|, it is used for allocation in the new tree.
   Otherwise, the same allocator used for |org| is used. */
struct avl_table *
avl_copy (const struct avl_table *org, avl_copy_func *copy,
          avl_item_func *destroy, struct libavl_allocator *allocator)
{
  struct avl_node *stack[2 * (AVL_MAX_HEIGHT + 1)];
  int height = 0;

  struct avl_table *c_new;
  const struct avl_node *x;
  struct avl_node *y;

  assert (org != NULL);
  c_new = avl_create (org->avl_compare, org->avl_param,
                    allocator != NULL ? allocator : org->avl_alloc);
  if (c_new == NULL)
    return NULL;
  c_new->avl_count = org->avl_count;
  if (c_new->avl_count == 0)
    return c_new;

  x = (const struct avl_node *) &org->avl_root;
  y = (struct avl_node *) &c_new->avl_root;
  for (;;)
    {
      while (x->avl_link[0] != NULL)
        {
          assert (height < 2 * (AVL_MAX_HEIGHT + 1));

          y->avl_link[0] =(struct avl_node *)
            c_new->avl_alloc->libavl_malloc (c_new->avl_alloc,
                                           sizeof *y->avl_link[0]);
          if (y->avl_link[0] == NULL)
            {
              if (y != (struct avl_node *) &c_new->avl_root)
                {
                  y->avl_data = NULL;
                  y->avl_link[1] = NULL;
                }

              copy_error_recovery (stack, height, c_new, destroy);
              return NULL;
            }

          stack[height++] = (struct avl_node *) x;
          stack[height++] = y;
          x = x->avl_link[0];
          y = y->avl_link[0];
        }
      y->avl_link[0] = NULL;

      for (;;)
        {
          y->avl_balance = x->avl_balance;
          if (copy == NULL)
            y->avl_data = x->avl_data;
          else
            {
              y->avl_data = copy (x->avl_data, org->avl_param);
              if (y->avl_data == NULL)
                {
                  y->avl_link[1] = NULL;
                  copy_error_recovery (stack, height, c_new, destroy);
                  return NULL;
                }
            }

          if (x->avl_link[1] != NULL)
            {
              y->avl_link[1] =(struct avl_node *)
                c_new->avl_alloc->libavl_malloc (c_new->avl_alloc,
                                               sizeof *y->avl_link[1]);
              if (y->avl_link[1] == NULL)
                {
                  copy_error_recovery (stack, height, c_new, destroy);
                  return NULL;
                }

              x = x->avl_link[1];
              y = y->avl_link[1];
              break;
            }
          else
            y->avl_link[1] = NULL;

          if (height <= 2)
            return c_new;

          y = stack[--height];
          x = stack[--height];
        }
    }
}

/* Frees storage allocated for |tree|.
   If |destroy != NULL|, applies it to each data item in inorder. */
void
avl_destroy (struct avl_table *tree, avl_item_func *destroy)
{
  struct avl_node *p, *q;

  assert (tree != NULL);

  for (p = tree->avl_root; p != NULL; p = q)
    if (p->avl_link[0] == NULL)
      {
        q = p->avl_link[1];
        if (destroy != NULL && p->avl_data != NULL)
          destroy (p->avl_data, tree->avl_param);
        tree->avl_alloc->libavl_free (tree->avl_alloc, p);
      }
    else
      {
        q = p->avl_link[0];
        p->avl_link[0] = q->avl_link[1];
        q->avl_link[1] = p;
      }

  tree->avl_alloc->libavl_free (tree->avl_alloc, tree);
}

/* Allocates |size| bytes of space using |malloc()|.
   Returns a null pointer if allocation fails. */
void *
avl_malloc (struct libavl_allocator *allocator, size_t size)
{
  assert (allocator != NULL && size > 0);
  return malloc (size);
}

/* Frees |block|. */
void
avl_free (struct libavl_allocator *allocator, void *block)
{
  assert (allocator != NULL && block != NULL);
  free (block);
}

/* Default memory allocator that uses |malloc()| and |free()|. */
struct libavl_allocator avl_allocator_default =
  {
    avl_malloc,
    avl_free
  };

#undef NDEBUG
#include <assert.h>

/* Asserts that |avl_insert()| succeeds at inserting |item| into |table|. */
void
(avl_assert_insert) (struct avl_table *table, void *item)
{
  void **p = avl_probe (table, item);
  assert (p != NULL && *p == item);
}

/* Asserts that |avl_delete()| really removes |item| from |table|,
   and returns the removed item. */
void *
(avl_assert_delete) (struct avl_table *table, void *item)
{
  void *p = avl_delete (table, item);
  assert (p != NULL);
  return p;
}

#ifndef Q_PTR_SIZE
#define Q_PTR_SIZE
#endif

/**    This structure is for a single queue element. All structures that 
       you wish to enqueue must have this structure contained in it.
*/

typedef struct Q_ELEM Q_PTR_SIZE *Q_ELEM_PTR ;
typedef struct Q_ELEM {
     Q_ELEM_PTR      q_prev ;      /* previous queue element */
     Q_ELEM_PTR      q_next ;      /* next element */
     int           q_priority ;  /* priority of queue */
     int           q_element ;   /* element number */
     void Q_PTR_SIZE * q_ptr ;       /* a ptr to something */ 
} Q_ELEM;


/** This is the header of a queue. There needs to be one of these allocated
        for every queue you wish to use.
*/

typedef struct Q_HEADER Q_PTR_SIZE *Q_HEADER_PTR;
typedef struct Q_HEADER {
     Q_ELEM_PTR      q_head ;       /* head element of the queue */
     Q_ELEM_PTR      q_tail ;       /* tail element of the queue */
     int           q_count ;      /* count of elements */
     my_bool         q_active ;     /* Is this queue active */
     int           q_magic ;      /* for q_element number */
} Q_HEADER ;

#define BEFORE 0
#define AFTER  1

/** Here are some useful macros for the queues
*/

/* Returns the head element of the queue */
#define QueueHead( queue )   ( (queue)->q_head ) 

/* Returns the tail element of the queue */
#define QueueTail( queue )   ( (queue)->q_tail )

/* Returns the number of elements in the queue */
#define QueueCount( queue )  ( (queue)->q_count ) 

/* Returns the queue element number for an element */
#define QueueElemNo( element )  ( (element)->q_element )

/* Returns the next queue element in the chain */
#define QueueNext( element )    ( (element)->q_next ) 

/* Returns the previous queue element in the chain */
#define QueuePrev( element )    ( (element)->q_prev )

/* Returns the queue ptr field */
#define QueuePtr( element )     ( (element)->q_ptr )  

#define GetQueueElemPriority( elem_ptr )   ( (elem_ptr)->q_priority )
#define SetQueueElemPriority( elem_ptr, value )  ( ( (elem_ptr)->q_priority ) = (value) )

#define GetQueueElemPtr( elem_ptr )  ( (elem_ptr)->q_ptr )
#define SetQueueElemPtr( elem_ptr, value )  ( ( (elem_ptr)->q_ptr ) = (value) )


/**/
/**

    Name:        DeQueueElem

    Description:    Dequeue an element for the specified queue.

    Modified:        8/9/1989   13:42:20

    Returns:        A pointer to the element that was dequeue'd or a null
                    if the queue was empty.

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

Q_ELEM_PTR  DeQueueElem(
Q_HEADER_PTR   queue )        /* The queue */
{
     Q_ELEM_PTR  element = NULL ;    


     if( ( element = queue->q_head ) != NULL ) {
          queue->q_count-- ;
          if( queue->q_tail == element ) {
               queue->q_tail = NULL ;
          }    
          queue->q_head = element->q_next ;
          if( element->q_next != NULL ) {
               element->q_next->q_prev = NULL ;
               element->q_next = NULL ;
          }
          element->q_next = NULL;   /* let's practice safe coding */
          element->q_prev = NULL;
     }

     return( element ) ;

}


/**/
/**

    Name:        EnQueueElem

    Description:    Enqueues an element on the specified queue. If the priority
                    field is TRUE, this function will enqueue the element in sorted
                    order based on the "q_priority" field in the element structure.

    Modified:        8/9/1989   13:38:31

    Returns:        A pointer to that element.

    Notes:        

    See also:        $/SEE( DeQeueuElem )$

    Declaration:

**/

Q_ELEM_PTR EnQueueElem( 
Q_HEADER_PTR   queue,        /* The destination queue */
Q_ELEM_PTR    element,         /* The element */
my_bool w_priority )         /* Enqueue w/ priority */
{
     Q_ELEM_PTR  telem ;
     /* If priority insertion is not set or there are no elements on
     the stack. */
     if( !w_priority || !queue->q_count ) {
          element->q_next = NULL ;               /* end of queue  */
          element->q_prev = queue->q_tail ;      /* previous = queue's old tail */
          if( !QueueCount( queue ) ) {
               queue->q_head = element ;      /* 0 elems .. element is head */
          } else {
               queue->q_tail->q_next = element ; /* old tail's next is elem */
          }
          queue->q_tail = element ;              /* tail is always element */
     } else {
          telem = QueueHead( queue ) ;           /* set temp to head */
          while ( telem ) { 
               if( element->q_priority <= telem->q_priority ) {
                    element->q_next = telem ;
                    element->q_prev = telem->q_prev ;
                    if( !telem->q_prev ) {
                         queue->q_head = element ;
                    } else {
                         telem->q_prev->q_next = element ;
                    }
                    telem->q_prev = element ;
                    /* takes care of if element is being inserted
                    at head */
                    break ;
               }
               telem = QueueNext( telem ) ;
          }
          /* insert at tail */
          if( !telem ) {
               element->q_next = NULL ;
               element->q_prev = queue->q_tail ;
               queue->q_tail->q_next = element ;
               queue->q_tail = element ;
          }
     }
     queue->q_count++ ;
     element->q_element = queue->q_magic++ ;        

     return( element ) ;

}


/**/
/**

    Name:        InitQueue

    Description:    Initializes a queue header

    Modified:        7/12/1989

    Returns:        It's a VOID dummy

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

void InitQueue(  
Q_HEADER_PTR     queue )
{
     queue->q_head = queue->q_tail = NULL ;
     queue->q_count = 0 ;
     queue->q_active = TRUE ; 
     queue->q_magic = 1 ;

     return ;
}


/**/
/**

    Name:        InitQelem

    Description:    Initialises a queue element.

    Modified:        8/9/1989   13:37:8

    Returns:        Nothing

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

void InitQElem(   
Q_ELEM_PTR  element )
{
     element->q_prev = element->q_next = NULL ;
     element->q_element = 0 ;
     element->q_priority = 0L ;

     return ;
}


/**/
/**

    Name:        PushElem

    Description:    This makes a queue function like a stack. Unlike EnQueueElem()
                  which puts the element at the tail of the specified queue, 
                  PushElem() puts the element on the head of the queue.

    Modified:        8/9/1989   13:52:59

    Returns:        Nothing

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

void PushElem(  
Q_HEADER_PTR   queue,
Q_ELEM_PTR     element )
{
     element->q_prev = NULL ;
     element->q_next = QueueHead( queue ) ;
     element->q_element = queue->q_magic++ ;        
     if( !QueueCount( queue ) ) {
          queue->q_tail = element ;
     } else {
          queue->q_head->q_prev = element ;
     }
     queue->q_head = element ;
     queue->q_count++ ;

     return ;
}

/**/
/**

    Name:        RemoveElem

    Description:    Routine for removing an element from a queue.
                    No error checking is performed and the element is assumed
                    to be a valid one. Currently this routine is used by
                    RemoveQueueElem() and SearchQueue().


    Modified:        10/16/1989   10:49:46

    Returns:        Nothing.

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

void RemoveElem( 
Q_HEADER_PTR  queue,
Q_ELEM_PTR    element )
{
     queue->q_count-- ;

     if( queue->q_head == element ) {
          queue->q_head = QueueNext( element ) ;
     }
     if( queue->q_tail == element ) {
          queue->q_tail = QueuePrev( element ) ;
     }
     if( element->q_prev != NULL ) {
          element->q_prev->q_next = QueueNext( element ) ;
     }
     if( element->q_next != NULL ) {
          element->q_next->q_prev = QueuePrev( element ) ;
     }
     element->q_prev = element->q_next = NULL ;

     return ;
}


/**/
/**

    Name:        SortQueue

    Description:   This function sorts the specified queue into order. A pointer
                    to the compare rountine is passed to the function. This compare
                    routine must follow the following conventions:

                    1) It takes two Q_ELEM pointers ( Q_ELEM_PTR ) as its
                    arguements.

                    2) It returns a <0 for parm1 < parm2, 0 for parm1 == parm2,
                    and >0 for parm1 > parm2.

                    3) This function is slower than molasses on the north pole.
                    You should build/maintain your queue in order so that you
                    never have to sort it.

    Modified:        10/16/1989   10:38:47

    Returns:        Nothing

    Notes:        

    See also:        $/SEE( )$

    Declaration:

**/

void SortQueue( 
Q_HEADER_PTR    queue,
int  ( *cmp )( Q_ELEM_PTR, Q_ELEM_PTR ) )
{
     int        i ;
     int         end ;
     my_bool     any_swaps ;
     Q_ELEM_PTR  cur_el, temp ;
 
     end = QueueCount( queue );
     if( end > 1 ) {
          do {
               cur_el = QueueHead( queue ) ;
               any_swaps = FALSE ;
               for( i = 1 ; i < end; i++ ) {
                    if( (*cmp)( cur_el, QueueNext(cur_el)) > 0) {
                         temp = QueueNext( cur_el );

                         if ( QueueHead( queue ) == cur_el ) {
                              queue->q_head = temp;
                         }
                         else {
                              QueuePrev( cur_el )->q_next = temp ;
                         }
                         temp->q_prev = QueuePrev( cur_el );

                         if ( QueueTail( queue ) == temp ) {
                              queue->q_tail = cur_el;
                         }
                         else {
                              QueueNext( temp )->q_prev = cur_el ;
                         }
                         cur_el->q_next = QueueNext( temp );

                         temp->q_next = cur_el;
                         cur_el->q_prev = temp;


                         any_swaps = TRUE ;
                    }
                    else {
                         cur_el = QueueNext( cur_el );
                    }
               }
               end -- ;
          } while( any_swaps ) ;
     }
     return ;
}

/* Functions to support general ended bitmaps.
   Copyright (C) 1997 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

/* Functions to support general ended bitmaps.
   Copyright (C) 1997 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

/* Number of words to use for each element in the linked list.  */

#ifndef BITMAP_ELEMENT_WORDS
#define BITMAP_ELEMENT_WORDS 2
#endif

#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#ifndef HOST_BITS_PER_WIDE_INT
#define HOST_BITS_PER_WIDE_INT (CHAR_BIT * sizeof (int))
#endif

/* Number of bits in each actual element of a bitmap.  We get slightly better
   code for bit % BITMAP_ELEMENT_ALL_BITS and bit / BITMAP_ELEMENT_ALL_BITS if
   bits is unsigned, assuming it is a power of 2.  */

#define BITMAP_ELEMENT_ALL_BITS \
  ((unsigned) (BITMAP_ELEMENT_WORDS * HOST_BITS_PER_WIDE_INT))

/* Bitmap set element.  We use a linked list to hold only the bits that
   are set.  This allows for use to grow the bitset dynamically without
   having to realloc and copy a giant bit array.  The `prev' field is
   undefined for an element on the free list.  */

typedef struct bitmap_element_def
{
  struct bitmap_element_def *next;                /* Next element. */
  struct bitmap_element_def *prev;                /* Previous element. */
  unsigned int indx;                        /* regno/BITMAP_ELEMENT_ALL_BITS. */
  unsigned int bits[BITMAP_ELEMENT_WORDS]; /* Bits that are set. */
} bitmap_element;

/* Head of bitmap linked list.  */
typedef struct bitmap_head_def {
  bitmap_element *first;        /* First element in linked list. */
  bitmap_element *current;        /* Last element looked at. */
  int indx;                        /* Index of last element looked at. */
} bitmap_head, *bitmap;

/* True if MAP is an empty bitmap.  */
my_bool bitmap_empty_p (bitmap map)
{
  return !map->first;
}

/* Free a bitmap element */

static void
bitmap_element_free (bitmap head, bitmap_element *elt)
{
  bitmap_element *next = elt->next;
  bitmap_element *prev = elt->prev;

  if (prev)
    prev->next = next;

  if (next)
    next->prev = prev;

  if (head->first == elt)
    head->first = next;

  /* Since the first thing we try is to insert before current,
     make current the next entry in preference to the previous.  */
  if (head->current == elt)
    head->current = next != 0 ? next : prev;

  free( elt);
}

/* Allocate a bitmap element.  The bits are cleared, but nothing else is.  */

static bitmap_element *
bitmap_element_allocate (void)
{
  bitmap_element *element;

      element = (bitmap_element *) malloc (sizeof (bitmap_element));

#if BITMAP_ELEMENT_WORDS == 2
  element->bits[0] = element->bits[1] = 0;
#else
  for (i = 0; i < BITMAP_ELEMENT_WORDS; i++)
    element->bits[i] = 0;
#endif

  return element;
}

/* Return nonzero if all bits in an element are zero.  */

static int
bitmap_element_zerop (bitmap_element *element)
{
#if BITMAP_ELEMENT_WORDS == 2
  return (element->bits[0] | element->bits[1]) == 0;
#else
  int i;

  for (i = 0; i < BITMAP_ELEMENT_WORDS; i++)
    if (element->bits[i] != 0)
      return 0;

  return 1;
#endif
}

/* Link the bitmap element into the current bitmap linked list.  */

static void
bitmap_element_link (bitmap head, bitmap_element *element)
{
  unsigned int indx = element->indx;
  bitmap_element *ptr;

  /* If this is the first and only element, set it in.  */
  if (head->first == 0)
    {
      element->next = element->prev = 0;
      head->first = element;
    }

  /* If this index is less than that of the current element, it goes someplace
     before the current element.  */
  else if ((int)indx < head->indx)
    {
      for (ptr = head->current;
           ptr->prev != 0 && ptr->prev->indx > indx;
           ptr = ptr->prev)
        ;

      if (ptr->prev)
        ptr->prev->next = element;
      else
        head->first = element;

      element->prev = ptr->prev;
      element->next = ptr;
      ptr->prev = element;
    }

  /* Otherwise, it must go someplace after the current element.  */
  else
    {
      for (ptr = head->current;
           ptr->next != 0 && ptr->next->indx < indx;
           ptr = ptr->next)
        ;

      if (ptr->next)
        ptr->next->prev = element;

      element->next = ptr->next;
      element->prev = ptr;
      ptr->next = element;
    }

  /* Set up so this is the first element searched.  */
  head->current = element;
  head->indx = indx;
}

/* Clear a bitmap by freeing the linked list.  */

void 
bitmap_clear (bitmap head)
{
  bitmap_element *element, *next;

  for (element = head->first; element != 0; element = next)
    {
      next = element->next;
      free(element);
    }

  head->first = head->current =  0;
}

/* Find a bitmap element that would hold a bitmap's bit.
   Update the `current' field even if we can't find an element that
   would hold the bitmap's bit to make eventual allocation
   faster.  */

static bitmap_element *
bitmap_find_bit (bitmap head, unsigned bit)
{
  bitmap_element *element;
  unsigned int indx = bit / BITMAP_ELEMENT_ALL_BITS;

  if (head->current == 0)
    return 0;

  if (head->indx > (int)indx)
    for (element = head->current;
         element->prev != 0 && element->indx > indx;
         element = element->prev)
      ;

  else
    for (element = head->current;
         element->next != 0 && element->indx < indx;
         element = element->next)
      ;

  /* `element' is the nearest to the one we want.  If it's not the one we
     want, the one we want doesn't exist.  */
  head->current = element;
  head->indx = element->indx;
  if (element != 0 && element->indx != indx)
    element = 0;

  return element;
}

/* Clear a single bit in a bitmap.  */

void
bitmap_clear_bit (bitmap head, int bit)
{
  bitmap_element *ptr = bitmap_find_bit (head, bit);

  if (ptr != 0)
    {
      unsigned bit_num  = bit % (unsigned) HOST_BITS_PER_WIDE_INT;
      unsigned word_num = ((bit / (unsigned) HOST_BITS_PER_WIDE_INT)
                           % BITMAP_ELEMENT_WORDS);
      ptr->bits[word_num] &= ~ (((unsigned int) 1) << bit_num);

      /* If we cleared the entire word, free up the element */
      if (bitmap_element_zerop (ptr))
        bitmap_element_free (head, ptr);
    }
}


/* Set a single bit in a bitmap.  */

void
bitmap_set_bit (bitmap head, int bit)
{
  bitmap_element *ptr = bitmap_find_bit (head, bit);
  unsigned word_num
    = ((bit / (unsigned) HOST_BITS_PER_WIDE_INT) % BITMAP_ELEMENT_WORDS);
  unsigned bit_num  = bit % (unsigned) HOST_BITS_PER_WIDE_INT;
  unsigned int bit_val = ((unsigned int) 1) << bit_num;

  if (ptr == 0)
    {
      ptr = bitmap_element_allocate ();
      ptr->indx = bit / BITMAP_ELEMENT_ALL_BITS;
      ptr->bits[word_num] = bit_val;
      bitmap_element_link (head, ptr);
    }
  else
    ptr->bits[word_num] |= bit_val;
}

/* Return whether a bit is set within a bitmap.  */

int
bitmap_bit_p (bitmap head, int bit)
{
  bitmap_element *ptr;
  unsigned bit_num;
  unsigned word_num;

  ptr = bitmap_find_bit (head, bit);
  if (ptr == 0)
    return 0;

  bit_num = bit % (unsigned) HOST_BITS_PER_WIDE_INT;
  word_num
    = ((bit / (unsigned) HOST_BITS_PER_WIDE_INT) % BITMAP_ELEMENT_WORDS);

  return
    (ptr->bits[word_num] & (((unsigned int) 1) << bit_num)) != 0;
}

#include "cuda.h"

/*********************************
 ** Initialization
 *********************************/
typedef CUresult (CUDAAPI *PFN_cuInit_v2000)(unsigned int Flags);

/*********************************
 ** Driver Version Query
 *********************************/
typedef CUresult (CUDAAPI *PFN_cuDriverGetVersion_v2020)(int *driverVersion);

/************************************
 **
 **    Device management
 **
 ***********************************/

typedef CUresult (CUDAAPI *PFN_cuDeviceGetCount_v2000)(int *count);
typedef CUresult (CUDAAPI *PFN_cuDeviceComputeCapability_v2000)(int *major, int *minor, CUdevice_v1 dev);
typedef CUresult (CUDAAPI *PFN_cuDeviceGetAttribute_v2000)(int *pi, CUdevice_attribute attrib, CUdevice_v1 dev);

/************************************
 **
 **    Context management
 **
 ***********************************/

typedef CUresult (CUDAAPI *PFN_cuDevicePrimaryCtxRetain_v7000)(CUcontext *pctx, CUdevice_v1 dev);
typedef CUresult (CUDAAPI *PFN_cuDevicePrimaryCtxRelease_v7000)(CUdevice_v1 dev);
typedef CUresult (CUDAAPI *PFN_cuCtxSetCurrent_v4000)(CUcontext ctx);
typedef CUresult (CUDAAPI *PFN_cuCtxGetCurrent_v4000)(CUcontext *pctx);

typedef CUresult (CUDAAPI *PFN_cuCtxGetDevice_v2000)(CUdevice_v1 *device);
typedef CUresult (CUDAAPI *PFN_cuCtxSynchronize_v2000)(void);


/************************************
 **
 **    Module management
 **
 ***********************************/

typedef CUresult (CUDAAPI *PFN_cuModuleLoad_v2000)(CUmodule *module, const char *fname);
typedef CUresult (CUDAAPI *PFN_cuModuleLoadData_v2000)(CUmodule *module, const void *image);
typedef CUresult (CUDAAPI *PFN_cuModuleLoadDataEx_v2010)(CUmodule *module, const void *image, unsigned int numOptions, CUjit_option *options, void **optionValues);
typedef CUresult (CUDAAPI *PFN_cuModuleLoadFatBinary_v2000)(CUmodule *module, const void *fatCubin);
typedef CUresult (CUDAAPI *PFN_cuModuleUnload_v2000)(CUmodule hmod);
typedef CUresult (CUDAAPI *PFN_cuModuleGetFunction_v2000)(CUfunction *hfunc, CUmodule hmod, const char *name);
typedef CUresult (CUDAAPI *PFN_cuLibraryLoadData_v12000)(CUlibrary *library, const void *code, CUjit_option *jitOptions, void **jitOptionsValues, unsigned int numJitOptions, CUlibraryOption *libraryOptions, void** libraryOptionValues, unsigned int numLibraryOptions);
typedef CUresult (CUDAAPI *PFN_cuLibraryGetKernel_v12000)(CUkernel *pKernel, CUlibrary library, const char *name);
typedef CUresult (CUDAAPI *PFN_cuLibraryGetModule_v12000)(CUmodule *pMod, CUlibrary library);
typedef CUresult (CUDAAPI *PFN_cuKernelGetFunction_v12000)(CUfunction *pFunc, CUkernel kernel);

/************************************
 **
 **    Memory management
 **
 ***********************************/
typedef CUresult (CUDAAPI *PFN_cuMemGetInfo_v3020)(size_t *free, size_t *total);
typedef CUresult (CUDAAPI *PFN_cuMemAlloc_v3020)(CUdeviceptr_v2 *dptr, size_t bytesize);

typedef CUresult (CUDAAPI *PFN_cuMemFree_v3020)(CUdeviceptr_v2 dptr);

typedef CUresult (CUDAAPI *PFN_cuMemFreeHost_v2000)(void *p);
typedef CUresult (CUDAAPI *PFN_cuMemHostAlloc_v2020)(void **pp, size_t bytesize, unsigned int Flags);

/************************************
 **
 **    Function management
 **
 ***********************************/


typedef CUresult (CUDAAPI *PFN_cuFuncGetAttribute_v2020)(int *pi, CUfunction_attribute attrib, CUfunction hfunc);
typedef CUresult (CUDAAPI *PFN_cuFuncSetAttribute_v9000)(CUfunction hfunc, CUfunction_attribute attrib, int value);
typedef CUresult (CUDAAPI *PFN_cuFuncSetCacheConfig_v3000)(CUfunction hfunc, CUfunc_cache config);
typedef CUresult (CUDAAPI *PFN_cuLaunchKernel_v4000)(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream, void **kernelParams, void **extra);
typedef CUresult (CUDAAPI *PFN_cuLaunchKernelEx_v11060)(const CUlaunchConfig *config, CUfunction f, void **kernelParams, void **extra);
typedef CUresult (CUDAAPI *PFN_cuLaunchHostFunc_v10000)(CUstream hStream, CUhostFn fn, void *userData);
typedef CUresult (CUDAAPI *PFN_cuGetProcAddress_v11030)(const char *symbol, void **pfn, int driverVersion, cuuint64_t flags);
typedef CUresult (CUDAAPI *PFN_cuGetProcAddress_v12000)(const char *symbol, void **pfn, int driverVersion, cuuint64_t flags, CUdriverProcAddressQueryResult *symbolFound);

/************************************
 **
 **    Streams
 **
 ***********************************/
typedef CUresult (CUDAAPI *PFN_cuStreamCreate_v2000)(CUstream *phStream, unsigned int Flags);
typedef CUresult (CUDAAPI *PFN_cuStreamWaitEvent_v3020)(CUstream hStream, CUevent hEvent, unsigned int Flags);
typedef CUresult (CUDAAPI *PFN_cuStreamAddCallback_v5000)(CUstream hStream, CUstreamCallback callback, void *userData, unsigned int flags);

typedef CUresult (CUDAAPI *PFN_cuStreamQuery_v2000)(CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuStreamSynchronize_v2000)(CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuStreamDestroy_v4000)(CUstream hStream);
typedef CUresult (CUDAAPI *PFN_cuStreamIsCapturing_v10000)(CUstream hStream, CUstreamCaptureStatus *captureStatus);

/************************************
 **
 **    Export tables
 **
 ***********************************/
typedef CUresult (CUDAAPI *PFN_cuGetExportTable_v3000)(const void **ppExportTable, const CUuuid *pExportTableId);

/************************************
 ************************************/

static PFN_cuInit_v2000 cuInit_fn;
static PFN_cuDriverGetVersion_v2020 cuDriverGetVersion_fn;
static PFN_cuDeviceGetCount_v2000 cuDeviceGetCount_fn;
static PFN_cuDeviceComputeCapability_v2000 cuDeviceComputeCapability_fn;
static PFN_cuDeviceGetAttribute_v2000 cuDeviceGetAttribute_fn;
static PFN_cuDevicePrimaryCtxRetain_v7000 cuDevicePrimaryCtxRetain_fn;
static PFN_cuDevicePrimaryCtxRelease_v7000 cuDevicePrimaryCtxRelease_fn;
static PFN_cuCtxSetCurrent_v4000 cuCtxSetCurrent_fn;
static PFN_cuCtxGetCurrent_v4000 cuCtxGetCurrent_fn;
static PFN_cuCtxGetDevice_v2000 cuCtxGetDevice_fn;
static PFN_cuCtxSynchronize_v2000 cuCtxSynchronize_fn;
static PFN_cuModuleLoad_v2000 cuModuleLoad_fn;
static PFN_cuModuleLoadData_v2000 cuModuleLoadData_fn;
static PFN_cuModuleLoadDataEx_v2010 cuModuleLoadDataEx_fn;
static PFN_cuModuleLoadFatBinary_v2000 cuModuleLoadFatBinary_fn;
static PFN_cuModuleUnload_v2000 cuModuleUnload_fn;
static PFN_cuModuleGetFunction_v2000 cuModuleGetFunction_fn;
static PFN_cuLibraryLoadData_v12000 cuLibraryLoadData_fn;
static PFN_cuLibraryGetKernel_v12000 cuLibraryGetKernel_fn;
static PFN_cuLibraryGetModule_v12000 cuLibraryGetModule_fn;
static PFN_cuKernelGetFunction_v12000 cuKernelGetFunction_fn;
static PFN_cuMemGetInfo_v3020 cuMemGetInfo_fn;
static PFN_cuMemAlloc_v3020 cuMemAlloc_fn;
static PFN_cuMemFree_v3020 cuMemFree_fn;
static PFN_cuMemFreeHost_v2000 cuMemFreeHost_fn;
static PFN_cuMemHostAlloc_v2020 cuMemHostAlloc_fn;
static PFN_cuFuncGetAttribute_v2020 cuFuncGetAttribute_fn;
static PFN_cuFuncSetAttribute_v9000 cuFuncSetAttribute_fn;
static PFN_cuFuncSetCacheConfig_v3000 cuFuncSetCacheConfig_fn;
static PFN_cuLaunchKernel_v4000 cuLaunchKernel_fn;
static PFN_cuLaunchKernelEx_v11060 cuLaunchKernelEx_fn;
static PFN_cuLaunchHostFunc_v10000 cuLaunchHostFunc_fn;
static PFN_cuGetProcAddress_v11030 cuGetProcAddress_fn;
static PFN_cuGetProcAddress_v12000 cuGetProcAddress_v2_fn;
static PFN_cuStreamCreate_v2000 cuStreamCreate_fn;
static PFN_cuStreamWaitEvent_v3020 cuStreamWaitEvent_fn;
static PFN_cuStreamAddCallback_v5000 cuStreamAddCallback_fn;
static PFN_cuStreamQuery_v2000 cuStreamQuery_fn;
static PFN_cuStreamSynchronize_v2000 cuStreamSynchronize_fn;
static PFN_cuStreamDestroy_v4000 cuStreamDestroy_fn;
static PFN_cuStreamIsCapturing_v10000 cuStreamIsCapturing_fn;
static PFN_cuGetExportTable_v3000 cuGetExportTable_fn;

#include "nvml.h"

typedef nvmlReturn_t (DECLDIR *PFN_nvmlInit_v2)(void);
typedef nvmlReturn_t (DECLDIR *PFN_nvmlDeviceGetCount_v2)(unsigned int *deviceCount);
typedef nvmlReturn_t (DECLDIR *PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned int index, nvmlDevice_t *device);
typedef nvmlReturn_t (DECLDIR *PFN_nvmlDeviceGetGraphicsRunningProcesses_v3)(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_t *infos);
typedef nvmlReturn_t (DECLDIR *PFN_nvmlDeviceGetComputeRunningProcesses_v3)(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_t *infos);
typedef nvmlReturn_t (DECLDIR *PFN_nvmlDeviceGetMemoryInfo_v2)(nvmlDevice_t device, nvmlMemory_v2_t *memory);

static PFN_nvmlInit_v2                    pfn_nvmlInit_v2;
static PFN_nvmlDeviceGetCount_v2          pfn_nvmlDeviceGetCount_v2;
static PFN_nvmlDeviceGetHandleByIndex_v2  pfn_nvmlDeviceGetHandleByIndex_v2;
static PFN_nvmlDeviceGetGraphicsRunningProcesses_v3 pfn_nvmlDeviceGetGraphicsRunningProcesses_v3;
static PFN_nvmlDeviceGetComputeRunningProcesses_v3 pfn_nvmlDeviceGetComputeRunningProcesses_v3;
static PFN_nvmlDeviceGetMemoryInfo_v2     pfn_nvmlDeviceGetMemoryInfo_v2;


#if !defined(__WIN__)
typedef void * (*PFN_DLSYM)(void *, const char *);
static PFN_DLSYM dlsym_ptr = NULL;
static int (*ioctl_ptr)(int dev,unsigned long cmd,...);
#endif /* !defined(__WIN__) */

  // configuration
//# define PROFILING // Define this to
#define TIME_SLICE // Define this to Set timeslice.
#define MEM_CONTROL
# define GATHERSTATS // Define this to
#define BUF_SIZE (1024 * 1024)
#define MAX_PIDS (1024)
#define CUDA_DEVICE_MAX_COUNT 16


/* Types  */

/* The kind of module or part thereof that we're in.  */
enum module_kind_bits
{
  MK_original = 0, // original kernels
  MK_persistent = 1, // Persistent Thread Block (PTB).
  MK_fusion = 2, // Kernel Fusion

  MK_MAX = 3,  /* We are in an export region.  */
};
static const char * const kind_strs[] = {
  "",
  "A",
  "B",
  "C",
};

typedef struct SlibList_s {
  void *elf;
  size_t elfSize;
  char *code;
  size_t size;
    char device_isa[64];
    CUlibrary library;
   unsigned module_kind;
   char *lpszName;
} SlibList_t;


struct kernargs {
    uint16_t ordinal;
    uint16_t offset;
    uint32_t size_bytes;
};

/* Information required to identify, finalize and run any given kernel.  */
struct kernel_info
{
  void *object;
  unsigned int reg_count;
  int mem_sz;
    void *hOwner;
};

// a structure with a bunch of funcs
typedef struct FuncList
{
  /* An array of kernel_info structures describing each kernel.  */
  struct kernel_info kernels[MK_MAX];
  Q_HEADER param_list;
  /* Name of the kernel, required to locate it within the GCN object-code
     module.  */
  char *name;

  /* Whether the agent has been initialized.  */
  my_bool initialized;
} FUNCLIST, *PFUNCLIST;

/* Information about the whole environment .  */
struct context_info
{
    CUcontext   hcuContext;
  struct agent_info *agent;

    Q_HEADER modList[MK_MAX];
  struct avl_table * fns;
    struct avl_table *name_map;

    int sharedMemPerBlock;
    int regsPerBlock;
    int shared_optin;
    int maxThreadsPerBlock;
    int warpSize;

  /* Driver version string. */
  char driver_version_s[30];
};

/* Description of an GPU agent (device) and the program associated with
   it.  */
struct agent_info
{
  /* The user-visible device number.  */
  unsigned device_id;
  int num_sms;

//int override_x_dim ;
//int nSlices;

  /* The instruction set architecture of the device. */
  char device_isae[64];
  /* Name of the agent. */
  char name[256];
};

struct thread_info {    /* Used as argument to thread_start() */
    pthread_t thread_id;        /* ID returned by pthread_create() */
    int       thread_num;       /* Application-defined thread # */
    Q_HEADER          *list_header ;  /* list of BUFs */
    pthread_mutex_t *CritSect;
};

typedef struct {
    long pid;
  int override_x_dim ;
  int nSlices;
    uint32_t mask[4];
    uint64_t timeslice_us;
    uint64_t  memory_limit;
} shrreg_proc_slot_t;

/* Global variables  */

/* Number of usable GPU agents in the system.  */
int agent_count;
/* Array of agent_info structures describing the individual agents.  */
struct agent_info *agents;

struct avl_table * fns;
static  Q_HEADER library_list;
struct avl_table * context;
/* command queue.  */
struct avl_table * aq;

static Q_HEADER  ModuleList;

static int (*bar1)(void*, void*);
static int (*bar2)(void*, void*, unsigned, void*, void*); // bar2( ctx, hfunc, 0, &dummy, &size );


int cuda_to_nvml_map_array[CUDA_DEVICE_MAX_COUNT];
unsigned int hostpid;

/* Runtime dimension overrides.  -1 indicates default.  */
static int override_x_dim = -1;
static int      nSlices = 1;
static uint32_t g_sm_maskA[4];
static my_bool   PARTITION_GLOBAL;
static my_bool   PARTITION_STREAM;
static my_bool   CUDA_CORE_SOFT_LIMIT;
#if defined(TIME_SLICE)
static uint64_t timeslice_us = 8192;
#endif
#if defined(MEM_CONTROL)
static uint64_t  memory_limit;
#endif

static pthread_mutex_t  g_csGlobal;

char *pBuf;
shrreg_proc_slot_t* my_slot;
pthread_t con;

#ifdef __WIN__
HANDLE    g_hExitEvent;
HANDLE hMapFile;
#else /* __WIN__ */
int shmid;
#endif /* __WIN__ */

static pthread_cond_t cond;
static pthread_mutex_t mutex;

#   ifdef GATHERSTATS
/* FOR DEBUGING PURPOSES ONLY.
*/
static    int passed;
static    int failed ;
static int classes[MK_MAX];

#   endif


int error_occurred;

/* Flag to decide whether print to stderr information about what is going on.
   Set in init_debug depending on environment variables.  */

static my_bool debug;

/* Print a message to stderr if GCN_DEBUG value is set to true.  */

#define DEBUG_PRINT(...) \
  do \
  { \
    if (debug) \
      { \
        fprintf (stderr, __VA_ARGS__); \
      } \
  } \
  while (false);

/* Flush stderr if GCN_DEBUG value is set to true.  */

#define DEBUG_FLUSH()                \
  do {                        \
    if (debug)                    \
      fflush (stderr);                \
  } while (false)

/* Print a logging message with PREFIX to stderr if GCN_DEBUG value
   is set to true.  */

#define DEBUG_LOG(prefix, ...)            \
  do                        \
    {                        \
      DEBUG_PRINT (prefix);            \
      DEBUG_PRINT (__VA_ARGS__);            \
      DEBUG_FLUSH ();                \
    } while (false)

/* Print a debugging message to stderr.  */

#define GCN_DEBUG(...) DEBUG_LOG ("GCN debug: ", __VA_ARGS__)

/* Print a warning message to stderr.  */

#define GCN_WARNING(...) DEBUG_LOG ("GCN warning: ", __VA_ARGS__)

#ifdef PROFILING
static  Q_HEADER dlist;
struct _MyDebugInfo {
    uint8_t *buffer; 
    size_t validSize;
} MyDebugInfo;

/*
 * Copyright 2011-2013 NVIDIA Corporation. All rights reserved
 *
 * Sample CUPTI app to print a trace of CUDA API and GPU activity
 *
 */

#include <stdio.h>
//#include <cuda.h>
#include <cupti.h>

#define CUPTI_CALL(call)                                                \
  do {                                                                  \
    CUptiResult _status = call;                                         \
    if (_status != CUPTI_SUCCESS) {                                     \
      const char *errstr;                                               \
      cuptiGetResultString(_status, &errstr);                           \
      fprintf(stderr, "%s:%d: error: function %s failed with error %s.\n", \
              __FILE__, __LINE__, #call, errstr);                       \
      if(_status == CUPTI_ERROR_LEGACY_PROFILER_NOT_SUPPORTED)          \
          exit(0);                                                      \
      else                                                              \
          exit(-1);                                                     \
    }                                                                   \
  } while (0)

//#define BUF_SIZE (32 * 1024)
#define ALIGN_SIZE (8)
#define ALIGN_BUFFER(buffer, align)                                            \
  (((uintptr_t) (buffer) & ((align)-1)) ? ((buffer) + (align) - ((uintptr_t) (buffer) & ((align)-1))) : (buffer))

// Timestamp at trace initialization time. Used to normalized other
// timestamps
static uint64_t startTimestamp;

const char *
getMemcpyKindString(CUpti_ActivityMemcpyKind kind)
{
  switch (kind) {
  case CUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
    return "HtoD";
  case CUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
    return "DtoH";
  case CUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
    return "HtoA";
  case CUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
    return "AtoH";
  case CUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
    return "AtoA";
  case CUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
    return "AtoD";
  case CUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
    return "DtoA";
  case CUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
    return "DtoD";
  case CUPTI_ACTIVITY_MEMCPY_KIND_HTOH:
    return "HtoH";
  default:
    break;
  }

  return "<unknown>";
}

const char *
getActivityOverheadKindString(CUpti_ActivityOverheadKind kind)
{
  switch (kind) {
  case CUPTI_ACTIVITY_OVERHEAD_DRIVER_COMPILER:
    return "COMPILER";
  case CUPTI_ACTIVITY_OVERHEAD_CUPTI_BUFFER_FLUSH:
    return "BUFFER_FLUSH";
  case CUPTI_ACTIVITY_OVERHEAD_CUPTI_INSTRUMENTATION:
    return "INSTRUMENTATION";
  case CUPTI_ACTIVITY_OVERHEAD_CUPTI_RESOURCE:
    return "RESOURCE";
  default:
    break;
  }

  return "<unknown>";
}

const char *
getActivityObjectKindString(CUpti_ActivityObjectKind kind)
{
  switch (kind) {
  case CUPTI_ACTIVITY_OBJECT_PROCESS:
    return "PROCESS";
  case CUPTI_ACTIVITY_OBJECT_THREAD:
    return "THREAD";
  case CUPTI_ACTIVITY_OBJECT_DEVICE:
    return "DEVICE";
  case CUPTI_ACTIVITY_OBJECT_CONTEXT:
    return "CONTEXT";
  case CUPTI_ACTIVITY_OBJECT_STREAM:
    return "STREAM";
  default:
    break;
  }

  return "<unknown>";
}

uint32_t
getActivityObjectKindId(CUpti_ActivityObjectKind kind, CUpti_ActivityObjectKindId *id)
{
  switch (kind) {
  case CUPTI_ACTIVITY_OBJECT_PROCESS:
    return id->pt.processId;
  case CUPTI_ACTIVITY_OBJECT_THREAD:
    return id->pt.threadId;
  case CUPTI_ACTIVITY_OBJECT_DEVICE:
    return id->dcs.deviceId;
  case CUPTI_ACTIVITY_OBJECT_CONTEXT:
    return id->dcs.contextId;
  case CUPTI_ACTIVITY_OBJECT_STREAM:
    return id->dcs.streamId;
  default:
    break;
  }

  return 0xffffffff;
}

const char *
getComputeApiKindString(CUpti_ActivityComputeApiKind kind)
{
  switch (kind) {
  case CUPTI_ACTIVITY_COMPUTE_API_CUDA:
    return "CUDA";
  case CUPTI_ACTIVITY_COMPUTE_API_CUDA_MPS:
    return "CUDA_MPS";
  default:
    break;
  }

  return "<unknown>";
}

static void
printActivity(CUpti_Activity *record, int *count)
{
  switch (record->kind) {
#if 0

  case CUPTI_ACTIVITY_KIND_DEVICE:
    {
      CUpti_ActivityDevice2 *device = (CUpti_ActivityDevice2 *)record;
      printf("DEVICE %s (%u), capability %u.%u, global memory (bandwidth %u GB/s, size %u MB), "
             "multiprocessors %u, clock %u MHz\n",
             device->name, device->id,
             device->computeCapabilityMajor, device->computeCapabilityMinor,
             (unsigned int)(device->globalMemoryBandwidth / 1024 / 1024),
             (unsigned int)(device->globalMemorySize / 1024 / 1024),
             device->numMultiprocessors, (unsigned int)(device->coreClockRate / 1000));
      break;
    }
  case CUPTI_ACTIVITY_KIND_DEVICE_ATTRIBUTE:
    {
      CUpti_ActivityDeviceAttribute *attribute = (CUpti_ActivityDeviceAttribute *)record;
      printf("DEVICE_ATTRIBUTE %u, device %u, value=0x%llx\n",
             attribute->attribute.cupti, attribute->deviceId, (unsigned long long)attribute->value.vUint64);
      break;
    }
  case CUPTI_ACTIVITY_KIND_CONTEXT:
    {
      CUpti_ActivityContext *context = (CUpti_ActivityContext *)record;
      printf("CONTEXT %u, device %u, compute API %s, NULL stream %d\n",
             context->contextId, context->deviceId,
             getComputeApiKindString((CUpti_ActivityComputeApiKind) context->computeApiKind),
             (int) context->nullStreamId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MEMCPY:
    {
      CUpti_ActivityMemcpy *memcpy = (CUpti_ActivityMemcpy *)record;
      printf("MEMCPY %s [ %llu - %llu ] device %u, context %u, stream %u, correlation %u/r%u\n",
             getMemcpyKindString((CUpti_ActivityMemcpyKind)memcpy->copyKind),
             (unsigned long long)(memcpy->start - startTimestamp),
             (unsigned long long)(memcpy->end - startTimestamp),
             memcpy->deviceId, memcpy->contextId, memcpy->streamId,
             memcpy->correlationId, memcpy->runtimeCorrelationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MEMSET:
    {
      CUpti_ActivityMemset *memset = (CUpti_ActivityMemset *)record;
      printf("MEMSET value=%u [ %llu - %llu ] device %u, context %u, stream %u, correlation %u\n",
             memset->value,
             (unsigned long long)(memset->start - startTimestamp),
             (unsigned long long)(memset->end - startTimestamp),
             memset->deviceId, memset->contextId, memset->streamId,
             memset->correlationId);
      break;
    }
#endif

  case CUPTI_ACTIVITY_KIND_KERNEL:
  case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL:
    {
      const char* kindString = (record->kind == CUPTI_ACTIVITY_KIND_KERNEL) ? "KERNEL" : "CONC KERNEL";
      CUpti_ActivityKernel4 *kernel = (CUpti_ActivityKernel4 *)record;
      printf("#%d: ", *count);
      printf("%s \"%s\" [ %llu - %llu ] duration %llu, device %u, context %u, stream %u, correlation %u",
             kindString,
             kernel->name,
             (unsigned long long)(kernel->start - startTimestamp),
             (unsigned long long)(kernel->end - startTimestamp),
                    (unsigned long long)(kernel->end - kernel->start),
             kernel->deviceId, kernel->contextId, kernel->streamId,
             kernel->correlationId);
      printf("    grid [%u,%u,%u], block [%u,%u,%u], shared memory (static %u, dynamic %u), registersPerThread %u\n",
             kernel->gridX, kernel->gridY, kernel->gridZ,
             kernel->blockX, kernel->blockY, kernel->blockZ,
             kernel->staticSharedMemory, kernel->dynamicSharedMemory, kernel->registersPerThread);
      (*count)++;
      break;
    }
#if 0

  case CUPTI_ACTIVITY_KIND_DRIVER:
    {
      CUpti_ActivityAPI *api = (CUpti_ActivityAPI *)record;
      printf("DRIVER cbid=%u [ %llu - %llu ] process %u, thread %u, correlation %u\n",
             api->cbid,
             (unsigned long long)(api->start - startTimestamp),
             (unsigned long long)(api->end - startTimestamp),
             api->processId, api->threadId, api->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_RUNTIME:
    {
      CUpti_ActivityAPI *api = (CUpti_ActivityAPI *)record;
      printf("RUNTIME cbid=%u [ %llu - %llu ] process %u, thread %u, correlation %u\n",
             api->cbid,
             (unsigned long long)(api->start - startTimestamp),
             (unsigned long long)(api->end - startTimestamp),
             api->processId, api->threadId, api->correlationId);
      break;
    }
  case CUPTI_ACTIVITY_KIND_NAME:
    {
      CUpti_ActivityName *name = (CUpti_ActivityName *)record;
      switch(name->objectKind) {
        case CUPTI_ACTIVITY_OBJECT_CONTEXT:
             printf("NAME  %s %u %s id %u, name %s\n",
                  getActivityObjectKindString(name->objectKind),
                  getActivityObjectKindId(name->objectKind, &name->objectId),
                  getActivityObjectKindString(CUPTI_ACTIVITY_OBJECT_DEVICE),
                  getActivityObjectKindId(CUPTI_ACTIVITY_OBJECT_DEVICE, &name->objectId),
                  name->name);
             break;
        case CUPTI_ACTIVITY_OBJECT_STREAM:
             printf("NAME %s %u %s %u %s id %u, name %s\n",
                  getActivityObjectKindString(name->objectKind),
                  getActivityObjectKindId(name->objectKind, &name->objectId),
                  getActivityObjectKindString(CUPTI_ACTIVITY_OBJECT_CONTEXT),
                  getActivityObjectKindId(CUPTI_ACTIVITY_OBJECT_CONTEXT, &name->objectId),
                  getActivityObjectKindString(CUPTI_ACTIVITY_OBJECT_DEVICE),
                  getActivityObjectKindId(CUPTI_ACTIVITY_OBJECT_DEVICE, &name->objectId),
                  name->name);
             break;
        default:
             printf("NAME %s id %u, name %s\n",
                  getActivityObjectKindString(name->objectKind),
                  getActivityObjectKindId(name->objectKind, &name->objectId),
                  name->name);
             break;
      }
      break;
    }
  case CUPTI_ACTIVITY_KIND_MARKER:
    {
      CUpti_ActivityMarker2 *marker = (CUpti_ActivityMarker2 *)record;
      printf("MARKER id %u [ %llu ], name %s, domain %s\n",
             marker->id, (unsigned long long)marker->timestamp, marker->name, marker->domain);
      break;
    }
  case CUPTI_ACTIVITY_KIND_MARKER_DATA:
    {
      CUpti_ActivityMarkerData *marker = (CUpti_ActivityMarkerData *)record;
      printf("MARKER_DATA id %u, color 0x%x, category %u, payload %llu/%f\n",
             marker->id, marker->color, marker->category,
             (unsigned long long)marker->payload.metricValueUint64,
             marker->payload.metricValueDouble);
      break;
    }
  case CUPTI_ACTIVITY_KIND_OVERHEAD:
    {
        CUpti_ActivityOverhead *overhead = (CUpti_ActivityOverhead *)record;
        printf("OVERHEAD %s [ %llu, %llu ] %s id %u\n",
               getActivityOverheadKindString(overhead->overheadKind),
               (unsigned long long)overhead->start -startTimestamp,
               (unsigned long long)overhead->end - startTimestamp,
               getActivityObjectKindString(overhead->objectKind),
               getActivityObjectKindId(overhead->objectKind, &overhead->objectId));
        break;
    }
#endif

  default:
#if 0
    printf("  <unknown>\n");
#endif
    break;
  }
}

static void CUPTIAPI
bufferRequested(uint8_t **buffer, size_t *size, size_t *maxNumRecords)
{
  uint8_t *rawBuffer;

  *size = 8*(1024 * 1024);
  rawBuffer = (uint8_t *)malloc(*size + ALIGN_SIZE);

  *buffer = ALIGN_BUFFER(rawBuffer, ALIGN_SIZE);
  *maxNumRecords = 0;

  if (*buffer == NULL) {
    printf("Error: out of memory\n");
    exit(-1);
  }
}

static void CUPTIAPI
bufferCompleted(CUcontext ctx, uint32_t streamId, uint8_t *buffer, size_t size, size_t validSize)
{
     struct _MyDebugInfo *    item ;
     Q_ELEM_PTR          qelem ;

     if ( (item = (struct _MyDebugInfo *)malloc(sizeof(Q_ELEM) + sizeof(struct _MyDebugInfo))) != NULL ) {
         qelem = (Q_ELEM_PTR)(item + 1);
         item->buffer = buffer ;
         item->validSize = validSize ;
         InitQElem( qelem ) ;
         SetQueueElemPtr( qelem, item ) ;
         EnQueueElem( &dlist, qelem, FALSE );
    }
  // report any records dropped from the queue
  size_t dropped;
  CUPTI_CALL(cuptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
  if (dropped != 0) {
    printf("Dropped %u activity records\n", (unsigned int)dropped);
  }

//free(buffer);
    (void)(size);
}

void
initTrace()
{
  // device activity record is created when CUDA initializes, so we
  // want to enable it before cuInit() or any CUDA runtime call
  CUPTI_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DEVICE));

  CUPTI_CALL(cuptiActivityRegisterCallbacks(bufferRequested, bufferCompleted));

  CUPTI_CALL(cuptiGetTimestamp(&startTimestamp));
}

void
finiTrace()
{
  CUPTI_CALL(cuptiActivityFlushAll(0));
}
#endif

#if defined(MAKE_NV64TYPES_8BYTES_ALIGNED) && defined(__i386__)
// ensure or force 8-bytes alignment of NV 64-bit types
#define OPTIONAL_ALIGN8_ATTR __attribute__((aligned(8)))
#else
// nothing needed
#define OPTIONAL_ALIGN8_ATTR
#endif // MAKE_NV64TYPES_8BYTES_ALIGNED && i386

// 8-bit: 'char' is the only 8-bit in the C89 standard and after.
typedef unsigned char      NvV8; /* "void": enumerated or multiple fields    */
typedef unsigned char      NvU8; /* 0 to 255                                 */
typedef   signed char      NvS8; /* -128 to 127                              */


// 32-bit: If the compiler tells us what we can use, then use it.
#ifdef __INT32_TYPE__
typedef unsigned __INT32_TYPE__ NvV32; /* "void": enumerated or multiple fields */
typedef unsigned __INT32_TYPE__ NvU32; /* 0 to 4294967295                       */
typedef   signed __INT32_TYPE__ NvS32; /* -2147483648 to 2147483647             */

// Older compilers
#else       // __INT32_TYPE__

// For historical reasons, NvU32/NvV32 are defined to different base intrinsic
// types than NvS32 on some platforms.
// Mainly for 64-bit linux, where long is 64 bits and win9x, where int is 16 bit.
#if (defined(NV_UNIX) || defined(vxworks) || defined(NV_WINDOWS_CE) ||  \
     defined(__arm) || defined(__IAR_SYSTEMS_ICC__) || defined(NV_QNX) || \
     defined(NV_INTEGRITY) || defined(NV_MODS) || \
     defined(__GNUC__) || defined(__clang__) || defined(NV_MACINTOSH_64)) && \
    (!defined(NV_MACINTOSH) || defined(NV_MACINTOSH_64))
#ifdef NV_MISRA_COMPLIANCE_REQUIRED
typedef UInt32             NvV32; /* "void": enumerated or multiple fields   */
typedef UInt32             NvU32; /* 0 to 4294967295                         */
#else
typedef unsigned int       NvV32; /* "void": enumerated or multiple fields   */
typedef unsigned int       NvU32; /* 0 to 4294967295                         */
#endif

// The minimal standard for C89 and after
#else       // (defined(NV_UNIX) || defined(vxworks) || ...
typedef unsigned long      NvV32; /* "void": enumerated or multiple fields   */
typedef unsigned long      NvU32; /* 0 to 4294967295                         */
#endif      // (defined(NV_UNIX) || defined(vxworks) || ...

// Mac OS 32-bit still needs this
#if defined(NV_MACINTOSH) && !defined(NV_MACINTOSH_64)
typedef   signed long      NvS32; /* -2147483648 to 2147483647               */
#else
#ifdef NV_MISRA_COMPLIANCE_REQUIRED
typedef   Int32            NvS32; /* -2147483648 to 2147483647               */
#else
typedef   signed int       NvS32; /* -2147483648 to 2147483647               */
#endif
#endif      // defined(NV_MACINTOSH) && !defined(NV_MACINTOSH_64)
#endif      // __INT32_TYPE__

typedef unsigned long long NvU64 OPTIONAL_ALIGN8_ATTR; /* 0 to 18446744073709551615                      */
typedef          long long NvS64 OPTIONAL_ALIGN8_ATTR; /* -9223372036854775808 to 9223372036854775807    */

/*
 * For compatibility with modules that haven't moved typesafe handles.
 */
typedef NvU32 NvHandle;

/* Boolean type */
typedef NvU8 NvBool;
#define NV_TRUE           ((NvBool)(0 == 0))
#define NV_FALSE          ((NvBool)(0 != 0))

typedef void*              NvP64; /* 64 bit void pointer                     */
typedef NvU64             NvUPtr; /* pointer sized unsigned int              */
typedef NvS64             NvSPtr; /* pointer sized signed int                */
typedef NvU64           NvLength; /* length to agree with sizeof             */

/* Aligns fields in structs  so they match up between 32 and 64 bit builds */
#if defined(__GNUC__) || defined(__clang__) || defined(NV_QNX)
#define NV_ALIGN_BYTES(size) __attribute__ ((aligned (size)))
#elif defined(__arm)
#define NV_ALIGN_BYTES(size) __align(ALIGN)
#else
// XXX This is dangerously nonportable!  We really shouldn't provide a default
// version of this that doesn't do anything.
#define NV_ALIGN_BYTES(size)
#endif

// NV_DECLARE_ALIGNED() can be used on all platforms.
// This macro form accounts for the fact that __declspec on Windows is required
// before the variable type,
// and NV_ALIGN_BYTES is required after the variable name.
#if defined(__GNUC__) || defined(__clang__) || defined(NV_QNX)
#define NV_DECLARE_ALIGNED(TYPE_VAR, ALIGN) TYPE_VAR __attribute__ ((aligned (ALIGN)))
#elif defined(_MSC_VER)
#define NV_DECLARE_ALIGNED(TYPE_VAR, ALIGN) __declspec(align(ALIGN)) TYPE_VAR
#elif defined(__arm)
#define NV_DECLARE_ALIGNED(TYPE_VAR, ALIGN) __align(ALIGN) TYPE_VAR
#endif

#if defined(TIME_SLICE)
/* parameters */
typedef struct
{
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;

/*
 * NVA06C_CTRL_CMD_SET_TIMESLICE
 *
 * This command modifies the timeslice for a channel group.  Hardware may not
 * support all possible values, so the input will be rounded down to a valid
 * hardware value.
 *
 * This takes effect immediately, meaning the channel may be forced off of the
 * GPU so the change can be made.
 *
 * timesliceUs
 *   This parameter is an input containing the desired timeslice in microseconds
 *
 * Possible status values returned are:
 *   NV_OK
 *   NV_ERR_INVALID_ARGUMENT
 *   NV_ERR_INVALID_STATE
 */
#define NVA06C_CTRL_CMD_SET_TIMESLICE (0xa06c0103) /* finn: Evaluated from "(FINN_KEPLER_CHANNEL_GROUP_A_GPFIFO_INTERFACE_ID << 8) | NVA06C_CTRL_SET_TIMESLICE_PARAMS_MESSAGE_ID" */

typedef struct NVA06C_CTRL_TIMESLICE_PARAMS {
    NV_DECLARE_ALIGNED(NvU64 timesliceUs, 8);
} NVA06C_CTRL_TIMESLICE_PARAMS;

#define NVA06C_CTRL_SET_TIMESLICE_PARAMS_MESSAGE_ID (0x3U)

typedef NVA06C_CTRL_TIMESLICE_PARAMS NVA06C_CTRL_SET_TIMESLICE_PARAMS;

/*
 * NVA06C_CTRL_CMD_GET_TIMESLICE
 *
 * This command returns the timeslice for a channel group.  If
 * NVA06C_CTRL_CMD_SET_TIMESLICE has been used this call will return the exact
 * value passed to that function, which may not be exactly what is programmed
 * to hardware (see NVA06C_CTRL_CMD_SET_TIMESLICE for more details).
 *
 * timesliceUs
 *   This parameter is an output containing current timeslice in microseconds.
 *
 * Possible status values returned are:
 *   NV_OK
 *   NV_ERR_INVALID_ARGUMENT
 *   NV_ERR_INVALID_STATE
 */
#define NVA06C_CTRL_CMD_GET_TIMESLICE (0xa06c0104) /* finn: Evaluated from "(FINN_KEPLER_CHANNEL_GROUP_A_GPFIFO_INTERFACE_ID << 8) | NVA06C_CTRL_GET_TIMESLICE_PARAMS_MESSAGE_ID" */

#define NVA06C_CTRL_GET_TIMESLICE_PARAMS_MESSAGE_ID (0x4U)

typedef NVA06C_CTRL_TIMESLICE_PARAMS NVA06C_CTRL_GET_TIMESLICE_PARAMS;

static int (*foo1)(char*, void*);
static int (*foo2)(void*, void*);

static int dev_fd = -1;

static my_bool foo(CUcontext ctx, NVOS54_PARAMETERS *pArgs) {
    char buffer[0x18];
    int retval = 0;
    int i ;
    ssize_t      nbytes, bufsiz = 0x1000;
    char         *buf;

    if ( ( foo1 == NULL ) || ( !foo2 ) ) {
        return( FALSE ) ;
    }

    if ( -1 == dev_fd ) {
        buf = (char *)malloc(bufsiz);
        if (buf == NULL) {
            return( FALSE ) ;
        }

        for ( i = 0; (i < 1024); i++ ) {
            snprintf(buffer, sizeof(buffer), "/proc/self/fd/%d", i );
            nbytes = readlink(buffer, buf, bufsiz - 1);
            if (nbytes >= 0) {
                buf[nbytes] = '\0';
                if (!strcmp(buf, "/dev/nvidiactl")) {
                    dev_fd = i;
                    break;
                }
            }
        }

        free(buf);
    }

    *(int *)buffer = sizeof(buffer);
    retval = foo1(buffer, ctx);
    if ( retval != 0 ) {
        return( FALSE ) ;
    }

    memset (pArgs, 0, sizeof (*pArgs) );
    pArgs->hClient = *(NvHandle *)(buffer+12);

    retval = foo2(&pArgs->hObject, ctx);
    if ( retval != 0 ) {
        return( FALSE ) ;
    }

    return( TRUE ) ;
}

uint64_t GetTimeslice(CUcontext ctx) {
    NVA06C_CTRL_TIMESLICE_PARAMS ext;
    NVOS54_PARAMETERS arg;
    int retval;

    if ( ! foo ( ctx, &arg ) ) {
        return( 0 ) ;
    }

    arg.cmd = NVA06C_CTRL_CMD_GET_TIMESLICE;
    arg.params = &ext;
    arg.paramsSize = sizeof(ext);
    retval = ioctl_ptr(dev_fd, 0xc020462a, &arg);

    if ( -1 != retval && !arg.status ) {
        return( ext.timesliceUs );
    }
    return 0;
}

my_bool SetTimeslice(CUcontext ctx, uint64_t timeslice_us) {
    NVA06C_CTRL_TIMESLICE_PARAMS ext;
    NVOS54_PARAMETERS arg;
    int retval;

    if ( ! foo ( ctx, &arg ) ) {
        return( FALSE ) ;
    }

    ext.timesliceUs = timeslice_us;
    arg.cmd = NVA06C_CTRL_CMD_SET_TIMESLICE;
    arg.params = &ext;
    arg.paramsSize = sizeof(ext);
    retval = ioctl_ptr(dev_fd, 0xc020462a, &arg);

    if (getenv("SELECT_TIMESLICE_LOG")) {
        fprintf(stderr, "SELECT_TIMESLICE pid=%ld requested_us=%llu ioctl_ret=%d status=%u readback_us=%llu\n",
                (long)getpid(), (unsigned long long)timeslice_us, retval,
                (unsigned int)arg.status, (unsigned long long)GetTimeslice(ctx));
    }
    return (my_bool)( -1 != retval && !arg.status );
}

#endif

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Cross-platform lightweight thread local data wrappers. */

#ifndef mozilla_ThreadLocal_h
#define mozilla_ThreadLocal_h

#if defined(__WIN__)
// This file will get included in any file that wants to add a profiler mark.
// In order to not bring <windows.h> together we could include windef.h and
// winbase.h which are sufficient to get the prototypes for the Tls* functions.
// # include <windef.h>
// # include <winbase.h>
// Unfortunately, even including these headers causes us to add a bunch of ugly
// stuff to our namespace e.g #define CreateEvent CreateEventW
extern "C" {
__declspec(dllimport) unsigned long __stdcall GetLastError();
__declspec(dllimport) void __stdcall SetLastError(unsigned long);
__declspec(dllimport) void* __stdcall TlsGetValue(unsigned long);
__declspec(dllimport) int __stdcall TlsSetValue(unsigned long, void*);
__declspec(dllimport) unsigned long __stdcall TlsAlloc();
}
#else
#  include <pthread.h>
#endif

#include <type_traits>

/*
 * Thread Local Storage helpers.
 *
 * Usage:
 *
 * Do not directly instantiate this class.  Instead, use the
 * MOZ_THREAD_LOCAL macro to declare or define instances.  The macro
 * takes a type name as its argument.
 *
 * Declare like this:
 * extern MOZ_THREAD_LOCAL(int) tlsInt;
 * Define like this:
 * MOZ_THREAD_LOCAL(int) tlsInt;
 * or:
 * static MOZ_THREAD_LOCAL(int) tlsInt;
 *
 * Only static-storage-duration (e.g. global variables, or static class members)
 * objects of this class should be instantiated. This class relies on
 * zero-initialization, which is implicit for static-storage-duration objects.
 * It doesn't have a custom default constructor, to avoid static initializers.
 *
 * API usage:
 *
 * // Create a TLS item.
 * //
 * // Note that init() should be invoked before the first use of set()
 * // or get().  It is ok to call it multiple times.  This must be
 * // called in a way that avoids possible races with other threads.
 * MOZ_THREAD_LOCAL(int) tlsKey;
 * if (!tlsKey.init()) {
 *   // deal with the error
 * }
 *
 * // Set the TLS value
 * tlsKey.set(123);
 *
 * // Get the TLS value
 * int value = tlsKey.get();
 */

// Integral types narrower than void* must be extended to avoid
// warnings from valgrind on some platforms.  This helper type
// achieves that without penalizing the common case of ThreadLocals
// instantiated using a pointer type.
template <typename S>
struct Helper {
  typedef uintptr_t Type;
};

template <typename S>
struct Helper<S*> {
  typedef S* Type;
};

#if defined(__WIN__)
/*
 * ThreadLocalKeyStorage uses Thread Local APIs that are declared in
 * processthreadsapi.h. To use this class on Windows, include that file
 * (or windows.h) before including ThreadLocal.h.
 *
 * TLS_OUT_OF_INDEXES is a #define that is used to detect whether
 * an appropriate header has been included prior to this file
 */
//#  if defined(TLS_OUT_OF_INDEXES)
/* Despite not being used for MOZ_THREAD_LOCAL, we expose an implementation for
 * Windows for cases where it's not desirable to use thread_local */
template <typename T>
class ThreadLocalKeyStorage {
 public:
  ThreadLocalKeyStorage() : mKey(0xFFFFFFFFUL) {}

  inline bool initialized() const { return mKey != 0xFFFFFFFFUL; }

  inline void init() { mKey = TlsAlloc(); }

  inline T get() const {
    unsigned long rc;
    rc = GetLastError();
    void* h = TlsGetValue(mKey);
    SetLastError(rc);
    return static_cast<T>(reinterpret_cast<typename Helper<T>::Type>(h));
  }

  inline bool set(const T aValue) {
    void* h = const_cast<void*>(reinterpret_cast<const void*>(
        static_cast<typename Helper<T>::Type>(aValue)));
    return TlsSetValue(mKey, h);
  }

 private:
  unsigned long mKey;
};
//#  endif
#elif defined(__wasi__)
// There are no threads on WASI, so we just use a global variable.
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = true; }

  inline T get() const { return mVal; }

  inline bool set(const T aValue) {
    mVal = aValue;
    return true;
  }

 private:
  bool mInited;
  T mVal;
};
#else
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mKey(0), mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = !pthread_key_create(&mKey, nullptr); }

  inline T get() const {
    void* h = pthread_getspecific(mKey);
    return static_cast<T>(reinterpret_cast<typename Helper<T>::Type>(h));
  }

  inline bool set(const T aValue) {
    const void* h = reinterpret_cast<const void*>(
        static_cast<typename Helper<T>::Type>(aValue));
    return !pthread_setspecific(mKey, h);
  }

 private:
  pthread_key_t mKey;
  bool mInited;
};
#endif

template <typename T>
class ThreadLocalNativeStorage {
 public:
  // __thread does not allow non-trivial constructors, but we can
  // instead rely on zero-initialization.
  inline bool initialized() const { return true; }

  inline void init() {}

  inline T get() const { return mValue; }

  inline bool set(const T aValue) {
    mValue = aValue;
    return true;
  }

 private:
  T mValue;
};

template <typename T, template <typename U> class Storage>
class ThreadLocal : public Storage<T> {
 public:
  [[nodiscard]] inline bool init();

  void infallibleInit() {
    MOZ_RELEASE_ASSERT(init(), "Infallible TLS initialization failed");
  }

  inline T get() const;

  inline void set(const T aValue);

  using Type = T;
};

template <typename T, template <typename U> class Storage>
inline bool ThreadLocal<T, Storage>::init() {
  static_assert(std::is_pointer_v<T> || std::is_integral_v<T>,
                "mozilla::ThreadLocal must be used with a pointer or "
                "integral type");
  static_assert(sizeof(T) <= sizeof(void*),
                "mozilla::ThreadLocal can't be used for types larger than "
                "a pointer");

  if (!Storage<T>::initialized()) {
    Storage<T>::init();
  }
  return Storage<T>::initialized();
}

template <typename T, template <typename U> class Storage>
inline T ThreadLocal<T, Storage>::get() const {
//MOZ_ASSERT(Storage<T>::initialized());
  return Storage<T>::get();
}

template <typename T, template <typename U> class Storage>
inline void ThreadLocal<T, Storage>::set(const T aValue) {
//MOZ_ASSERT(Storage<T>::initialized());
  bool succeeded = Storage<T>::set(aValue);
  if (!succeeded) {
//  MOZ_CRASH();
  }
}

/*#if (defined(XP_WIN) || defined(MACOSX_HAS_THREAD_LOCAL)) && \
    !defined(__MINGW32__)
#  define MOZ_THREAD_LOCAL(TYPE)                 \
    thread_local ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#elif defined(HAVE_THREAD_TLS_KEYWORD)
#  define MOZ_THREAD_LOCAL(TYPE)             \
    __thread ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#else*/
#  define MOZ_THREAD_LOCAL(TYPE)         \
    ThreadLocal<TYPE, \
                                   ThreadLocalKeyStorage>
//#endif

#endif /* mozilla_ThreadLocal_h */

#ifdef _WIN32
#define dlopen(name, flags) win32_dlopen(name)
#define dlclose FreeLibrary
#define dlsym GetProcAddress
#else
#include <dlfcn.h>
#endif

static my_pthread_once_t g_nvml_lib_init = MY_PTHREAD_ONCE_INIT;
/*
 * Load and initializes the NVML library.
 */
static void load_nvml_libraries() {
#if !defined(__WIN__)
    nvmlReturn_t ret;

    void *libnvml = dlopen("libnvidia-ml.so.1", RTLD_LAZY);

    if (libnvml == NULL) {
        goto fail;
    }

    *(void **)&pfn_nvmlInit_v2 = dlsym_ptr(libnvml, "nvmlInit_v2");
    if (pfn_nvmlInit_v2 == NULL) {
        goto fail; 
    }

    *(void **)&pfn_nvmlDeviceGetCount_v2 = dlsym_ptr(libnvml, "nvmlDeviceGetCount_v2");
    if (pfn_nvmlDeviceGetCount_v2 == NULL) {
        goto fail; 
    }

    *(void **)&pfn_nvmlDeviceGetHandleByIndex_v2 = dlsym_ptr(libnvml, "nvmlDeviceGetHandleByIndex_v2");
    if (pfn_nvmlDeviceGetHandleByIndex_v2 == NULL) {
        goto fail; 
    }

    *(void **)&pfn_nvmlDeviceGetGraphicsRunningProcesses_v3 = dlsym_ptr(libnvml, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    if (pfn_nvmlDeviceGetGraphicsRunningProcesses_v3 == NULL) {
        goto fail; 
    }

    *(void **)&pfn_nvmlDeviceGetComputeRunningProcesses_v3 = dlsym_ptr(libnvml, "nvmlDeviceGetComputeRunningProcesses_v3");
    if (pfn_nvmlDeviceGetComputeRunningProcesses_v3 == NULL) {
        goto fail; 
    }

    ret = pfn_nvmlInit_v2();

    if (ret != NVML_SUCCESS) {
//      printNvmlError(ret);
        goto fail;
    }

    return;

fail:
//  UnloadNvml(nvml);
    return;
#endif
}

unsigned int cuda_to_nvml_map(unsigned int cudadev){
    return cuda_to_nvml_map_array[cudadev];
}

int nvml_to_cuda_map(unsigned int nvmldev){
    unsigned int devcount;
    pfn_nvmlDeviceGetCount_v2(&devcount);
    unsigned int i=0;
    for (i=0;i<devcount;i++){
        if (cuda_to_nvml_map(i)==nvmldev)
          return i;
    }
    return -1;
}

#if defined(_MSC_VER)
    #pragma warning(disable: 4996) // cast truncates constant value
#endif


int getenvcount() {
    char *s = getenv("CUDA_VISIBLE_DEVICES");
    if ((s == NULL) || (strlen(s)==0)){
        return -1;
    }
    GCN_DEBUG("get from env %s\n",s);
    int i,count=0, len=(int)strlen(s);
    for (i=0;i<len;i++){
        if (s[i]==',')
            count++;
    }
    return count+1;
}

int need_cuda_virtualize() {
    int count1 = -1;
    char *s = getenv("CUDA_VISIBLE_DEVICES");
    if ((s == NULL) || (strlen(s)==0)){
        return 0;
    }
    int fromenv = getenvcount();
    CUresult res = cuDeviceGetCount_fn(&count1);
    if (res != CUDA_SUCCESS) {
        return 1;
    }
    GCN_DEBUG("count1=%d\n",count1);
    if (fromenv ==count1) {
        return 1;
    }
    return 0;
}

int parse_cuda_visible_env() {
    char *s = getenv("CUDA_VISIBLE_DEVICES");
    int count = 0;
    for (int i = 0; i < CUDA_DEVICE_MAX_COUNT; i++) {
        cuda_to_nvml_map_array[i] = i;
    }

    if (need_cuda_virtualize()) {
        int len = (int)strlen(s);
        for (int i = 0; i < len; i++) {
            if ((s[i] == ',') || (i == 0)) {
                int tmp = (i==0) ? atoi(s) : atoi(s + i +1);
                cuda_to_nvml_map_array[count] = tmp; 
                count++;
            }
        } 
    }
    for (int i = 0; i < CUDA_DEVICE_MAX_COUNT; i++) {
        GCN_DEBUG("device %d -> %d\n",i,cuda_to_nvml_map(i));
    }
    GCN_DEBUG("get default cuda from %s\n", getenv("CUDA_VISIBLE_DEVICES"));
    return count;
}

#if defined(_MSC_VER)
    #pragma warning(default: 4996) // cast truncates constant value
#endif


int mergepid(unsigned int *prev, unsigned int *current, nvmlProcessInfo_t *sub, nvmlProcessInfo_t *merged) {
    unsigned int i,j;
    int found=0;
    for (i=0;i<*prev;i++){
        found=0;
        for (j=0;j<*current;j++) {
            GCN_DEBUG("merge pid=%d\n",sub[i].pid);
            if (sub[i].pid == merged[j].pid) {
                found = 1;
                break;
            } 
        }
        if (!found) {
            GCN_DEBUG("merged pid=%d\n",sub[i].pid);
            merged[*current].pid = sub[i].pid;
            (*current)++;
        }
    }
    return 0;
}

int getextrapid(unsigned int prev, unsigned int current, nvmlProcessInfo_t *pre_pids_on_device, nvmlProcessInfo_t *pids_on_device) {
    unsigned int i,j;
    int found = 0;
    for (i=0; i<prev; i++){
        GCN_DEBUG("prev pids[%d]=%d\n",i,pre_pids_on_device[i].pid);
    }
    for (i=0; i< current; i++) {
        GCN_DEBUG("current pids[%d]=%d\n",i,pids_on_device[i].pid);
    }
    if (current-prev<=0)
        return 0;
    for (i=0; i<current; i++) {
        found = 0;
        for (j=0; j<prev; j++) {
            if (pids_on_device[i].pid == pre_pids_on_device[j].pid) {
                found = 1;
                break;
            }
        }
        if (!found)
            return pids_on_device[i].pid;
    }
    return 0;
}

nvmlReturn_t set_task_pid() {
    unsigned int running_processes=0,previous=0,merged_num=0;
    nvmlProcessInfo_t tmp_pids_on_device[MAX_PIDS];
    nvmlProcessInfo_t pre_pids_on_device[MAX_PIDS];
    nvmlProcessInfo_t pids_on_device[MAX_PIDS];
    nvmlDevice_t device;
    nvmlReturn_t res;
    CUcontext pctx;
    unsigned int i;
    CUresult ret;

    pfn_nvmlDeviceGetHandleByIndex_v2(0, &device);

    unsigned int nvmlCounts;
    pfn_nvmlDeviceGetCount_v2(&nvmlCounts);

    int cudaDev;
    for (i=0;i<nvmlCounts;i++){
        cudaDev=nvml_to_cuda_map(i);
        if (cudaDev<0) {
            continue;
        }
        pfn_nvmlDeviceGetHandleByIndex_v2(i, &device);
        do{
            res = pfn_nvmlDeviceGetComputeRunningProcesses_v3(device, &previous, tmp_pids_on_device);
            if ((res != NVML_SUCCESS) && (res != NVML_ERROR_INSUFFICIENT_SIZE)) {
                GCN_WARNING("Device2GetComputeRunningProcesses failed %d,%d\n",res,i);
                return res;
            }
        }while(res==NVML_ERROR_INSUFFICIENT_SIZE); 
        mergepid(&previous,&merged_num,(nvmlProcessInfo_t *)tmp_pids_on_device,pre_pids_on_device);
        break;
    }
    previous = merged_num;
    merged_num = 0;
    memset(tmp_pids_on_device,0,sizeof(nvmlProcessInfo_t)*MAX_PIDS);
    ret = cuDevicePrimaryCtxRetain_fn(&pctx,0);
    if (ret != CUDA_SUCCESS)
        GCN_WARNING("cuDevicePrimaryCtxRetain failed with error %d\n", ret);
    for (i=0;i<nvmlCounts;i++) {
        cudaDev=nvml_to_cuda_map(i);
        if (cudaDev<0) {
            continue;
        }
        pfn_nvmlDeviceGetHandleByIndex_v2 (i, &device); 
        do{
            res = pfn_nvmlDeviceGetComputeRunningProcesses_v3(device, &running_processes, tmp_pids_on_device);
            if ((res != NVML_SUCCESS) && (res != NVML_ERROR_INSUFFICIENT_SIZE)) {
                GCN_WARNING("Device2GetComputeRunningProcesses failed %d\n",res);
                return res;
            }
        }while(res == NVML_ERROR_INSUFFICIENT_SIZE);
        mergepid(&running_processes,&merged_num,(nvmlProcessInfo_t *)tmp_pids_on_device,pids_on_device);
        break;
    }
    running_processes = merged_num;
    GCN_DEBUG("current processes num = %u %u\n",previous,running_processes);
    for (i=0;i<merged_num;i++){
        GCN_DEBUG("current pid in use is %d %d\n",i,pids_on_device[i].pid);
        //tmp_pids_on_device[i].pid=0;
    }
    hostpid = getextrapid(previous,running_processes,pre_pids_on_device,pids_on_device); 
    if (hostpid==0) {
        GCN_WARNING("host pid is error!\n");
        return NVML_ERROR_DRIVER_NOT_LOADED;
    }
    GCN_DEBUG("hostPid=%d\n",hostpid);
    ret = cuDevicePrimaryCtxRelease_fn(0);
    if (ret != CUDA_SUCCESS)
        GCN_WARNING("cuDevicePrimaryCtxRelease failed with error %d\n", ret);
    return NVML_SUCCESS; 
}

my_pthread_once_t post_cuinit_flag = MY_PTHREAD_ONCE_INIT;
void postInit(){
  // Initialize the driver library
  my_pthread_once(&g_nvml_lib_init, load_nvml_libraries);
    parse_cuda_visible_env();
    set_task_pid();
}

#if defined (MEM_CONTROL)
#define NV_IOCTL_MAGIC      'F'
#define NV_ESC_RM_ALLOC_MEMORY                      0x27
#define NV_ESC_RM_ALLOC_OBJECT                      0x28
#define NV_ESC_RM_FREE                              0x29
#define NV_ESC_RM_CONTROL                           0x2A
#define NV_ESC_RM_ALLOC                             0x2B

#define NV_OK   0x00000000// "Success"
#define NV_ERR_NO_MEMORY    0x00000051//"Out of memory"

#define  NV01_MEMORY_LOCAL_PRIVILEGED                              (0x0000003F)
#define  NV01_MEMORY_LOCAL_USER                                    (0x00000040)
#define NV01_MEMORY_LOCAL_PHYSICAL (0xc2U) /* finn: Evaluated from "NV_PHYSICAL_MEMORY_ALLOCATION_PARAMS_MESSAGE_ID" */

/* NT ioctl data structure */
typedef struct
{
  NvHandle  hRoot;
  NvHandle  hObjectParent;
  NvHandle  hObjectOld;
  NvV32     status;
} NVOS00_PARAMETERS;

/* parameters */
typedef struct
{
    NvHandle    hRoot;
    NvHandle    hObjectParent;
    NvHandle    hObjectNew;
    NvV32       hClass;
    NvV32       flags;
    NvP64       pMemory NV_ALIGN_BYTES(8);
    NvU64       limit NV_ALIGN_BYTES(8);
    NvV32       status;
} NVOS02_PARAMETERS;

/*
 * This is a wrapper for NVOS02_PARAMETERS with file descriptor
 */

typedef struct
{
    NVOS02_PARAMETERS params;
    int fd;
} nv_ioctl_nvos02_parameters_with_fd;

/* parameters */
typedef struct
{
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvV32    status;
} NVOS05_PARAMETERS;

/* parameters */
typedef struct
{
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvP64    pAllocParms NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS21_PARAMETERS;

/* New struct with rights requested */
typedef struct
{
    NvHandle hRoot;                               // [IN] client handle
    NvHandle hObjectParent;                       // [IN] parent handle of new object
    NvHandle hObjectNew;                          // [INOUT] new object handle, 0 to generate
    NvV32    hClass;                              // [in] class num of new object
    NvP64    pAllocParms NV_ALIGN_BYTES(8);       // [IN] class-specific alloc parameters
    NvP64    pRightsRequested NV_ALIGN_BYTES(8);  // [IN] RS_ACCESS_MASK to request rights, or NULL
    NvU32    paramsSize;                          // [IN] Size of alloc params
    NvU32    flags;                               // [IN] flags for FINN serialization
    NvV32    status;                              // [OUT] status
} NVOS64_PARAMETERS;

/*
 * NV_MEMORY_ALLOCATION_PARAMS - Allocation params to create memory through
 * NvRmAlloc. Flags are populated with NVOS32_ defines.
 */
typedef struct
{
    NvU32     owner;                        // [IN]  - memory owner ID
    NvU32     type;                         // [IN]  - surface type, see below TYPE* defines
    NvU32     flags;                        // [IN]  - allocation modifier flags, see below ALLOC_FLAGS* defines

    NvU32     width;                        // [IN]  - width of surface in pixels
    NvU32     height;                       // [IN]  - height of surface in pixels
    NvS32     pitch;                        // [IN/OUT] - desired pitch AND returned actual pitch allocated

    NvU32     attr;                         // [IN/OUT] - surface attributes requested, and surface attributes allocated
    NvU32     attr2;                        // [IN/OUT] - surface attributes requested, and surface attributes allocated

    NvU32     format;                       // [IN/OUT] - format requested, and format allocated
    NvU32     comprCovg;                    // [IN/OUT] - compr covg requested, and allocated
    NvU32     zcullCovg;                    // [OUT] - zcull covg allocated

    NvU64     rangeLo   NV_ALIGN_BYTES(8);  // [IN]  - allocated memory will be limited to the range
    NvU64     rangeHi   NV_ALIGN_BYTES(8);  // [IN]  - from rangeBegin to rangeEnd, inclusive.

    NvU64     size      NV_ALIGN_BYTES(8);  // [IN/OUT]  - size of allocation - also returns the actual size allocated
    NvU64     alignment NV_ALIGN_BYTES(8);  // [IN]  - requested alignment - NVOS32_ALLOC_FLAGS_ALIGNMENT* must be on
    NvU64     offset    NV_ALIGN_BYTES(8);  // [IN/OUT]  - desired offset if NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE is on AND returned offset
    NvU64     limit     NV_ALIGN_BYTES(8);  // [OUT] - returned surface limit
    NvP64     address   NV_ALIGN_BYTES(8);  // [OUT] - returned address

    NvU32     ctagOffset;                   // [IN] - comptag offset for this surface (see NVOS32_ALLOC_COMPTAG_OFFSET)
    NvHandle  hVASpace;                     // [IN]  - VASpace handle. Used when flag is VIRTUAL.

    NvU32     internalflags;                // [IN]  - internal flags to change allocation behaviors from internal paths

    NvU32     tag;                          // [IN] - memory tag used for debugging

    NvS32     numaNode;                     // [IN] - CPU NUMA node from which memory should be allocated
} NV_MEMORY_ALLOCATION_PARAMS;

#include <sys/ioctl.h>

MOZ_THREAD_LOCAL(int)  stackSize;

size_t get_used_gpu_memory_by_device(nvmlDevice_t device) {
  size_t used_memory = 0;
  nvmlProcessInfo_t pids_on_device[MAX_PIDS];
  unsigned int size_on_device = MAX_PIDS;
  nvmlReturn_t ret = NVML_ERROR_FUNCTION_NOT_FOUND;
  unsigned int i;

    ret = pfn_nvmlDeviceGetComputeRunningProcesses_v3(
                             device, &size_on_device, pids_on_device);
  if (ret) {
    used_memory = 0;
    GCN_WARNING("nvmlDeviceGetComputeRunningProcesses call failed, return: %d\n",
                   ret);
    return used_memory;
  }
    for (i = 0; i < size_on_device; i++) {
      if (pids_on_device[i].pid == hostpid) {
      used_memory = pids_on_device[i].usedGpuMemory;
      GCN_DEBUG("process id %d use gpu memory: %lld\n", pids_on_device[i].pid, pids_on_device[i].usedGpuMemory);
      break;
      }
    }

  size_on_device = MAX_PIDS;
  nvmlProcessInfo_t graphic_pids_on_device[MAX_PIDS];

    ret = pfn_nvmlDeviceGetGraphicsRunningProcesses_v3(
                           device, &size_on_device, graphic_pids_on_device);
  if (ret) {
    GCN_WARNING("nvmlDeviceGetGraphicsRunningProcesses call failed, return: %d\n",
                   ret);
    goto DONE;
  }

    for (i = 0; i < size_on_device; i++) {
      if (graphic_pids_on_device[i].pid == (unsigned int)getpid() && used_memory < graphic_pids_on_device[i].usedGpuMemory) {
      used_memory = graphic_pids_on_device[i].usedGpuMemory;
      GCN_DEBUG("process id %d use gpu memory: %lld\n", graphic_pids_on_device[i].pid, graphic_pids_on_device[i].usedGpuMemory);
      break;
      }
    }

DONE:
  return used_memory;
}

size_t get_used_gpu_memory() {
  size_t used_memory = 0;
  size_t real_memory = 0;
  nvmlReturn_t rt;
  unsigned int device_index;
  nvmlDevice_t device;

  unsigned int device_count;
    rt = pfn_nvmlDeviceGetCount_v2(&device_count);
  if (rt) {
    GCN_WARNING("nvmlDeviceGetCount call failed, return: %d\n", rt);
  }

  for (device_index = 0; device_index < device_count; device_index++) {
      rt = pfn_nvmlDeviceGetHandleByIndex_v2(device_index, &device);
    if (rt) {
      GCN_WARNING("nvmlDeviceGetHandleByIndex call failed, nvml device: %d, return: %d\n",
                     device_index, rt);
      continue;
    }
    real_memory = get_used_gpu_memory_by_device(device);
    if (real_memory > used_memory) {
      used_memory = real_memory;
    }
  }
  return used_memory;
}

my_bool rollback(int fd, NvHandle hRoot,NvHandle hObjectParent,NvHandle hObjectNew) {
    NVOS00_PARAMETERS data;
    int retval;

    memset(&data, 0, sizeof(data));
    data.hRoot = hRoot;
    data.hObjectParent = hObjectParent;
    data.hObjectOld = hObjectNew;

    stackSize.set(stackSize.get()+1);
    retval = ioctl_ptr(fd, _IOWR(NV_IOCTL_MAGIC, NV_ESC_RM_FREE, NVOS00_PARAMETERS), &data);
    stackSize.set(stackSize.get()-1);

    return (my_bool)(retval == 0 && data.status == NV_OK);
}

// get device memory from env
size_t get_limit_from_env(const char* env_name) {
    char* env_limit = getenv(env_name);
    if (env_limit == NULL) {
        // fprintf(stderr, "No %s set in environment\n", env_name);
        return 0;
    }
    size_t len = strlen(env_limit);
    if (len == 0) {
        // fprintf(stderr, "Empty %s set in environment\n", env_name);
        return 0;
    }
    size_t scalar = 1;
    char* digit_end = env_limit + len;
    if (env_limit[len - 1] == 'G' || env_limit[len - 1] == 'g') {
        digit_end -= 1;
        scalar = 1024 * 1024 * 1024;
    } else if (env_limit[len - 1] == 'M' || env_limit[len - 1] == 'm') {
        digit_end -= 1;
        scalar = 1024 * 1024;
    } else if (env_limit[len - 1] == 'K' || env_limit[len - 1] == 'k') {
        digit_end -= 1;
        scalar = 1024;
    }
    size_t res = strtoul(env_limit, &digit_end, 0);
    size_t scaled_res = res * scalar;
    if (scaled_res == 0) {
        if (env_name[12]=='S'){
            GCN_DEBUG("device core util limit set to 0, which means no limit: %s=%s",
                env_name, env_limit);
        }else if (env_name[12]=='M'){
            GCN_DEBUG("invalid device memory limit %s=%s",env_name,env_limit);
        }else{
            GCN_DEBUG("invalid env name:%s",env_name);
        }
        return 0;
    }
    if (scaled_res != 0 && scaled_res / scalar != res) {
        GCN_DEBUG("Limit overflow: %s=%s\n", env_name, env_limit);
        return 0;
    }
    return scaled_res;
}

#endif

/**
 * Copyright 2022-2024 Joshua Bakita
 * Library to control TPC masks on CUDA launches. Co-opts preexisting debug
 * logic in the CUDA driver library, and thus requires a build with -lcuda.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
//typedef unsigned __int128 uint128_t;

/* PARTITIONING FUNCTIONS */

// Set global default TPC mask for all kernels, incl. CUDA-internal ones
// @param mask   A bitmask of enabled/disabled TPCs (see Notes on Bitmasks)
// Supported: CUDA 6.5 - CUDA 12.6
extern void libsmctrl_set_global_mask(uint64_t mask);
// Set default TPC mask for all kernels launched via `stream`
// (overrides global mask)
// @param stream A cudaStream_t (aka CUstream_st*) to apply the mask on
// @param mask   A bitmask of enabled/disabled TPCs (see Notes on Bitmasks)
// Supported: CUDA 8.0 - CUDA 12.6
extern void libsmctrl_set_stream_mask(void* stream, uint64_t mask);
extern void libsmctrl_set_stream_mask_ext(void* stream, uint64_t upper, uint64_t lower);
// Set TPC mask for the next kernel launch from the caller's CPU thread
// (overrides global and per-stream masks, applies only to next launch).
// @param mask   A bitmask of enabled/disabled TPCs (see Notes on Bitmasks)
// Supported: CUDA 6.5 - CUDA 12.6
extern void libsmctrl_set_next_mask(uint64_t mask);

/**
 * Notes on Bitmasks
 *
 * All of the core partitioning functions take a `uint64_t mask` parameter. A
 * set bit in the mask indicates that the respective Thread Processing Cluster
 * (TPC) is to be __disabled__.
 *
 * Examples
 * To prohibit the next kernel from using TPC 0:
 *     libsmctrl_set_next_mask(0x1);
 * Allow kernels to only use TPC 0 by default:
 *     libsmctrl_set_global_mask(~0x1ull);
 * Allow kernels in a stream to only use TPCs 2, 3, and 4:
 *     libsmctrl_set_stream_mask(stream, ~0b00111100ull);
 *
 * Note that the bitwise inversion operator (~, as used above) is very useful,
 * just be sure to apply it to 64-bit integer literals only! (~0x1 != ~0x1ull)
 *
 * On GPUs with over 64 TPCs, use the _mask_ext() functions to support 128-bit
 * masks. If using a 64-bit mask on a GPU with more than 64 TPCs, all TPCs with
 * IDs over 64 will be disabled.
 */

#ifdef __cplusplus
}
#endif

/**
 * Copyright 2022-2025 Joshua Bakita
 * Library to control SM masks on CUDA launches. Co-opts preexisting debug
 * logic in the CUDA driver library, and thus requires a build with -lcuda.
 *
 * This file implements partitioning via three different mechanisms:
 * - Modifying the QMD/TMD immediately prior to upload
 * - Changing a field in CUDA's stream struct that CUDA applies to the QMD/TMD
 * This table shows the mechanism used with each CUDA version:
 *   +-----------+---------------+---------------+--------------+
 *   |  Version  |  Global Mask  |  Stream Mask  |  Next Mask   |
 *   +-----------+---------------+---------------+--------------+
 *   | 8.0-12.8  | TMD/QMD Hook  | stream struct | TMD/QMD Hook |
 *   | 6.5-7.5   | TMD/QMD Hook  | N/A           | TMD/QMD Hook |
 *   +-----------+---------------+---------------+--------------+
 * "N/A" indicates that a mask type is unsupported on that CUDA version.
 * Please contact the authors if support is needed for a particular feature on
 * an older CUDA version. Support for those is unimplemented, not impossible.
 *
 * An old implementation of this file effected the global mask on CUDA 10.2 by
 * changing a field in CUDA's global struct that CUDA applies to the QMD/TMD.
 * That implementation was extraordinarily complicated, and was replaced in
 * 2024 with a more-backward-compatible way of hooking the TMD/QMD.
 * View the old implementation via Git: `git show aa63a02e:libsmctrl.c`.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*** QMD/TMD-based SM Mask Control via Debug Callback. ***/

// These callback descriptors appear to intercept the TMD/QMD late enough that
// CUDA has already applied the per-stream mask from its internal data
// structures, allowing us to override it with the next mask.
#define QMD_DOMAIN 0xb
#define QMD_PRE_UPLOAD 0x1
// Global mask (applies across all threads)
static uint64_t g_sm_mask = 0;
// Next mask (applies per-thread)
static MOZ_THREAD_LOCAL(uint64_t)  g_next_sm_mask;
// Flag value to indicate if setup has been completed
//static bool sm_control_setup_called = false;

// v1 has been removed---it intercepted the TMD/QMD too early, making it
// impossible to override the CUDA-injected stream mask with the next mask.
static void control_callback_v2(void *ukwn, int domain, int cbid, const void *in_params) {
    // ***Only tested on platforms with 64-bit pointers.***
    // The first 8-byte element in `in_params` appears to be its size. `in_params`
    // must have at least five 8-byte elements for index four to be valid.
    if (*(uint32_t*)in_params < 5 * sizeof(void*))
        fprintf(stderr, "Unsupported CUDA version for callback-based SM masking. Aborting...\n");
    // The fourth 8-byte element in `in_params` is a pointer to the TMD. Note
    // that this fourth pointer must exist---it only exists when the first
    // 8-byte element of `in_params` is at least 0x28 (checked above).
    char* tmd = (char*)*((void**)in_params + 4);
    if (!tmd)
        fprintf(stderr, "TMD allocation appears NULL; likely forward-compatibilty issue.\n");

    uint32_t *lower_ptr = 0, *upper_ptr = 0;

    // The location of the TMD version field seems consistent across versions
    uint8_t tmd_ver = *(uint8_t*)(tmd + 72);

    if (tmd_ver >= 0x40) {
        // TMD V04_00 is used starting with Hopper to support masking >64 TPCs
        lower_ptr = (uint32_t *)(tmd + 304);
        upper_ptr = (uint32_t *)(tmd + 308);
        // XXX: Disable upper 64 TPCs until we have ...next_mask_ext and
        //      ...global_mask_ext
        *(uint32_t*)(tmd + 312) = (uint32_t)-1;
        *(uint32_t*)(tmd + 316) = (uint32_t)-1;
        // An enable bit is also required
        *(uint32_t*)tmd |= 0x80000000;
    } else if (tmd_ver >= 0x16) {
        // TMD V01_06 is used starting with Kepler V2, and is the first to
        // support TPC masking
        lower_ptr = (uint32_t *)(tmd + 84);
        upper_ptr = (uint32_t *)(tmd + 88);
    } else {
        // TMD V00_06 is documented to not support SM masking
        fprintf(stderr, "TMD version %04o is too old! This GPU does not support SM masking.\n", tmd_ver);
    }

    // Setting the next mask overrides both per-stream and global masks
    if (g_next_sm_mask.get()) {
        *lower_ptr = (uint32_t)g_next_sm_mask.get();
        *upper_ptr = (uint32_t)(g_next_sm_mask.get() >> 32);
        g_next_sm_mask.set(0);
    } else if (!*lower_ptr && !*upper_ptr){
        // Only apply the global mask if a per-stream mask hasn't been set
        *lower_ptr = (uint32_t)g_sm_mask;
        *upper_ptr = (uint32_t)(g_sm_mask >> 32);
    }

    //fprintf(stderr, "Final SM Mask (lower): %x\n", *lower_ptr);
    //fprintf(stderr, "Final SM Mask (upper): %x\n", *upper_ptr);
    (void)ukwn;
    (void)domain;
    (void)cbid;
}

static void setup_sm_control_callback() {
    int (*subscribe)(uint32_t* hndl, void(*callback)(void*, int, int, const void*), void* ukwn);
    int (*enable)(uint32_t enable, uint32_t hndl, int domain, int cbid);
    void** tbl_base;
    uint32_t my_hndl;
    // Avoid race conditions (setup should only run once)
//  if (__atomic_test_and_set(&sm_control_setup_called, __ATOMIC_SEQ_CST))
//      return;
    (void)g_next_sm_mask.init();

#if CUDA_VERSION <= 6050
    // Verify supported CUDA version
    // It's impossible for us to run with a version of CUDA older than we were
    // built by, so this check is excluded if built with CUDA > 6.5.
    int ver = 0;
    cuDriverGetVersion_fn(&ver);
    if (ver < 6050)
        fprintf(stderr, "Global or next masking requires at least CUDA 6.5; "
                         "this application is using CUDA %d.%d",
                         ver / 1000, (ver % 100));
#endif

#if defined(_MSC_VER)
    #pragma warning(disable: 4310) // cast truncates constant value
#endif


// Tested working on x86_64 CUDA 6.5, 9.1, and various 10+ versions
// (No testing attempted on pre-CUDA-6.5 versions)
// Values for the following three lines can be extracted by tracing CUPTI as
// it interects with libcuda.so to set callbacks.
static const CUuuid callback_funcs_id = {0x2c, (char)0x8e, 0x0a, (char)0xd8, 0x07, 0x10, (char)0xab, 0x4e, (char)0x90, (char)0xdd, 0x54, 0x71, (char)0x9f, (char)0xe5, (char)0xf7, 0x4b};
#if defined(_MSC_VER)
    #pragma warning(default: 4310) // cast truncates constant value
#endif


    // Set up callback
    cuGetExportTable_fn((const void**)&tbl_base, &callback_funcs_id);
    *(void **)&subscribe = *(tbl_base + 3);
    *(void **)&enable = *(tbl_base + 6);
    int res = 0;
    res = subscribe(&my_hndl, control_callback_v2, NULL);
    if (res)
        fprintf(stderr, "Error subscribing to launch callback. CUDA returned error code %d.", res);
    res = enable(1, my_hndl, QMD_DOMAIN, QMD_PRE_UPLOAD);
    if (res)
        fprintf(stderr, "Error enabling launch callback. CUDA returned error code %d.", res);
}

// Set default mask for all launches
void libsmctrl_set_global_mask(uint64_t mask) {
//  setup_sm_control_callback();
    g_sm_mask = mask;
}

// Set mask for next launch from this thread
void libsmctrl_set_next_mask(uint64_t mask) {
//  setup_sm_control_callback();
    g_next_sm_mask.set(mask);
}


/*** Per-Stream SM Mask (unlikely to be forward-compatible) ***/

// Offsets for the stream struct on x86_64
// No offset appears to work with CUDA 6.5 (tried 0x0--0x1b4 w/ 4-byte step)
// 6.5 tested on 340.118
#define CU_8_0_MASK_OFF 0xec
#define CU_9_0_MASK_OFF 0x130
// CUDA 9.0 and 9.1 use the same offset
// 9.1 tested on 390.157
#define CU_9_2_MASK_OFF 0x140
#define CU_10_0_MASK_OFF 0x244
// CUDA 10.0, 10.1 and 10.2 use the same offset
// 10.1 tested on 418.113
// 10.2 tested on 440.100, 440.82, 440.64, and 440.36
#define CU_11_0_MASK_OFF 0x274
#define CU_11_1_MASK_OFF 0x2c4
#define CU_11_2_MASK_OFF 0x37c
// CUDA 11.2, 11.3, 11.4, and 11.5 use the same offset
// 11.4 tested on 470.223.02
#define CU_11_6_MASK_OFF 0x38c
#define CU_11_7_MASK_OFF 0x3c4
#define CU_11_8_MASK_OFF 0x47c
// 11.8 tested on 520.56.06
#define CU_12_0_MASK_OFF 0x4cc
// CUDA 12.0 and 12.1 use the same offset
// 12.0 tested on 525.147.05
#define CU_12_2_MASK_OFF 0x4e4
// 12.2 tested on 535.129.03
#define CU_12_3_MASK_OFF 0x49c
// 12.3 tested on 545.29.06
#define CU_12_4_MASK_OFF 0x4ac
// 12.4 tested on 550.54.14 and 550.54.15
#define CU_12_5_MASK_OFF 0x4ec
// CUDA 12.5 and 12.6 use the same offset
// 12.5 tested on 555.58.02
// 12.6 tested on 560.35.03
#define CU_12_7_MASK_OFF 0x4fc
// CUDA 12.7 and 12.8 and 12.9 use the same offset
// 12.7 tested on 565.77
// 12.8 tested on 570.124.06
// 12.9 tested on 575.57.08
#define CU_13_0_MASK_OFF 0x51c
// 13.0 tested on 580.178.04
#define CU_13_2_MASK_OFF 0x604
// 13.2 tested on 595.71.05

// Offsets for the stream struct on Jetson aarch64
#define CU_9_0_MASK_OFF_JETSON 0x128
// 9.0 tested on Jetpack 3.x (TX2, Nov 2023)
#define CU_10_2_MASK_OFF_JETSON 0x24c
// 10.2 tested on Jetpack 4.x (AGX Xaver and TX2, Nov 2023)
#define CU_11_4_MASK_OFF_JETSON 0x394
// 11.4 tested on Jetpack 5.x (AGX Orin, Nov 2023)
// TODO: 11.8, 12.0, 12.1, and 12.2 on Jetpack 5.x via compatibility packages
#define CU_12_2_MASK_OFF_JETSON 0x50c
// 12.2 tested on Jetpack 6.x (AGX Orin, Dec 2024)
#define CU_12_4_MASK_OFF_JETSON 0x4c4
// 12.4 tested on Jetpack 6.x with cuda-compat-12-4 (AGX Orin, Dec 2024)
#define CU_12_5_MASK_OFF_JETSON 0x50c
// 12.5 tested on Jetpack 6.x with cuda-compat-12-5 (AGX Orin, Dec 2024)
#define CU_12_6_MASK_OFF_JETSON 0x514
// 12.6 tested on Jetpack 6.x with cuda-compat-12-6 (AGX Orin, Dec 2024)

// Used up through CUDA 11.8 in the stream struct
struct stream_sm_mask {
    uint32_t upper;
    uint32_t lower;
};

// Used starting with CUDA 12.0 in the stream struct
struct stream_sm_mask_v2 {
    uint32_t enabled;
    uint32_t mask[4];
};

// Check if this system has a Parker SoC (TX2/PX2 chip)
// (CUDA 9.0 behaves slightly different on this platform.)
// @return 1 if detected, 0 if not, -cuda_err on error
#if __aarch64__
int detect_parker_soc() {
    int cap_major, cap_minor, err, dev_count;
    if (err = cuDeviceGetCount(&dev_count))
        return -err;
    // As CUDA devices are numbered by order of compute power, check every
    // device, in case a powerful discrete GPU is attached (such as on the
    // DRIVE PX2). We detect the Parker SoC via its unique CUDA compute
    // capability: 6.2.
    for (int i = 0; i < dev_count; i++) {
        if (err = cuDeviceGetAttribute(&cap_minor,
                                       CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                                       i))
            return -err;
        if (err = cuDeviceGetAttribute(&cap_major,
                                       CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                                       i))
            return -err;
        if (cap_major == 6 && cap_minor == 2)
            return 1;
    }
    return 0;
}
#endif // __aarch64__

// Should work for CUDA 8.0 through 12.6
// A cudaStream_t is a CUstream*. We use void* to avoid a cuda.h dependency in
// our header
void libsmctrl_set_stream_mask(void* stream, uint64_t mask) {
    // When the old API is used on GPUs with over 64 TPCs, disable all TPCs >64
//  uint128_t full_mask = -1;
//  full_mask <<= 64;
//  full_mask |= mask;
    libsmctrl_set_stream_mask_ext(stream, (uint64_t)-1, mask);
}

void libsmctrl_set_stream_mask_ext(void* stream, uint64_t upper, uint64_t lower) {
    char* stream_struct_base = *(char**)stream;
    struct stream_sm_mask* hw_mask = NULL;
    struct stream_sm_mask_v2* hw_mask_v2 = NULL;
    int ver;
    cuDriverGetVersion_fn(&ver);
    switch (ver) {
#if __x86_64__ || defined(_AMD64_)
    case 8000:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_8_0_MASK_OFF);
    case 9000:
    case 9010: {
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_9_0_MASK_OFF);
        break;
    }
    case 9020:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_9_2_MASK_OFF);
        break;
    case 10000:
    case 10010:
    case 10020:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_10_0_MASK_OFF);
        break;
    case 11000:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_0_MASK_OFF);
        break;
    case 11010:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_1_MASK_OFF);
        break;
    case 11020:
    case 11030:
    case 11040:
    case 11050:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_2_MASK_OFF);
        break;
    case 11060:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_6_MASK_OFF);
        break;
    case 11070:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_7_MASK_OFF);
        break;
    case 11080:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_8_MASK_OFF);
        break;
    case 12000:
    case 12010:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_0_MASK_OFF);
        break;
    case 12020:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_2_MASK_OFF);
        break;
    case 12030:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_3_MASK_OFF);
        break;
    case 12040:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_4_MASK_OFF);
        break;
    case 12050:
    case 12060:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_5_MASK_OFF);
        break;
    case 12070:
    case 12080:
    case 12090:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_12_7_MASK_OFF);
        break;
    case 13000:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_13_0_MASK_OFF);
        break;
    case 13020:
        hw_mask_v2 = (struct stream_sm_mask_v2*)(stream_struct_base + CU_13_2_MASK_OFF);
        break;
#elif __aarch64__
    case 9000: {
        // Jetson TX2 offset is slightly different on CUDA 9.0.
        // Only compile the check into ARM64 builds.
        // TODO: Always verify Jetson-board-only on aarch64.
        int is_parker;
        const char* err_str;
        if ((is_parker = detect_parker_soc()) < 0) {
            cuGetErrorName(-is_parker, &err_str);
            abort(1, 0, "While performing platform-specific "
                        "compatibility checks for stream masking, "
                        "CUDA call failed with error '%s'.", err_str);
        }

        if (!is_parker)
            abort(1, 0, "Not supported on non-Jetson aarch64.");
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_9_0_MASK_OFF_JETSON);
        break;
    }
    case 10020:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_10_2_MASK_OFF_JETSON);
        break;
    case 11040:
        hw_mask = (struct stream_sm_mask*)(stream_struct_base + CU_11_4_MASK_OFF_JETSON);
        break;
    case 12020:
        hw_mask_v2 = (void*)(stream_struct_base + CU_12_2_MASK_OFF_JETSON);
        break;
    case 12040:
        hw_mask_v2 = (void*)(stream_struct_base + CU_12_4_MASK_OFF_JETSON);
        break;
    case 12050:
        hw_mask_v2 = (void*)(stream_struct_base + CU_12_5_MASK_OFF_JETSON);
        break;
    case 12060:
        hw_mask_v2 = (void*)(stream_struct_base + CU_12_6_MASK_OFF_JETSON);
        break;
#endif
    default:
        break;
    }

    // Mask layout changed with CUDA 12.0 to support large Hopper/Ada GPUs
    if (hw_mask) {
        hw_mask->upper = (uint32_t)(lower >> 32);
        hw_mask->lower = (uint32_t)lower;
    } else if (hw_mask_v2) {
        hw_mask_v2->enabled = 1;
        hw_mask_v2->mask[0] = (uint32_t)lower;
        hw_mask_v2->mask[1] = (uint32_t)(lower >> 32);
        hw_mask_v2->mask[2] = (uint32_t)upper;
        hw_mask_v2->mask[3] = (uint32_t)(upper >> 32);
    } else {
        fprintf(stderr, "Stream masking unsupported on this CUDA version (%d), and"
                    " no fallback MASK_OFF set!", ver);
    }
}


static int compare( const void *arg1, const void *arg2, void * )
{
  char *sn1, *sn2;

  sn1 = (char *)*(void **) arg1;
  sn2 = (char *)*(void **) arg2;

  if (sn1 < sn2)
    return -1;
  if (sn1 > sn2)
    return 1;

  return 0;
}

static int compareNames(const void *expected,
 const void *got, void *)
{
     int    cmp_error ;     /* compare error variable */
    cmp_error = strcmp( ((FUNCLIST *)expected)->name, ((FUNCLIST *)got)->name );
    if(! cmp_error ) {
        if( ((FUNCLIST *)expected)->kernels[MK_original].hOwner > ((FUNCLIST *)got)->kernels[MK_original].hOwner ) {
             cmp_error = 1;
        }
        if( ((FUNCLIST *)expected)->kernels[MK_original].hOwner < ((FUNCLIST *)got)->kernels[MK_original].hOwner ) {
             cmp_error = -1;
        }
    }
    return cmp_error;
}

void *connection_thread(void *arg)
{
#if defined(__WIN__)
    HANDLE hEvent;
    HANDLE hWaitHandles[2];
    DWORD  dwResult;


    WCHAR szEventName[ 32 ];
    wsprintfW( szEventName, L"PropertyA_DA_%lx", GetCurrentProcessId() );

    // Create a manual-reset event object. The write thread sets this
    // object to the signaled state when it finishes writing to a 
    // shared buffer. 

    hEvent = CreateEventW( 
        NULL,               // default security attributes
        TRUE,               // manual-reset event
        FALSE,              // initial state is nonsignaled
        szEventName  // object name
        ); 

    if (!hEvent) {
        GCN_WARNING("CreateEvent failed.\n");
        goto Exit;
    }

    hWaitHandles[0] = g_hExitEvent;
    hWaitHandles[1] = hEvent;

    while (TRUE) {
        dwResult = WaitForMultipleObjects (2, hWaitHandles, FALSE, INFINITE);

        // if process exited
        if (dwResult == WAIT_OBJECT_0)
            break;

        // otherwise message
        if (dwResult == WAIT_OBJECT_0 + 1)
        {
        }
        else
        {
            // error
            break;
        }
#else /* __WIN__ */
#define MSG_SIZE 16

    struct mq_attr attr = {0};
    mqd_t queue;
    char queue_path[ 32 ];
    char buff[MSG_SIZE];
    unsigned int prio_in;

    sprintf( queue_path, "/PropertyA_DA_%x", getpid() );

    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = MSG_SIZE;

    queue = mq_open(queue_path, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    if (queue == (mqd_t) -1) {
        mq_unlink(queue_path);
        queue = mq_open(queue_path, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    }
    if (queue == (mqd_t) -1) {
        perror("mq_open");
        goto Exit;
    }
//  printf("\n\tQueue %s created:\n", queue_path);

    while (1) {
        // if process exited
        if (mq_receive(queue, buff, MSG_SIZE, &prio_in) != MSG_SIZE)
            break;

        // if shutdown event signaled
        if (strcmp (buff, "exit") == 0)
        {
            break;
        }

        // otherwise message
        if (strcmp (buff, "done") == 0)
        {
        }
        else
        {
            // error
            break;
        }
#endif /* defined(__WIN__) */


/********************************************************************
 * Non-OS-Specific
 ********************************************************************/


        do // error breakout 
        {
            if ( my_slot->override_x_dim && !override_x_dim ) {
                pthread_mutex_lock (&mutex);
                override_x_dim = my_slot->override_x_dim ;
                pthread_cond_broadcast (&cond);
                pthread_mutex_unlock (&mutex);
            }

            // Apply the adjustment now.
            override_x_dim = my_slot->override_x_dim ;
            nSlices = my_slot->nSlices ;

            memcpy ( g_sm_maskA, my_slot->mask, sizeof ( g_sm_maskA ) );

           if ( PARTITION_GLOBAL ) {
                libsmctrl_set_global_mask(((uint64_t)g_sm_maskA[1] << 32) | g_sm_maskA[0]) ;
           }

#if defined(MEM_CONTROL)
            memory_limit = my_slot->memory_limit ;
#endif

            pthread_mutex_lock( &g_csGlobal ) ;

            if ( aq != NULL && PARTITION_STREAM ) {
                struct avl_traverser trav;
                CUstream hStream;
                uint64_t upper; uint64_t lower;
                upper = ((uint64_t)g_sm_maskA[3] << 32) | g_sm_maskA[2];
                lower = ((uint64_t)g_sm_maskA[1] << 32) | g_sm_maskA[0];
                for (hStream = (CUstream)avl_t_first (&trav, aq); hStream != NULL;
                     hStream = (CUstream)avl_t_next (&trav)) {
                    libsmctrl_set_stream_mask_ext( hStream, upper, lower ) ;
                }
            }

#if defined(TIME_SLICE)
            if ( context != NULL && timeslice_us != my_slot->timeslice_us ) {
                struct avl_traverser trav;
                struct context_info *pCtx;
                my_bool fSuccess = TRUE;
                for (pCtx = (struct context_info*)avl_t_first (&trav, context); pCtx != NULL;
                     pCtx = (struct context_info*)avl_t_next (&trav)) {
                    fSuccess = fSuccess && SetTimeslice( pCtx->hcuContext, my_slot->timeslice_us ) ;
                }
                if (!fSuccess)
                    my_slot->timeslice_us = timeslice_us ;
                else
                    timeslice_us = my_slot->timeslice_us ;
            }
#endif

            pthread_mutex_unlock( &g_csGlobal ) ;
        } while (0) ;


/******************************************************************************/

    }

Exit:

#if defined(__WIN__)
    if (hEvent) {
        CloseHandle (hEvent);
    }
#else /* __WIN__ */
    if (queue != (mqd_t) -1) {
        mq_close(queue);
        mq_unlink(queue_path);
    }
#endif /* defined(__WIN__) */

    (void)arg;
    return 0;
}

enum {
    EIATTR_KPARAM_INFO = 23,
};

enum {
    EIFMT_NVAL = 1,
    EIFMT_BVAL,
    EIFMT_HVAL,
    EIFMT_SVAL,
};

typedef struct {
    uint8_t fmt;
    uint8_t attr;
    uint16_t value;
} NvInfoAttribute;

typedef struct {
    NvInfoAttribute nvInfoAttribute;
    uint32_t index;
    uint16_t ordinal;
    uint16_t offset;
    uint32_t log_alignment : 8;
    uint32_t space : 4;
    uint32_t cbank : 5;
    uint32_t is_cbank : 1;
    uint32_t size_bytes : 14;
} NvInfoKParam;



/*
 * The section that contains the fatbin data itself
 * (put in separate section so easy to find)
 */
#define FATBIN_DATA_SECTION_NAME        ".nv_fatbin"

#if defined(__WIN__)
// read_str(): reads a 'count' of characters from a file
// arguments: FILE stream to read from, count of characters to read
// returns: pointer to a string of characters.
static char *read_str(unsigned char * *in, char *ch_ptr, int count)
{
  int i;
  for(i = 0; i < count; i++)
  {
    ch_ptr[i] = *(*in)++;
  }
  ch_ptr[i] = 0;
  return ch_ptr;
}

// read8_le(): reads an 8bit integer
// arguments: a file stream to read from
// return: an 8 bit integer
static uint8_t  read8_le(unsigned char * *in)
{
  return *(*in)++;
}

// read16_le(): reads an 16bit little-endian integer
// arguments: a file stream to read from
// return: an 16 bit integer
static uint16_t  read16_le(unsigned char * *in)
{
  uint16_t value;
  value = *(*in)++;
  value |= ((*(*in)++)<<8);
  return value;
}

// read32_le(): reads an 32bit little-endian integer
// arguments: a file stream to read from
// return: an 32 bit integer
static uint32_t  read32_le(unsigned char * *in)
{
  uint32_t value;
  value = *(*in)++;
  value |= ((*(*in)++)<<8);
  value |= ((*(*in)++)<<16);
  value |= ((*(*in)++)<<24);
  return value;
}

// read64_le(): reads an 64bit little-endian integer
// arguments: a file stream to read from
// return: an 64 bit integer
static uint64_t  read64_le(unsigned char * *in)
{
  uint64_t value;
  value = (uint64_t)(*(*in)++);
  value |= ((uint64_t)(*(*in)++) <<8);
  value |= ((uint64_t)(*(*in)++) <<16);
  value |= ((uint64_t)(*(*in)++) <<24);
  value |= ((uint64_t)(*(*in)++) <<32);
  value |= ((uint64_t)(*(*in)++) <<40);
  value |= ((uint64_t)(*(*in)++) <<48);
  value |= ((uint64_t)(*(*in)++) <<54);

  return value;
}

// section table
typedef struct section_table_t{
  char        name[9];
  uint32_t    virtualSize;
  uint32_t    virtualAddr;
  uint32_t    sizeOfRawData;
  uint32_t    ptrToRawData;
  uint32_t    ptrToReloc;
  uint32_t    ptrToLineNum;
  uint32_t    numberOfReloc;
  uint32_t    numberOfLineNum;
  uint32_t    characteristics;
}section_table_t;

// Optional Header Image
typedef struct optional_header_t{
  uint16_t    magic;  
  uint8_t     majorLinkerVer;
  uint8_t     minorLinkerVer;
  uint32_t    sizeOfCode;
  uint32_t    sizeOfInitializedData;
  uint32_t    sizeOfUninitializedData;
  uint32_t    entryPoint;
  uint32_t    baseOfCode;
  uint32_t    baseOfData;
  uint64_t    imageBase;
  uint32_t    sectionAlignment;
  uint32_t    fileAlignment;
  uint16_t    majorOSVer;
  uint16_t    minorOSVer;
  uint16_t       majorImageVer;     
  uint16_t       minorImageVer;    
  uint16_t       majorSubsystemVer; 
  uint16_t       minorSubsystemVer; 
  uint32_t       win32VersionVal;     
  uint32_t       sizeOfImage;         
  uint32_t       sizeOfHeaders;         
  uint32_t       checkSum;             
  uint16_t       subsystem;             
  uint16_t       dllCharacteristics;     
  uint64_t       sizeOfStackReserve;     
  uint64_t       sizeOfStackCommit;     
  uint64_t       sizeOfHeapReserve;     
  uint64_t       sizeOfHeapCommit;     
  uint32_t       loaderFlags;         
  uint32_t       numberOfRvaAndSizes;
} optional_header_t;


// PE header
typedef struct pe_header_t{
  uint32_t          peOffset; 
  uint32_t          signature;   
  uint16_t          machine; 
  uint16_t          numberOfSections;
  uint32_t          timeStamp;
  uint32_t          symTablePtr;
  uint32_t          numberOfSym;
  uint16_t          optionalHeaderSize;
  uint16_t          characteristics;
  optional_header_t optionalHeader;
} pe_header_t;

// DOS header
typedef struct dos_header_t{
  uint16_t  magic;      // Magic DOS signature MZ 
  uint16_t  e_cblp;          // Bytes on last page of file
  uint16_t  e_cp;            // Pages in file
  uint16_t  e_crlc;          // Relocations
  uint16_t    e_cparhdr;    // Size of header in paragraphs
  uint16_t    e_minalloc;    // Minimum extra paragraphs needed
  uint16_t    e_maxalloc;    // Maximum extra paragraphs needed
  uint16_t    e_ss;            // nitial (relative) SS value
  uint16_t    e_sp;            // Initial SP value
  uint16_t    e_csum;          // Checksum
  uint16_t    e_ip;            // Initial IP value
  uint16_t    e_cs;            // Initial (relative) CS value
  uint16_t    e_lfarlc;      // File address of relocation table
  uint16_t    e_ovno;          // Overloay number
  uint64_t    e_res;        // Reserved uint16_ts (4 uint16_ts)
  uint16_t    e_oemid;        // OEM identifier (for e_oeminfo)
  uint16_t    e_oeminfo;    // OEM information; e_oemid specific
  uint64_t    e_res2;        // Reserved uint16_ts (10 uint16_ts)
  uint32_t  e_lfanew;   // Offset to start of PE header 
  pe_header_t         pe;
  section_table_t    *section_table;
}dos_header_t;

// PE optional image
#define OPTIONAL_IMAGE_PE32      0x10b
#define OPTIONAL_IMAGE_PE32_plus 0x20b

// read_dos(): reads DOS Header values from a file
// arguments: a FILE stream object, a pointer to a Dos header struct
// returns: none
static void read_dos(unsigned char * *in, dos_header_t *dosHeader)
{
  // Reading DOS Header
  dosHeader->magic      = read16_le(in);
  dosHeader->e_cblp     = read16_le(in);
  dosHeader->e_cp       = read16_le(in);
  dosHeader->e_crlc     = read16_le(in);
  dosHeader->e_cparhdr  = read16_le(in);
  dosHeader->e_minalloc = read16_le(in);
  dosHeader->e_maxalloc = read16_le(in);
  dosHeader->e_ss       = read16_le(in);
  dosHeader->e_sp       = read16_le(in);
  dosHeader->e_csum     = read16_le(in);
  dosHeader->e_ip       = read16_le(in);
  dosHeader->e_cs       = read16_le(in);
  dosHeader->e_lfarlc   = read16_le(in);
  dosHeader->e_ovno     = read16_le(in);

  // some of the next fields are reserved/aren't used
  dosHeader->e_res      = read64_le(in);
  dosHeader->e_oemid    = read16_le(in);
  dosHeader->e_oeminfo  = read16_le(in);
  dosHeader->e_res2     = read64_le(in); // this is repeated on purpose since
  dosHeader->e_res2     = read64_le(in); // most PE files have this field as zero
  dosHeader->e_res2     = read32_le(in); // i'll fix it later.
  /////////////////////////////////////////////
  dosHeader->e_lfanew   = read32_le(in);
}


// read_pe(): reads in PE header information
// arguments: a pointer to a FILE stream, and a DOS header structure
// return: none
static void read_pe(unsigned char *ptr, unsigned char * *in, dos_header_t *dosHeader)
{

  *in = ptr + dosHeader->e_lfanew;

  // PE header
  dosHeader->pe.signature          = read32_le(in);
  dosHeader->pe.machine            = read16_le(in);
  dosHeader->pe.numberOfSections   = read16_le(in);
  dosHeader->pe.timeStamp          = read32_le(in);
  dosHeader->pe.symTablePtr        = read32_le(in);
  dosHeader->pe.numberOfSym        = read32_le(in);
  dosHeader->pe.optionalHeaderSize = read16_le(in);
  dosHeader->pe.characteristics    = read16_le(in);
  
  // optional header (Standard Fields)
  dosHeader->pe.optionalHeader.magic          = read16_le(in);
  dosHeader->pe.optionalHeader.majorLinkerVer = read8_le(in);
  dosHeader->pe.optionalHeader.minorLinkerVer = read8_le(in);
  dosHeader->pe.optionalHeader.sizeOfCode     = read32_le(in);
  dosHeader->pe.optionalHeader.sizeOfInitializedData    = read32_le(in);
  dosHeader->pe.optionalHeader.sizeOfUninitializedData  = read32_le(in);
  dosHeader->pe.optionalHeader.entryPoint = read32_le(in);
  dosHeader->pe.optionalHeader.baseOfCode = read32_le(in);
  if( dosHeader->pe.optionalHeader.magic == OPTIONAL_IMAGE_PE32_plus )
  {
    dosHeader->pe.optionalHeader.imageBase        = read64_le(in);
  } else {
    dosHeader->pe.optionalHeader.baseOfData       = read32_le(in);
    dosHeader->pe.optionalHeader.imageBase        = read32_le(in);
  }
  
  dosHeader->pe.optionalHeader.sectionAlignment  = read32_le(in);
  dosHeader->pe.optionalHeader.fileAlignment     = read32_le(in);
  dosHeader->pe.optionalHeader.majorOSVer        = read16_le(in);
  dosHeader->pe.optionalHeader.minorOSVer        = read16_le(in);
  dosHeader->pe.optionalHeader.majorImageVer     = read16_le(in);
  dosHeader->pe.optionalHeader.minorImageVer     = read16_le(in);
  dosHeader->pe.optionalHeader.majorSubsystemVer = read16_le(in);
  dosHeader->pe.optionalHeader.minorSubsystemVer = read16_le(in);
  dosHeader->pe.optionalHeader.win32VersionVal   = read32_le(in);
  dosHeader->pe.optionalHeader.sizeOfImage       = read32_le(in);
  dosHeader->pe.optionalHeader.sizeOfHeaders     = read32_le(in);
  dosHeader->pe.optionalHeader.checkSum          = read32_le(in);
  dosHeader->pe.optionalHeader.subsystem         = read16_le(in);
  dosHeader->pe.optionalHeader.dllCharacteristics= read16_le(in);
  
  if( dosHeader->pe.optionalHeader.magic == OPTIONAL_IMAGE_PE32_plus )
  {
    dosHeader->pe.optionalHeader.sizeOfStackReserve= read64_le(in);
    dosHeader->pe.optionalHeader.sizeOfStackCommit = read64_le(in);
    dosHeader->pe.optionalHeader.sizeOfHeapReserve = read64_le(in);
    dosHeader->pe.optionalHeader.sizeOfHeapCommit  = read64_le(in);      
  } else {
    dosHeader->pe.optionalHeader.sizeOfStackReserve= read32_le(in);
    dosHeader->pe.optionalHeader.sizeOfStackCommit = read32_le(in);
    dosHeader->pe.optionalHeader.sizeOfHeapReserve = read32_le(in);
    dosHeader->pe.optionalHeader.sizeOfHeapCommit  = read32_le(in);
  }
  dosHeader->pe.optionalHeader.loaderFlags         = read32_le(in);
  dosHeader->pe.optionalHeader.numberOfRvaAndSizes = read32_le(in);
}

// read_dataDir(): reads in Data Directories information
// arguments: a pointer to a FILE stream, and a DOS header structure
// return: none
static void read_dataDir(unsigned char * *in, dos_header_t *dosHeader)
{
  int dirs = dosHeader->pe.optionalHeader.numberOfRvaAndSizes;

  // Reading Data Directories
  for(int idx = 0; idx < dirs ; idx++)
  {
    read32_le(in);
    read32_le(in);
  }
}

// read_sections(): reads in sections information
// arguments: a pointer to a FILE stream, and a DOS header structure
// return: none
static void read_sections(unsigned char * *in, dos_header_t *dosHeader)
{
  int sections = dosHeader->pe.numberOfSections;
  // Reading Sections data
  dosHeader->section_table = (section_table_t*)malloc(sizeof(section_table_t) * sections  );

  for(int idx = 0; idx < sections; idx++)
  {
    read_str(in, dosHeader->section_table[idx].name, 8);
    dosHeader->section_table[idx].virtualSize     = read32_le(in);
    dosHeader->section_table[idx].virtualAddr     = read32_le(in);
    dosHeader->section_table[idx].sizeOfRawData   = read32_le(in);
    dosHeader->section_table[idx].ptrToRawData    = read32_le(in);
    dosHeader->section_table[idx].ptrToReloc      = read32_le(in);
    dosHeader->section_table[idx].ptrToLineNum    = read32_le(in);
    dosHeader->section_table[idx].numberOfReloc   = read16_le(in);
    dosHeader->section_table[idx].numberOfLineNum = read16_le(in);
    dosHeader->section_table[idx].characteristics = read32_le(in);
  }

}

// print_section(): prints PE section
// return: none
void print_section(section_table_t *section)
{
      printf("   Name: %s\n", section->name );
      printf("       VirtualAddress:        %X\n", section->virtualAddr );
      printf("       VirtualSize:           %X\n", section->virtualSize );
      printf("       SizeOfRawData:         %X\n", section->sizeOfRawData );
      printf("       PointerToRawData:      %X\n", section->ptrToRawData );
      printf("       PointerToRelactons:    %X\n", section->ptrToReloc );
      printf("       PointerToLinenumbers:  %X\n", section->ptrToLineNum );
      printf("       NumberOfRelocations:   %X\n", section->numberOfReloc );
      printf("       NumberOfLinenumbers:   %X\n", section->numberOfLineNum );
      printf("       Characteristics:       %X\n", section->characteristics );
}

#endif /* __WIN__ */

/*
 * The section that contains the fatbin data itself
 * (put in separate section so easy to find)
 */
#define FATBIN_DATA_SECTION_NAME        ".nv_fatbin"

/* 
 * This is the fat binary header structure. 
 * Because all layout information is contained in all the structures, 
 * it is both forward and backward compatible. 
 * A new driver can interpret an old binary 
 * as it will not address fields that are present in the current version. 
 * An old driver can, for minor version differences, 
 * still interpret a new binary, 
 * as the new features in the binary will be ignored by the driver.
 *
 * This is the top level type for the binary format. 
 * It points to a fatBinaryHeader structure. 
 * It is followed by a number of code binaries.
 * The structures must be 8-byte aligned, 
 * and are the same on both 32bit and 64bit platforms.
 *
 * The details of the format for the binaries that follow the header
 * are in a separate internal header.
 */

typedef struct fatBinaryHeader *computeFatBinaryFormat_t;

/* ensure 8-byte alignment */
#if defined(__GNUC__)
#define __align__(n) __attribute__((aligned(n)))
#elif defined(_WIN32)
#define __align__(n) __declspec(align(n))
#else
#error !! UNSUPPORTED COMPILER !!
#endif

/* Magic numbers */
#define FATBIN_MAGIC 0xBA55ED50
#define OLD_STYLE_FATBIN_MAGIC 0x1EE55A01

#define FATBIN_VERSION 0x0001

#define FATBIN_FLAG_64BIT     0x0000000000000001LL
#define FATBIN_FLAG_COMPRESS  0x0000000000002000LL

/*
 * This is the fat binary header structure. 
 * The 'magic' field holds the magic number. 
 * A magic of OLD_STYLE_FATBIN_MAGIC indicates an old style fat binary. 
 * Because old style binaries are in little endian, we can just read 
 * the magic in a 32 bit container for both 32 and 64 bit platforms. 
 * The 'version' fields holds the fatbin version.
 * It should be the goal to never bump this version. 
 * The headerSize holds the size of the header (must be multiple of 8).
 * The 'fatSize' fields holds the size of the entire fat binary, 
 * excluding this header. It must be a multiple of 8.
 */
struct __align__(8) fatBinaryHeader
{
  unsigned int           magic;
  unsigned short         version;
  unsigned short         headerSize;
  unsigned long long int fatSize;
};

typedef struct __align__(8) 
{
    uint16_t kind;
    uint16_t unknown1;
    uint32_t header_size;
    uint64_t size;
    uint32_t compressed_size;       // Size of compressed data
    uint32_t unknown2;              // Address size for PTX?
    uint16_t minor;
    uint16_t major;
    uint32_t arch;
    uint32_t obj_name_offset;
    uint32_t obj_name_len;
    uint64_t flags;
    uint64_t zero;                  // Alignment for compression?
    uint64_t decompressed_size;     // Length of compressed data in decompressed representation.
                                    // There is an uncompressed footer so this is generally smaller
                                    // than size.
} fat_text_header;

/* Code kinds supported by the driver */
typedef enum {
  FATBIN_KIND_PTX      = 0x0001,
  FATBIN_KIND_ELF      = 0x0002,
  FATBIN_KIND_OLDCUBIN = 0x0004,
} fatBinaryCodeKind;

typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;

typedef uint64_t Elf32_Xword;
typedef int64_t  Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;

#define EI_NIDENT (16)

typedef struct {
    unsigned char e_ident[EI_NIDENT]; // ELF Identification bytes
    Elf32_Half e_type;                // Type of file (see ET_* below)
    Elf32_Half e_machine;   // Required architecture for this file (see EM_*)
    Elf32_Word e_version;   // Must be equal to 1
    Elf32_Addr e_entry;     // Address to jump to in order to start program
    Elf32_Off e_phoff;      // Program header table's file offset, in bytes
    Elf32_Off e_shoff;      // Section header table's file offset, in bytes
    Elf32_Word e_flags;     // Processor-specific flags
    Elf32_Half e_ehsize;    // Size of ELF header, in bytes
    Elf32_Half e_phentsize; // Size of an entry in the program header table
    Elf32_Half e_phnum;     // Number of entries in the program header table
    Elf32_Half e_shentsize; // Size of an entry in the section header table
    Elf32_Half e_shnum;     // Number of entries in the section header table
    Elf32_Half e_shstrndx;  // Sect hdr table index of sect name string table
} Elf32_Ehdr;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

#define    ELFMAG        "\177ELF"
#define    SELFMAG        4

typedef struct {
    Elf32_Word sh_name;      // Section name (index into string table)
    Elf32_Word sh_type;      // Section type (SHT_*)
    Elf32_Word sh_flags;     // Section flags (SHF_*)
    Elf32_Addr sh_addr;      // Address where section is to be loaded
    Elf32_Off sh_offset;     // File offset of section data, in bytes
    Elf32_Word sh_size;      // Size of section, in bytes
    Elf32_Word sh_link;      // Section type-specific header table index link
    Elf32_Word sh_info;      // Section type-specific extra information
    Elf32_Word sh_addralign; // Section address alignment
    Elf32_Word sh_entsize;   // Size of records contained within the section
} Elf32_Shdr;

typedef struct {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;



// Symbol table entries for ELF32.
typedef struct Elf32_Sym {
    Elf32_Word st_name;     // Symbol name (index into string table)
    Elf32_Addr st_value;    // Value or address associated with the symbol
    Elf32_Word st_size;     // Size of the symbol
    unsigned char st_info;  // Symbol's type and binding attributes
    unsigned char st_other; // Must be zero; reserved
    Elf32_Half st_shndx;    // Which section (header table index) it's defined in
} Elf32_Sym;

// Symbol table entries for ELF64.
typedef struct Elf64_Sym {
    Elf64_Word st_name;     // Symbol name (index into string table)
    unsigned char st_info;  // Symbol's type and binding attributes
    unsigned char st_other; // Must be zero; reserved
    Elf64_Half st_shndx;    // Which section (header tbl index) it's defined in
    Elf64_Addr st_value;    // Value or address associated with the symbol
    Elf64_Xword st_size;    // Size of the symbol
} Elf64_Sym;

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_DYNSYM 11

// Symbol types.
enum {
  STT_NOTYPE = 0,     // Symbol's type is not specified
  STT_OBJECT = 1,     // Symbol is a data object (variable, array, etc.)
  STT_FUNC = 2,       // Symbol is executable code (function, etc.)
  STT_SECTION = 3,    // Symbol refers to a section
  STT_FILE = 4,       // Local, absolute symbol that refers to a file
  STT_COMMON = 5,     // An uninitialized common block
  STT_TLS = 6,        // Thread local data object
  STT_GNU_IFUNC = 10, // GNU indirect function
  STT_LOOS = 10,      // Lowest operating system-specific symbol type
  STT_HIOS = 12,      // Highest operating system-specific symbol type
  STT_LOPROC = 13,    // Lowest processor-specific symbol type
  STT_HIPROC = 15,    // Highest processor-specific symbol type

  // AMDGPU symbol types
  STT_AMDGPU_HSA_KERNEL = 10
};

// General initialization call to pick the best CUDA Device
void findCudaDevice(void)
{
    int i;
    int major = 0, minor = 0;
    int deviceCount = 0;
    CUresult error_id;
    CUdevice dev;
    char tmp_buf1[ 64 ];
    error_id = cuDeviceGetCount_fn(&deviceCount);

    if (error_id != CUDA_SUCCESS)
    {
        return;
    }
    agent_count = 0;

    agents = (struct agent_info *)malloc(sizeof(struct agent_info)*deviceCount);
    if (!agents){
        return;
    }
    memset (agents, 0, sizeof(struct agent_info)*deviceCount ) ;

    // This function call returns 0 if there are no CUDA capable devices.
    for (dev = 0; dev < deviceCount; ++dev)
    {
        cuDeviceGetAttribute_fn(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
        cuDeviceGetAttribute_fn(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);

        snprintf (tmp_buf1, sizeof (tmp_buf1), "sm_%d%d", major, minor);
        for (i = 0; i < agent_count; i++) {
           if ( !strcmp( agents[i].device_isae, tmp_buf1 ) ) {
               agents[i].device_id |= (1 << dev) ;
                break ;
           }
        }

        if ( i == agent_count ) {
            agent_count++;
            memcpy ( agents[i].device_isae, tmp_buf1, strlen(tmp_buf1)+1 ) ;
            cuDeviceGetAttribute_fn(&agents[i].num_sms, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev);
            agents[i].device_id =  1 << dev ;
        }

    }
}
// end of CUDA Helper Functions

#ifndef HAVE_STRRSTR
/*
** Copyright (c) 2000-2004  University of Illinois Board of Trustees
** Copyright (c) 2000-2005  Mark D. Roth
** All rights reserved.
**
** Developed by: Campus Information Technologies and Educational Services,
** University of Illinois at Urbana-Champaign
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** ``Software''), to deal with the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** * Redistributions of source code must retain the above copyright
**   notice, this list of conditions and the following disclaimers.
**
** * Redistributions in binary form must reproduce the above copyright
**   notice, this list of conditions and the following disclaimers in the
**   documentation and/or other materials provided with the distribution.
**
** * Neither the names of Campus Information Technologies and Educational
**   Services, University of Illinois at Urbana-Champaign, nor the names
**   of its contributors may be used to endorse or promote products derived
**   from this Software without specific prior written permission.
**
** THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR
** ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
** OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE.
*/
char *strrstr(const char *string, const char *find)
{
  size_t stringlen, findlen;
  const char *cp;

  findlen = strlen(find);
  stringlen = strlen(string);
  if (findlen > stringlen)
    return NULL;

  for (cp = string + stringlen - findlen; cp >= string; cp--)
    if (strncmp(cp, find, findlen) == 0)
      return (char*) cp;

  return NULL;
}
#endif

#ifndef HAVE_STRLCAT
/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left).  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MS_MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
size_t strlcat(char *dst, const char *src, size_t siz)
{
  char *d = dst;
  const char *s = src;
  size_t n = siz;
  size_t dlen;

  /* Find the end of dst and adjust bytes left but don't go past end */
  while (n-- != 0 && *d != '\0')
    d++;
  dlen = d - dst;
  n = siz - dlen;

  if (n == 0)
    return(dlen + strlen(s));
  while (*s != '\0') {
    if (n != 1) {
      *d++ = *s;
      n--;
    }
    s++;
  }
  *d = '\0';

  return(dlen + (s - src));/* count does not include NUL */
}
#endif

#if defined(__WIN__)
/*
 * Selection of a file name for a temporary file.
 * This is highly system-dependent, and you may want to customize it.
 */
void
select_file_name (char * fname)
{
  char * env;
  char * ptr;
  FILE * tfile;
   errno_t err;
   size_t len;
   char pValue[] = ".";
static pthread_mutex_t CritSect;      /* to single-thread queue operations */
static int next_file_num;       /* to distinguish among several temp files */
    char *text;

    pthread_mutex_lock(&CritSect);
  /* Keep generating file names till we find one that's not in use */
  for (;;) {
    /* Get temp directory name from environment TMP or TEMP variable;
     * if none, use "."
     */
    if ((err = _dupenv_s( &env, &len, "TMP" )) != 0)
      if ((err = _dupenv_s( &env, &len, "TEMP" )) == NULL)
        env = pValue;
    if (*env == '\0')           /* null string means "." */
      env = pValue;
    ptr = fname;                /* copy name to fname */
    text = env;
    while (*text != '\0')
      *ptr++ = *text++;
    if (ptr[-1] != '\\' && ptr[-1] != '/')
      *ptr++ = '\\';            /* append backslash if not in env variable */
    /* Append a suitable file name */
    next_file_num++;            /* advance counter */
    wsprintf(ptr, "JPG%03d.ptx", next_file_num);
    /* Probe to see if file name is already in use */
    if ((fopen_s( &tfile, fname, "rb" )) != 0)
      {free( env );break;}
    fclose(tfile);              /* oops, it's there; close tfile & try again */
    free( env );
  }
    pthread_mutex_unlock(&CritSect);
}


#else
#ifndef TEMP_DIRECTORY        /* can override from jconfig.h or Makefile */
#define TEMP_DIRECTORY  "/tmp/" /* recommended setting for Unix */
#endif

#ifndef TEMP_FILE_NAME        /* can override from jconfig.h or Makefile */
#define TEMP_FILE_NAME  "%sJPG%03d.ptx"
#endif

#ifndef NO_ERRNO_H
#include <errno.h>        /* to define ENOENT */
#endif

/* ANSI C specifies that errno is a macro, but on older systems it's more
 * likely to be a plain int variable.  And not all versions of errno.h
 * bother to declare it, so we have to in order to be most portable.  Thus:
 */
#ifndef errno
extern int errno;
#endif


void
select_file_name (char * fname)
{
  FILE * tfile;
static int next_file_num;    /* to distinguish among several temp files */
static pthread_mutex_t CritSect;      /* to single-thread queue operations */

    pthread_mutex_lock(&CritSect);
  /* Keep generating file names till we find one that's not in use */
  for (;;) {
    next_file_num++;        /* advance counter */
    sprintf(fname, TEMP_FILE_NAME, TEMP_DIRECTORY, next_file_num);
    if ((tfile = fopen(fname, "rb")) == NULL) {
      /* fopen could have failed for a reason other than the file not
       * being there; for example, file there but unreadable.
       * If <errno.h> isn't available, then we cannot test the cause.
       */
#ifdef ENOENT
      if (errno != ENOENT)
        continue;
#endif
      break;
    }
    fclose(tfile);        /* oops, it's there; close tfile & try again */
  }
    pthread_mutex_unlock(&CritSect);
}

#endif /* defined(__WIN__) */

# if 1 /* For debugging purposes */
static my_bool elfLoadAndKernelLaunch(char* ptxCode, void* *elf, size_t *elfSize, char    *architecture)
{
    char tmp_buf1[80];
    char tmp_buf2[80];
    char buffer[512];
    FILE * fp ;
    my_bool fResult = FALSE;
    int retval;

    if ( !ptxCode ) {
        goto fn_exit;
    }

    select_file_name(tmp_buf1);
    select_file_name(tmp_buf2);

#if defined(__WIN__)
    snprintf (buffer, sizeof (buffer), "nvcc --fatbin -arch=%s -o %s %s >NUL 2>&1", architecture, tmp_buf2, tmp_buf1);
#else
    snprintf (buffer, sizeof (buffer), "nvcc --fatbin -arch=%s -o %s %s >/dev/null 2>&1", architecture, tmp_buf2, tmp_buf1);
#endif /* defined(__WIN__) */

#if defined(__WIN__)
    if ( ( fopen_s( &fp, tmp_buf1, "wb" ) ) != 0 ) {
#else
    if ( ( fp = fopen ( tmp_buf1 , "wb" ) ) == NULL ) {
#endif /* defined(__WIN__) */
        printf ( "Error : cannot open file %s\n" , tmp_buf1 ) ;
        goto fn_exit;
    }
    else {
        fwrite( ptxCode, sizeof(char), strlen(ptxCode), fp);
        fclose(fp);
    }

    retval = system( buffer ) ;
    if ( retval ) {
        goto fn_fail;
    }

#if defined(__WIN__)
    if ( ( fopen_s( &fp, tmp_buf2, "rb" ) ) != 0 ) {
#else
    if ( ( fp = fopen ( tmp_buf2 , "rb" ) ) == NULL ) {
#endif /* defined(__WIN__) */
        printf ( "Error : cannot open file %s\n" , tmp_buf2 ) ;
        goto fn_fail;
    }
    else {
        if( fseek( fp, 0, SEEK_END ) != 0 ) {
            goto fn_fail2;
        }
        *elfSize = ftell( fp ) ;
        fseek( fp, 0L, SEEK_SET ) ;
        *elf = malloc(*elfSize);
        if( !*elf ) goto fn_fail2;
        fResult = (fread( (void *)*elf, sizeof( char ), *elfSize, fp )==*elfSize) ;
        fclose(fp);
    }

    /* delete the file tmp file */
    unlink( tmp_buf1 ) ;
    unlink( tmp_buf2 ) ;

fn_exit:
    return fResult;

fn_fail2:
    fclose(fp);
    /* delete the file tmp file */
    unlink( tmp_buf2 ) ;

fn_fail:
    /* delete the file tmp file */
    unlink( tmp_buf1 ) ;
    goto fn_exit;
}
#else
#define NVPTXCOMPILER_SAFE_CALL(x)                                       \
    do {                                                                 \
        nvPTXCompileResult result = x;                                   \
        if (result != NVPTXCOMPILE_SUCCESS) {                            \
            printf("error: %s failed with error code %d\n", #x, result); \
        }                                                                \
    } while(0)


#include "nvPTXCompiler.h"
static my_bool elfLoadAndKernelLaunch(char* ptxCode, void* *elf, size_t *elfSize, char    *architecture)
{
    nvPTXCompilerHandle compiler = NULL;
    nvPTXCompileResult status;

    size_t infoSize, errorSize;
    char *infoLog, *errorLog;

    char buffer[ 128 ];

    const char* compile_options[] = { buffer
                                    };

    snprintf (buffer, sizeof (buffer), "--gpu-name=%s", architecture);

    NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerCreate(&compiler,
                                                (size_t)strlen(ptxCode),  /* ptxCodeLen */
                                                ptxCode)                  /* ptxCode */
                            );

    status = nvPTXCompilerCompile(compiler,
                                  1,                 /* numCompileOptions */
                                  compile_options);  /* compileOptions */

    if (status != NVPTXCOMPILE_SUCCESS) {
        NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetErrorLogSize(compiler, &errorSize));

        if (errorSize != 0) {
            errorLog = (char*)malloc(errorSize+1);
            NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetErrorLog(compiler, errorLog));
//          printf("Error log: %s\n", errorLog);
            free(errorLog);
        }
        NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerDestroy(&compiler));
        return FALSE;
    }

    NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetCompiledProgramSize(compiler, elfSize));

    *elf = malloc(*elfSize);
    NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetCompiledProgram(compiler, (void*)*elf));

    NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetInfoLogSize(compiler, &infoSize));

    if (infoSize != 0) {
        infoLog = (char*)malloc(infoSize+1);
        NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerGetInfoLog(compiler, infoLog));
//      printf("Info log: %s\n", infoLog);
        free(infoLog);
    }

    NVPTXCOMPILER_SAFE_CALL(nvPTXCompilerDestroy(&compiler));

    return TRUE;
}
# endif

/* Thread start function. */
static void *
thread_start(void *arg)
{
    struct thread_info *tinfo = (struct thread_info *)arg;
    Q_ELEM_PTR  cur_el ;
    SlibList_t *entry = NULL;
    my_bool fResult = FALSE;
    char *elf = NULL;
    size_t elfSize;
    FILE*   fileHandle;
   char *pdest;
    int i = -1;
   char temp_buff[ 255 ];

    do {
        pthread_mutex_lock( tinfo->CritSect ) ;
        cur_el = DeQueueElem( tinfo->list_header ) ;
        if ( cur_el != NULL ) {
            entry = (SlibList_t *)QueuePtr( cur_el );
        }
        pthread_mutex_unlock( tinfo->CritSect ) ;

        if( !cur_el ) break ;
        elf = NULL;

        // Search backward.
        for (i = 1; i < MK_MAX; i++) {
            memcpy ( temp_buff, kind_strs[i], strlen (kind_strs[i]) );
            memcpy ( temp_buff+strlen (kind_strs[i]), ".ptx.fatbin", sizeof (".ptx.fatbin") );
            pdest = strrstr( entry->lpszName, temp_buff );
            if ( ( pdest != NULL ) &&
                 ( !pdest[ strlen( temp_buff ) ] )  )
               break;
        }

        if ( i == MK_MAX ) {
           goto failure;
        }
        entry->module_kind = i;

        fResult = FALSE;
#if defined(__WIN__)
        if( fopen_s( &fileHandle, entry->lpszName, "rb" ) == 0 )
#else
        if( ( fileHandle = fopen ( entry->lpszName , "rb" ) ) != NULL )
#endif /* defined(__WIN__) */
        {
            fseek( fileHandle, 0, SEEK_END );
            elfSize = ftell( fileHandle ) ;
            fseek( fileHandle, 0L, SEEK_SET ) ;
            if ( elfSize ) elf = (char *)malloc(elfSize);
            if ( elf != 0 )
            {
                fResult = (fread( (void *)elf, sizeof( char ), elfSize, fileHandle )==elfSize) ;
            }
            fclose( fileHandle );
        }
        else
        {
            fResult = elfLoadAndKernelLaunch(entry->code, (void**)&elf, &elfSize, entry->device_isa);
#if defined(__WIN__)
            if ( fopen_s( &fileHandle, entry->lpszName, "wb" ) == 0 )
#else
            if( ( fileHandle = fopen ( entry->lpszName , "wb" ) ) != NULL )
#endif /* defined(__WIN__) */
            {
                if ( fResult ) fResult = (fwrite(elf, sizeof(char), elfSize, fileHandle)==elfSize);
                fclose(fileHandle);
            }
        }

        if ( !fResult ) {
           goto failure;
        }

        entry->elf = elf ;
        entry->elfSize = elfSize ;
#if 1
        if ( cuLibraryLoadData_fn != NULL ) {
            CUjit_option jitOptions[] = {
                    CU_JIT_MAX_REGISTERS,
            };
            void* jitOptionsValues[] = {
                NULL,
            };
            CUlibraryOption libraryOptions[] = {
                CU_LIBRARY_BINARY_IS_PRESERVED,
                CU_LIBRARY_HOST_UNIVERSAL_FUNCTION_AND_DATA_TABLE, 
            };
            void* libraryOptionValues[] = {
                (void*)1, 
                (void*)1 
            };
            cuLibraryLoadData_fn(&entry->library, elf , jitOptions, jitOptionsValues,
                0, libraryOptions, libraryOptionValues, 1 );
        }
#endif
        InitQElem( cur_el ) ;
        SetQueueElemPtr( cur_el, entry ) ;

        pthread_mutex_lock( tinfo->CritSect ) ;
        PushElem( &library_list, cur_el );
        pthread_mutex_unlock( tinfo->CritSect ) ;

#   ifdef GATHERSTATS
        passed++;
#   endif
       continue;

failure :
        free (entry->code);
        free (entry);
        free (elf);

#   ifdef GATHERSTATS
        failed++;
#   endif
    } while( TRUE ) ;

    return NULL;
}

char *AbbrevA (char *lpsz)
{
   char *lpszTemp;

   lpszTemp = lpsz + strlen(lpsz) - 1;
   while (lpszTemp > lpsz && lpszTemp[-1] != '\\' && lpszTemp[-1] != '/')
      lpszTemp--;
   return lpszTemp;
}

#if defined(__WIN__)
wchar_t *Abbrev (wchar_t *lpsz)
{
   wchar_t *lpszTemp;

   lpszTemp = lpsz + wcslen(lpsz) - 1;
   while (lpszTemp > lpsz && lpszTemp[-1] != L'\\' && lpszTemp[-1] != L'/')
      lpszTemp--;
   return lpszTemp;
}
#endif /* defined(__WIN__) */


static int CompareSize(
     Q_ELEM_PTR e1,         // I - queue element 1
     Q_ELEM_PTR e2          // I - queue element 2
     )
 {
    int iRet = 0;
    size_t temp,temp2;

    temp = (( SlibList_t * )e1->q_ptr)->size;
    temp2 = (( SlibList_t * )e2->q_ptr)->size;

    if (temp2 > temp)
        iRet = 1;
    else if (temp2 == temp)
        iRet = 0;
    else
        iRet = -1;
     return (iRet);
 }

my_bool char_is_numeric(char p_char) {return p_char >= '0' && p_char <= '9';}
my_bool char_is_hexnumeric(char p_char) {return char_is_numeric(p_char) || (p_char >= 'a' && p_char <= 'f') || (p_char >= 'A' && p_char <= 'F');}
my_bool char_is_ascii_alpha_upper(char p_char) {return p_char >= 'A' && p_char <= 'Z';}
my_bool char_is_ascii_alpha_lower(char p_char) {return p_char >= 'a' && p_char <= 'z';}
my_bool char_is_ascii_alpha(char p_char) {return char_is_ascii_alpha_lower(p_char) || char_is_ascii_alpha_upper(p_char);}
my_bool char_is_ascii_alphanumeric(char p_char) {return char_is_ascii_alpha(p_char) || char_is_numeric(p_char);}

my_bool isWordChar(unsigned c) {
    // FIX ME map Unicode ranges somehow
    return c >= 128 || char_is_ascii_alphanumeric((char)c) || c == '_';
}

/*++

Routine Description:

  Searches the string counting the number of occurances of
  SearchString exist in SourceString.

Arguments:

  SourceString - Specifies the text that may or may not contain
                 search text

  SearchString - Specifies the text phrase to count

Return Value:

  The number of times SearchString appears in SourceString.

--*/

unsigned
CountInstancesOfSubStringA (
    const char *SourceString,
    const char *SearchString
    )
{
    const char *p;
    unsigned count;
    size_t searchBytes;

    count = 0;
    p = SourceString;
    searchBytes = strlen (SearchString);

    while ((p = strstr (p, SearchString))) {    //lint !e720
        if ( !isWordChar( *(p + searchBytes) ) &&
             ((p == SourceString) ||
              (!isWordChar( *(p - 1) ) ) ) )
            count++;
        p += searchBytes;
    }

    return count;
}


/*++

Routine Description:

  Searches and replaces all occurances of SearchString with
  ReplaceString.

Arguments:

  SourceString - String that contiains zero or more instances
                 of the search text

  SearchString - String to search for.  Cannot be zero-length or NULL.

  ReplaceString - String to replace.  Can be zero-length but cannot
                  be NULL.

Return Value:

  A pointer to the pool-allocated string, or NULL if no instances
  of SearchString were found in SourceString.  Free the non-NULL
  pointer with FreePathString.

--*/

char *
StringSearchAndReplaceA (
    const char *SourceString,
    const char *SearchString,
    const char *ReplaceString
    )
{
    char *newString;
    char *p, *q;
    char *dest;
    unsigned count;
    size_t size;
    size_t searchBytes;
    size_t replaceBytes;
    unsigned untouchedBytes;

    //
    // count occurances within the string
    //

    count = CountInstancesOfSubStringA (
                SourceString,
                SearchString
                );

    if (!count) {
        return NULL;
    }

    searchBytes = strlen (SearchString);
    replaceBytes = strlen (ReplaceString);

    size = strlen (SourceString) + 1 -
           count * searchBytes +
           count * replaceBytes;

    newString = (char *) malloc (size);
    if (!newString) {
        return NULL;
    }

    p = (char *) SourceString;
    dest = (char *) newString;

    while ((q = (char *) strstr ((char *) p, SearchString))) {    //lint !e720

        if ( isWordChar( *(q + searchBytes) ) ||
             ((q != SourceString) &&
              (isWordChar( *(q - 1) ) ) ) ) {
               p = q+1 ;
               continue ;
        }
        untouchedBytes = (unsigned)(q - p);

        if (untouchedBytes) {
            memcpy (dest, p, (size_t) untouchedBytes);
            dest += untouchedBytes;
        }

        if (replaceBytes) {
            memcpy (dest, (char *) ReplaceString, (size_t) replaceBytes);
            dest += replaceBytes;
        }

        p = q + searchBytes;
    }

    memcpy ((char *) dest, (char *) p, strlen (p) + 1);

    return newString;
}


#if !defined(__WIN__)
// This common implementation is used by both strtok() and strtok_s()
#ifdef __cplusplus
extern "C"
#endif
char* strtok_s(
    char*       string,
    char const* control,
    char**      context
    )
{
    // Clear control map.  The control characters are stored in a bitmap, one
    // bit per character.  The null character is always a control character.
    unsigned char map[32];
    for (int count = 0; count < 32; count++)
    {
        map[count] = 0;
    }

    // Set bits in delimiter table
    unsigned char const* unsigned_control = (unsigned char const*)(control);
    do
    {
        map[*unsigned_control >> 3] |= (1 << (*unsigned_control & 7));
    }
    while (*unsigned_control++);

    // If string is null, set the iterator to the saved pointer (i.e., continue
    // breaking tokens out of the string from the last strtok call):
    char* it = string != NULL
        ? string
        : *context;

    // Find beginning of token (skip over leading delimiters). Note that
    // there is no token iff this loop sets it to point to the terminal
    // null (*it == '\0')
    while ((map[*(unsigned char*)it >> 3] & (1 << (*(unsigned char*)it & 7))) && *it)
    {
        ++it;
    }

    char* const token_first = it;

    // Find the end of the token. If it is not the end of the string,
    // put a null there.
    for (; *it; ++it)
    {
        if (map[*(unsigned char*)it >> 3] & (1 << (*(unsigned char*)it & 7)))
        {
            *it++ = '\0';
            break;
        }
    }

    // Update the saved pointer:
    *context = it;

    // Determine if a token has been found.
    return it != token_first ? token_first : NULL;
}
#endif /* !defined(__WIN__) */

// *DoRawLog writes to *buf of *size and move them past the written portion.
// It returns true iff there was no overflow or error.
static bool DoRawLog(char** buf, size_t* size, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(*buf, *size, format, ap);
  va_end(ap);
  if (n < 0 || (size_t)n > *size) return false;
  *size -= (size_t)n;
  *buf += n;
  return true;
}

void ptb_kernel_converter (char *buffout, size_t size, char *input, const char    *architecture)
{
#define ISSPACE(c) ((c==' ')||(c=='\t')||(c=='\n')||(c=='\r'))
    /* helpers */
    char *buf_ptr;

    #define SKIP_BLANKS()   for ( ; *buf_ptr && ISSPACE(*buf_ptr); buf_ptr++)
    #define SKIP_TO_BLANKS()        for ( ; *buf_ptr && !ISSPACE(*buf_ptr); buf_ptr++)
    #define CHECK_NULL()    if (!*buf_ptr) continue;

    /* helpers */
    char seps[]   = "\n";
    char *token;
    char *next_token1 = NULL;

    /* helpers */
    char **    item = NULL ;
    Q_ELEM_PTR          qelem ;
    Q_ELEM_PTR qe_ptr ;
    Q_HEADER temp_list;
    char *p;
    char *out_ptr = buffout;

    /* registers.  */
    const char * const origGridDim_x_reg = "x_reg_ori";
    const char * const origGridDim_y_reg = "y_reg_ori";
    const char * const origGridDim_z_reg = "z_reg_ori";
    const char * const newBlockIdx_z_reg = "z_reg_ne";
    const char * const newBlockIdx_y_reg = "y_reg_ne";
    const char * const newBlockIdx_x_reg = "x_reg_ne";

    const char *l[] = { "lab1", "lab2" };

    /* states.  */
    Q_ELEM_PTR fdecl = NULL;
    Q_ELEM_PTR fbody = NULL;
    Q_ELEM_PTR parameter = NULL;
    int count = 0;

    InitQueue( &temp_list );

    // 1. Tokenize the Input PTX
    // Split the input string by newlines and store each line in a queue.

    // Establish string and get the first token:
    token = strtok_s( input, seps, &next_token1 );
    // Note: strtok is deprecated; consider using strtok_s instead
    while( token != NULL )
    {
        // While there are tokens in "string"
        if ( (item = (char **)malloc(sizeof(Q_ELEM) + sizeof(char *))) == NULL ) {
            goto error ;
        }

        qelem = (Q_ELEM_PTR)(item + 1);
        *item = token ;
        InitQElem( qelem ) ;
        SetQueueElemPtr( qelem, item ) ;
        EnQueueElem( &temp_list, qelem, 0 );

        // Get next token:
        token = strtok_s( NULL, seps, &next_token1 );
    }

    // 2. Iterate through PTX lines to identify structure

    qelem = QueueHead( &temp_list );
    for ( ; qelem != NULL ; qelem = QueueNext( qelem ) ) {
        buf_ptr = *(char **)QueuePtr( qelem );

        SKIP_BLANKS();                  // Skip initial blanks

        p = buf_ptr;

        // count the number of open braces
        for ( buf_ptr = p; *buf_ptr; buf_ptr++)
            if (*buf_ptr == '{')
                count++;

        if ( ( count != 0 ) && ( fdecl ) && ( fbody == NULL ) ) {
            // we are visiting a function body (first opening brace after declaration)
            fbody = qelem ;
        }

        // count the number of Close braces
        for ( buf_ptr = p; *buf_ptr; buf_ptr++)
            if (*buf_ptr == '}')
                count--;

        // Identify Kernel Entry Point
        if ( (fdecl == NULL) &&
             (strncmp (p, ".visible .entry", sizeof (".visible .entry") - 1) == 0 ||
              strncmp (p, ".entry", sizeof (".entry") - 1) == 0 ||
              strncmp (p, ".weak .entry", sizeof (".weak .entry") - 1) == 0) ) {
            fdecl = qelem ;
        }

        // Identify Parameter List
        if( ( !strncmp (p, ".param", sizeof (".param") - 1) ) && ( !fbody && fdecl && !parameter ) ) {
            // parsing the param list.
            parameter = qelem ;
        }

        // --- Processing Header / Global Scope ---
        if( fdecl == NULL ) {
            // Upgrade sm version if host device is newer than PTX target
            if (!strncmp(*(char **)QueuePtr( qelem ), ".target", (sizeof(".target")-1)) &&
            (atoi ( *(char **)QueuePtr( qelem )+(sizeof(".target sm_")-1)  ) > atoi ( architecture+(sizeof("sm_")-1)  ) )) {
                DoRawLog (&out_ptr, &size, ".target %s\n", architecture);
            } else {
                DoRawLog (&out_ptr, &size, "%s\n", *(char **)QueuePtr( qelem ));
            }
        }

        // --- Processing Function Body ---
        if( fbody && ( count == 0 ) ) {
            // Now must be at end of kernel

            qe_ptr = fdecl;

            for ( ; qe_ptr != qelem ; qe_ptr = QueueNext( qe_ptr ) ) {
                if( NULL == parameter ) {
                    // No parameters, ignore such kernels for now!
                    DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qe_ptr ) ) ;
                    continue ;
                }

                // --- 3. Parameter Injection ---
                if( qe_ptr == parameter ) {
                    //  insert params
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_x") ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_y") ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_z") ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "thread_num_cd") ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "sharedMemBytes" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "lowerbound" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "upperbound" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "unused" ) ;
                }

                // --- 4. Intercept Returns/Exits ---
                // Instead of terminating the thread, we increment the loop counter
                // and jump back to the start to see if more work is available.
                if ( ( strncmp ( *(char **)QueuePtr( qe_ptr ), "ret;", sizeof ("ret;") - 1 ) == 0 ) ||
                     ( strstr( *(char **)QueuePtr( qe_ptr ), "exit;" ) != NULL ) ) {
                    // Boundary Check: If curr_idx >= upper_bound, we are done.
                    DoRawLog( &out_ptr, &size, "setp.ge.s32 %s, %s, %s;\n", "check_index_pred_reg", "curr_idx_reg", "upper_bound" ) ;
                    DoRawLog( &out_ptr, &size, "@%s bra %s;\n", "check_index_pred_reg", l[1] ) ;
                    DoRawLog( &out_ptr, &size, "\n") ;
                    // If batch done, jump to lab1 (Start of Persistent Loop) to fetch new task
                    DoRawLog( &out_ptr, &size, "bra.uni %s;\n", l[0]) ;
                } else {
                    // --- 5. Replace Hardware IDs ---
                    // Replace read-only hardware block IDs with our writable 
                    // software registers (newBlockIdx), allowing us to "move" the block logically.
                    if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%ctaid.x" )) ) {
                        memcpy (p, newBlockIdx_x_reg, strlen(newBlockIdx_x_reg));
                    }
                    else if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%ctaid.y" )) ) {
                        memcpy (p, newBlockIdx_y_reg, strlen(newBlockIdx_y_reg));
                    }
                    else if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%ctaid.z" )) ) {
                        memcpy (p, newBlockIdx_z_reg, strlen(newBlockIdx_z_reg));
                    }
                    // Map %nctaid (gridDim) to our passed-in original dimensions
                    else if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%nctaid.x" )) ) {
                        memcpy (p, origGridDim_x_reg, strlen(origGridDim_x_reg));
                    }
                    else if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%nctaid.y" )) ) {
                        memcpy (p, origGridDim_y_reg, strlen(origGridDim_y_reg));
                    }
                    else if ( (p = strstr ( *(char **)QueuePtr( qe_ptr ), "%nctaid.z" )) ) {
                        memcpy (p, origGridDim_z_reg, strlen(origGridDim_z_reg));
                    }
                    DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qe_ptr )) ;
                }

                // --- 6. Prolog: Inject Scheduler Logic ---
                if (qe_ptr == fbody) {
                    /* Variable Declarations */
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_x_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_y_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_z_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_z_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_y_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_x_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "temp_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "temp_reg2" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xy_tbs_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "curr_idx_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "lower_bound" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "upper_bound" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xctaid" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xnctaid" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .pred %s;\n", "check_index_pred_reg" );

                    DoRawLog( &out_ptr, &size, "\n" );

                    /* load parameters */
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_x_reg, "origGridDim_x" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_y_reg, "origGridDim_y" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_z_reg, "origGridDim_z" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "lower_bound", "lowerbound" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "upper_bound", "upperbound" ) ;

                    /* -- Calculate Task Limits -- */
                    // xy_tbs = origGridDim.x * origGridDim.y
                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "xy_tbs_reg", origGridDim_x_reg, origGridDim_y_reg ) ;

                    // Current Cluster to be processed: initially the launched cluster:
                    DoRawLog( &out_ptr, &size, "mov.b32 %s, %s;\n", "xctaid", "%ctaid.x" ) ;
                    DoRawLog( &out_ptr, &size, "mov.b32 %s, %s;\n", "xnctaid", "%nctaid.x" ) ;

                    DoRawLog( &out_ptr, &size, "add.s32 %s, %s, %s;\n", "curr_idx_reg", "xctaid", "lower_bound" ) ;

                    DoRawLog( &out_ptr, &size, "\n" );

                    /* -- Persistent Loop Start -- */
                    DoRawLog( &out_ptr, &size, "%s:\n", l[0] );

                    /* -- Loop Check & Coordinate Mapping -- */
                    // calculate the current index
                    DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", newBlockIdx_z_reg, "curr_idx_reg", "xy_tbs_reg" ) ;
                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "temp_reg", newBlockIdx_z_reg, "xy_tbs_reg" ) ;
                    DoRawLog( &out_ptr, &size, "sub.s32 %s, %s, %s;\n", "temp_reg2", "curr_idx_reg", "temp_reg" ) ;
                    DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", newBlockIdx_y_reg, "temp_reg2", origGridDim_x_reg ) ;
                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "temp_reg", newBlockIdx_y_reg, origGridDim_x_reg ) ;
                    DoRawLog( &out_ptr, &size, "sub.s32 %s, %s, %s;\n", newBlockIdx_x_reg, "temp_reg2", "temp_reg" ) ;

                    // Get task
                    DoRawLog( &out_ptr, &size, "add.s32 %s, %s, %s;\n", "curr_idx_reg", "xnctaid", "curr_idx_reg" ) ;

                    DoRawLog( &out_ptr, &size, "\n" );
                }

            }

            // This is the actual exit point when no more tasks exist.
            DoRawLog( &out_ptr, &size, "%s:", l[1] );
            DoRawLog( &out_ptr, &size, "ret;\n" );

            // the last line
            DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qelem ) );

            /*reset state flags*/
            fbody = fdecl = parameter = NULL;
            count = 0;
        }

    }

error :

    while ( ( qelem = DeQueueElem( &temp_list ) ) != NULL ) {
        free( QueuePtr( qelem ) );
    }
}

int
calcSizeAlt(char *str){
     int size = 0;
  char *p;
    if (strstr(str, ".s64") || strstr(str, ".u64") || strstr(str, ".f64") || strstr(str, ".b64"))
    {
      size = 8;
    }
    else if (strstr(str, ".b32") || strstr(str, ".f32") || strstr(str, ".u32") || strstr(str, ".f16x2") || strstr(str, ".s32"))
    {
      size = 4;
    }
    else if (strstr(str, ".b16") || strstr(str, ".f16") || strstr(str, ".u16") || strstr(str, ".s16"))
    {
      size = 2;
    }
    else if (strstr(str, ".b8") || strstr(str, ".u8") || strstr(str, ".s8"))
    {
      size = 1;
    }
    else if (strstr(str, ".b128"))
    {
      size = 16;
    }

  if (strstr(str, ".v4"))
  {
    size = size * 4;
  }
  else if (strstr(str, ".v2"))
  {
    size = size * 2;
  }

    p = str;
    while ((p = strchr (p, '[')) != NULL) {
      p += 1 ;size = size * atoi(p);
    }

/* This macro rounds x up to the y boundary.  */
#define ROUND_UP(x,y) (((x) + (y) - 1) & ~((y) - 1))

    if ((p = strstr (str, ".align ")) != NULL)
    {
      size = ROUND_UP(size,atoi(p+strlen(".align ")));
    }

  return size;
}

my_bool
fnPtr (
Q_ELEM_PTR start, Q_ELEM_PTR end,
char *pszName,
int *refcnt
){
    int fFarPtr = 0;
    int in_brackets;
    char *p;
  char *q;
  *refcnt = 0;
   for ( ; start != end ; start = QueueNext( start ) ) {
    in_brackets = 0;
    if( (p = strstr( *(char **)QueuePtr (start), pszName )) != NULL ) {
        if ( isWordChar( *(p + strlen(pszName)) ) ||
             ((p != *(char **)QueuePtr (start)) &&
              isWordChar( *(p - 1) ) ) )
            continue;
        *refcnt += 1;
        q = *(char **)QueuePtr (start);
        while ( p > q ) {
          if (*q == '[')
              in_brackets++;
          else if (*q == ']')
              in_brackets--;
            q++;
        }
        if (in_brackets != 0)
            continue;
        if ( strstr (*(char **)QueuePtr (start), ".u32")  ||
             strstr (*(char **)QueuePtr (start), ".s32")  ||
             strstr (*(char **)QueuePtr (start), ".b32") ){
            fFarPtr--;
        } else if (strstr (*(char **)QueuePtr (start), ".u64")  ||
                   strstr (*(char **)QueuePtr (start), ".s64")  ||
                   strstr (*(char **)QueuePtr (start), ".b64")) {
            fFarPtr++;
        }
    }
   }

    return (fFarPtr > 0);
}

void fused_kernel_converter (char *buffout, size_t size, char *input, const char    *architecture)
{
    /* helpers */
    char *buf_ptr;
    char seps[]   = "\n";
    char *token;
    char *next_token1 = NULL;

    /* helpers */
    char **    item = NULL ;
    Q_ELEM_PTR          qelem ;
    Q_ELEM_PTR qe_ptr ;
    Q_HEADER temp_list;
    Q_ELEM_PTR    element;
   Q_ELEM_PTR q_elem_ptr;
      char *p, *p2;
    char *out_ptr = buffout;
    int i;
    char *newData, *oldData;
    size_t dest_len;
  int regCount = 0;

    /* registers.  */
    const char * const origGridDim_x_reg = "x_reg_ori";
    const char * const origGridDim_y_reg = "y_reg_ori";
    const char * const origGridDim_z_reg = "z_reg_ori";
    const char * const newBlockIdx_z_reg = "z_reg_ne";
    const char * const newBlockIdx_y_reg = "y_reg_ne";
    const char * const newBlockIdx_x_reg = "x_reg_ne";
    const char * const origBlockDim_z_reg = "z_reg_o";
    const char *l[] = { "lab1", "lab2" };

    /* states.  */
    Q_ELEM_PTR fdecl = NULL;
    Q_ELEM_PTR fbody = NULL;
    Q_ELEM_PTR parameter = NULL;
    int count = 0;
    my_bool fFarPtr = FALSE;
    my_bool fPatchable = TRUE;
    Q_HEADER    name_list[4] ;

    InitQueue( &temp_list );
    for (i = 0; i < int( sizeof( name_list ) / sizeof(name_list[0]) ); i ++ ) {
         InitQueue( &name_list[i] );
    }

    // 1. Tokenize the Input PTX
    // Split the input string by newlines and store each line in a queue.

    // Establish string and get the first token:
    token = strtok_s( input, seps, &next_token1 );
    // Note: strtok is deprecated; consider using strtok_s instead
    while( token != NULL )
    {
        // While there are tokens in "string"
        if ( (item = (char **)malloc(sizeof(Q_ELEM) + sizeof(char *))) == NULL ) {
            goto error ;
        }

        qelem = (Q_ELEM_PTR)(item + 1);
        *item = token ;
        InitQElem( qelem ) ;
        SetQueueElemPtr( qelem, item ) ;
        EnQueueElem( &temp_list, qelem, 0 );

        // Get next token:
        token = strtok_s( NULL, seps, &next_token1 );
    }

    // 2. Iterate through PTX lines to identify structure

    qelem = QueueHead( &temp_list );
    for ( ; qelem != NULL ; qelem = QueueNext( qelem ) ) {
        buf_ptr = *(char **)QueuePtr( qelem );

        SKIP_BLANKS();                  // Skip initial blanks

        p = buf_ptr;

        // count the number of open braces
        for ( buf_ptr = p; *buf_ptr; buf_ptr++)
            if (*buf_ptr == '{')
                count++;

        if ( ( count != 0 ) && ( fdecl ) && ( fbody == NULL ) ) {
            // we are visiting a function body (first opening brace after declaration)
            fbody = qelem ;
        }

        // count the number of Close braces
        for ( buf_ptr = p; *buf_ptr; buf_ptr++)
            if (*buf_ptr == '}')
                count--;

        // Identify Kernel Entry Point
        if ( (fdecl == NULL) &&
             (strncmp (p, ".visible .entry", sizeof (".visible .entry") - 1) == 0 ||
              strncmp (p, ".entry", sizeof (".entry") - 1) == 0 ||
              strncmp (p, ".weak .entry", sizeof (".weak .entry") - 1) == 0) ) {
            fdecl = qelem ;
//          if (strstr(p, "mul_mat_vec")) {
//            regCount = 32;
//          }
        }

        // Identify Parameter List
        if( ( !strncmp (p, ".param", sizeof (".param") - 1) ) && ( !fbody && fdecl && !parameter ) ) {
            // parsing the param list.
            parameter = qelem ;
        }

        // --- Processing Header / Global Scope ---
        if( fdecl == NULL ) {
            // Upgrade sm version if host device is newer than PTX target
            if (!strncmp(*(char **)QueuePtr( qelem ), ".target", (sizeof(".target")-1)) &&
                (atoi ( *(char **)QueuePtr( qelem )+(sizeof(".target sm_")-1)  ) > atoi ( architecture+(sizeof("sm_")-1)  ) )) {
                DoRawLog (&out_ptr, &size, ".target %s\n", architecture);
            } else {
                DoRawLog (&out_ptr, &size, "%s\n", *(char **)QueuePtr( qelem ));
            }

            if (!strncmp(*(char **)QueuePtr( qelem ), ".extern .shared", (sizeof(".extern .shared")-1))) {
                char       *name_buf = NULL;
                size_t          name_len;
                char *temp_buf = strrchr( *(char **)QueuePtr( qelem ), '[' ) ;
                char *q;

                if ( temp_buf != NULL ) {
                    for (q = temp_buf; *q && !ISSPACE(*q);q--);

                    name_len = temp_buf - q ;
                    if ( (name_buf = (char *)malloc(sizeof(Q_ELEM) + name_len)) != NULL ) {
                         qe_ptr = (Q_ELEM_PTR)(name_buf + name_len);
                         memcpy ( name_buf, q + 1, name_len - 1 ) ;
                         *( name_buf + name_len - 1 ) = '\0';
                         InitQElem( qe_ptr ) ;
                         SetQueueElemPtr( qe_ptr, name_buf ) ;
                         EnQueueElem( &name_list[0], qe_ptr, FALSE );
                    }

                }
            }
        }

        if ( !strncmp( p, ".shared ", (sizeof(".shared ")-1)) ) {
            char       *name_buf = NULL;
            size_t          name_len;

            name_len = strlen( p )+1 ;
            if ( (name_buf = (char *)malloc(sizeof(Q_ELEM) + name_len)) != NULL ) {
                 qe_ptr = (Q_ELEM_PTR)(name_buf + name_len);
                 memcpy ( name_buf, p, name_len ) ;
                 InitQElem( qe_ptr ) ;
                 SetQueueElemPtr( qe_ptr, name_buf ) ;
                 EnQueueElem( fdecl ? &name_list[3] : &name_list[2], qe_ptr, FALSE );
            }
        }

        if( strncmp( p, "mov.u32 %r", sizeof ("mov.u32 %r") - 1 ) == 0 ||
           strncmp( p, "mov.u64 %r", sizeof ("mov.u64 %r") - 1 ) == 0 ) {
            char       *name_buf = NULL;
            size_t name_len = 0;
            char *temp_buf = strchr( p+sizeof ("mov.u32 %r"), ',' ) ;
            char *q = NULL ;
            char *pdest;

            if ( temp_buf != NULL ) {
                for (q = temp_buf + 1; *q && ISSPACE(*q);q++);

                pdest = strchr( q, ';' );
                name_len = pdest - q;

                for ( qe_ptr = QueueHead( &name_list[0] ); qe_ptr != NULL; qe_ptr = QueueNext( qe_ptr ) ) {
                    if ( ( !strncmp ((char *)QueuePtr( qe_ptr ), q,name_len ) ) &&
                         ( (name_buf = (char *)malloc(sizeof(Q_ELEM) + name_len + 1)) != NULL ) ) {
                         qe_ptr = (Q_ELEM_PTR)(name_buf + name_len + 1);
                         memcpy ( name_buf, q, name_len ) ;
                         *( name_buf + name_len ) = '\0';
                         InitQElem( qe_ptr ) ;
                         SetQueueElemPtr( qe_ptr, name_buf ) ;
                         EnQueueElem( &name_list[1], qe_ptr, FALSE );
                         fFarPtr = ( strncmp ( p, "mov.u64 %r", sizeof ("mov.u64 %r") - 1 ) == 0 );
                        break ;
                    }
                }
            }
        }

        if ( !strncmp( p, "barrier", sizeof ("barrier") - 1 ) ||
          ( !strncmp( p, "bar.sync", sizeof ("bar.sync") - 1 ) &&
          strncmp( p, "bar.sync 0", sizeof ("bar.sync 0") - 1 ) ) ||
          ( !strncmp( p, "bar.cta.sync", sizeof ("bar.cta.sync") - 1 ) &&
          strncmp( p, "bar.cta.sync 0", sizeof ("bar.cta.sync 0") - 1 ) ) ||
          !strncmp( p, "bar.cta.arrive", sizeof ("bar.cta.arrive") - 1 ) ||
          !strncmp( p, "bar.arrive", sizeof ("bar.arrive") - 1 ) ||
          !strncmp( p, "bar.cta.red", sizeof ("bar.cta.red") - 1 ) ||
          !strncmp( p, "bar.red", sizeof ("bar.red") - 1 ) ) {
             fPatchable = FALSE ;
        }

        // --- Processing Function Body ---
        if( fbody && ( count == 0 ) ) {
            // Now must be at end of kernel

            qe_ptr = fdecl;

            for ( ; qe_ptr != qelem ; qe_ptr = QueueNext( qe_ptr ) ) {
                if( NULL == parameter ) {
                    // No parameters, ignore such kernels for now!
                    DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qe_ptr ) ) ;
                    continue ;
                }

                // --- 3. Parameter Injection ---
                if( qe_ptr == parameter ) {
                    //  insert params
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_x" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_y" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "origGridDim_z" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "thread_num_cd" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "sharedMemBytes" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "lowerbound" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "upperbound" ) ;
                    DoRawLog( &out_ptr, &size, ".param .u32 %s,\n", "thread_num" ) ;
                }

                if (qe_ptr == fbody) {
                    if ( regCount) {
                        DoRawLog( &out_ptr, &size, ".maxnreg %d\n", regCount ) ;
                    }
                }

                // !!!IMPORTANT!!!
                if ( strncmp( *(char **)QueuePtr( qe_ptr ), ".maxntid", (sizeof(".maxntid")-1) ) ) {
                    DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qe_ptr ) ) ;
                }

                // --- 6. Prolog: Inject Scheduler Logic ---
                if (qe_ptr == fbody) {
                    /* Variable Declarations */
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_x_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_y_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origGridDim_z_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "temp_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "temp_reg2" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xy_tbs_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_z_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_y_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", newBlockIdx_x_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "curr_idx_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "next_id" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "lower_bound" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "upper_bound" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xctaid" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "xnctaid" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "ztid" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "thread_num_cd_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "unroll_factor" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "findex" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", origBlockDim_z_reg ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "thread_id" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "stride_reg" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "num_threads" ) ;
                    DoRawLog( &out_ptr, &size, ".reg .pred %s;\n", "check_index_pred_reg" );
                    DoRawLog( &out_ptr, &size, ".reg .pred %s;\n", "check_value" ) ;

                    if ( QueueCount( &name_list[1] ) ) {
                        if ( fFarPtr ) {
                            DoRawLog( &out_ptr, &size, ".reg .b64 %s;\n", "shared_mem_bytes" ) ;
                            DoRawLog( &out_ptr, &size, ".reg .b64 %s;\n", "shared_mem_offset" ) ;
                            DoRawLog( &out_ptr, &size, ".reg .b64 %s;\n", "temp_reg3" ) ;
                        } else {
                            DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "shared_mem_bytes" ) ;
                            DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "shared_mem_offset" ) ;
                            DoRawLog( &out_ptr, &size, ".reg .b32 %s;\n", "temp_reg3" ) ;
                        }
                    }

                    DoRawLog( &out_ptr, &size, "\n" );

                    /* load parameters */
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_x_reg, "origGridDim_x" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_y_reg, "origGridDim_y" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", origGridDim_z_reg, "origGridDim_z" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "thread_num_cd_reg", "thread_num_cd" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "lower_bound", "lowerbound" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "upper_bound", "upperbound" ) ;
                    DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "num_threads", "thread_num" ) ;
                    if ( QueueCount( &name_list[1] ) ) {
                        if ( fFarPtr ) {
                            DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "temp_reg", "sharedMemBytes" ) ;
                            DoRawLog( &out_ptr, &size, "cvt.u64.u32 %s, %s;\n", "shared_mem_bytes", "temp_reg" ) ;
                        } else {
                            DoRawLog( &out_ptr, &size, "ld.param.u32 %s, [%s];\n", "shared_mem_bytes", "sharedMemBytes" ) ;
                        }
                    }

                    /* -- Calculate Task Limits -- */
                    // xy_tbs = origGridDim.x * origGridDim.y
                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "xy_tbs_reg", origGridDim_x_reg, origGridDim_y_reg ) ;

                    // Current Cluster to be processed: initially the launched cluster:
                    DoRawLog( &out_ptr, &size, "mov.b32 %s, %s;\n", "xctaid", "%ctaid.x" ) ;
                    DoRawLog( &out_ptr, &size, "mov.b32 %s, %s;\n", "xnctaid", "%nctaid.x" ) ;

                    /* Setup Coordinates */
                    DoRawLog( &out_ptr, &size, "mov.u32 %s, %s;\n", "ztid", "%tid.z" ) ;
                    DoRawLog( &out_ptr, &size, "mov.u32 %s, %s;\n", "temp_reg", "%ntid.z" ) ;

                    // blockDim.z is enlarged by the fusion factor at launch.
                    DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", "unroll_factor", "temp_reg", "thread_num_cd_reg" ) ;
                    DoRawLog( &out_ptr, &size, "mov.u32 %s, %s;\n", origBlockDim_z_reg, "thread_num_cd_reg" ) ;

                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "next_id", "xctaid", "unroll_factor" ) ;
                    DoRawLog( &out_ptr, &size, "add.s32 %s, %s, %s;\n", "next_id", "lower_bound", "next_id" ) ;
                    DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "stride_reg", "xnctaid", "unroll_factor" ) ;

                    // The physical z thread range is split into fusion_factor
                    // logical CTAs.  This mapping is invariant for the launch.
                    DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", "findex", "ztid", "thread_num_cd_reg" ) ;
                    DoRawLog( &out_ptr, &size, "mul.lo.u32 %s, %s, %s;\n", "temp_reg2", "findex", "thread_num_cd_reg" ) ;
                    DoRawLog( &out_ptr, &size, "sub.u32 %s, %s, %s;\n", "thread_id", "ztid", "temp_reg2" ) ;

                    DoRawLog( &out_ptr, &size, "\n" );

                    /* -- Persistent Loop Start -- */
                    DoRawLog( &out_ptr, &size, "%s:\n", l[0]);

                    /* -- Loop Check & Coordinate Mapping -- */

                    // Get task
                    DoRawLog( &out_ptr, &size, "mov.u32 %s, %s;\n", "curr_idx_reg", "next_id" ) ;
                    DoRawLog( &out_ptr, &size, "add.s32 %s, %s, %s;\n", "next_id", "stride_reg", "next_id" ) ;
                    DoRawLog( &out_ptr, &size, "add.s32 %s, %s, %s;\n", "curr_idx_reg", "curr_idx_reg", "findex" ) ;

                    DoRawLog( &out_ptr, &size, "\n" );

                    // Emit one body for every runtime fusion factor.  Hardware
                    // IDs are virtualized with the invariant fusion_index.
                    newData = out_ptr;

                    for ( element = fbody; element != qelem; element = QueueNext( element ) ) {
                        my_bool fFound = FALSE;

                        p2 = *(char **)QueuePtr( element );
                        while (*p2 && !ISSPACE ((unsigned char) *p2))
                          p2++;

                        if ( QueueCount( &name_list[1] ) ) {
                            char *temp_buf = NULL ;

                            temp_buf = StringSearchAndReplaceA( *(char **)QueuePtr( element ),
                                 (char *) QueuePtr( QueueHead( &name_list[1] ) ), "shared_mem_offset" ) ;
                            if ( temp_buf ) {
                                DoRawLog( &out_ptr, &size, "%s\n", temp_buf );
                                free( temp_buf ) ;
                                fFound = TRUE;
                            }
                        }

                        // --- 4. Intercept Returns/Exits ---
                        // Instead of terminating the thread, we increment the loop counter
                        // and jump back to the start to see if more work is available.
                        if ( ( strncmp ( *(char **)QueuePtr( element ), "ret;", sizeof ("ret;") - 1 ) == 0 ) ||
                             ( strstr( *(char **)QueuePtr( element ), "exit;" ) != NULL ) ) {
                            // Boundary Check: If curr_idx >= upper_bound, we are done.
                            DoRawLog( &out_ptr, &size, "setp.ge.s32 %s, %s, %s;\n", "check_index_pred_reg", "next_id", "upper_bound" ) ;
                            DoRawLog( &out_ptr, &size, "@%s bra %s;\n", "check_index_pred_reg", l[1] ) ;
                            DoRawLog( &out_ptr, &size, "\n") ;

                            // If batch done, jump to lab1 (Start of Persistent Loop) to fetch new task
                            DoRawLog( &out_ptr, &size, "bra.uni %s;\n", l[0]) ;
                        } else {
                            // --- 5. Replace Hardware IDs ---
                            // Replace read-only hardware block IDs with our writable
                            // software registers (newBlockIdx), allowing us to "move" the block logically.
                            if ( (p = strstr ( *(char **)QueuePtr( element ), "%ctaid.x" )) ) {
                                memcpy (p, newBlockIdx_x_reg, strlen(newBlockIdx_x_reg));
                            }
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%ctaid.y" )) ) {
                                memcpy (p, newBlockIdx_y_reg, strlen(newBlockIdx_y_reg));
                            }
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%ctaid.z" )) ) {
                                memcpy (p, newBlockIdx_z_reg, strlen(newBlockIdx_z_reg));
                            }
                            // Map %nctaid (gridDim) to our passed-in original dimensions
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%nctaid.x" )) ) {
                                memcpy (p, origGridDim_x_reg, strlen(origGridDim_x_reg));
                            }
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%nctaid.y" )) ) {
                                memcpy (p, origGridDim_y_reg, strlen(origGridDim_y_reg));
                            }
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%nctaid.z" )) ) {
                                memcpy (p, origGridDim_z_reg, strlen(origGridDim_z_reg));
                            }
                            else if ( (p = strstr ( *(char **)QueuePtr( element ), "%ntid.z" )) ) {
                                memcpy (p, origBlockDim_z_reg, strlen(origBlockDim_z_reg));
                            }
                            if ( fPatchable && strstr ( *(char **)QueuePtr( element ), "bar.sync 0" ) ) {
                                DoRawLog( &out_ptr, &size, "barrier.cta.sync %s, %s;\n", "findex", "num_threads" ) ;
                            }
                            else if (!fFound && strncmp( *(char **)QueuePtr( element ), ".maxntid", sizeof (".maxntid") - 1 ) &&
                                     strncmp( p2, ".shared ", sizeof (".shared ") - 1 )) {
                                DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( element ) ) ;
                            }
                        }

                        if ( element == fbody ) {
                            Q_HEADER *temp_array[] = {&name_list[2], &name_list[3]};
                            unsigned  j;
                             for ( j = 0; j < sizeof (temp_array) / sizeof (temp_array[0]); j++ ) {
                                for ( q_elem_ptr = QueueHead( temp_array[j] ); q_elem_ptr != NULL; q_elem_ptr = QueueNext( q_elem_ptr ) ) {
                                   int ob_refcnt;
                                    char *name;
                                    char temp;
                                    char *pos=strchr((char *)QueuePtr( q_elem_ptr ),'[');
                                    if (!pos)
                                      pos=strrchr((char *)QueuePtr( q_elem_ptr ),';');

                                    temp = *pos; *pos = '\0';
                                    for (name = pos-1; *name && !ISSPACE(*name);name--) ;
                                    name++;

                                    if ( fnPtr(fbody, qelem, name, &ob_refcnt) ) {
                                         DoRawLog( &out_ptr, &size, "%sA[16]%c%s\n", (char *)QueuePtr( q_elem_ptr ), temp, pos+1 ) ;

                                         DoRawLog( &out_ptr, &size, ".reg .b64 %s, %sB;\n", name, name ) ;
                                         DoRawLog( &out_ptr, &size, "cvt.u64.u32 %sB, %s;\n", name, "findex" ) ;
                                         DoRawLog( &out_ptr, &size, "mul.lo.u64 %s, %sB, ", name, name ) ;

                                         *pos = temp;
                                         DoRawLog( &out_ptr, &size, "%d;\n", calcSizeAlt((char *)QueuePtr( q_elem_ptr )) ) ;
                                         *pos = '\0';

                                         DoRawLog( &out_ptr, &size, "mov.u64 %sB, %sA;\n", name, name ) ;
                                         DoRawLog( &out_ptr, &size, "add.u64 %s, %sB, %s;\n", name, name, name ) ;
                                    } else if (ob_refcnt) {
                                         DoRawLog( &out_ptr, &size, "%sA[16]%c%s\n", (char *)QueuePtr( q_elem_ptr ), temp, pos+1 ) ;

                                         DoRawLog( &out_ptr, &size, ".reg .b32 %s, %sB;\n", name, name ) ;
                                         DoRawLog( &out_ptr, &size, "mul.lo.u32 %s, %s, ", name, "findex" ) ;

                                         *pos = temp;
                                         DoRawLog( &out_ptr, &size, "%d;\n", calcSizeAlt((char *)QueuePtr( q_elem_ptr )) ) ;
                                         *pos = '\0';

                                         DoRawLog( &out_ptr, &size, "mov.u32 %sB, %sA;\n", name, name ) ;
                                         DoRawLog( &out_ptr, &size, "add.u32 %s, %sB, %s;\n", name, name, name ) ;
                                    }

                                    *pos = temp;
                                }
                             }

                            DoRawLog( &out_ptr, &size, "setp.ge.u32 %s, %s, %s;\n", "check_value", "curr_idx_reg", "upper_bound" ) ;
                            DoRawLog( &out_ptr, &size, "@%s bra %s;\n", "check_value", l[1]);

                            if( QueueCount( &name_list[1] ) ) {
                                if ( fFarPtr ) {
                                    DoRawLog( &out_ptr, &size, "cvt.u64.u32 %s, %s;\n", "temp_reg3", "findex" ) ;
                                    DoRawLog( &out_ptr, &size, "mul.lo.u64 %s, %s, %s;\n", "shared_mem_offset", "temp_reg3", "shared_mem_bytes" ) ;
                                    DoRawLog( &out_ptr, &size, "mov.u64 %s, %s;\n", "temp_reg3", (char *) QueuePtr( QueueHead( &name_list[1] ) ) ) ;
                                } else {
                                    DoRawLog( &out_ptr, &size, "mul.lo.u32 %s, %s, %s;\n", "shared_mem_offset", "findex", "shared_mem_bytes" ) ;
                                    DoRawLog( &out_ptr, &size, "mov.u32 %s, %s;\n", "temp_reg3", (char *) QueuePtr( QueueHead( &name_list[1] ) ) ) ;
                                }
                                DoRawLog( &out_ptr, &size, fFarPtr ? "add.u64 %s, %s, %s;\n" : "add.u32 %s, %s, %s;\n", "shared_mem_offset", "temp_reg3", "shared_mem_offset" ) ;
                            }

                            DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", newBlockIdx_z_reg, "curr_idx_reg", "xy_tbs_reg" ) ;
                            DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "temp_reg", newBlockIdx_z_reg, "xy_tbs_reg" ) ;
                            DoRawLog( &out_ptr, &size, "sub.s32 %s, %s, %s;\n", "temp_reg2", "curr_idx_reg", "temp_reg" ) ;
                            DoRawLog( &out_ptr, &size, "div.u32 %s, %s, %s;\n", newBlockIdx_y_reg, "temp_reg2", origGridDim_x_reg ) ;
                            DoRawLog( &out_ptr, &size, "mul.lo.s32 %s, %s, %s;\n", "temp_reg", newBlockIdx_y_reg, origGridDim_x_reg ) ;
                            DoRawLog( &out_ptr, &size, "sub.s32 %s, %s, %s;\n", newBlockIdx_x_reg, "temp_reg2", "temp_reg" ) ;
                            DoRawLog( &out_ptr, &size, "\n" );
                        }

                        // !!!IMPORTANT!!!
                        qe_ptr = element;
                    }

                    // the last line
                    DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( element ) );

                    // Replace the physical thread ID (%tid.z) with the virtual thread_id calculated above.
                    oldData = StringSearchAndReplaceA (newData, "%tid.z", "thread_id");
                    if (oldData) {
                        dest_len = strlen(oldData);
                        memcpy (newData, oldData, dest_len);
                        free (oldData);
                        out_ptr = newData + dest_len;
                    }

                    DoRawLog( &out_ptr, &size, "\n" );
                }

            }

            // This is the actual exit point when no more tasks exist.
            DoRawLog( &out_ptr, &size, "%s:\n", l[1]);
            DoRawLog( &out_ptr, &size, "ret;\n");

            // the last line
            DoRawLog( &out_ptr, &size, "%s\n", *(char **)QueuePtr( qelem ));

            /*reset state flags*/
            fbody = fdecl = parameter = NULL;
            regCount = count = 0;
            fFarPtr = FALSE;
            fPatchable = TRUE;
            while ( ( qe_ptr = DeQueueElem( &name_list[1] ) ) != NULL ) {
               free( QueuePtr( qe_ptr ) );
            }
            while ( ( qe_ptr = DeQueueElem( &name_list[3] ) ) != NULL ) {
               free( QueuePtr( qe_ptr ) );
            }
        }

    }

error :

    while ( ( qelem = DeQueueElem( &temp_list ) ) != NULL ) {
        free( QueuePtr( qelem ) );
    }
    for (i = 0; i < int( sizeof( name_list ) / sizeof(name_list[0]) ); i ++ ) {
        while ( ( qelem = DeQueueElem( &name_list[i] ) ) != NULL ) {
            free( QueuePtr( qelem ) );
        }
    }
}

size_t decompress(unsigned char* input, size_t input_size, unsigned char* output, size_t output_size)
{
    size_t ipos = 0, opos = 0;  
    size_t next_nclen;  // length of next non-compressed segment
    size_t next_clen;   // length of next compressed segment
    size_t back_offset; // negative offset where redudant data is located, relative to current opos

    while (ipos < input_size) {
        next_nclen = (input[ipos] & 0xf0) >> 4;
        next_clen = 4 + (input[ipos] & 0xf);
        if (next_nclen == 0xf) {
            do {
                next_nclen += input[++ipos];
            } while (input[ipos] == 0xff);
        }

        memcpy(output + opos, input + (++ipos), next_nclen);

        ipos += next_nclen;
        opos += next_nclen;
        if (ipos >= input_size || opos >= output_size) {
            break;
        }
        back_offset = input[ipos] + (input[ipos + 1] << 8);
        ipos += 2;
        if (next_clen == 0xf+4) {
            do {
                next_clen += input[ipos++];
            } while (input[ipos - 1] == 0xff);
        }

        if (next_clen <= back_offset) {
            memcpy(output + opos, output + opos - back_offset, next_clen);
        } else {
            memcpy(output + opos, output + opos - back_offset, back_offset);
            for (size_t i = back_offset; i < next_clen; i++) {
                output[opos + i] = output[opos + i - back_offset];
            }
        }

        opos += next_clen;
    }
    return opos;
}

void find_cubin_offset(char *start_ptr, uint64_t remaining_size, Q_HEADER_PTR   list_ptr,char *lpszName)
{
    computeFatBinaryFormat_t fatheader;
    fat_text_header  *fatTextHeader;
    uint64_t acclen;
    void *raw = NULL;
    char *p;
    Q_ELEM_PTR qelem ;
    size_t  length;
    SlibList_t *entry;
    size_t newSize;
    int count = 0;
     char *temp_buf = NULL ;
  const char* abbrevptr = AbbrevA (lpszName);
  char *code;

    while (remaining_size)
    {
        // padding
        while (remaining_size >= sizeof (*fatheader)) {
            fatheader = (computeFatBinaryFormat_t)start_ptr;
            if ( (fatheader->magic == FATBIN_MAGIC) &&
                 fatheader->version == FATBIN_VERSION )
               break;
            start_ptr++, remaining_size--;
        }

        if (!(remaining_size >= sizeof (*fatheader))) break;

        fatheader = (computeFatBinaryFormat_t)start_ptr;
        start_ptr +=sizeof (*fatheader);
        remaining_size -=sizeof (*fatheader);
        acclen=0;
        while (acclen < fatheader->fatSize)
        {
            fatTextHeader = (fat_text_header*) start_ptr;
            start_ptr += sizeof (*fatTextHeader);
            remaining_size -= sizeof (*fatTextHeader);
            if (sizeof(fat_text_header) < fatTextHeader->header_size){
                start_ptr += fatTextHeader->header_size-sizeof(fat_text_header);
                remaining_size -= fatTextHeader->header_size-sizeof(fat_text_header);
            }

            if ( FATBIN_KIND_ELF  == fatTextHeader->kind ){
                // an elf
                if ( fatTextHeader->flags  &FATBIN_FLAG_COMPRESS ){
                    // compressed
                    if (!(raw = malloc((size_t)fatTextHeader->decompressed_size))) {
                        goto cleanup_and_exit;
                    }
                    decompress((unsigned char *)start_ptr, (size_t)fatTextHeader->compressed_size, (unsigned char *)raw, (size_t)fatTextHeader->decompressed_size);
                    p = (char *)raw;
                } else {
                    // uncompressed
                    p = start_ptr;
                }

                free (raw);
                raw = NULL;
            } else if (FATBIN_KIND_PTX  == fatTextHeader->kind) {
                // PTX
                if ( fatTextHeader->flags & FATBIN_FLAG_COMPRESS ){
                    // compressed
                    if (!(raw = malloc((size_t)fatTextHeader->decompressed_size + 1))) {
                        goto cleanup_and_exit;
                    }
                    decompress((unsigned char *)start_ptr, (size_t)fatTextHeader->compressed_size, (unsigned char *)raw, (size_t)fatTextHeader->decompressed_size);
                    length = (size_t)fatTextHeader->decompressed_size;
                } else if ( (fatTextHeader->compressed_size >= 4) &&
                   (ZSTD_MAGIC_NUMBER == *(unsigned int *)start_ptr) ) {
                    // compressed, Zstandard
                    dictionary_t* const parsed_dict = create_dictionary();
                    size_t out_capacity = ZSTD_get_decompressed_size(start_ptr, fatTextHeader->compressed_size);
                    if (out_capacity == (size_t)-1) {
                        out_capacity = fatTextHeader->decompressed_size;
                    }
                    if (!(raw = malloc(out_capacity + 1))) {
                        goto cleanup_and_exit;
                    }
                    ZSTD_decompress_with_dict(raw, out_capacity,
                                              start_ptr, fatTextHeader->compressed_size,
                                              parsed_dict);
                    free_dictionary(parsed_dict);
                    length = out_capacity;
                } else {
                    // uncompressed
                    if (!(raw = malloc((size_t)fatTextHeader->size + 1))) {
                        goto cleanup_and_exit;
                    }
                    memcpy (raw, start_ptr, fatTextHeader->size);
                    length = fatTextHeader->size;
                }

                p = (char *)raw;
                *(p + length) = '\0';

                if (!(temp_buf = (char*)malloc(length + 1))) {
                    goto cleanup_and_exit;
                }

                if ( length >= sizeof (".version ") + sizeof (".target sm_20") + sizeof (".address_size ") ) {
                    ++count;

                    for (int iDevice = 0; iDevice < agent_count; iDevice++)
                    {
                        newSize = (length + 1000000L <= length * 2) ? (length * 2) : (length + 1000000L);
                        memcpy ( temp_buf, p, length + 1 ) ;
                        if ( (entry = (SlibList_t*)malloc(sizeof(Q_ELEM) + sizeof(*entry) + strlen(abbrevptr) + 128)) != NULL &&
                             (code = (char*)malloc(newSize)) != NULL ) {
                            memset (entry, 0, sizeof (*entry) );
                            entry->lpszName = (char*)entry + sizeof(*entry);
                            entry->code = code;

                            fused_kernel_converter (entry->code, newSize, temp_buf, agents[iDevice].device_isae);
                            snprintf (entry->lpszName, strlen(abbrevptr) + 128, "%s.%d.%s%s.ptx.fatbin",
                                abbrevptr, count, agents[iDevice].device_isae, kind_strs[MK_fusion]);
                            memcpy ( &entry->device_isa, agents[iDevice].device_isae, strlen ( agents[iDevice].device_isae )+1 );
                            entry->size = strlen(entry->code);

                            qelem = (Q_ELEM_PTR)((char *)entry + sizeof(*entry) + strlen(abbrevptr) + 128);
                            InitQElem( qelem ) ;
                            SetQueueElemPtr( qelem, entry ) ;
                            PushElem( list_ptr, qelem );
                        } else {
                            free(entry);
                        }

                        memcpy ( temp_buf, p, length + 1 ) ;
                        if ( (entry = (SlibList_t*)malloc(sizeof(Q_ELEM) + sizeof(*entry) + strlen(abbrevptr) + 128)) != NULL &&
                             (code = (char*)malloc(newSize)) != NULL ) {
                            memset (entry, 0, sizeof (*entry) );
                            entry->lpszName = (char*)entry + sizeof(*entry);
                            entry->code = code;

                            ptb_kernel_converter (entry->code, newSize, temp_buf, agents[iDevice].device_isae);
                            snprintf (entry->lpszName, strlen(abbrevptr) + 128, "%s.%d.%s%s.ptx.fatbin",
                                abbrevptr, count, agents[iDevice].device_isae, kind_strs[MK_persistent]);
                            memcpy ( &entry->device_isa, agents[iDevice].device_isae, strlen ( agents[iDevice].device_isae )+1 );
                            entry->size = strlen(entry->code);

                            qelem = (Q_ELEM_PTR)((char *)entry + sizeof(*entry) + strlen(abbrevptr) + 128);
                            InitQElem( qelem ) ;
                            SetQueueElemPtr( qelem, entry ) ;
                            PushElem( list_ptr, qelem );
                        } else {
                            free(entry);
                        }

                    }

                }

                free (raw);
                free (temp_buf);
                raw = temp_buf = NULL;
            }
            start_ptr += fatTextHeader->size;
            remaining_size -= fatTextHeader->size;
            acclen += fatTextHeader->header_size;
            acclen += fatTextHeader->size;
        }
    }

cleanup_and_exit:
    free (raw);
    free (temp_buf);
}

void find_cubin_offsetA(char *start_ptr, uint64_t remaining_size, unsigned module_kind, struct context_info *ctx, void *hOwner)
{
    computeFatBinaryFormat_t fatheader;
    fat_text_header  *fatTextHeader;
    uint64_t acclen;
    void *raw = NULL;
    char *p;
     int i;
     FUNCLIST *item ;

    while (remaining_size)
    {
        // padding
        while (remaining_size >= sizeof (*fatheader)) {
            fatheader = (computeFatBinaryFormat_t)start_ptr;
            if ( (fatheader->magic == FATBIN_MAGIC) &&
                 fatheader->version == FATBIN_VERSION )
               break;
            start_ptr++, remaining_size--;
        }

        if (!(remaining_size >= sizeof (*fatheader))) break;

        fatheader = (computeFatBinaryFormat_t)start_ptr;
        start_ptr +=sizeof (*fatheader);
        remaining_size -=sizeof (*fatheader);
        acclen=0;
        while (acclen < fatheader->fatSize)
        {
            fatTextHeader = (fat_text_header*) start_ptr;
            start_ptr += sizeof (*fatTextHeader);
            remaining_size -= sizeof (*fatTextHeader);
            if (sizeof(fat_text_header) < fatTextHeader->header_size){
                start_ptr += fatTextHeader->header_size-sizeof(fat_text_header);
                remaining_size -= fatTextHeader->header_size-sizeof(fat_text_header);
            }

            if ( FATBIN_KIND_ELF  == fatTextHeader->kind ){
                // an elf
                if ( fatTextHeader->flags  &FATBIN_FLAG_COMPRESS ){
                    // compressed
                    if (!(raw = malloc((size_t)fatTextHeader->decompressed_size))) {
                        goto cleanup_and_exit;
                    }
                    decompress((unsigned char *)start_ptr, (size_t)fatTextHeader->compressed_size, (unsigned char *)raw, (size_t)fatTextHeader->decompressed_size);
                    p = (char *)raw;
                } else if ( (fatTextHeader->compressed_size >= 4) &&
                   (ZSTD_MAGIC_NUMBER == *(unsigned int *)start_ptr) ) {
                    // compressed, Zstandard
                    dictionary_t* const parsed_dict = create_dictionary();
                    size_t out_capacity = ZSTD_get_decompressed_size(start_ptr, fatTextHeader->compressed_size);
                    if (out_capacity == (size_t)-1) {
                        out_capacity = fatTextHeader->decompressed_size;
                    }
                    if (!(raw = malloc(out_capacity + 1))) {
                        goto cleanup_and_exit;
                    }
                    ZSTD_decompress_with_dict(raw, out_capacity,
                                              start_ptr, fatTextHeader->compressed_size,
                                              parsed_dict);
                    free_dictionary(parsed_dict);
                    p = (char *)raw;
                } else {
                    // uncompressed
                    p = start_ptr;
                }

#if defined(__i386__) || defined(DETOURS_X86)
                Elf32_Ehdr *ehdr_info;
                Elf32_Shdr *sect_hdr;
                Elf32_Half strtab_idx;
                Elf32_Shdr *shstrtab;
                Elf32_Off shstrtab_offset;

                ehdr_info = (Elf32_Ehdr *)p;
                sect_hdr = (Elf32_Shdr *)(p + ehdr_info->e_shoff);
                strtab_idx = ehdr_info->e_shstrndx;
                shstrtab_offset = ehdr_info->e_shoff +
                                               (ehdr_info->e_shentsize * strtab_idx);
                shstrtab = (Elf32_Shdr *)(p + shstrtab_offset);

                /* Loop through all the section headers, looking for one whose
                   name is ".nv.info.".  */
                for (i = 0; i < ehdr_info->e_shnum; ++i)
                  {
                    Elf32_Off name_offset;
                    Elf32_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

                    sect_hdr = (Elf32_Shdr *)(p + offset);
                    name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

                    if (memcmp(p + name_offset, ".nv.info.", strlen (".nv.info.")) == 0
                        || memcmp(p + name_offset, ".text.", strlen (".text.")) == 0)
                      {
                        FUNCLIST temp = {};

                        temp.name = p + name_offset + (*(p + name_offset +1) != 't' ? strlen (".nv.info.") : strlen (".text."));
                        temp.kernels[MK_original].hOwner = hOwner;
                        if ( ( !(item = (PFUNCLIST)avl_find( ctx->name_map, &temp )) ) &&
                             ( (item = (FUNCLIST *)malloc(sizeof(*item) + strlen(temp.name) + 1)) != NULL ) ) {
                            memset (item, 0, sizeof (*item) );
                            item->name = (char *)item + sizeof(*item);
                            memcpy( item->name, temp.name, strlen(temp.name)+1 );
                            item->kernels[MK_original].hOwner = hOwner;
                            if (!avl_probe (ctx->name_map, item)){
                                 free ( item );
                                 item = NULL ;
                            }
                        }

                        if ( ( item != NULL )                   &&
                             ( !memcmp( p + name_offset, ".nv.info.", strlen (".nv.info.") ) ) ) {
                             item->kernels[module_kind].hOwner = module;
                             item->kernels[module_kind].reg_count = ((int)sect_hdr->sh_info & 0xFF000000) >> 24 ;
                        }
                      }
                  }

#elif defined(__x86_64__) || defined(DETOURS_X64)
                Elf64_Ehdr *ehdr_info;
                Elf64_Shdr *sect_hdr;
                Elf64_Half strtab_idx;
                Elf64_Shdr *shstrtab;
                Elf64_Off shstrtab_offset;

                ehdr_info = (Elf64_Ehdr *)p;
                sect_hdr = (Elf64_Shdr *)(p + ehdr_info->e_shoff);
                strtab_idx = ehdr_info->e_shstrndx;
                shstrtab_offset = ehdr_info->e_shoff +
                                               (ehdr_info->e_shentsize * strtab_idx);
                shstrtab = (Elf64_Shdr *)(p + shstrtab_offset);

                /* Loop through all the section headers, looking for one whose
                   name is ".nv.info.".  */
                for (i = 0; i < ehdr_info->e_shnum; ++i)
                  {
                    Elf64_Off name_offset;
                    Elf64_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

                    sect_hdr = (Elf64_Shdr *)(p + offset);
                    name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

                    if (memcmp(p + name_offset, ".nv.info.", strlen (".nv.info.")) == 0
                        || memcmp(p + name_offset, ".text.", strlen (".text.")) == 0)
                      {
                        FUNCLIST temp = {};

                        temp.name = p + name_offset + (*(p + name_offset +1) != 't' ? strlen (".nv.info.") : strlen (".text."));
                        temp.kernels[MK_original].hOwner = hOwner;
                        if ( ( !(item = (PFUNCLIST)avl_find( ctx->name_map, &temp )) ) &&
                             ( (item = (FUNCLIST *)malloc(sizeof(*item) + strlen(temp.name) + 1)) != NULL ) ) {
                            memset (item, 0, sizeof (*item) );
                            item->name = (char *)item + sizeof(*item);
                            memcpy( item->name, temp.name, strlen(temp.name)+1 );
                            item->kernels[MK_original].hOwner = hOwner;
                            if (!avl_probe (ctx->name_map, item)){
                                 free ( item );
                                 item = NULL ;
                            }
                        }

                        if ( ( item != NULL )                   &&
                             ( !memcmp( p + name_offset, ".nv.info.", strlen (".nv.info.") ) ) ) {
                             item->kernels[module_kind].hOwner = hOwner;
                             item->kernels[module_kind].reg_count = ((int)sect_hdr->sh_info & 0xFF000000) >> 24 ;
                        }
                      }
                  }

#endif

                free (raw);
                raw = NULL;
            } else if (FATBIN_KIND_PTX  == fatTextHeader->kind) {
                // PTX
                if ( fatTextHeader->flags & FATBIN_FLAG_COMPRESS ){
                    // compressed
                    if (!(raw = malloc((size_t)fatTextHeader->decompressed_size + 1))) {
                        goto cleanup_and_exit;
                    }
                    decompress((unsigned char *)start_ptr, (size_t)fatTextHeader->compressed_size, (unsigned char *)raw, (size_t)fatTextHeader->decompressed_size);
                } else if ( (fatTextHeader->compressed_size >= 4) &&
                   (ZSTD_MAGIC_NUMBER == *(unsigned int *)start_ptr) ) {
                    // compressed, Zstandard
                    dictionary_t* const parsed_dict = create_dictionary();
                    size_t out_capacity = ZSTD_get_decompressed_size(start_ptr, fatTextHeader->compressed_size);
                    if (out_capacity == (size_t)-1) {
                        out_capacity = fatTextHeader->decompressed_size;
                    }
                    if (!(raw = malloc(out_capacity + 1))) {
                        goto cleanup_and_exit;
                    }
                    ZSTD_decompress_with_dict(raw, out_capacity,
                                              start_ptr, fatTextHeader->compressed_size,
                                              parsed_dict);
                    free_dictionary(parsed_dict);
                } else {
                    // uncompressed
                    p = start_ptr;
                }

                free (raw);
                raw = NULL;
            }
            start_ptr += fatTextHeader->size;
            remaining_size -= fatTextHeader->size;
            acclen += fatTextHeader->header_size;
            acclen += fatTextHeader->size;
        }
    }

cleanup_and_exit:
    free (raw);
}


#ifdef __WIN__
// **************************************************************************
BOOL OpenFileMapped(LPWSTR wszFile, LPVOID *ppvFile, DWORD *pcbFile)
{
    BOOL fSuccess = FALSE;
    HANDLE  hMMF = NULL;
    HANDLE  hFile = INVALID_HANDLE_VALUE;
    LPVOID  pvFile = NULL;
    DWORD   fileSizeLow;
    DWORD   fileSizeHigh;

    *ppvFile = NULL;
    if (pcbFile != NULL)
        *pcbFile = 0;

    hFile = CreateFileW(wszFile, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        goto done;

    fileSizeLow = GetFileSize( hFile, &fileSizeHigh );
    if ( ( fileSizeLow == 0xFFFFFFFF ) &&
         ( GetLastError() != NO_ERROR ) )
        goto done;

    hMMF = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hMMF == NULL)
        goto done;

    pvFile = MapViewOfFile(hMMF, FILE_MAP_READ, 0, 0, 0);
    if (pvFile == NULL)
        goto done;

    *ppvFile = pvFile;
    if (pcbFile != NULL)
        *pcbFile = fileSizeLow;

    fSuccess = TRUE;
done:
    if (hMMF != NULL)
        CloseHandle(hMMF);
    if (hFile != NULL)
        CloseHandle(hFile);

    return fSuccess;
}

void
ComputeModInfo(
    void
    )
{
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;
    unsigned int i, ii;
    Q_ELEM_PTR     qe_ptr ;
    unsigned char  *pdmp = NULL;
    DWORD           dw;
    unsigned char *inBuf;
    dos_header_t dosHeader = {0};
    wchar_t        *file_name;
    Q_HEADER temp_list;
    pthread_mutex_t CritSect;
    int num_threads = 64;
    int tnum;
    int s;
    struct thread_info  *tinfo;
    SYSTEM_INFO  SystemInfo;        /* Passed to GetSystemInfo API. */
    Q_ELEM_PTR      CurrentPtr ;
    SlibList_t *entry;
    FILE*   fileHandle;
    wchar_t   date_str1[300] ;
    wchar_t   date_str2[300]  ;
    Q_HEADER   name_list;
    Q_ELEM_PTR q_elem_ptr;
    size_t    name_length;

    InitQueue( &temp_list );
    InitQueue( &name_list );

    pthread_mutex_lock( &g_csGlobal ) ;
    if ( agent_count == 0 ) {
         findCudaDevice();
    }

    if ( !CUDA_CORE_SOFT_LIMIT ) {
         goto exit;
    }

    pthread_mutex_init( &CritSect, NULL );
   // Get a list of all the modules in this process.

    if( EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded))
    {
        for ( i = 0; i < (cbNeeded / sizeof(HMODULE)); i++ )
        {
            WCHAR szModName[MAX_PATH];

            // Get the full path to the module's file.

            if ( GetModuleFileNameExW( hProcess, hMods[i], szModName,
                                      sizeof(szModName) / sizeof(WCHAR)))
            {
                for ( qe_ptr = QueueHead( &ModuleList ); qe_ptr != NULL; qe_ptr = QueueNext( qe_ptr ) ) {
                    if ( !wcscmp(( wchar_t* )QueuePtr( qe_ptr ), szModName) ) {
                        break ;
                    }
                }

                if ( qe_ptr == NULL ) {
                    wsprintfW (date_str1, L"%s.abc", Abbrev( szModName ));

                    fileHandle = NULL;
                    if( _wfopen_s( &fileHandle, date_str1, L"r" ) == 0 )
                    {
                        char line[300];
                        while(fgets(line,300,fileHandle) != NULL)
                        {
                            name_length = strlen( line );
                            if ( (entry = (SlibList_t *)malloc(sizeof(Q_ELEM) + sizeof(*entry) + name_length + 1)) != NULL ) {
                                memset (entry, 0, sizeof (*entry) );
                                entry->lpszName = (char *)entry + sizeof(*entry);
                                memcpy ( entry->lpszName, line, name_length + 1 ) ;
                                if ( name_length && (entry->lpszName[name_length-1] == '\n') ) entry->lpszName[name_length-1] = '\0' ;
                                q_elem_ptr = (Q_ELEM_PTR)((char *)entry + sizeof(*entry) + name_length + 1);
                                InitQElem( q_elem_ptr ) ;
                                SetQueueElemPtr( q_elem_ptr, entry ) ;
                                PushElem( &temp_list, q_elem_ptr );
                            }
                        }
                        fclose(fileHandle);
                    }

                    if( fileHandle == NULL &&
                        OpenFileMapped( szModName, (LPVOID *)&pdmp, &dw ) ) {

                        inBuf = pdmp;
                        memset (&dosHeader, 0, sizeof (dosHeader) );

                        // read headers
                        read_dos(&inBuf, &dosHeader);    
                        read_pe(pdmp, &inBuf, &dosHeader);
                        read_dataDir(&inBuf, &dosHeader);
                        read_sections(&inBuf, &dosHeader);

                        for ( ii = 0; ii < dosHeader.pe.numberOfSections; ii++ ) {
                            if ( !strncmp( dosHeader.section_table[ii].name, FATBIN_DATA_SECTION_NAME,sizeof (dosHeader.section_table[ii].name) - 2 ) ) {
                                char    ModuleName[MAX_PATH] = {0};
//                              print_section(dosHeader.section_table + ii);
                                if (::GetModuleFileNameExA(hProcess, hMods[i],
                                            ModuleName, sizeof(ModuleName))) {
                                    wsprintfW (date_str1, L"%s.abc123", Abbrev(szModName));
                                    _wfopen_s( &fileHandle, date_str1, L"wt" );
                                    CurrentPtr = QueueHead( &temp_list ) ;
                                    find_cubin_offset( (char *)pdmp + dosHeader.section_table[ii].ptrToRawData, dosHeader.section_table[ii].virtualSize, &temp_list, ModuleName ) ;
                                    for ( q_elem_ptr = QueueHead( &temp_list ); 
                                          CurrentPtr != q_elem_ptr; 
                                          q_elem_ptr = QueueNext( q_elem_ptr ) ) {
                                        entry = ( SlibList_t * )QueuePtr( q_elem_ptr );
                                        if ( fileHandle ) fprintf( fileHandle, "%s\n", entry->lpszName);
                                    }
                                    if ( fileHandle ) fclose( fileHandle );
                                }

                            }
                        }

                        free (dosHeader.section_table);
                    }

                    if ( ( file_name= (wchar_t*)malloc( sizeof(Q_ELEM)*2 + sizeof ( wchar_t )* (wcslen ( szModName ) + 1) ))) {
                        q_elem_ptr = (Q_ELEM_PTR)(file_name + wcslen ( szModName ) + 1);
                        memcpy ( file_name, szModName, sizeof ( wchar_t )* (wcslen ( szModName ) + 1) );
                        InitQElem( q_elem_ptr ) ;
                        SetQueueElemPtr( q_elem_ptr, file_name ) ;
                        EnQueueElem( &ModuleList, q_elem_ptr, FALSE );
                        q_elem_ptr ++;
                        InitQElem( q_elem_ptr ) ;
                        SetQueueElemPtr( q_elem_ptr, file_name ) ;
                        EnQueueElem( &name_list, q_elem_ptr, FALSE );
                    }

                    if ( pdmp != NULL ) {
                        UnmapViewOfFile(pdmp);
                        pdmp = NULL;
                    }
                }
            }
        }
    }

    SortQueue( &temp_list, CompareSize );

    GetSystemInfo(&SystemInfo);
    num_threads = (int)SystemInfo.dwNumberOfProcessors;

    if ( (QueueCount( &temp_list )) &&
         ( tinfo = ( struct thread_info  * ) malloc( num_threads * sizeof( struct thread_info ) ) ) != NULL ) {
        memset (tinfo, 0, num_threads * sizeof( struct thread_info ) );

        /* Create one thread for each command-line argument. */
        for (tnum = 0; tnum < num_threads; tnum++) {
            tinfo[tnum].thread_num = tnum + 1;
            tinfo[tnum].CritSect = &CritSect;
            tinfo[tnum].list_header = &temp_list;

            /* The pthread_create() call stores the thread ID into
               corresponding element of tinfo[]. */
            s = pthread_create(&tinfo[tnum].thread_id, NULL,
                               &thread_start, &tinfo[tnum]);
            if (s != 0)
                { errno = s; perror("pthread_create"); };
        }

        /* Now join with each thread, and display its returned value. */
        for (tnum = 0; tnum < num_threads; tnum++) {
            s = pthread_join(tinfo[tnum].thread_id, NULL);
            if (s != 0)
                { errno = s; perror("pthread_join"); };
        }

        free(tinfo);
    }

    while ( (q_elem_ptr = QueueHead(&name_list)) != NULL ) {
        RemoveElem( &name_list, q_elem_ptr ) ;
        wsprintfW (date_str1, L"%s.abc123", Abbrev((wchar_t *)GetQueueElemPtr( q_elem_ptr )));
        wsprintfW (date_str2, L"%s.abc", Abbrev((wchar_t *)GetQueueElemPtr( q_elem_ptr )));
        MoveFileW( date_str1, date_str2 ) ;
    }

    pthread_mutex_destroy( &CritSect );
    // Release the handle to the process.

    CloseHandle( hProcess );

    goto exit;
 exit:
    pthread_mutex_unlock( &g_csGlobal ) ;
}
#else /* __WIN__ */
static int OpenFileMapped(char *pszFile, void **ppvFile, size_t *pcbFile, int *pfd)
{
    int fSuccess = 0;
    int          fd = -1;
    struct stat  sb = {0};
    void         *addr = MAP_FAILED;

    fd = open(pszFile, O_RDONLY);
    if ( fd == -1 )
    {
        goto done;
    }

    if (fstat(fd, &sb) == -1)           /* To obtain file size */
    {
        fprintf(stderr, "GetFileSize failed, err=%d\n", errno);
        goto done;
    }

    addr = mmap(NULL, sb.st_size, PROT_READ,
                MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED)
    {
        fprintf(stderr, "MapViewOfFile failed, err=%d\n", errno);
        goto done;
    }

    *ppvFile = addr;
    *pcbFile = sb.st_size;
    *pfd = fd;

    fSuccess = 1;
done:
    if (!fSuccess && addr != MAP_FAILED)
        munmap(addr, sb.st_size);
    if (!fSuccess && fd != - 1)
        close(fd);

    return fSuccess;
}

void
ComputeModInfo(
    void
    )
{
    int i;
    FILE *fp;
    char prot_buf[5];
    char *shared_library_fullname;
    Q_ELEM_PTR qe_ptr;
    char *file_name;
    int          fd;
    char         *addr;
    size_t       length;
    Q_HEADER temp_list;
    pthread_mutex_t CritSect;
    int num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    int tnum;
    int s;
    struct thread_info  *tinfo;
    Q_ELEM_PTR      CurrentPtr ;
    SlibList_t *entry;
    FILE*   fileHandle;
    char   date_str1[300] ;
    char   date_str2[300]  ;
    Q_HEADER   name_list;
    Q_ELEM_PTR q_elem_ptr;
    size_t    name_length;
    Q_HEADER   preload;

    InitQueue( &temp_list );
    InitQueue( &name_list );
    InitQueue( &preload );

    pthread_mutex_lock( &g_csGlobal ) ;

    if ( agent_count == 0 ) {
         findCudaDevice();
    }
    if ( !CUDA_CORE_SOFT_LIMIT ) {
         goto exit;
    }

    pthread_mutex_init( &CritSect, NULL );

    /* Open the current process' maps file.  It describes one VMA per line.  */
    fp = fopen ("/proc/self/maps", "r");
    if (fp)
    {
        for (;;)
        {
            unsigned long start, end;
            int c;

            if (fscanf (fp, "%lx-%lx", &start, &end) != 2)
                break;

            while (c = getc (fp), ISSPACE(c)) continue;
            ungetc (c, fp);prot_buf[0] = getc (fp);
            prot_buf[1] = getc (fp);
            prot_buf[2] = getc (fp);
            prot_buf[3] = getc (fp);
            prot_buf[4] = '\0';

            /* Found it.  Now see if this line contains a filename.  */
            while (c = getc (fp), c != EOF && c != '\n' && c != '/')
                continue;
            if (prot_buf[2] == 'x')
            {
                size_t size;
                int len;

                ungetc (c, fp);
                shared_library_fullname = NULL; size = 0;
                len = getline (&shared_library_fullname, &size, fp);
                if (len >= 0)
                {
                    /* Success: filled shared_library_fullname.  */
                    if (len > 0 && shared_library_fullname[len - 1] == '\n')
                        shared_library_fullname[len - 1] = '\0';

                    for ( qe_ptr = QueueHead( &ModuleList ); 
                          NULL != qe_ptr; 
                          qe_ptr = QueueNext( qe_ptr ) ) {
                        if ( !strcmp(( char* )QueuePtr( qe_ptr ), shared_library_fullname) ) {
                            break;
                        }
                    }

                    if ( qe_ptr == NULL ) {
                        snprintf (date_str1, sizeof (date_str1), "%s.abc", AbbrevA( shared_library_fullname ));

                        if ((fileHandle = fopen( date_str1,"rt")) != NULL) // C4996
                        {
                            char line[300];
                            while(fgets(line,300,fileHandle) != NULL)
                            {
                                name_length = strlen( line );
                                if ( (entry = (SlibList_t *)malloc(sizeof(Q_ELEM) + sizeof(*entry) + name_length + 1)) != NULL ) {
                                    memset (entry, 0, sizeof (*entry) );
                                    entry->lpszName = (char *)entry + sizeof(*entry);
                                    memcpy ( entry->lpszName, line, name_length + 1 ) ;
                                    if ( name_length && (entry->lpszName[name_length-1] == '\n') ) entry->lpszName[name_length-1] = '\0' ;
                                    q_elem_ptr = (Q_ELEM_PTR)((char *)entry + sizeof(*entry) + name_length + 1);
                                    InitQElem( q_elem_ptr ) ;
                                    SetQueueElemPtr( q_elem_ptr, entry ) ;
                                    PushElem( &temp_list, q_elem_ptr );
                                }
                            }
                            fclose(fileHandle);
                        }

                        if ( fileHandle == NULL &&
                             OpenFileMapped( shared_library_fullname, (void **)&addr, &length, &fd ) ) {
#if defined(__i386__)
                            Elf32_Ehdr *ehdr_info;
                            Elf32_Shdr *sect_hdr;
                            Elf32_Half strtab_idx;
                            Elf32_Shdr *shstrtab;
                            Elf32_Off shstrtab_offset;

                            ehdr_info = (Elf32_Ehdr *)addr;
                            sect_hdr = (Elf32_Shdr *)(addr + ehdr_info->e_shoff);
                            strtab_idx = ehdr_info->e_shstrndx;
                            shstrtab_offset = ehdr_info->e_shoff +
                                                           (ehdr_info->e_shentsize * strtab_idx);
                            shstrtab = (Elf32_Shdr *)(addr + shstrtab_offset);

                            /* Loop through all the section headers, looking for one whose
                               name is ".nv_fatbin".  */
                            for (i = 0; i < ehdr_info->e_shnum; ++i)
                              {
                                Elf32_Off name_offset;
                                Elf32_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

                                sect_hdr = (Elf32_Shdr *)(addr + offset);
                                name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

                                if (memcmp (addr + name_offset, FATBIN_DATA_SECTION_NAME, strlen (FATBIN_DATA_SECTION_NAME)) == 0)
                                  {
                                    snprintf (date_str1, sizeof (date_str1), "%s.abc123", AbbrevA( shared_library_fullname ));
                                    fileHandle = fopen( date_str1,"wt");
                                    CurrentPtr = QueueHead( &temp_list ) ;
                                    find_cubin_offset( addr + sect_hdr->sh_offset, sect_hdr->sh_size, &temp_list, shared_library_fullname ) ;
                                    for ( q_elem_ptr = QueueHead( &temp_list ); 
                                          CurrentPtr != q_elem_ptr; 
                                          q_elem_ptr = QueueNext( q_elem_ptr ) ) {
                                        entry = ( SlibList_t * )QueuePtr( q_elem_ptr );
                                        if ( fileHandle ) fprintf( fileHandle, "%s\n", entry->lpszName);
                                    }
                                    if ( fileHandle ) fclose( fileHandle );
                                    break;
                                  }
                              }

#elif defined(__x86_64__)
                            Elf64_Ehdr *ehdr_info;
                            Elf64_Shdr *sect_hdr;
                            Elf64_Half strtab_idx;
                            Elf64_Shdr *shstrtab;
                            Elf64_Off shstrtab_offset;

                            ehdr_info = (Elf64_Ehdr *)addr;
                            sect_hdr = (Elf64_Shdr *)(addr + ehdr_info->e_shoff);
                            strtab_idx = ehdr_info->e_shstrndx;
                            shstrtab_offset = ehdr_info->e_shoff +
                                                           (ehdr_info->e_shentsize * strtab_idx);
                            shstrtab = (Elf64_Shdr *)(addr + shstrtab_offset);

                            /* Loop through all the section headers, looking for one whose
                               name is ".nv_fatbin".  */
                            for (i = 0; i < ehdr_info->e_shnum; ++i)
                              {
                                Elf64_Off name_offset;
                                Elf64_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

                                sect_hdr = (Elf64_Shdr *)(addr + offset);
                                name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

                                if (memcmp (addr + name_offset, FATBIN_DATA_SECTION_NAME, strlen (FATBIN_DATA_SECTION_NAME)) == 0)
                                  {
                                    snprintf (date_str1, sizeof (date_str1), "%s.abc123", AbbrevA( shared_library_fullname ));
                                    fileHandle = fopen( date_str1,"wt");
                                    CurrentPtr = QueueHead( &temp_list ) ;
                                    find_cubin_offset( addr + sect_hdr->sh_offset, sect_hdr->sh_size, &temp_list, shared_library_fullname ) ;
                                    for ( q_elem_ptr = QueueHead( &temp_list ); 
                                          CurrentPtr != q_elem_ptr; 
                                          q_elem_ptr = QueueNext( q_elem_ptr ) ) {
                                        entry = ( SlibList_t * )QueuePtr( q_elem_ptr );
                                        if ( fileHandle ) fprintf( fileHandle, "%s\n", entry->lpszName);
                                    }
                                    if ( fileHandle ) fclose( fileHandle );
                                    break;
                                  }
                              }

#endif

                            munmap(addr, length);
                            close(fd);
                        }

                        if ( ( file_name= (char*)malloc( sizeof(Q_ELEM)*2 + sizeof ( char )* (strlen ( shared_library_fullname ) + 1) ))) {
                            q_elem_ptr = (Q_ELEM_PTR)(file_name + strlen ( shared_library_fullname ) + 1);
                            memcpy ( file_name, shared_library_fullname, sizeof ( char )* (strlen ( shared_library_fullname ) + 1) );
                            InitQElem( q_elem_ptr ) ;
                            SetQueueElemPtr( q_elem_ptr, file_name ) ;
                            EnQueueElem( &ModuleList, q_elem_ptr, FALSE );
                            q_elem_ptr ++;
                            InitQElem( q_elem_ptr ) ;
                            SetQueueElemPtr( q_elem_ptr, file_name ) ;
                            EnQueueElem( &name_list, q_elem_ptr, FALSE );
                        }
                    }
                }
                free (shared_library_fullname);
            }
            while (c = getc (fp), c != EOF && c != '\n')
                continue;
        }
        fclose (fp);
    }

    SortQueue( &temp_list, CompareSize );

    if ( (QueueCount( &temp_list )) &&
         ( tinfo = ( struct thread_info  * ) malloc( num_threads * sizeof( struct thread_info ) ) ) != NULL ) {
        memset (tinfo, 0, num_threads * sizeof( struct thread_info ) );

        /* Create one thread for each command-line argument. */
        for (tnum = 0; tnum < num_threads; tnum++) {
            tinfo[tnum].thread_num = tnum + 1;
            tinfo[tnum].CritSect = &CritSect;
            tinfo[tnum].list_header = &temp_list;

            /* The pthread_create() call stores the thread ID into
               corresponding element of tinfo[]. */
            s = pthread_create(&tinfo[tnum].thread_id, NULL,
                               &thread_start, &tinfo[tnum]);
            if (s != 0)
                { errno = s; perror("pthread_create"); };
        }

        /* Now join with each thread, and display its returned value. */
        for (tnum = 0; tnum < num_threads; tnum++) {
            s = pthread_join(tinfo[tnum].thread_id, NULL);
            if (s != 0)
                { errno = s; perror("pthread_join"); };
        }

        free(tinfo);
    }

    while ( (q_elem_ptr = QueueHead(&name_list)) != NULL ) {
        RemoveElem( &name_list, q_elem_ptr ) ;
        snprintf (date_str1, sizeof (date_str1), "%s.abc123", AbbrevA( (char *)GetQueueElemPtr( q_elem_ptr ) ));
        snprintf (date_str2, sizeof (date_str2), "%s.abc", AbbrevA( (char *)GetQueueElemPtr( q_elem_ptr ) ));
        rename( date_str1, date_str2 ) ;
    }

    pthread_mutex_destroy( &CritSect );
    goto exit;
 exit:
    pthread_mutex_unlock( &g_csGlobal ) ;
}
#endif /* __WIN__ */

static void fetch_meta( FUNCLIST *p_this )
{
    int temp_buff[ 32 ] = { sizeof (temp_buff) } ;
    int i;

     if (!bar1) return ;

     for( i = 0; i < MK_MAX; i++ ) {
          if ( p_this->kernels[i].object != NULL ) {
               bar1( p_this->kernels[i].object, temp_buff ) ;
               p_this->kernels[i].reg_count = p_this->kernels[i].reg_count ? p_this->kernels[i].reg_count : temp_buff[2] ; // reg
               if ( cuFuncGetAttribute_fn != NULL) {
                    cuFuncGetAttribute_fn( &p_this->kernels[i].mem_sz, CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, (CUfunction)p_this->kernels[i].object ) ;
               } else {
                    p_this->kernels[i].mem_sz = temp_buff[8] ;
               }
//             temp_buff[11] ; // bar
          }
     }
}

static void destroy_ctx(struct context_info *ctx)
{
    struct avl_traverser trav;
    Q_ELEM_PTR elem_ptr;
     int               i ;

    if ( ! ctx ) {
         return;
    }

    pthread_mutex_lock( &g_csGlobal ) ;
    avl_delete( context, ctx );
    pthread_mutex_unlock( &g_csGlobal ) ;

     if ( ctx->name_map ) {
          avl_destroy( ctx->name_map, NULL );
     }

     if ( ctx->fns ) {
        FUNCLIST *first;
        for (first = (FUNCLIST *)avl_t_first (&trav, ctx->fns); first != NULL;
             first = (FUNCLIST *)avl_t_first (&trav, ctx->fns)) {
            avl_delete( ctx->fns, first );
            while ( (elem_ptr = QueueHead(&first->param_list)) != NULL ) {
                RemoveElem( &first->param_list, elem_ptr ) ;
                free( GetQueueElemPtr( elem_ptr ) ) ;
            }
            free( first ) ;
        }
        avl_destroy( ctx->fns, NULL ) ;
     }

     for ( i = 0; (i < MK_MAX); i++ ) {
          while ( (elem_ptr = QueueHead(&ctx->modList[i])) != NULL ) {
              if( (cuModuleUnload_fn != NULL) && (*(CUmodule *)QueuePtr( elem_ptr ) != NULL) ) {
                 cuModuleUnload_fn( *(CUmodule *)QueuePtr( elem_ptr ) ) ;
              }
              RemoveElem( &ctx->modList[i], elem_ptr ) ;
              free( QueuePtr( elem_ptr ) ) ;
          }
     }


    free( ctx ) ;
}

static struct context_info *create_ctx(CUcontext cuContext)
{
    struct context_info *ctx = NULL;

     Q_ELEM_PTR elem_ptr;
    SlibList_t *entry;
    CUmodule module;
    CUresult cuStatus;
     Q_ELEM_PTR          qelem ;
    void **p;
    CUdevice dev;
    int i;
    struct avl_traverser trav;
     CUmodule    *item ;
     FUNCLIST     *temp1 ;
     FUNCLIST     *temp2 ;
    struct kernargs *kernargs;

    ctx = (struct context_info*)malloc ( sizeof ( *ctx ) );

    if ( ! ctx ) {
         goto fn_fail;
    }

     memset( ctx, 0, sizeof( *ctx ) ) ;

    if ( cuCtxGetDevice_fn( &dev ) == CUDA_SUCCESS ) {
        cuDeviceGetAttribute_fn(&ctx->regsPerBlock, CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK, dev);
        cuDeviceGetAttribute_fn(&ctx->sharedMemPerBlock, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK, dev);
        cuDeviceGetAttribute_fn(&ctx->shared_optin, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN, dev);
        cuDeviceGetAttribute_fn(&ctx->maxThreadsPerBlock, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, dev);
        cuDeviceGetAttribute_fn(&ctx->warpSize, CU_DEVICE_ATTRIBUTE_WARP_SIZE, dev);
        for (i = 0; i < agent_count; i++) {
           if (  agents[i].device_id & (1 << dev) ) {
               ctx->agent = &agents[i] ;
                break ;
           }
        }
    }

    for (i = 0; i < MK_MAX; i++) {
        InitQueue( &ctx->modList[i] );
    }
    ctx->hcuContext = cuContext ;
    ctx->fns = avl_create (compare, NULL, NULL);
    ctx->name_map = avl_create (compareNames, NULL, NULL);

    if ( ! ctx->fns || ! ctx->name_map ) {
        goto fn_fail2;
    }

    pthread_mutex_lock( &g_csGlobal ) ;
    for ( elem_ptr = QueueTail( &library_list ); 
          NULL != elem_ptr; 
          elem_ptr = QueuePrev( elem_ptr ) ) {

        entry = (SlibList_t *)QueuePtr( elem_ptr );

        cuStatus = cuModuleLoadData_fn(&module, entry->elf);
        if (cuStatus != CUDA_SUCCESS)
        {
//          printf("cuModuleLoad error!\n");
        }
        else if ( (item = (CUmodule *)malloc(sizeof(Q_ELEM) + sizeof(*item))) != NULL ) {
            qelem = (Q_ELEM_PTR)(item + 1);
            *item = module ;
            InitQElem( qelem ) ;
            SetQueueElemPtr( qelem, item ) ;
            EnQueueElem( &ctx->modList[entry->module_kind], qelem, FALSE );

            find_cubin_offsetA ( (char *)entry->elf, entry->elfSize, entry->module_kind, ctx, module );
        }
    }

    p = avl_probe (context, ctx);

    pthread_mutex_unlock( &g_csGlobal ) ;

    if ( !p ) {
       goto fn_fail2;
    }

    pthread_mutex_lock( &g_csGlobal ) ;
    for (temp1 = (FUNCLIST *)avl_t_first (&trav, fns); temp1 != NULL;
         temp1 = (FUNCLIST *)avl_t_next (&trav)) {
        if ( (temp2 = (FUNCLIST *)malloc(sizeof(*temp2) + strlen(temp1->name) + 1)) != NULL ) {
            memset (temp2, 0, sizeof (*temp2) );
            temp2->name = (char *)temp2 + sizeof(*temp2);
            memcpy( temp2->name, temp1->name, strlen(temp1->name)+1 );
            InitQueue( &temp2->param_list ) ;
           for ( i = 0 ; i < MK_MAX ; i++ ) {
               if ( (cuKernelGetFunction_fn != NULL) &&
                    ( temp1->kernels[i].object != NULL ) ) {
                     cuKernelGetFunction_fn( (CUfunction*)&temp2->kernels[i].object, (CUkernel)temp1->kernels[i].object ) ;
                }
           }
          for( elem_ptr = QueueHead( &temp1->param_list ) ; elem_ptr != NULL ; elem_ptr = QueueNext( elem_ptr ) ) {
               if ( (kernargs = (struct kernargs *)malloc(sizeof(Q_ELEM) + sizeof(*kernargs))) != NULL ) {
                   qelem = (Q_ELEM_PTR)(kernargs + 1);
                   memcpy( kernargs, QueuePtr( elem_ptr ), sizeof(*kernargs) );
                   InitQElem( qelem ) ;
                   SetQueueElemPtr( qelem, kernargs ) ;
                   EnQueueElem( &temp2->param_list, qelem, FALSE );
               }
          }
          fetch_meta( temp2 ) ;
          temp2->kernels[MK_original].object = temp1->kernels[MK_original].object;
          temp2->kernels[MK_original].hOwner = temp1->kernels[MK_original].hOwner;
          avl_probe (ctx->fns, temp2);
          avl_probe (ctx->name_map, temp2);
        }
    }
    pthread_mutex_unlock( &g_csGlobal ) ;

#if defined(TIME_SLICE)
    // MPS clients do not expose a local channel for private timeslice control.
    if (!getenv("SELECT_SKIP_TIMESLICE")) {
        if ( timeslice_us == 0 ) {
            timeslice_us = GetTimeslice( ctx->hcuContext ) ;
            if ( !timeslice_us ) timeslice_us = 2047 ;
            my_slot->timeslice_us = timeslice_us ;
        } else if ( timeslice_us != GetTimeslice( ctx->hcuContext ) ) {
            SetTimeslice( ctx->hcuContext, timeslice_us ) ;
        }
    }
#endif

    // Initialization check: get device properties on first run
     if ( override_x_dim == -1 ) {
          override_x_dim = ctx->agent->num_sms;
        my_slot->override_x_dim = override_x_dim ;
     }

fn_exit:
    return ctx;

fn_fail2:
    destroy_ctx( ctx );
    ctx = NULL;

fn_fail:
    goto fn_exit;
}

static my_bool parseParam(const char *elf, const char *name, FUNCLIST *funcList)
{
    struct kernargs *kernargs;
    Q_ELEM_PTR          qelem ;
    int i;
    my_bool fSuccess;               // success or failure

    fSuccess = TRUE;

#if defined(__i386__) || defined(DETOURS_X86)
    Elf32_Ehdr *ehdr_info;
    Elf32_Shdr *sect_hdr;
    Elf32_Half strtab_idx;
    Elf32_Shdr *shstrtab;
    Elf32_Off shstrtab_offset;

    ehdr_info = (Elf32_Ehdr *)elf;
    sect_hdr = (Elf32_Shdr *)(elf + ehdr_info->e_shoff);
    strtab_idx = ehdr_info->e_shstrndx;
    shstrtab_offset = ehdr_info->e_shoff +
                                   (ehdr_info->e_shentsize * strtab_idx);
    shstrtab = (Elf32_Shdr *)(elf + shstrtab_offset);

    /* Loop through all the section headers, looking for one whose
       name is ".nv.info.".  */
    for (i = 0; i < ehdr_info->e_shnum; ++i)
      {
        Elf32_Off name_offset;
        Elf32_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

        sect_hdr = (Elf32_Shdr *)(elf + offset);
        name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

        if (memcmp (elf + name_offset, ".nv.info.", strlen (".nv.info.")) == 0 && 
            memcmp (elf + name_offset + strlen (".nv.info."), name, strlen (name)) == 0)
          {
            const NvInfoAttribute *PAttr;
            int count = 0;
            /* We found the section; get its load offset and
               size.  */
            GCN_DEBUG ("%s\n", elf + name_offset);
            PAttr = (const NvInfoAttribute *)(elf + sect_hdr->sh_offset);
            while (PAttr < (NvInfoAttribute *) (elf + sect_hdr->sh_offset + sect_hdr->sh_size)) {
                size_t size = 0;
                switch (PAttr->fmt) {
                    case EIFMT_NVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_BVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_HVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_SVAL:
                        size = sizeof(NvInfoAttribute) + PAttr->value;
                        break;
                }
                ++count;
                if (PAttr->attr == EIATTR_KPARAM_INFO) {
                    const NvInfoKParam *nvInfoKParam = (NvInfoKParam *) PAttr;

                    GCN_DEBUG ("\t<0x%d>\n", count);
                    GCN_DEBUG ("\tAttribute:\tEIATTR_KPARAM_INFO\n");
                    GCN_DEBUG ("\tFormat:\tEIFMT_SVAL\n");
                    GCN_DEBUG ("\tValue:\tIndex : 0x%x\tOrdinal : 0x%x\tOffset  : 0x%x\tSize    : 0x%x\n", nvInfoKParam->index, nvInfoKParam->ordinal, nvInfoKParam->offset, nvInfoKParam->size_bytes);
                    GCN_DEBUG ("\t\tPointee's logAlignment : 0x%x\tSpace : 0x%x\tcbank : 0x%x\tParameter Space : %s\n", nvInfoKParam->log_alignment, nvInfoKParam->space, nvInfoKParam->cbank, nvInfoKParam->is_cbank ? "SMEM" : "CBANK");

                    // add param to the list.
                    if ( (kernargs = (struct kernargs *)malloc(sizeof(Q_ELEM) + sizeof(*kernargs))) != NULL ) {
                        qelem = (Q_ELEM_PTR)(kernargs + 1);
                        kernargs->ordinal = nvInfoKParam->ordinal;
                        kernargs->offset = nvInfoKParam->offset;
                        kernargs->size_bytes = nvInfoKParam->size_bytes;
                        InitQElem( qelem ) ;
                        SetQueueElemPtr( qelem, kernargs ) ;
                        EnQueueElem( &funcList->param_list, qelem, FALSE );
                    } else {
                        fSuccess = FALSE;
                    }

                }
                PAttr = (NvInfoAttribute *) ((char *) PAttr + size);
            }
          }
      }

#elif defined(__x86_64__) || defined(DETOURS_X64)
    Elf64_Ehdr *ehdr_info;
    Elf64_Shdr *sect_hdr;
    Elf64_Half strtab_idx;
    Elf64_Shdr *shstrtab;
    Elf64_Off shstrtab_offset;

    ehdr_info = (Elf64_Ehdr *)elf;
    sect_hdr = (Elf64_Shdr *)(elf + ehdr_info->e_shoff);
    strtab_idx = ehdr_info->e_shstrndx;
    shstrtab_offset = ehdr_info->e_shoff +
                                   (ehdr_info->e_shentsize * strtab_idx);
    shstrtab = (Elf64_Shdr *)(elf + shstrtab_offset);

    /* Loop through all the section headers, looking for one whose
       name is ".nv.info.".  */
    for (i = 0; i < ehdr_info->e_shnum; ++i)
      {
        Elf64_Off name_offset;
        Elf64_Off offset = ehdr_info->e_shoff + (ehdr_info->e_shentsize * i);

        sect_hdr = (Elf64_Shdr *)(elf + offset);
        name_offset = shstrtab->sh_offset +  sect_hdr->sh_name;

        if (memcmp (elf + name_offset, ".nv.info.", strlen (".nv.info.")) == 0 && 
            memcmp (elf + name_offset + strlen (".nv.info."), name, strlen (name)) == 0)
          {
            const NvInfoAttribute *PAttr;
            int count = 0;
            /* We found the section; get its load offset and
               size.  */
            GCN_DEBUG ("%s\n", elf + name_offset);
            PAttr = (const NvInfoAttribute *)(elf + sect_hdr->sh_offset);
            while (PAttr < (NvInfoAttribute *) (elf + sect_hdr->sh_offset + sect_hdr->sh_size)) {
                size_t size = 0;
                switch (PAttr->fmt) {
                    case EIFMT_NVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_BVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_HVAL:
                        size = sizeof(NvInfoAttribute);
                        break;
                    case EIFMT_SVAL:
                        size = sizeof(NvInfoAttribute) + PAttr->value;
                        break;
                }
                ++count;
                if (PAttr->attr == EIATTR_KPARAM_INFO) {
                    const NvInfoKParam *nvInfoKParam = (NvInfoKParam *) PAttr;

                    GCN_DEBUG ("\t<0x%d>\n", count);
                    GCN_DEBUG ("\tAttribute:\tEIATTR_KPARAM_INFO\n");
                    GCN_DEBUG ("\tFormat:\tEIFMT_SVAL\n");
                    GCN_DEBUG ("\tValue:\tIndex : 0x%x\tOrdinal : 0x%x\tOffset  : 0x%x\tSize    : 0x%x\n", nvInfoKParam->index, nvInfoKParam->ordinal, nvInfoKParam->offset, nvInfoKParam->size_bytes);
                    GCN_DEBUG ("\t\tPointee's logAlignment : 0x%x\tSpace : 0x%x\tcbank : 0x%x\tParameter Space : %s\n", nvInfoKParam->log_alignment, nvInfoKParam->space, nvInfoKParam->cbank, nvInfoKParam->is_cbank ? "SMEM" : "CBANK");

                    // add param to the list.
                    if ( (kernargs = (struct kernargs *)malloc(sizeof(Q_ELEM) + sizeof(*kernargs))) != NULL ) {
                        qelem = (Q_ELEM_PTR)(kernargs + 1);
                        kernargs->ordinal = nvInfoKParam->ordinal;
                        kernargs->offset = nvInfoKParam->offset;
                        kernargs->size_bytes = nvInfoKParam->size_bytes;
                        InitQElem( qelem ) ;
                        SetQueueElemPtr( qelem, kernargs ) ;
                        EnQueueElem( &funcList->param_list, qelem, FALSE );
                    } else {
                        fSuccess = FALSE;
                    }

                }
                PAttr = (NvInfoAttribute *) ((char *) PAttr + size);
            }
          }
      }

#endif

    return fSuccess;
}

void waitIfNeeded() {
    if (!override_x_dim) {
        pthread_mutex_lock (&mutex);

        while (!override_x_dim)
        {
            // Buffer is full - sleep so consumers can get items.
            pthread_cond_wait (&cond, &mutex);
        }

        pthread_mutex_unlock (&mutex);
    }
}

CUresult    invokeA(struct context_info* ctx, const CUlaunchConfig *config, FUNCLIST *fnList, void **kernelParams, void **extra, int icount, int num_slices) {
    CUresult error_id = CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES;
   int i, j;
    int cb_size = 0;
    char *temp_buf = NULL ;
    void **lpv1 = NULL;
    char *lpv2 = NULL;
    void **ppv = NULL;
    unsigned long index = 0;
    int count = 0;
  CUlaunchConfig configA;
    int lowerbound;
    int upperbound;
    int *lower_bounds = (int *) &lowerbound;
    int *upper_bounds = (int *) &upperbound;
     int arr[129];
    int num_threads;

     if( icount<=0 || fnList->kernels[MK_persistent].object == 0 ) {
          goto done ;
     }

     memcpy ( &configA, config, sizeof ( configA ) );

    // align dynamic shared memory size to a multiple of 16 bytes.
    // for ``.extern .shared .align 16 .b8 meta[];''
    configA.sharedMemBytes = (config->sharedMemBytes + 15) / 16 * 16;
     memcpy ( (void *)&config->sharedMemBytes, &configA.sharedMemBytes, sizeof ( configA.sharedMemBytes ) );

      *lower_bounds = 0;
      *upper_bounds = configA.gridDimX * configA.gridDimY * configA.gridDimZ;
    num_threads = configA.blockDimX*configA.blockDimY*configA.blockDimZ;

    if ( (int)(sizeof(arr)/sizeof(arr[0])) <= num_slices ) {
         num_slices = (int)(sizeof(arr)/sizeof(arr[0]))-1;
    }

    arr[0] = 0;
    for (i = 1; i < num_slices + 1; i ++ ) {
         arr[i] = configA.gridDimX * configA.gridDimY * configA.gridDimZ * i / num_slices ;
    }

     if( arr[1] < icount ) {
          goto done ;
     }

    // Parameter Marshaling: Standard Arguments (void**)
    if ( kernelParams != NULL ) {
        lpv1 = (void **) malloc( QueueCount(&fnList->param_list) * sizeof(void *) );
        if ( lpv1 == NULL ) {
            error_id = CUDA_ERROR_OUT_OF_MEMORY;
            goto done ;
        }

        // Inject instrumentation parameters
        lpv1[count++] = (void *)&config->gridDimX;
        lpv1[count++] = (void *)&config->gridDimY;
        lpv1[count++] = (void *)&config->gridDimZ;
        lpv1[count++] = (void *)&config->blockDimZ;
        lpv1[count++] = (void *)&config->sharedMemBytes;
        lpv1[count++] = lower_bounds;
        lpv1[count++] = upper_bounds;
        lpv1[count++] = &num_threads;

        // Copy user parameters
        for ( i = count; (i < QueueCount(&fnList->param_list)); i++ ) {
            lpv1[ i ] = kernelParams[ i-count ] ;
        }
    }

    // Parameter Marshaling: Extra Arguments (struct packed)
    if ( extra != NULL ) {
        for( i = 0; (extra[i] != CU_LAUNCH_PARAM_END) ; i++ ) {
            if ( extra[i] == CU_LAUNCH_PARAM_BUFFER_SIZE ) {
                cb_size = *(int *)extra[++i] ;
            } else if ( extra[i] == CU_LAUNCH_PARAM_BUFFER_POINTER ) {
                temp_buf = (char *)extra[++i] ;
            }
        }

        lpv2 = (char *) malloc( sizeof (unsigned) * 8 + cb_size );
        ppv = (void **) malloc( sizeof(void *) * ( i + 1 ) );
        if ( (lpv2 == NULL) || (ppv == NULL) ) {
            error_id = CUDA_ERROR_OUT_OF_MEMORY;
            goto done ;
        }

        // Deep copy the original extra array to modify it
        memcpy ( ppv, extra, sizeof(void *) * ( i + 1 ) ) ;

        // Inject parameters into the binary blob
        *((unsigned int *)&lpv2[index]) = configA.gridDimX; index += sizeof (configA.gridDimX);
        *((unsigned int *)&lpv2[index]) = configA.gridDimY; index += sizeof (configA.gridDimY);
        *((unsigned int *)&lpv2[index]) = configA.gridDimZ; index += sizeof (configA.gridDimZ);
        *((unsigned int *)&lpv2[index]) = configA.blockDimZ; index += sizeof (configA.blockDimZ);
        *((unsigned int *)&lpv2[index]) = configA.sharedMemBytes; index += sizeof (configA.sharedMemBytes);
        lower_bounds = (int *)&lpv2[index]; *lower_bounds = lowerbound; index += sizeof (lowerbound);
        upper_bounds = (int *)&lpv2[index]; *upper_bounds = upperbound; index += sizeof (upperbound);
        *((int *)&lpv2[index]) = num_threads; index += sizeof (num_threads);

        memcpy (&lpv2[index], temp_buf, cb_size);

        // Fixup pointers
        for( i = 0; (ppv[i] != CU_LAUNCH_PARAM_END) ; i++ ) {
            if ( ppv[i] == CU_LAUNCH_PARAM_BUFFER_SIZE ) {
                *(int *)ppv[++i] = cb_size + sizeof (unsigned) * 8 ;
            } else if ( ppv[i] == CU_LAUNCH_PARAM_BUFFER_POINTER ) {
                ppv[++i] = lpv2 ;
            }
        }
    }

     i = ( 16-1 < arr[1] / icount ) ? ( 16-1 ) : ( arr[1] / icount );
     for ( ; i > 0; i-- ) {
          if ( (fnList->kernels[ MK_fusion ].object != NULL) &&
               fnList->kernels[ MK_fusion ].reg_count*(config->blockDimX* config->blockDimY* config->blockDimZ*(i+1)) <= (unsigned)ctx->regsPerBlock &&
               (config->sharedMemBytes*(i+1)+fnList->kernels[ MK_fusion ].mem_sz <= (unsigned)ctx->shared_optin) &&
               (config->blockDimX * config->blockDimY * config->blockDimZ*(i+1) <= (unsigned)ctx->maxThreadsPerBlock) &&
               (config->blockDimX*config->blockDimY*config->blockDimZ % ctx->warpSize == 0) ) {
             if ( cuLaunchKernelEx_fn != NULL ) {
                  configA.gridDimX = icount;
                  configA.gridDimY = configA.gridDimZ = 1;
                  configA.blockDimZ = config->blockDimZ*(i+1);
                  configA.sharedMemBytes = config->sharedMemBytes*(i+1);
                  for ( j = 1; j < num_slices+1; j++ ) {
                     *lower_bounds = arr[j-1];
                     *upper_bounds = arr[j];
                     waitIfNeeded ();
                     error_id = cuLaunchKernelEx_fn(&configA, (CUfunction)fnList->kernels[MK_fusion].object,
                                            lpv1 ? lpv1 : kernelParams, ppv ? ppv : extra);
                     if (j < num_slices) cuStreamSynchronize_fn(config->hStream);
                  }

             }
             else {
                  for ( j = 1; j < num_slices+1; j++ ) {
                     *lower_bounds = arr[j-1];
                     *upper_bounds = arr[j];
                     waitIfNeeded ();
                     error_id = cuLaunchKernel_fn((CUfunction)fnList->kernels[MK_fusion].object,  icount, 1, 1,
                                            config->blockDimX, config->blockDimY, config->blockDimZ*(i+1),
                                            config->sharedMemBytes*(i+1), config->hStream,
                                            lpv1 ? lpv1 : kernelParams, ppv ? ppv : extra);
                     if (j < num_slices) cuStreamSynchronize_fn(config->hStream);
                  }
             }
#   ifdef GATHERSTATS
             if( CUDA_SUCCESS == error_id ) {
                classes[MK_fusion]++;
             }
#   endif
             if ( ( error_id != CUDA_ERROR_INVALID_VALUE ) &&
                  ( error_id != CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES ) ) {
                 break;
             }
          }
     }

    if (CUDA_SUCCESS == error_id)
        goto done;

    if ( cuLaunchKernelEx_fn != NULL ) {
         memcpy ( &configA, config, sizeof ( configA ) );
         configA.gridDimX = icount;
         configA.gridDimY = configA.gridDimZ = 1;
         for ( j = 1; j < num_slices+1; j++ ) {
            *lower_bounds = arr[j-1];
            *upper_bounds = arr[j];
            waitIfNeeded ();
            error_id = cuLaunchKernelEx_fn(&configA, (CUfunction)fnList->kernels[MK_persistent].object,
                                   lpv1 ? lpv1 : kernelParams, ppv ? ppv : extra);
            if (j < num_slices) cuStreamSynchronize_fn(configA.hStream);
         }
    } else {
         for ( j = 1; j < num_slices+1; j++ ) {
            *lower_bounds = arr[j-1];
            *upper_bounds = arr[j];
            waitIfNeeded ();
            error_id = cuLaunchKernel_fn((CUfunction)fnList->kernels[MK_persistent].object,  icount, 1, 1,
                                   config->blockDimX, config->blockDimY, config->blockDimZ,
                                   config->sharedMemBytes, config->hStream,
                                   lpv1 ? lpv1 : kernelParams, ppv ? ppv : extra);
            if (j < num_slices) cuStreamSynchronize_fn(configA.hStream);
         }
    }

#   ifdef GATHERSTATS
    if( CUDA_SUCCESS == error_id ) {
       classes[MK_persistent]++;
    }
#   endif

     /* here for error processing */
done:
    free( lpv1 ) ;
    free( lpv2 ) ;
    free( ppv ) ;
    return error_id;
}

CUresult cuLaunchKernelA(const CUlaunchConfig *config, CUfunction f, void **kernelParams, void **extra)
{
    CUcontext   hcuContext = NULL;
    struct context_info* ctx = NULL;
    FUNCLIST              *  fnList = NULL;
     CUresult          error_id        = CUDA_ERROR_UNKNOWN ;

    if ( cuCtxGetCurrent_fn != NULL ) {
        cuCtxGetCurrent_fn( &hcuContext ) ;
        pthread_mutex_lock( &g_csGlobal ) ;
        ctx = (struct context_info *)avl_find( context, &hcuContext ) ;
        pthread_mutex_unlock( &g_csGlobal ) ;
        if ( !ctx ) {
           ctx = create_ctx( hcuContext ) ;
        }
    }
    // Context registration above is still required for timeslice control.
    // Without PTB, use the original launch below, including its pause gate;
    // kernel argument metadata and transformed kernels are unnecessary.
    if (!ctx || !CUDA_CORE_SOFT_LIMIT)
        goto fallback;

    // Search for the given kernel
    pthread_mutex_lock( &g_csGlobal ) ;
    fnList = (FUNCLIST *)avl_find( ctx->fns, &f ) ;
    pthread_mutex_unlock( &g_csGlobal ) ;

    // Check if the stream is currently capturing, which might require fallback
    if ( cuStreamIsCapturing_fn != NULL ) {
        CUstreamCaptureStatus ret_val ;
        if ( ( cuStreamIsCapturing_fn( config->hStream, &ret_val ) == CUDA_SUCCESS ) &&
             ( ret_val != CU_STREAM_CAPTURE_STATUS_NONE ) ) {
            goto fallback;
        }
    }

    if ( !(fnList != NULL && QueueCount( &fnList->param_list )) ) {
         goto fallback;
    }

    if ( ( error_id = invokeA( ctx, config, fnList, kernelParams, extra, override_x_dim, nSlices ) ) != CUDA_SUCCESS ) {
         goto fallback ;
    }

     return( error_id ) ;
fallback:
    waitIfNeeded();
    if (cuLaunchKernelEx_fn)
      error_id = cuLaunchKernelEx_fn(config, f, kernelParams, extra);
    else error_id = (cuLaunchKernel_fn(f,
                          config->gridDimX, config->gridDimY, config->gridDimZ, /* grid dim */
                          config->blockDimX, config->blockDimY, config->blockDimZ, /* block dim */
                          config->sharedMemBytes, config->hStream, /* shared mem, stream */
                          kernelParams, /* arguments */
                          extra));
#   ifdef GATHERSTATS
    if( CUDA_SUCCESS == error_id ) {
       classes[MK_original]++;
    }
#   endif
    return error_id;
}


static const char *nexttoken(const char *q,  int sep)
{
    if (q)
        q = strchr(q, sep);
    if (q)
        q++;
    return q;
}

static int nextnumber(const char *str, char **end, unsigned int *result)
{
    errno = 0;
    if (str == NULL || *str == '\0' || !isdigit(*str))
        return -EINVAL;
    *result = (unsigned int) strtoul(str, end, 10);
    if (errno)
        return -errno;
    if (str == *end)
        return -EINVAL;
    return 0;
}

/*
 * Parses string with list of CPU ranges.
 * Returns 0 on success.
 * Returns 1 on error.
 * Returns 2 if fail is set and a cpu number passed in the list doesn't fit
 * into the cpu_set. If fail is not set cpu numbers that do not fit are
 * ignored and 0 is returned instead.
 */
int cpulist_parse(const char *str, uint32_t *set, size_t setsize, int fail)
{
    const size_t max = (8 * (setsize));
    const char *p, *q;
    char *end = NULL;

    q = str;
    memset(set, 0, setsize);

    while (p = q, q = nexttoken(q, ','), p) {
        unsigned int a;    /* beginning of range */
        unsigned int b;    /* end of range */
        unsigned int s;    /* stride */
        const char *c1, *c2;

        if (nextnumber(p, &end, &a) != 0 ||
            (*end && *end != ',' && *end != '-'))
            return 1;
        b = a;
        s = 1;
        p = end;

        c1 = nexttoken(p, '-');
        c2 = nexttoken(p, ',');

        if (c1 != NULL && (c2 == NULL || c1 < c2)) {
            if (nextnumber(c1, &end, &b) != 0 ||
                (*end && *end != ',' && *end != ':'))
                return 1;

            c1 = nexttoken(end, ':');

            if (c1 != NULL && (c2 == NULL || c1 < c2)) {
                if (nextnumber(c1, &end, &s) != 0 ||
                    (*end && *end != ','))
                    return 1;
                if (s == 0)
                    return 1;
            }
        }

        if (!(a <= b))
            return 1;
        while (a <= b) {
            if (a >= max) {
                if (fail)
                    return 2;
                else
                    break;
            }
            set[a / (8 * sizeof (uint32_t))] |= (uint32_t) 1 << (a % (8 * sizeof (uint32_t)));
            a += s;
        }
    }

    if (end && *end)
        return 1;
    return 0;
}

/* Initialize debug and suppress_host_fallback according to the environment.  */

static void
init_environment_variables (void)
{
#if defined(__WIN__)
   size_t requiredSize;
    char buff[32];

   getenv_s( &requiredSize, NULL, 0, "GCN_DEBUG");
  if (requiredSize != 0)
    debug = TRUE;
  else
    debug = FALSE;

   getenv_s( &requiredSize, buff, sizeof(buff) / sizeof(char), "GCN_NUM_THREADS" );
  if (!requiredSize)
    getenv_s( &requiredSize, buff, sizeof(buff) / sizeof(char), "GCN_NUM_WORKERS" );
  if (requiredSize)
    override_x_dim = atoi (buff);

   getenv_s( &requiredSize, NULL, 0, "CUDA_CORE_SOFT_LIMIT");
  if (requiredSize != 0)
    CUDA_CORE_SOFT_LIMIT = TRUE;
  else
    CUDA_CORE_SOFT_LIMIT = FALSE;

#else
  if (getenv ("GCN_DEBUG"))
    debug = TRUE;
  else
    debug = FALSE;

  const char *z = getenv ("GCN_NUM_THREADS");
  if (!z)
    z = getenv ("GCN_NUM_WORKERS");
  if (z)
    override_x_dim = atoi (z);

#if defined (MEM_CONTROL)
  memory_limit = get_limit_from_env ("CUDA_DEVICE_MEMORY_LIMIT_0");
  if (!memory_limit)
    memory_limit = get_limit_from_env ("CUDA_DEVICE_MEMORY_LIMIT");
#endif

  if (getenv ("PARTITION_GLOBAL"))
    PARTITION_GLOBAL = TRUE;
  else
    PARTITION_GLOBAL = FALSE;

  if (getenv ("PARTITION_STREAM"))
    PARTITION_STREAM = TRUE;
  else
    PARTITION_STREAM = FALSE;

  if (getenv ("CUDA_CORE_SOFT_LIMIT"))
    CUDA_CORE_SOFT_LIMIT = TRUE;
  else
    CUDA_CORE_SOFT_LIMIT = FALSE;

  z = getenv ("CUDA_DEVICE_SM_LIMIT");
  if (z) {
    cpulist_parse (z, g_sm_maskA, sizeof(g_sm_maskA), 0);
    g_sm_maskA[0] = ~g_sm_maskA[0];
    g_sm_maskA[1] = ~g_sm_maskA[1];
    g_sm_maskA[2] = ~g_sm_maskA[2];
    g_sm_maskA[3] = ~g_sm_maskA[3];
  }
#endif /* defined(__WIN__) */

}

static pid_t select_init_pid;

static void
bye(void)
{
#if defined(__linux__)
    if (select_init_pid && select_init_pid != getpid()) return;
#endif
#ifdef PROFILING
  {
    Q_ELEM_PTR elem_ptr;
  CUptiResult status;
  CUpti_Activity *record = NULL;
     int    count;
     count = 0;

  finiTrace();

     while ( (elem_ptr = QueueHead(&dlist)) != NULL ) {
          RemoveElem( &dlist, elem_ptr ) ;
          do {
            status = cuptiActivityGetNextRecord(((struct _MyDebugInfo *)GetQueueElemPtr( elem_ptr ))->buffer, ((struct _MyDebugInfo *)GetQueueElemPtr( elem_ptr ))->validSize, &record);
            if(status == CUPTI_SUCCESS) {
              printActivity(record, &count);
            }
            else if (status == CUPTI_ERROR_MAX_LIMIT_REACHED) {
              break;
            }
            else if (status == CUPTI_ERROR_INVALID_KIND) {
              break;
            }
            else {
              CUPTI_CALL(status);
            }
          } while (1);

          free( GetQueueElemPtr( elem_ptr ) ) ;
     }
  }
#endif

  {
#if defined(__WIN__)
    SetEvent (g_hExitEvent);
#else /* __WIN__ */
    mqd_t mqdes;
    char queue_path[ 32 ];
    char buff[MSG_SIZE];
    sprintf( queue_path, "/PropertyA_DA_%x", getpid() );

    mqdes = mq_open(queue_path, O_RDWR);
    if (mqdes != (mqd_t) -1) {
        memcpy (buff, "exit", sizeof("exit"));
        mq_send(mqdes, buff, MSG_SIZE, 0);
        mq_close (mqdes);
        mq_unlink (queue_path);
    }
#endif /* defined(__WIN__) */

    pthread_join(con, NULL);

#if defined(__WIN__)
    CloseHandle (g_hExitEvent);
#endif /* defined(__WIN__) */

  }

  {
    Q_ELEM_PTR elem_ptr;
    struct avl_traverser trav;

    while ( (elem_ptr = QueueHead(&ModuleList)) != NULL ) {
        RemoveElem( &ModuleList, elem_ptr ) ;
        free( GetQueueElemPtr( elem_ptr ) ) ;
    }

    while ( (elem_ptr = QueueHead(&library_list)) != NULL ) {
        SlibList_t *list;
        list = (SlibList_t *)QueuePtr( elem_ptr );
        RemoveElem( &library_list, elem_ptr ) ;
        free( list->code ) ;
        free( list->elf ) ;
        free( list ) ;
    }

    if ( context != 0 ) {
        struct context_info* ctx;
        for (ctx = (struct context_info *)avl_t_first (&trav, context); ctx != NULL;
             ctx = (struct context_info *)avl_t_first (&trav, context)) {
            avl_delete( context, ctx );
            destroy_ctx( ctx ) ;
        }
        avl_destroy( context, NULL ) ;
    }

    if ( aq != 0 ) {
        avl_destroy( aq, NULL ) ;
    }

    if ( fns != NULL ) {
        FUNCLIST *funcList;
        for (funcList = (FUNCLIST *)avl_t_first (&trav, fns); funcList != NULL;
             funcList = (FUNCLIST *)avl_t_first (&trav, fns)) {
            avl_delete( fns, funcList );
            while ( (elem_ptr = QueueHead(&funcList->param_list)) != NULL ) {
                RemoveElem( &funcList->param_list, elem_ptr ) ;
                free( GetQueueElemPtr( elem_ptr ) ) ;
            }
            free( funcList ) ;
        }
    }

  }

   if (pBuf != NULL)
   {
        my_slot->pid = 0;

#if defined(__WIN__)
       UnmapViewOfFile(pBuf);
#else
        /* Detach shared memory segment.  */
        shmdt(pBuf);
#endif /* defined(__WIN__) */
   }

#if defined(__WIN__)
   if (hMapFile != NULL)
   {
       CloseHandle(hMapFile);

   }
#endif /* defined(__WIN__) */

    /* Finally, clean up the special lock */
    pthread_mutex_destroy(&g_csGlobal);

#   ifdef GATHERSTATS
    /* debug print message */
    printf("\n\n     Total    Passed    Failed\n");
    printf("   ==============================\n");
    printf("%10d%10d%10d\n", passed + failed, passed, failed);

  for (int i = 0; i < (int)(sizeof (classes) / sizeof (classes[0])); ++i) {
    if ((i % 8) == 0)
      printf("\n      ");
    else
      printf(" ");
    printf("0x%05x,", classes[i]);
  }
  printf("\n");
#   endif
}

// Initialization
void DetourFunctions() ;
void DetachFunctions() ;

  static my_pthread_once_t once_control= MY_PTHREAD_ONCE_INIT;

/* Linux UVM control is opt-in and does not change the shared slot/MQ ABI.
 * Counters below measure completed prefetch requests, never physical residency.
 * Lock order: life -> state. CUDA calls hold life only, at most one chunk;
 * status/list never acquire life. A snapshot carries IDs, not dangling pointers.
 */
#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <limits.h>

static CUresult (CUDAAPI *uvm_alloc_fn)(CUdeviceptr *, size_t, unsigned int);
/* Explicit signatures: CUDA headers may alias the public prefetch names. */
static CUresult (CUDAAPI *uvm_prefetch_v1)(CUdeviceptr, size_t, CUdevice, CUstream);
static CUresult (CUDAAPI *uvm_prefetch_v2)(CUdeviceptr, size_t, CUmemLocation, unsigned int, CUstream);
static CUresult (CUDAAPI *uvm_destroy_fn)(CUcontext);
typedef CUresult (CUDAAPI *uvm_primary_fn)(CUdevice);
static uvm_primary_fn uvm_release_v1, uvm_release_v2;
static uvm_primary_fn uvm_reset_v1, uvm_reset_v2;

struct uvm_allocation {
    uint64_t id;
    CUdeviceptr ptr;
    size_t bytes;
    CUcontext ctx;
    CUdevice device;
    int concurrent;
    uvm_allocation *next;
};
struct uvm_range {
    uint64_t id;
    size_t offset, bytes;
    CUdevice device;
    uvm_range *next;
};
static pthread_mutex_t uvm_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t uvm_life = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t uvm_wake = PTHREAD_COND_INITIALIZER;
static pthread_once_t uvm_start_once = PTHREAD_ONCE_INIT;
static pid_t uvm_pid;
static int uvm_enabled, uvm_convert, uvm_stop, uvm_busy;
static int uvm_listener_fd = -1, uvm_client_fd = -1;
static int uvm_worker_started, uvm_listener_started;
static pthread_t uvm_worker_thread, uvm_listener_thread;
static size_t uvm_chunk = 16u * 1024u * 1024u, uvm_page = 4096;
static uvm_allocation *uvm_allocations;
static uvm_range *uvm_job;
static uint64_t uvm_next_id, uvm_bytes, uvm_count, uvm_allocation_generation;
static uint64_t uvm_job_id, uvm_completed_job, uvm_job_bytes;
static uint64_t uvm_cpu_bytes, uvm_gpu_bytes;
static CUresult uvm_last_error = CUDA_SUCCESS;
static char uvm_socket_path[sizeof(((sockaddr_un *)0)->sun_path)];
static dev_t uvm_socket_dev;
static ino_t uvm_socket_ino;

static int uvm_inherited(void)
{
    /* Entry wrappers can arrive concurrently before the old init once ends. */
    pid_t owner = __atomic_load_n(&uvm_pid, __ATOMIC_ACQUIRE);
    return owner && owner != getpid();
}

static int uvm_active(void)
{
    return uvm_enabled && !uvm_inherited();
}

static void uvm_fork_child(void)
{
    /* No inherited mutex or CUDA call is safe here. close, never shutdown or
     * unlink: these descriptors and the pathname still belong to the parent. */
    if (uvm_listener_fd >= 0) close(uvm_listener_fd);
    if (uvm_client_fd >= 0) close(uvm_client_fd);
    uvm_listener_fd = uvm_client_fd = -1;
}

static int uvm_uint(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    if (!s || !*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (*s - '0')) / 10)
            return 0;
        n = n * 10 + (*s - '0');
    }
    *out = n;
    return 1;
}

static void uvm_load(void *driver)
{
    *(void **)&uvm_alloc_fn = dlsym_ptr(driver, "cuMemAllocManaged");
    *(void **)&uvm_prefetch_v1 = dlsym_ptr(driver, "cuMemPrefetchAsync");
    *(void **)&uvm_prefetch_v2 = dlsym_ptr(driver, "cuMemPrefetchAsync_v2");
    *(void **)&uvm_destroy_fn = dlsym_ptr(driver, "cuCtxDestroy_v2");
    *(void **)&uvm_release_v1 = dlsym_ptr(driver, "cuDevicePrimaryCtxRelease");
    *(void **)&uvm_release_v2 = dlsym_ptr(driver, "cuDevicePrimaryCtxRelease_v2");
    *(void **)&uvm_reset_v1 = dlsym_ptr(driver, "cuDevicePrimaryCtxReset");
    *(void **)&uvm_reset_v2 = dlsym_ptr(driver, "cuDevicePrimaryCtxReset_v2");
    const char *s = getenv("SELECT_UVM_ENABLE");
    uvm_enabled = s && !strcmp(s, "1");
    s = getenv("SELECT_UVM_CONVERT_ALLOC");
    uvm_convert = s && !strcmp(s, "1");
    uint64_t mb;
    s = getenv("SELECT_UVM_CHUNK_MB");
    if (uvm_uint(s, &mb) && mb >= 1 && mb <= 256)
        uvm_chunk = (size_t)mb * 1024 * 1024;
    long page = sysconf(_SC_PAGESIZE);
    if (page > 0) uvm_page = (size_t)page;
    __atomic_store_n(&uvm_pid, getpid(), __ATOMIC_RELEASE);
    if (pthread_atfork(NULL, NULL, uvm_fork_child) != 0)
        uvm_enabled = 0;
}

static uvm_allocation *uvm_find(uint64_t id)
{
    for (uvm_allocation *a = uvm_allocations; a; a = a->next)
        if (a->id == id) return a;
    return NULL;
}

static void uvm_remove(CUdeviceptr ptr, CUcontext ctx)
{
    /* Caller owns life; a NULL ctx means removal by exact allocation pointer. */
    uint64_t removed = 0;
    pthread_mutex_lock(&uvm_state);
    uvm_allocation **link = &uvm_allocations;
    while (*link) {
        uvm_allocation *a = *link;
        if ((ctx && a->ctx == ctx) || (!ctx && a->ptr == ptr)) {
            *link = a->next;
            uvm_bytes -= a->bytes;
            --uvm_count;
            ++removed;
            free(a);
        } else link = &a->next;
    }
    if (removed) ++uvm_allocation_generation;
    pthread_mutex_unlock(&uvm_state);
}

static uint64_t uvm_remove_by_device(CUdevice device)
{
    /* Caller owns life. Do not retain/release to discover the primary context:
     * invalidate this device conservatively, including explicit-context records.
     * A release need not destroy the context; valid allocations may become
     * untracked, but a stale snapshot can never follow a recycled context/VA. */
    uint64_t removed = 0;
    pthread_mutex_lock(&uvm_state);
    uvm_allocation **link = &uvm_allocations;
    while (*link) {
        uvm_allocation *a = *link;
        if (a->device == device) {
            *link = a->next;
            uvm_bytes -= a->bytes;
            --uvm_count;
            ++removed;
            free(a);
        } else link = &a->next;
    }
    if (removed) ++uvm_allocation_generation;
    pthread_mutex_unlock(&uvm_state);
    return removed;
}

static void uvm_free_ranges(uvm_range *r)
{
    while (r) {
        uvm_range *next = r->next;
        free(r);
        r = next;
    }
}

static int uvm_add_range(uvm_range ***tail, uvm_allocation *a,
                         size_t offset, size_t bytes, CUdevice device)
{
    if (!bytes) return 1;
    uvm_range *r = (uvm_range *)calloc(1, sizeof(*r));
    if (!r) return 0;
    r->id = a->id;
    r->offset = offset;
    r->bytes = bytes;
    r->device = device;
    **tail = r;
    *tail = &r->next;
    return 1;
}

static void uvm_saturating_add(uint64_t *value, size_t bytes)
{
    *value = bytes > UINT64_MAX - *value ? UINT64_MAX : *value + bytes;
}

static CUresult uvm_migrate_chunk(uvm_range *range, size_t done, size_t bytes)
{
    CUresult result;
    CUstream stream = NULL;
    CUdeviceptr ptr = 0;
    CUcontext ctx = NULL;
    pthread_mutex_lock(&uvm_life);
    pthread_mutex_lock(&uvm_state);
    uvm_allocation *a = uvm_find(range->id);
    if (a) {
        ptr = a->ptr + range->offset + done;
        ctx = a->ctx;
    }
    int stopping = uvm_stop;
    pthread_mutex_unlock(&uvm_state);
    if (stopping) result = CUDA_ERROR_DEINITIALIZED;
    else if (!a) result = CUDA_ERROR_INVALID_VALUE;
    else {
        result = cuCtxSetCurrent_fn(ctx);
        if (result == CUDA_SUCCESS)
            result = cuStreamCreate_fn(&stream, CU_STREAM_NON_BLOCKING);
        if (result == CUDA_SUCCESS) {
            if (uvm_prefetch_v1)
                result = uvm_prefetch_v1(ptr, bytes, range->device, stream);
            else {
                CUmemLocation location = {};
                location.type = range->device == CU_DEVICE_CPU ?
                    CU_MEM_LOCATION_TYPE_HOST : CU_MEM_LOCATION_TYPE_DEVICE;
                location.id = range->device == CU_DEVICE_CPU ? 0 : range->device;
                result = uvm_prefetch_v2(ptr, bytes, location, 0, stream);
            }
            if (result == CUDA_SUCCESS)
                result = cuStreamSynchronize_fn(stream);
        }
        if (stream) {
            CUresult destroyed = cuStreamDestroy_fn(stream);
            if (result == CUDA_SUCCESS) result = destroyed;
        }
        /* The worker owns its thread's context; never synchronizes the app. */
        CUresult cleared = cuCtxSetCurrent_fn(NULL);
        if (result == CUDA_SUCCESS) result = cleared;
    }
    pthread_mutex_unlock(&uvm_life);
    return result;
}

static void *uvm_worker(void *)
{
    pthread_mutex_lock(&uvm_state);
    for (;;) {
        while (!uvm_busy && !uvm_stop) pthread_cond_wait(&uvm_wake, &uvm_state);
        if (uvm_stop && !uvm_busy) break;
        uvm_range *job = uvm_job;
        pthread_mutex_unlock(&uvm_state);
        CUresult result = CUDA_SUCCESS;
        for (uvm_range *r = job; r && result == CUDA_SUCCESS; r = r->next) {
            size_t done = 0;
            while (done < r->bytes) {
                size_t bytes = r->bytes - done;
                if (bytes > uvm_chunk) bytes = uvm_chunk;
                result = uvm_migrate_chunk(r, done, bytes);
                if (result != CUDA_SUCCESS) break;
                done += bytes;
                pthread_mutex_lock(&uvm_state);
                uvm_saturating_add(&uvm_job_bytes, bytes);
                uvm_saturating_add(r->device == CU_DEVICE_CPU ?
                                   &uvm_cpu_bytes : &uvm_gpu_bytes, bytes);
                pthread_mutex_unlock(&uvm_state);
            }
        }
        uvm_free_ranges(job);
        pthread_mutex_lock(&uvm_state);
        uvm_job = NULL;
        uvm_last_error = result;
        uvm_completed_job = uvm_job_id;
        uvm_busy = 0;
        if (uvm_stop) break;
    }
    pthread_mutex_unlock(&uvm_state);
    return NULL;
}

static void uvm_send(int fd, const char *s)
{
    size_t left = strlen(s);
    while (left) {
        ssize_t n = send(fd, s, left, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        s += n;
        left -= (size_t)n;
    }
}

static void uvm_error(int fd, const char *error)
{
    char reply[192];
    snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"%s\"}\n", error);
    uvm_send(fd, reply);
}

static void uvm_command(int fd, char *line)
{
    char *words[8], *save = NULL;
    int count = 0;
    for (char *s = strtok_r(line, " \t\r", &save); s;
         s = strtok_r(NULL, " \t\r", &save)) {
        if (count == 8) { uvm_error(fd, "invalid_command"); return; }
        words[count++] = s;
    }
    if (!count) { uvm_error(fd, "invalid_command"); return; }
    if (count == 1 && !strcmp(words[0], "status")) {
        char reply[640];
        pthread_mutex_lock(&uvm_state);
        snprintf(reply, sizeof(reply),
            "{\"ok\":true,\"pid\":%ld,\"managed_bytes\":%llu,\"allocation_count\":%llu,"
            "\"allocation_generation\":%llu,\"busy\":%s,\"job_id\":%llu,"
            "\"completed_job\":%llu,\"last_error\":%d,"
            "\"completed_cpu_bytes\":%llu,\"completed_gpu_bytes\":%llu,\"job_completed_bytes\":%llu}\n",
            (long)uvm_pid, (unsigned long long)uvm_bytes, (unsigned long long)uvm_count,
            (unsigned long long)uvm_allocation_generation,
            uvm_busy ? "true" : "false", (unsigned long long)uvm_job_id,
            (unsigned long long)uvm_completed_job, (int)uvm_last_error,
            (unsigned long long)uvm_cpu_bytes, (unsigned long long)uvm_gpu_bytes,
            (unsigned long long)uvm_job_bytes);
        pthread_mutex_unlock(&uvm_state);
        uvm_send(fd, reply);
        return;
    }
    if (count == 1 && !strcmp(words[0], "list")) {
        pthread_mutex_lock(&uvm_state);
        if (uvm_count > (SIZE_MAX - 64) / 128) {
            pthread_mutex_unlock(&uvm_state);
            uvm_error(fd, "out_of_memory"); return;
        }
        size_t capacity = 64 + (size_t)uvm_count * 128;
        char *reply = (char *)malloc(capacity);
        if (!reply) {
            pthread_mutex_unlock(&uvm_state);
            uvm_error(fd, "out_of_memory"); return;
        }
        size_t pos = (size_t)snprintf(reply, capacity,
            "{\"ok\":true,\"allocation_generation\":%llu,\"allocations\":[",
            (unsigned long long)uvm_allocation_generation);
        for (uvm_allocation *a = uvm_allocations; a; a = a->next)
            pos += (size_t)snprintf(reply + pos, capacity - pos,
                "%s{\"id\":%llu,\"bytes\":%llu,\"device\":%d}",
                a == uvm_allocations ? "" : ",", (unsigned long long)a->id,
                (unsigned long long)a->bytes, a->device);
        snprintf(reply + pos, capacity - pos, "]}\n");
        pthread_mutex_unlock(&uvm_state);
        uvm_send(fd, reply);
        free(reply);
        return;
    }
    int blocksv = count == 6 && !strcmp(words[0], "blocksv");
    int prefetch = count == 5 && !strcmp(words[0], "prefetch");
    int target = count == 2 && !strcmp(words[0], "target");
    int targetv = count == 3 && !strcmp(words[0], "targetv");
    int retarget = count == 4 && !strcmp(words[0], "retarget");
    int full_cache_refresh = count == 8 && !strcmp(words[0], "windowfullv");
    int windowv = full_cache_refresh || (count == 8 && !strcmp(words[0], "windowv"));
    uint64_t id = 0, offset = 0, bytes = 0;
    uint64_t block = 0, expected_count = 0, old_begin = 0, old_end = 0;
    uint64_t new_begin = 0, new_end = 0;
    if ((!prefetch && !target && !targetv && !retarget && !windowv && !blocksv) ||
        (blocksv && (!uvm_uint(words[1], &id) || !uvm_uint(words[2], &block) ||
                     !uvm_uint(words[3], &expected_count) || !uvm_uint(words[4], &bytes))) ||
        (windowv && (!uvm_uint(words[1], &id) || !uvm_uint(words[2], &block) ||
                     !uvm_uint(words[3], &expected_count) ||
                     !uvm_uint(words[4], &old_begin) || !uvm_uint(words[5], &old_end) ||
                     !uvm_uint(words[6], &new_begin) || !uvm_uint(words[7], &new_end))) ||
        (target && !uvm_uint(words[1], &bytes)) ||
        (targetv && (!uvm_uint(words[1], &id) ||
                     !uvm_uint(words[2], &bytes))) ||
        (retarget && (!uvm_uint(words[1], &id) ||
                      !uvm_uint(words[2], &offset) ||
                      !uvm_uint(words[3], &bytes))) ||
        (prefetch && (!uvm_uint(words[1], &id) || !uvm_uint(words[2], &offset) ||
                      !uvm_uint(words[3], &bytes) ||
                      (strcmp(words[4], "cpu") && strcmp(words[4], "gpu"))))) {
        uvm_error(fd, "invalid_command"); return;
    }
    const char *error = NULL;
    uvm_range *job = NULL, **tail = &job;
    uint64_t accepted = 0;
    pthread_mutex_lock(&uvm_state);
    if (uvm_busy) error = "busy";
    else if (uvm_stop) error = "stopping";
    else if (!uvm_worker_started || (!uvm_prefetch_v1 && !uvm_prefetch_v2))
        error = "not_supported";
    else if (uvm_job_id == UINT64_MAX) error = "job_id_exhausted";
    else if (prefetch) {
        uvm_allocation *a = uvm_find(id);
        if (!a) error = "unknown_id";
        else if (!bytes || offset >= a->bytes || bytes > a->bytes - offset)
            error = "invalid_range";
        else if (offset % uvm_page || (bytes % uvm_page && bytes != a->bytes - offset))
            error = "unaligned_range";
        else if (!a->concurrent) error = "concurrent_managed_access_required";
        else if (!uvm_add_range(&tail, a, (size_t)offset, (size_t)bytes,
                                !strcmp(words[4], "cpu") ? CU_DEVICE_CPU : a->device))
            error = "out_of_memory";
    } else if (blocksv) {
        /* GPU-only sparse blocks. The explicit bitset is independent of the
         * old single window; cold pages remain under UVM demand reclamation. */
        uint64_t matched = 0;
        size_t digits = strlen(words[5]);
        if (id != uvm_allocation_generation) error = "stale_allocations";
        else if (!block || !bytes || bytes > block || bytes % uvm_page || !expected_count)
            error = "invalid_block_geometry";
        else if (!digits || digits > 256) error = "invalid_block_mask";
        uint64_t blocks = error ? 0 : (block / bytes + (block % bytes != 0));
        int any = 0;
        for (size_t d = 0; d < digits && !error; ++d) {
            char ch = words[5][digits - 1 - d];
            int value = ch >= '0' && ch <= '9' ? ch - '0' :
                        ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
            if (value < 0) { error = "invalid_block_mask"; break; }
            for (unsigned bit = 0; bit < 4; ++bit) if (value & (1 << bit)) {
                any = 1;
                if (d * 4 + bit >= blocks) error = "block_out_of_range";
            }
        }
        if (!error && !any) error = "empty_block_mask";
        for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
            if (a->bytes != block) continue;
            matched++;
            if (!a->concurrent) error = "concurrent_managed_access_required";
        }
        if (!error && matched != expected_count) error = "cache_allocation_count_mismatch";
        for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
            if (a->bytes != block) continue;
            uint64_t begin = UINT64_MAX;
            for (uint64_t index = 0; index <= blocks; ++index) {
                size_t digit = (size_t)(index / 4);
                int selected = 0;
                if (index < blocks && digit < digits) {
                    char ch = words[5][digits - 1 - digit];
                    int value = ch <= '9' ? ch - '0' : ch - 'a' + 10;
                    selected = (value >> (index % 4)) & 1;
                }
                if (selected && begin == UINT64_MAX) begin = index;
                if (!selected && begin != UINT64_MAX) {
                    uint64_t end = index > block / bytes ? block : index * bytes;
                    if (!uvm_add_range(&tail, a, (size_t)(begin * bytes),
                                       (size_t)(end - begin * bytes), a->device))
                        error = "out_of_memory";
                    begin = UINT64_MAX;
                }
            }
        }
    } else if (windowv) {
        /* One generation-checked plan covers the same byte window in every
         * cache allocation. Initial placement prepares non-cache allocations
         * in full. Updates only touch the symmetric difference of the windows.
         * The eviction phase precedes every growth phase. */
        int initial = old_begin == UINT64_MAX && old_end == UINT64_MAX;
        uint64_t matched = 0;
        if (id != uvm_allocation_generation) error = "stale_allocations";
        else if (!block || !expected_count || new_begin > new_end || new_end > block ||
                 new_begin % uvm_page || (new_end != block && new_end % uvm_page) ||
                 (!initial && (old_begin > old_end || old_end > block ||
                  old_begin % uvm_page || (old_end != block && old_end % uvm_page))))
            error = "invalid_window";
        for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
            if (a->bytes == block) matched++;
            if ((initial || a->bytes == block) && !a->concurrent)
                error = "concurrent_managed_access_required";
        }
        if (!error && matched != expected_count) error = "cache_allocation_count_mismatch";
        for (int phase = 0; phase < 2 && !error; ++phase) {
            for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
                if (a->bytes != block) {
                    if (initial && phase && !uvm_add_range(&tail, a, 0, a->bytes, a->device))
                        error = "out_of_memory";
                    continue;
                }
                uint64_t begin[2], finish[2];
                int spans = 2;
                if (initial || full_cache_refresh) {
                    if (!phase) {
                        begin[0] = 0; finish[0] = new_begin;
                        begin[1] = new_end; finish[1] = block;
                    } else {
                        begin[0] = new_begin; finish[0] = new_end; spans = 1;
                    }
                } else {
                    uint64_t source_begin = phase ? new_begin : old_begin;
                    uint64_t source_end = phase ? new_end : old_end;
                    uint64_t other_begin = phase ? old_begin : new_begin;
                    uint64_t other_end = phase ? old_end : new_end;
                    begin[0] = source_begin;
                    finish[0] = source_end < other_begin ? source_end : other_begin;
                    begin[1] = source_begin > other_end ? source_begin : other_end;
                    finish[1] = source_end;
                }
                for (int j = 0; j < spans && !error; ++j) {
                    if (finish[j] > begin[j] &&
                        !uvm_add_range(&tail, a, (size_t)begin[j],
                                       (size_t)(finish[j] - begin[j]),
                                       phase ? a->device : CU_DEVICE_CPU))
                        error = "out_of_memory";
                }
            }
        }
    } else if (retarget) {
        uint64_t generation = id, old_target = offset, new_target = bytes;
        if (generation != uvm_allocation_generation)
            error = "stale_allocations";
        else if (old_target > uvm_bytes || new_target > uvm_bytes)
            error = "target_exceeds_managed_bytes";
        /* Both boundaries must satisfy the same allocation-relative alignment
         * rule as target. */
        for (int boundary = 0; boundary < 2 && !error; ++boundary) {
            uint64_t remaining = boundary ? new_target : old_target;
            for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
                size_t gpu = remaining > a->bytes ? a->bytes : (size_t)remaining;
                remaining -= gpu;
                if (gpu != a->bytes && gpu % uvm_page) error = "unaligned_target";
                else if (!a->concurrent) error = "concurrent_managed_access_required";
            }
        }
        if (!error && old_target != new_target) {
            uint64_t lower = old_target < new_target ? old_target : new_target;
            uint64_t upper = old_target < new_target ? new_target : old_target;
            uint64_t base = 0;
            for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
                uint64_t end = base + a->bytes;
                uint64_t begin = lower > base ? lower : base;
                uint64_t finish = upper < end ? upper : end;
                if (finish > begin &&
                    !uvm_add_range(&tail, a, (size_t)(begin - base),
                                   (size_t)(finish - begin),
                                   new_target > old_target ? a->device : CU_DEVICE_CPU))
                    error = "out_of_memory";
                base = end;
            }
        }
    } else {
        if (targetv && id != uvm_allocation_generation)
            error = "stale_allocations";
        else if (bytes > uvm_bytes) error = "target_exceeds_managed_bytes";
        /* Allocation list is insertion/ID order. A partial split must be page
         * aligned relative to that allocation, except at its final boundary. */
        uint64_t remaining = bytes;
        for (uvm_allocation *a = uvm_allocations; a && !error; a = a->next) {
            size_t gpu = remaining > a->bytes ? a->bytes : (size_t)remaining;
            remaining -= gpu;
            if (gpu != a->bytes && gpu % uvm_page) error = "unaligned_target";
            else if (!a->concurrent) error = "concurrent_managed_access_required";
        }
        /* Evict the CPU portion before prefetching any GPU portion. */
        for (int phase = 0; phase < 2 && !error; ++phase) {
            remaining = bytes;
            for (uvm_allocation *a = uvm_allocations; a; a = a->next) {
                size_t gpu = remaining > a->bytes ? a->bytes : (size_t)remaining;
                remaining -= gpu;
                if (!uvm_add_range(&tail, a, phase ? 0 : gpu,
                                   phase ? gpu : a->bytes - gpu,
                                   phase ? a->device : CU_DEVICE_CPU)) {
                    error = "out_of_memory"; break;
                }
            }
        }
    }
    if (!error) {
        uvm_job = job;
        uvm_busy = 1;
        uvm_job_bytes = 0;
        uvm_last_error = CUDA_SUCCESS;
        accepted = ++uvm_job_id;
        pthread_cond_signal(&uvm_wake);
    }
    pthread_mutex_unlock(&uvm_state);
    if (error) {
        uvm_free_ranges(job);
        uvm_error(fd, error);
    } else {
        char reply[96];
        snprintf(reply, sizeof(reply), "{\"ok\":true,\"job_id\":%llu}\n",
                 (unsigned long long)accepted);
        uvm_send(fd, reply);
    }
}

static void *uvm_listener(void *)
{
    for (;;) {
        struct pollfd pfd = {uvm_listener_fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 200);
        pthread_mutex_lock(&uvm_state);
        int stopping = uvm_stop;
        pthread_mutex_unlock(&uvm_state);
        if (stopping) break;
        if (ready <= 0 || !(pfd.revents & POLLIN)) continue;
        int fd = accept4(uvm_listener_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue;
        pthread_mutex_lock(&uvm_state);
        uvm_client_fd = fd;
        stopping = uvm_stop;
        pthread_mutex_unlock(&uvm_state);
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (!stopping && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
            size == sizeof(peer) && peer.uid == getuid()) {
            /* A partial line cannot monopolize the listener indefinitely. */
            struct timeval timeout = {1, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            char line[512];
            size_t used = 0;
            int complete = 0, invalid = 0;
            while (used + 1 < sizeof(line)) {
                char c;
                ssize_t n = recv(fd, &c, 1, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n != 1) break;
                if (c == '\n') { complete = 1; break; }
                if (!c) { invalid = 1; break; }
                line[used++] = c;
            }
            line[used] = 0;
            if (complete && !invalid) uvm_command(fd, line);
            else uvm_error(fd, "invalid_line");
        }
        pthread_mutex_lock(&uvm_state);
        uvm_client_fd = -1;
        close(fd);
        pthread_mutex_unlock(&uvm_state);
    }
    return NULL;
}

static void uvm_unlink_owned(void)
{
    struct stat st;
    if (uvm_socket_ino && lstat(uvm_socket_path, &st) == 0 &&
        S_ISSOCK(st.st_mode) && st.st_uid == getuid() &&
        st.st_dev == uvm_socket_dev && st.st_ino == uvm_socket_ino)
        unlink(uvm_socket_path);
    uvm_socket_ino = 0;
}

static void uvm_shutdown(void)
{
    if (!uvm_active()) return;
    pthread_mutex_lock(&uvm_state);
    uvm_stop = 1;
    if (uvm_client_fd >= 0) shutdown(uvm_client_fd, SHUT_RDWR);
    pthread_cond_broadcast(&uvm_wake);
    pthread_mutex_unlock(&uvm_state);
    if (uvm_listener_started) pthread_join(uvm_listener_thread, NULL);
    if (uvm_worker_started) pthread_join(uvm_worker_thread, NULL);
    uvm_listener_started = uvm_worker_started = 0;
    if (uvm_listener_fd >= 0) close(uvm_listener_fd);
    uvm_listener_fd = -1;
    uvm_unlink_owned();
}

static void uvm_start(void)
{
    if (!uvm_active()) return;
    if (!uvm_alloc_fn || (!uvm_prefetch_v1 && !uvm_prefetch_v2) ||
        !cuCtxGetCurrent_fn || !cuCtxGetDevice_fn || !cuCtxSetCurrent_fn ||
        !cuDeviceGetAttribute_fn || !cuStreamCreate_fn || !cuStreamSynchronize_fn ||
        !cuStreamDestroy_fn || !cuMemFree_fn || !uvm_destroy_fn) {
        GCN_WARNING("SELECT UVM: required CUDA entry point unavailable\n");
        return;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    snprintf(uvm_socket_path, sizeof(uvm_socket_path), "/tmp/select-uvm-%lu-%ld.sock",
             (unsigned long)getuid(), (long)getpid());
    memcpy(address.sun_path, uvm_socket_path, strlen(uvm_socket_path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    /* Never remove a pre-existing pathname, even if it looks stale. */
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        GCN_WARNING("SELECT UVM: socket bind failed: %s\n", strerror(errno));
        close(fd); return;
    }
    struct stat st;
    if (lstat(uvm_socket_path, &st) != 0) { close(fd); return; }
    uvm_socket_dev = st.st_dev;
    uvm_socket_ino = st.st_ino;
    if (chmod(uvm_socket_path, 0600) != 0 || listen(fd, 16) != 0) {
        close(fd); uvm_unlink_owned(); return;
    }
    uvm_listener_fd = fd;
    if (atexit(uvm_shutdown) != 0) {
        close(fd); uvm_listener_fd = -1; uvm_unlink_owned(); return;
    }
    if (pthread_create(&uvm_worker_thread, NULL, uvm_worker, NULL) != 0) {
        close(fd); uvm_listener_fd = -1; uvm_unlink_owned(); return;
    }
    uvm_worker_started = 1;
    if (pthread_create(&uvm_listener_thread, NULL, uvm_listener, NULL) != 0) {
        uvm_shutdown(); return;
    }
    uvm_listener_started = 1;
}
#endif /* __linux__ */

static void
init(void)
{
    select_init_pid = getpid();
    error_occurred = 1;
  init_environment_variables ();

#if !defined(__WIN__)
    if (atexit(bye) != 0) {
        GCN_WARNING("cannot set exit function\n");
        exit(EXIT_FAILURE);
    }
#endif  /* not __WIN__ */

#if !defined(__WIN__)
    DetourFunctions();
#endif  /* not __WIN__ */

    pthread_mutex_init(&g_csGlobal, NULL);

    InitQueue( &ModuleList );
    InitQueue( &library_list );

    context = avl_create (compare, NULL, NULL);
    if (context == NULL)
    {
        GCN_WARNING ("  Out of memory creating tree.\n");
        goto fail;
    }

    aq = avl_create (compare, NULL, NULL);
    if (aq == NULL)
    {
        GCN_WARNING ("  Out of memory creating tree.\n");
        goto fail;
    }

    fns = avl_create (compare, NULL, NULL);
    if (fns == NULL)
    {
        GCN_WARNING ("  Out of memory creating tree.\n");
        goto fail;
    }

#if defined(__WIN__)
    g_hExitEvent = CreateEvent (NULL, FALSE, FALSE, NULL);

    if (!g_hExitEvent) {
        GCN_WARNING("Failed to create exit event with error\n");
        goto fail;
    }
#endif /* defined(__WIN__) */

  if (pthread_mutex_init(&mutex, NULL) != 0) {                                  
    perror("pthread_mutex_init() error");                                       
    goto fail;
  }                                                                             

  if (pthread_cond_init(&cond, NULL) != 0) {                                    
    perror("pthread_cond_init() error");                                        
    goto fail;
  }                                                                             

    /* now you create the thread */
    if (pthread_create(&con, NULL, connection_thread, 
                       (void *)NULL) != 0)
    {
        GCN_WARNING("Could not create thread\n");
        goto fail;
    }

    do {
#if defined(__WIN__)
        WCHAR szName[]=L"MyFileMappingObject";

        hMapFile = CreateFileMappingW(
                      INVALID_HANDLE_VALUE,    // use paging file
                      NULL,                    // default security
                      PAGE_READWRITE,          // read/write access
                      0,                       // maximum object size (high-order DWORD)
                      BUF_SIZE,                // maximum object size (low-order DWORD)
                      szName);                 // name of mapping object

        if (hMapFile == NULL)
        {
            printf("Could not create file mapping object.\n");
            goto fail;
        }
        pBuf = (char*) MapViewOfFile(hMapFile,   // handle to map object
                             FILE_MAP_ALL_ACCESS, // read/write permission
                             0,
                             0,
                             BUF_SIZE);

        if (pBuf == NULL)
        {
           printf("Could not map view of file.\n");

            CloseHandle(hMapFile);

           goto fail;
        }

        my_slot = (shrreg_proc_slot_t *)pBuf;
        for (;;) {
            if ( ! InterlockedCompareExchange ( & my_slot->pid , getpid() , 0 ) ) {
                break;
            }
            my_slot++ ;
        }
#else /* __WIN__ */
        key_t key= 2647;
        char *p;

        char name_buffer[100];
        if (gethostname(name_buffer, sizeof(name_buffer)) == -1) {
            goto fail;
        }
        name_buffer[sizeof(name_buffer) - 1] = 0;

        p = name_buffer;
        while (*p != '\0') {
            key += *p + 1;
            p++;
        }

        /* Create shared memory and semaphore set containing one
           semaphore. */

        shmid = shmget(key, BUF_SIZE, IPC_CREAT | 0600);
        if ( shmid == -1 ) {
            perror( "shmget" );
            goto fail;
        }

        /* Attach shared memory into our address space. */
        pBuf = (char*) shmat(shmid, NULL, 0);
        if ( pBuf == (void *) -1 ) {
            perror( "shmat" );
            goto fail;
        }

        my_slot = (shrreg_proc_slot_t *)pBuf;
        for (;;) {
            if ( ! __sync_val_compare_and_swap ( & my_slot->pid , 0 , getpid() ) ) {
                break;
            }
            my_slot++ ;
        }
#endif /* defined(__WIN__) */

        my_slot->override_x_dim = override_x_dim;
        my_slot->nSlices = nSlices;
        memcpy ( &my_slot->mask, g_sm_maskA, sizeof ( my_slot->mask ) );
#if defined(TIME_SLICE)
        my_slot->timeslice_us = timeslice_us;
#endif
#if defined(MEM_CONTROL)
        my_slot->memory_limit = memory_limit;
#endif

    } while (0);

#if defined(TIME_SLICE)
    if ( cuGetExportTable_fn != NULL ) {
        void** tbl_base;

#if defined(_MSC_VER)
    #pragma warning(disable: 4310) // cast truncates constant value
#endif


        static const CUuuid callback_funcs_id = {{0x14, 0x06, 0x18, 0x0d, 0x72, (char)0xd6, (char)0xcb, 0x47, (char)0xab, 0x6e, (char)0xb9, 0x63, (char)0xe5, 0x1b, (char)0xf4, (char)0xc6}};
#if defined(_MSC_VER)
    #pragma warning(default: 4310) // cast truncates constant value
#endif


        cuGetExportTable_fn( (const void**)&tbl_base, &callback_funcs_id ) ;
        *(void **)&foo1 = *(tbl_base + 1);
        *(void **)&foo2 = *(tbl_base + 42);
    }

#endif

     if ( PARTITION_GLOBAL ) {
          setup_sm_control_callback() ;
     }

    // Private kernel metadata callbacks are needed only for legacy PTB.
    // Keep the separate timeslice export table above enabled.
    if ( CUDA_CORE_SOFT_LIMIT && cuGetExportTable_fn != NULL ) {
        void** tbl_base = NULL;

#if defined(_MSC_VER)
    #pragma warning(disable: 4310) // cast truncates constant value
#endif


        static const CUuuid callback_funcs_id = {{ 0x6E, 0x16, 0x3F, (char)0xBE, (char)0xB9, 0x58, 0x44, 0x4D, (char)0x83, 0x5C, (char)0xE1, (char)0x82, (char)0xAF, (char)0xF1, (char)0x99, 0x1E}};
#if defined(_MSC_VER)
    #pragma warning(default: 4310) // cast truncates constant value
#endif


        if ( cuGetExportTable_fn( (const void**)&tbl_base, &callback_funcs_id ) == CUDA_SUCCESS  ) {
            *(void **)&bar1 = *(tbl_base + 1);
            *(void **)&bar2 = *(tbl_base + 3);
        }
        
    }

    error_occurred = 0;
fail:
    return;
}

//
#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#include <Windows.h>

#ifdef UNICODE
static LPCWSTR __CudaLibName = L"nvcuda.dll";
#else
static LPCSTR __CudaLibName = "nvcuda.dll";
#endif

typedef HMODULE CUDADRIVER;

static CUresult LOAD_LIBRARY(CUDADRIVER *pInstance)
{
    *pInstance = LoadLibrary(__CudaLibName);

    if (*pInstance == NULL)
    {
        printf("LoadLibrary \"%s\" failed!\n", __CudaLibName);
        return CUDA_ERROR_UNKNOWN;
    }

    return CUDA_SUCCESS;
}

#elif defined(__unix__) || defined(__APPLE__) || defined(__MACOSX)

#include <dlfcn.h>

#if defined(__APPLE__) || defined(__MACOSX)
static char __CudaLibName[] = "/usr/local/cuda/lib/libcuda.dylib";
#else
static char __CudaLibName[] = "libcuda.so";
#endif

typedef void *CUDADRIVER;

static CUresult LOAD_LIBRARY(CUDADRIVER *pInstance)
{
    *pInstance = dlopen(__CudaLibName, RTLD_NOW);

    if (*pInstance == NULL)
    {
        printf("dlopen \"%s\" failed!\n", __CudaLibName);
        return CUDA_ERROR_UNKNOWN;
    }

    return CUDA_SUCCESS;
}

#else
#error unsupported platform
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
CUresult CUDAAPI cuMemAllocManaged(CUdeviceptr *dptr, size_t bytesize, unsigned int flags)
{
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
    my_pthread_once(&once_control, init);
    if (!uvm_alloc_fn) return CUDA_ERROR_NOT_SUPPORTED;
    if (!uvm_active()) return uvm_alloc_fn(dptr, bytesize, flags);
    if (!dptr || !bytesize) return CUDA_ERROR_INVALID_VALUE;
    uvm_allocation *a = (uvm_allocation *)calloc(1, sizeof(*a));
    if (!a) return CUDA_ERROR_OUT_OF_MEMORY;
    pthread_mutex_lock(&uvm_life);
    CUresult result = cuCtxGetCurrent_fn ? cuCtxGetCurrent_fn(&a->ctx) : CUDA_ERROR_NOT_SUPPORTED;
    if (result == CUDA_SUCCESS && !a->ctx) result = CUDA_ERROR_INVALID_CONTEXT;
    if (result == CUDA_SUCCESS)
        result = cuCtxGetDevice_fn ? cuCtxGetDevice_fn(&a->device) : CUDA_ERROR_NOT_SUPPORTED;
    if (result == CUDA_SUCCESS)
        result = cuDeviceGetAttribute_fn ? cuDeviceGetAttribute_fn(&a->concurrent,
            CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS, a->device) : CUDA_ERROR_NOT_SUPPORTED;
    pthread_mutex_lock(&uvm_state);
    if (result == CUDA_SUCCESS && (uvm_next_id == UINT64_MAX || bytesize > UINT64_MAX - uvm_bytes))
        result = CUDA_ERROR_OUT_OF_MEMORY;
    pthread_mutex_unlock(&uvm_state);
    if (result == CUDA_SUCCESS) result = uvm_alloc_fn(dptr, bytesize, flags);
    if (result == CUDA_SUCCESS) {
        a->ptr = *dptr;
        a->bytes = bytesize;
        pthread_mutex_lock(&uvm_state);
        a->id = ++uvm_next_id;
        uvm_allocation **tail = &uvm_allocations;
        while (*tail) tail = &(*tail)->next;
        *tail = a;
        uvm_bytes += bytesize;
        ++uvm_count;
        ++uvm_allocation_generation;
        pthread_mutex_unlock(&uvm_state);
    } else free(a);
    pthread_mutex_unlock(&uvm_life);
    return result;
}

CUresult CUDAAPI cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
    my_pthread_once(&once_control, init);
    if (uvm_active() && uvm_convert)
        return cuMemAllocManaged(dptr, bytesize, CU_MEM_ATTACH_GLOBAL);
    return cuMemAlloc_fn ? cuMemAlloc_fn(dptr, bytesize) : CUDA_ERROR_NOT_SUPPORTED;
}

CUresult CUDAAPI cuMemFree_v2(CUdeviceptr dptr)
{
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
    my_pthread_once(&once_control, init);
    if (!cuMemFree_fn) return CUDA_ERROR_NOT_SUPPORTED;
    if (!uvm_active()) return cuMemFree_fn(dptr);
    pthread_mutex_lock(&uvm_life);
    CUresult result = cuMemFree_fn(dptr);
    if (result == CUDA_SUCCESS) uvm_remove(dptr, NULL);
    pthread_mutex_unlock(&uvm_life);
    return result;
}

CUresult CUDAAPI cuCtxDestroy_v2(CUcontext ctx)
{
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
    my_pthread_once(&once_control, init);
    if (!uvm_destroy_fn) return CUDA_ERROR_NOT_SUPPORTED;
    if (!uvm_active()) return uvm_destroy_fn(ctx);
    pthread_mutex_lock(&uvm_life);
    CUresult result = uvm_destroy_fn(ctx);
    if (result == CUDA_SUCCESS && ctx) uvm_remove(0, ctx);
    pthread_mutex_unlock(&uvm_life);
    return result;
}

static CUresult uvm_primary_call(CUdevice device, uvm_primary_fn *real, const char *name)
{
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
    my_pthread_once(&once_control, init);
    if (!*real) return CUDA_ERROR_NOT_SUPPORTED;
    if (!uvm_active()) return (*real)(device);
    pthread_mutex_lock(&uvm_life);
    CUresult result = (*real)(device);
    uint64_t removed = result == CUDA_SUCCESS ? uvm_remove_by_device(device) : 0;
    pthread_mutex_unlock(&uvm_life);
    if (removed)
        fprintf(stderr, "SELECT UVM: %s succeeded on device %d; conservatively invalidated "
                "%llu allocation records (release may leave the context alive).\n",
                name, device, (unsigned long long)removed);
    return result;
}

/* Keep both exported generations: their argument ABI agrees, but their driver
 * behavior must be selected by the actual symbol, never substituted blindly. */
#undef cuDevicePrimaryCtxRelease
#undef cuDevicePrimaryCtxReset
CUresult CUDAAPI cuDevicePrimaryCtxRelease(CUdevice device)
{
    return uvm_primary_call(device, &uvm_release_v1, "cuDevicePrimaryCtxRelease");
}

CUresult CUDAAPI cuDevicePrimaryCtxRelease_v2(CUdevice device)
{
    return uvm_primary_call(device, &uvm_release_v2, "cuDevicePrimaryCtxRelease_v2");
}

CUresult CUDAAPI cuDevicePrimaryCtxReset(CUdevice device)
{
    return uvm_primary_call(device, &uvm_reset_v1, "cuDevicePrimaryCtxReset");
}

CUresult CUDAAPI cuDevicePrimaryCtxReset_v2(CUdevice device)
{
    return uvm_primary_call(device, &uvm_reset_v2, "cuDevicePrimaryCtxReset_v2");
}

/* Match the actual driver pointer as well as the requested version. In
 * particular cuMemAlloc/cuMemFree v1 use a different ABI and pass through. */
static void *uvm_proc(const char *symbol, void *real, int version)
{
    if (!real) return NULL;
    if ((!strcmp(symbol, "cuDevicePrimaryCtxRelease") ||
         !strcmp(symbol, "cuDevicePrimaryCtxRelease_v2")) && version >= 7000) {
        if (real == (void *)uvm_release_v1) return (void *)cuDevicePrimaryCtxRelease;
        if (real == (void *)uvm_release_v2) return (void *)cuDevicePrimaryCtxRelease_v2;
    }
    if ((!strcmp(symbol, "cuDevicePrimaryCtxReset") ||
         !strcmp(symbol, "cuDevicePrimaryCtxReset_v2")) && version >= 7000) {
        if (real == (void *)uvm_reset_v1) return (void *)cuDevicePrimaryCtxReset;
        if (real == (void *)uvm_reset_v2) return (void *)cuDevicePrimaryCtxReset_v2;
    }
    if (!strcmp(symbol, "cuMemAllocManaged") && version >= 6000 && real == (void *)uvm_alloc_fn)
        return (void *)cuMemAllocManaged;
    if ((!strcmp(symbol, "cuMemAlloc") || !strcmp(symbol, "cuMemAlloc_v2")) &&
        version >= 3020 && real == (void *)cuMemAlloc_fn) return (void *)cuMemAlloc_v2;
    if ((!strcmp(symbol, "cuMemFree") || !strcmp(symbol, "cuMemFree_v2")) &&
        version >= 3020 && real == (void *)cuMemFree_fn) return (void *)cuMemFree_v2;
    if ((!strcmp(symbol, "cuCtxDestroy") || !strcmp(symbol, "cuCtxDestroy_v2")) &&
        version >= 4000 && real == (void *)uvm_destroy_fn) return (void *)cuCtxDestroy_v2;
    return NULL;
}
#endif /* __linux__ */

CUresult CUDAAPI cuInit(unsigned int Flags)
{
#if defined(__linux__)
    if (uvm_inherited()) return CUDA_ERROR_NOT_INITIALIZED;
#endif
    GCN_DEBUG("cuInit()\n");

    my_pthread_once(&once_control, init);
#ifdef PROFILING
   InitQueue( &dlist );

  // initialize the activity trace
  // make sure activity is enabled before any CUDA API
  initTrace();

  // enable all activity record kinds
//cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONTEXT);
//cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER);
//cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME);
//cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY);
//cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMSET);
  cuptiActivityEnable(CUPTI_ACTIVITY_KIND_NAME);
  cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MARKER);
  cuptiActivityEnable(CUPTI_ACTIVITY_KIND_KERNEL);
  cuptiActivityEnable(CUPTI_ACTIVITY_KIND_OVERHEAD);
#endif // PROFILING

    CUresult result = cuInit_fn ? cuInit_fn(Flags) : CUDA_ERROR_NOT_INITIALIZED;
    if (result == CUDA_SUCCESS) {
        my_pthread_once(&post_cuinit_flag, (void(*) (void))postInit);
#if defined(__linux__)
        pthread_once(&uvm_start_once, uvm_start);
#endif
    }
    return result;
}

CUresult CUDAAPI cuDeviceGetAttribute(int *pi, CUdevice_attribute attrib, CUdevice dev)
{
    GCN_DEBUG("cuDeviceGetAttribute()\n");
    return cuDeviceGetAttribute_fn(pi, attrib, dev);
}

CUresult CUDAAPI cuCtxSynchronize(void)
{
    GCN_DEBUG("cuCtxSynchronize()\n");
    return cuCtxSynchronize_fn();
}

CUresult CUDAAPI cuModuleLoad(CUmodule *module, const char *fname)
{
    GCN_DEBUG("cuModuleLoad()\n");
    ComputeModInfo() ;
    return cuModuleLoad_fn(module, fname);
}

CUresult CUDAAPI cuModuleLoadData(CUmodule *module, const void *image)
{
    GCN_DEBUG("cuModuleLoadData()\n");
    ComputeModInfo() ;
    return cuModuleLoadData_fn(module, image);
}

CUresult CUDAAPI cuModuleLoadDataEx(CUmodule *module, const void *image, unsigned int numOptions, CUjit_option *options, void **optionValues)
{
    GCN_DEBUG("cuModuleLoadDataEx()\n");
    ComputeModInfo() ;
    return cuModuleLoadDataEx_fn(module, image, numOptions, options, optionValues);
}

CUresult CUDAAPI cuModuleLoadFatBinary(CUmodule *module, const void *fatCubin)
{
    GCN_DEBUG("cuModuleLoadFatBinary()\n");
    ComputeModInfo() ;
    return cuModuleLoadFatBinary_fn(module, fatCubin);
}

CUresult CUDAAPI cuModuleUnload(CUmodule hmod)
{
    GCN_DEBUG("cuModuleUnload()\n");
    return cuModuleUnload_fn(hmod);
}

CUresult CUDAAPI cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name)
{
    Q_ELEM_PTR elem_ptr;
    FUNCLIST *item = NULL;
    int driverVersion = 0;
    const char **pptr;
    CUresult               ret_val = CUDA_ERROR_OUT_OF_MEMORY ;
    my_bool fFound = FALSE;
    CUcontext   hcuContext = NULL;
    struct context_info* ctx = NULL;
     int                 i ;
    CUmodule    hcuModule = hmod;

    GCN_DEBUG("cuModuleGetFunction()\n");

    ComputeModInfo() ;

    // CUmodule is opaque. Its private ELF layout is only used by the
    // legacy PTB path; ordinary launches need no argument metadata.
    if (!CUDA_CORE_SOFT_LIMIT)
        return cuModuleGetFunction_fn(hfunc, hmod, name);

    if (!cuDriverGetVersion_fn)
        goto fn_fail;
    *hfunc = NULL;

    if ( cuCtxGetCurrent_fn != NULL ) {
        cuCtxGetCurrent_fn( &hcuContext ) ;
        pthread_mutex_lock( &g_csGlobal ) ;
        ctx = (struct context_info *)avl_find( context, &hcuContext ) ;
        pthread_mutex_unlock( &g_csGlobal ) ;
        if ( !ctx ) {
           ctx = create_ctx( hcuContext ) ;
        }
    }
    if (!ctx)
        goto fn_fail;

    pthread_mutex_lock( &g_csGlobal ) ;

  {
  FUNCLIST temp = {};
   temp.name = (char *)name;
   temp.kernels[MK_original].hOwner = hmod;
    item = (PFUNCLIST)avl_find( ctx->name_map, &temp );
  }
    if ( item == NULL ) {
        if ( (item = (FUNCLIST *)malloc(sizeof(*item) + strlen(name) + 1)) != NULL ) {
            memset (item, 0, sizeof (*item) );
            item->name = (char *)item + sizeof(*item);
            memcpy( item->name, name, strlen(name)+1 );
        }
    }
    else {
        fFound = TRUE;
        //don't forget this
        ret_val = CUDA_SUCCESS ;
    }

     for ( i = 1; (i < MK_MAX); i++ ) {
          for ( elem_ptr = QueueHead( &ctx->modList[i] ); elem_ptr != NULL; elem_ptr = QueueNext( elem_ptr ) ) {
              if ( item && ( *(CUmodule*)QueuePtr( elem_ptr ) != NULL ) &&
                   ( cuModuleGetFunction_fn( hfunc, *(CUmodule*)QueuePtr( elem_ptr ), name ) == CUDA_SUCCESS ) ) {
                  hcuModule = *(CUmodule*)QueuePtr( elem_ptr ) ;
                  item->kernels[i].object= *hfunc ;
                  item->kernels[i].hOwner= hcuModule ;
                  break;
              }
          }
     }

    pthread_mutex_unlock( &g_csGlobal ) ;

    if(item == NULL)
        goto fn_fail;

    if (!item->initialized ) {
         InitQueue( &item->param_list ) ;
         ret_val = cuModuleGetFunction_fn((CUfunction *)&item->kernels[MK_original].object, hmod, name) ;
         item->kernels[MK_original].hOwner= hmod ;
    }

#if defined(__i386__) || defined(DETOURS_X86)
    cuDriverGetVersion_fn(&driverVersion);
    if ( driverVersion < 10020) {
        pptr = (const char **)((char *)hcuModule + 0x20);
    } else if ( ( driverVersion/1000 == 10 ) ||
                ( driverVersion/1000 == 11 ) ||
                ( driverVersion <= 12020    ) ) {
        pptr = (const char **)((char *)hcuModule + 0x24);
    }
    else if ( driverVersion/1000 >= 12 ) {
        pptr = (const char **)((char *)hcuModule + 0x34);
        pptr = (const char **)((char *)*pptr + 0x18);
    }
    else
        pptr = NULL;

#elif defined(__x86_64__) || defined(DETOURS_X64)
    cuDriverGetVersion_fn(&driverVersion);
    if ( driverVersion < 10020) {
        pptr = (const char **)((char *)hcuModule + 0x28);
    } else if ( ( driverVersion/1000 == 10 ) ||
                ( driverVersion/1000 == 11 ) ||
                ( driverVersion <= 12020    ) ) {
        pptr = (const char **)((char *)hcuModule + 0x30);
    }
    else if ( driverVersion/1000 >= 12 && driverVersion/1000 < 13 ) {
        pptr = (const char **)((char *)hcuModule + 0x50);
        pptr = (const char **)((char *)*pptr + 0x20);
    }
    else if ( driverVersion/1000 >= 13 ) {
        pptr = (const char **)((char *)hcuModule + 0x58);
        pptr = (const char **)((char *)*pptr + 0x20);
    }
    else
        pptr = NULL;

#endif

    if ( ( !item->initialized )                   &&
         ( !parseParam( *pptr, name, item ) ) ) {
        goto fn_fail;
    }

    *hfunc = (CUfunction) item->kernels[MK_original].object;

    // add this kernel to the list.
    if ( (ret_val == CUDA_SUCCESS) && !item->initialized  ) {
        void **p;

        fetch_meta( item );

        pthread_mutex_lock( &g_csGlobal ) ;
        p = avl_probe (ctx->fns, item);
        if ( p != 0 && !fFound ) {
             p = avl_probe (ctx->name_map, item) ;
        }
        pthread_mutex_unlock( &g_csGlobal ) ;
        if ( p != NULL ) {
            item->initialized = TRUE;
            item = NULL ;
        } else {
            ret_val = CUDA_ERROR_OUT_OF_MEMORY ;
        }
    }

fn_fail:
    // cleanup the kernel structure if needed
    if( ctx == NULL ||
        ( ( ret_val != CUDA_SUCCESS ) &&
          ( item != NULL ) && !fFound ) ) {
        while ( (elem_ptr = QueueHead(&item->param_list)) != NULL ) {
            RemoveElem( &item->param_list, elem_ptr ) ;
            free( GetQueueElemPtr( elem_ptr ) ) ;
        }
        avl_delete( ctx->name_map, item );
        avl_delete( ctx->fns, item );
        free( item ) ;
    }

    return( ret_val ) ;
}

CUresult CUDAAPI cuLibraryLoadData(CUlibrary *library, const void *code, CUjit_option *jitOptions, void **jitOptionsValues, unsigned int numJitOptions, CUlibraryOption *libraryOptions, void** libraryOptionValues, unsigned int numLibraryOptions)
{
    GCN_DEBUG("cuLibraryLoadData()\n");
    ComputeModInfo() ;
    return cuLibraryLoadData_fn(library, code, jitOptions, jitOptionsValues, numJitOptions, libraryOptions, libraryOptionValues, numLibraryOptions);
}

CUresult CUDAAPI cuLibraryGetKernel(CUkernel *pKernel, CUlibrary library, const char *name)
{
    Q_ELEM_PTR elem_ptr;
    FUNCLIST *item = NULL;
    SlibList_t *entry;
    CUresult               ret_val = CUDA_ERROR_OUT_OF_MEMORY ;
    CUlibrary lib = library;
    const char **pptr;

    GCN_DEBUG("cuLibraryGetKernel()\n");
    ComputeModInfo() ;

    // CUlibrary is opaque: library + 0x10 is not a supported ELF pointer.
    // The no-PTB path forwards the handle and result without inspecting it.
    if (!CUDA_CORE_SOFT_LIMIT)
        return cuLibraryGetKernel_fn(pKernel, library, name);

    if ( (item = (FUNCLIST *)malloc(sizeof(*item) + strlen(name) + 1)) != NULL ) {
        memset (item, 0, sizeof (*item) );
        item->name = (char *)item + sizeof(*item);
        memcpy( item->name, name, strlen(name)+1 );
         InitQueue( &item->param_list ) ;
    }

    pthread_mutex_lock( &g_csGlobal ) ;
     for ( elem_ptr = QueueHead( &library_list ); elem_ptr != NULL; elem_ptr = QueueNext( elem_ptr ) ) {
         entry = (SlibList_t*)QueuePtr( elem_ptr ) ;
         if ( item && ( entry->library != NULL ) &&
              ( cuLibraryGetKernel_fn( pKernel, entry->library, name ) == CUDA_SUCCESS ) ) {
             item->kernels[entry->module_kind].object= *pKernel ;
             item->kernels[entry->module_kind].hOwner= entry->library ;
             lib= entry->library ;
         }
     }
    pthread_mutex_unlock( &g_csGlobal ) ;

    ret_val = cuLibraryGetKernel_fn(pKernel, library, name);
    item->kernels[MK_original].object= *pKernel ;
    item->kernels[MK_original].hOwner= library ;
    if ( ret_val != CUDA_SUCCESS ) {
       goto fn_fail;
    }

    pptr = (const char **)((char *)lib + 0x10);
    if ( ( !item )                   ||
         ( !parseParam( *pptr, name, item ) ) ) {
        goto fn_fail;
    }

    // add this kernel to the list.
    if( ret_val == CUDA_SUCCESS ) {
        void **p;
        pthread_mutex_lock( &g_csGlobal ) ;
        p = avl_probe (fns, item);
        pthread_mutex_unlock( &g_csGlobal ) ;
        if ( p != NULL ) {
            item = NULL ;
        } else {
            ret_val = CUDA_ERROR_OUT_OF_MEMORY ;
        }
    }

fn_fail:
    // cleanup the kernel structure if needed
    if( ( ret_val != CUDA_SUCCESS ) &&
          ( item != NULL ) ) {
        while ( (elem_ptr = QueueHead(&item->param_list)) != NULL ) {
            RemoveElem( &item->param_list, elem_ptr ) ;
            free( GetQueueElemPtr( elem_ptr ) ) ;
        }
        avl_delete( fns, item );
        free( item ) ;
    }
    return( ret_val ) ;
}

CUresult CUDAAPI cuKernelGetFunction(CUfunction *pFunc, CUkernel kernel)
{
    GCN_DEBUG("cuKernelGetFunction()\n");
    ComputeModInfo() ;
    return cuKernelGetFunction_fn(pFunc, kernel);
}

CUresult CUDAAPI cuMemGetInfo_v2(size_t *free, size_t *total)
{
    GCN_DEBUG("cuMemGetInfo_v2()\n");
#if defined (MEM_CONTROL)
    CUresult result;
    result = cuMemGetInfo_fn(free, total);
     if ( ( CUDA_SUCCESS == result ) && ( memory_limit != 0 ) ) {
       *free = get_used_gpu_memory() ;
       *free = (*free <= memory_limit) ? ( memory_limit - *free ) : 0 ;
        *total = memory_limit;
     }
    return result;
#endif
    return cuMemGetInfo_fn(free, total);
}

CUresult CUDAAPI cuFuncSetAttribute(CUfunction hfunc, CUfunction_attribute attrib, int value)
{
    CUcontext hcuContext = NULL;
    struct context_info* ctx = NULL;
    FUNCLIST *fnList = NULL;
    int i ;

    GCN_DEBUG("cuFuncSetAttribute()\n");

    // Retrieve current CUDA Context if not already set
    if ( cuCtxGetCurrent_fn != NULL ) {
        cuCtxGetCurrent_fn( &hcuContext ) ;
        pthread_mutex_lock( &g_csGlobal ) ;
        ctx = (struct context_info *)avl_find( context, &hcuContext ) ;
        pthread_mutex_unlock( &g_csGlobal ) ;
        if ( !ctx ) {
           ctx = create_ctx( hcuContext ) ;
        }
    }
    if (!ctx)
        goto fn_exit;

    // Search for the given kernel
    pthread_mutex_lock( &g_csGlobal ) ;
    fnList = (FUNCLIST *)avl_find( ctx->fns, &hfunc ) ;
    pthread_mutex_unlock( &g_csGlobal ) ;

    if (!fnList)
        goto fn_exit;

    for (i = 0; i < (int)(sizeof (fnList->kernels) / sizeof (fnList->kernels[0])); i ++ ) {
        if ( fnList->kernels[i].object ) {
             cuFuncSetAttribute_fn( (CUfunction)fnList->kernels[i].object, attrib, value ) ;
        }
    }

fn_exit:
    return cuFuncSetAttribute_fn(hfunc, attrib, value);
}

CUresult CUDAAPI cuLaunchKernel(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ, unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream, void **kernelParams, void **extra)
{
    CUlaunchConfig config= {gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream};
    GCN_DEBUG("cuLaunchKernel()\n");
    return cuLaunchKernelA(&config, f, kernelParams, extra);
}

CUresult CUDAAPI cuLaunchKernelEx(const CUlaunchConfig *config, CUfunction f, void **kernelParams, void **extra)
{
    GCN_DEBUG("cuLaunchKernelEx()\n");
    return cuLaunchKernelA(config, f, kernelParams, extra);
}

CUresult CUDAAPI cuStreamCreate(CUstream *phStream, unsigned int Flags)
{
    CUresult  ret_val ;
    void *pv;
    uint64_t upper; uint64_t lower;

    GCN_DEBUG("cuStreamCreate()\n");

    ret_val = cuStreamCreate_fn(phStream, Flags) ;
    if ( ret_val != CUDA_SUCCESS ) {
        goto fn_exit;
    }

    pthread_mutex_lock( &g_csGlobal ) ;
    pv = avl_probe(aq, *phStream);
    pthread_mutex_unlock( &g_csGlobal ) ;
    if (!pv){
        goto fn_fail;
    }

    if ( PARTITION_STREAM ) {
        upper = ((uint64_t)g_sm_maskA[3] << 32) | g_sm_maskA[2];
        lower = ((uint64_t)g_sm_maskA[1] << 32) | g_sm_maskA[0];
        libsmctrl_set_stream_mask_ext( *phStream, upper, lower ) ;
    }

fn_exit:
    return ret_val;

fn_fail:
    if (cuStreamDestroy_fn) cuStreamDestroy_fn(*phStream);
    ret_val = CUDA_ERROR_OUT_OF_MEMORY;

    goto fn_exit;
}

CUresult CUDAAPI cuStreamWaitEvent(CUstream hStream, CUevent hEvent, unsigned int Flags)
{
    GCN_DEBUG("cuStreamWaitEvent()\n");
    return cuStreamWaitEvent_fn(hStream, hEvent, Flags);
}

CUresult CUDAAPI cuStreamAddCallback(CUstream hStream, CUstreamCallback callback, void *userData, unsigned int flags)
{
    GCN_DEBUG("cuStreamAddCallback()\n");
    return cuStreamAddCallback_fn(hStream, callback, userData, flags);
}

CUresult CUDAAPI cuStreamQuery(CUstream hStream)
{
    GCN_DEBUG("cuStreamQuery()\n");
    return cuStreamQuery_fn(hStream);
}

CUresult CUDAAPI cuStreamSynchronize(CUstream hStream)
{
    GCN_DEBUG("cuStreamSynchronize()\n");
    return cuStreamSynchronize_fn(hStream);
}

CUresult CUDAAPI cuStreamDestroy_v2(CUstream hStream)
{
    CUresult  ret_val ;

    GCN_DEBUG("cuStreamDestroy_v2()\n");
    ret_val = cuStreamDestroy_fn(hStream) ;

    pthread_mutex_lock( &g_csGlobal ) ;
    avl_delete(aq, hStream);
    pthread_mutex_unlock( &g_csGlobal ) ;

    return ret_val;
}

#undef cuGetProcAddress
CUresult CUDAAPI cuGetProcAddress(const char *symbol, void **pfn, int cudaVersion, cuuint64_t flags)
{
    GCN_DEBUG("cuGetProcAddress()\n");
    my_pthread_once(&once_control, init);
    if (!cuGetProcAddress_fn) return CUDA_ERROR_NOT_SUPPORTED;
    CUresult query = cuGetProcAddress_fn(symbol, pfn, cudaVersion, flags);
    if (query != CUDA_SUCCESS || !pfn || !*pfn) return query;
#if defined(__linux__)
    void *replacement = uvm_proc(symbol, *pfn, cudaVersion);
    if (replacement) { *pfn = replacement; return query; }
#endif
    if ((!strcmp(symbol, "cuMemGetInfo") && cudaVersion < 3020) ||
        (!strcmp(symbol, "cuStreamDestroy") && cudaVersion < 4000)) return query;
    if (!strcmp (symbol, "cuInit")) {
        *pfn = (void *)cuInit;
    } else if (!strcmp (symbol, "cuDeviceGetAttribute")) {
        *pfn = (void *)cuDeviceGetAttribute;
    } else if (!strcmp (symbol, "cuCtxSynchronize")) {
        *pfn = (void *)cuCtxSynchronize;
    } else if (!strcmp (symbol, "cuModuleLoad")) {
        *pfn = (void *)cuModuleLoad;
    } else if (!strcmp (symbol, "cuModuleLoadData")) {
        *pfn = (void *)cuModuleLoadData;
    } else if (!strcmp (symbol, "cuModuleLoadDataEx")) {
        *pfn = (void *)cuModuleLoadDataEx;
    } else if (!strcmp (symbol, "cuModuleLoadFatBinary")) {
        *pfn = (void *)cuModuleLoadFatBinary;
    } else if (!strcmp (symbol, "cuModuleUnload")) {
        *pfn = (void *)cuModuleUnload;
    } else if (!strcmp (symbol, "cuModuleGetFunction")) {
        *pfn = (void *)cuModuleGetFunction;
    } else if (!strcmp (symbol, "cuLibraryLoadData")) {
        *pfn = (void *)cuLibraryLoadData;
    } else if (!strcmp (symbol, "cuLibraryGetKernel")) {
        *pfn = (void *)cuLibraryGetKernel;
    } else if (!strcmp (symbol, "cuKernelGetFunction")) {
        *pfn = (void *)cuKernelGetFunction;
    } else if (!strcmp (symbol, "cuMemGetInfo")) {
        *pfn = (void *)cuMemGetInfo_v2;
    } else if (!strcmp (symbol, "cuFuncSetAttribute")) {
        *pfn = (void *)cuFuncSetAttribute;
    } else if (!strcmp (symbol, "cuLaunchKernel")) {
        *pfn = (void *)cuLaunchKernel;
    } else if (!strcmp (symbol, "cuLaunchKernelEx")) {
        *pfn = (void *)cuLaunchKernelEx;
    } else if (!strcmp (symbol, "cuStreamCreate")) {
        *pfn = (void *)cuStreamCreate;
    } else if (!strcmp (symbol, "cuStreamWaitEvent")) {
        *pfn = (void *)cuStreamWaitEvent;
    } else if (!strcmp (symbol, "cuStreamAddCallback")) {
        *pfn = (void *)cuStreamAddCallback;
    } else if (!strcmp (symbol, "cuStreamQuery")) {
        *pfn = (void *)cuStreamQuery;
    } else if (!strcmp (symbol, "cuStreamSynchronize")) {
        *pfn = (void *)cuStreamSynchronize;
    } else if (!strcmp (symbol, "cuStreamDestroy")) {
        *pfn = (void *)cuStreamDestroy_v2;
    } else if (!strcmp (symbol, "cuGetExportTable")) {
        *pfn = (void *)cuGetExportTable;
    } else if (!strcmp (symbol, "cuGetProcAddress")) {
        if( cudaVersion >= 12000 ) {
            *pfn = (void *)cuGetProcAddress_v2;
        }
        else {
            *pfn = (void *)cuGetProcAddress;
        }
    } else {
        return query;
    }
    return( CUDA_SUCCESS );
}

CUresult CUDAAPI cuGetProcAddress_v2(const char *symbol, void **pfn, int cudaVersion, cuuint64_t flags, CUdriverProcAddressQueryResult *symbolStatus)
{
    GCN_DEBUG("cuGetProcAddress_v2()\n");
    my_pthread_once(&once_control, init);
    if (!cuGetProcAddress_v2_fn) return CUDA_ERROR_NOT_SUPPORTED;
    CUdriverProcAddressQueryResult found = CU_GET_PROC_ADDRESS_SUCCESS;
    CUresult query = cuGetProcAddress_v2_fn(symbol, pfn, cudaVersion, flags, &found);
    if (symbolStatus) *symbolStatus = found;
    if (query != CUDA_SUCCESS || found != CU_GET_PROC_ADDRESS_SUCCESS || !pfn || !*pfn)
        return query;
#if defined(__linux__)
    void *replacement = uvm_proc(symbol, *pfn, cudaVersion);
    if (replacement) { *pfn = replacement; return query; }
#endif
    if ((!strcmp(symbol, "cuMemGetInfo") && cudaVersion < 3020) ||
        (!strcmp(symbol, "cuStreamDestroy") && cudaVersion < 4000)) return query;
    if (!strcmp (symbol, "cuInit")) {
        *pfn = (void *)cuInit;
    } else if (!strcmp (symbol, "cuDeviceGetAttribute")) {
        *pfn = (void *)cuDeviceGetAttribute;
    } else if (!strcmp (symbol, "cuCtxSynchronize")) {
        *pfn = (void *)cuCtxSynchronize;
    } else if (!strcmp (symbol, "cuModuleLoad")) {
        *pfn = (void *)cuModuleLoad;
    } else if (!strcmp (symbol, "cuModuleLoadData")) {
        *pfn = (void *)cuModuleLoadData;
    } else if (!strcmp (symbol, "cuModuleLoadDataEx")) {
        *pfn = (void *)cuModuleLoadDataEx;
    } else if (!strcmp (symbol, "cuModuleLoadFatBinary")) {
        *pfn = (void *)cuModuleLoadFatBinary;
    } else if (!strcmp (symbol, "cuModuleUnload")) {
        *pfn = (void *)cuModuleUnload;
    } else if (!strcmp (symbol, "cuModuleGetFunction")) {
        *pfn = (void *)cuModuleGetFunction;
    } else if (!strcmp (symbol, "cuLibraryLoadData")) {
        *pfn = (void *)cuLibraryLoadData;
    } else if (!strcmp (symbol, "cuLibraryGetKernel")) {
        *pfn = (void *)cuLibraryGetKernel;
    } else if (!strcmp (symbol, "cuKernelGetFunction")) {
        *pfn = (void *)cuKernelGetFunction;
    } else if (!strcmp (symbol, "cuMemGetInfo")) {
        *pfn = (void *)cuMemGetInfo_v2;
    } else if (!strcmp (symbol, "cuFuncSetAttribute")) {
        *pfn = (void *)cuFuncSetAttribute;
    } else if (!strcmp (symbol, "cuLaunchKernel")) {
        *pfn = (void *)cuLaunchKernel;
    } else if (!strcmp (symbol, "cuLaunchKernelEx")) {
        *pfn = (void *)cuLaunchKernelEx;
    } else if (!strcmp (symbol, "cuStreamCreate")) {
        *pfn = (void *)cuStreamCreate;
    } else if (!strcmp (symbol, "cuStreamWaitEvent")) {
        *pfn = (void *)cuStreamWaitEvent;
    } else if (!strcmp (symbol, "cuStreamAddCallback")) {
        *pfn = (void *)cuStreamAddCallback;
    } else if (!strcmp (symbol, "cuStreamQuery")) {
        *pfn = (void *)cuStreamQuery;
    } else if (!strcmp (symbol, "cuStreamSynchronize")) {
        *pfn = (void *)cuStreamSynchronize;
    } else if (!strcmp (symbol, "cuStreamDestroy")) {
        *pfn = (void *)cuStreamDestroy_v2;
    } else if (!strcmp (symbol, "cuGetExportTable")) {
        *pfn = (void *)cuGetExportTable;
    } else if (!strcmp (symbol, "cuGetProcAddress")) {
        if ( cudaVersion >= 12000 ) {
            *pfn = (void *)cuGetProcAddress_v2;
        } else {
            *pfn = (void *)cuGetProcAddress;
        }
    } else {
        return query;
    }
    if ( symbolStatus != NULL )
        *symbolStatus = CU_GET_PROC_ADDRESS_SUCCESS;
    return( CUDA_SUCCESS );
}

CUresult CUDAAPI cuGetExportTable(const void **ppExportTable, const CUuuid *pExportTableId)
{
    GCN_DEBUG("cuGetExportTable()\n");
    return cuGetExportTable_fn(ppExportTable, pExportTableId);
}

#if defined (MEM_CONTROL)
nvmlReturn_t DECLDIR nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory)
{
    GCN_DEBUG("nvmlDeviceGetMemoryInfo_v2()\n");
    nvmlReturn_t result;
    result = pfn_nvmlDeviceGetMemoryInfo_v2( device, memory ) ;
     if ( ( NVML_SUCCESS == result ) && ( memory_limit != 0 ) ) {
          memory->total = memory_limit ;
          memory->free = (memory->free <= memory_limit) ? ( memory->free + memory_limit - memory->total ) : 0 ;
     }
    return result;
}
#endif

#if !defined(__WIN__)
/*
 * Protect against dlsym interception.
 *
 * We implement the whole API, so we don't need to intercept dlsym -- dlopen is
 * enough. However we need to protect against other dynamic libraries
 * intercepting dlsym, to prevent infinite recursion,
 *
 * In particular the Steam Community Overlay exports dlsym.  See also
 * http://lists.freedesktop.org/archives/apitrace/2013-March/000573.html
 */
void *
dlsym(void * handle, const char * symbol)
{
    void *retval = 0;

    my_pthread_once(&once_control, init);

   // NOTICE: important!!!. BUG workaround.
   if ( !strcmp( symbol, "ompt_start_tool" ) ) {
      return retval;
   }

#if defined(__linux__)
    if (!strcmp(symbol, "cuInit") || !strcmp(symbol, "cuMemAllocManaged") ||
        !strcmp(symbol, "cuMemAlloc_v2") || !strcmp(symbol, "cuMemFree_v2") ||
        !strcmp(symbol, "cuCtxDestroy_v2") ||
        !strcmp(symbol, "cuDevicePrimaryCtxRelease") ||
        !strcmp(symbol, "cuDevicePrimaryCtxRelease_v2") ||
        !strcmp(symbol, "cuDevicePrimaryCtxReset") ||
        !strcmp(symbol, "cuDevicePrimaryCtxReset_v2")) {
        void *real = dlsym_ptr(handle, symbol);
        if (!strcmp(symbol, "cuInit")) return real ? (void *)cuInit : real;
        void *replacement = uvm_proc(symbol, real, INT_MAX);
        return replacement ? replacement : real;
    }
#endif
    if (strcmp(symbol, "cuGetProcAddress_v2") == 0) {
        retval = (void *) cuGetProcAddress_v2;
    } else if (strcmp(symbol, "cuGetProcAddress") == 0) {
        retval = (void *) cuGetProcAddress;
    } else if (strcmp(symbol, "cuGetExportTable") == 0) {
        retval = (void *) cuGetExportTable;
    } else if (strcmp(symbol, "nvmlDeviceGetMemoryInfo_v2") == 0) {
        *(void **)&pfn_nvmlDeviceGetMemoryInfo_v2 = dlsym_ptr(handle, symbol);
        retval = (void *) nvmlDeviceGetMemoryInfo_v2;
    } else
        retval = dlsym_ptr(handle, symbol);
    return retval;
}
int ioctl(int fd, unsigned long cmd, ...)
{    
    va_list vargs;
    void * argv;
    my_pthread_once(&once_control, init);

    va_start(vargs, cmd);
    argv = va_arg(vargs, void *);
    va_end(vargs);

#if defined (MEM_CONTROL)
  size_t allocatedSize = 0;
  size_t used_memory = 0;
  NvV32 *pStatus;
    int retval = 0;
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
  void *ra = __builtin_extract_return_addr (__builtin_return_address (0));
Dl_info  DlInfo;

   if ( ( memory_limit == 0 ) ||
        ( _IOC_TYPE(cmd) != NV_IOCTL_MAGIC ) ||
        (( _IOC_NR(cmd) != NV_ESC_RM_ALLOC_MEMORY ) &&
        ( _IOC_NR(cmd) != NV_ESC_RM_ALLOC_OBJECT ) &&
        ( _IOC_NR(cmd) != NV_ESC_RM_FREE ) &&
        ( _IOC_NR(cmd) != NV_ESC_RM_ALLOC )) ) {
      goto fallback;
   }

    (void)stackSize.init();
    if (stackSize.get()) {
        goto fallback;
    }

    // Lookup the name of the function given the function pointer
    if ((retval = dladdr(ra, &DlInfo)) != 0 && DlInfo.dli_fname != 0 && strrstr(DlInfo.dli_fname, "libnvidia-ml.so") != 0)
        goto fallback;

    stackSize.set(stackSize.get()+1);
  // Initialize the driver library
  pthread_once(&g_nvml_lib_init, load_nvml_libraries);
    stackSize.set(stackSize.get()-1);

    if ( _IOC_NR(cmd) == NV_ESC_RM_ALLOC_MEMORY &&
         _IOC_SIZE(cmd) == sizeof(nv_ioctl_nvos02_parameters_with_fd) ) {
        nv_ioctl_nvos02_parameters_with_fd *pApi;
        NVOS02_PARAMETERS *pParms;

        pApi = (nv_ioctl_nvos02_parameters_with_fd *)argv;
        pParms = &pApi->params;
        pStatus = &pParms->status;
        hRoot = pParms->hRoot;
        hObjectParent = pParms->hObjectParent;
        hObjectNew = pParms->hObjectNew;
        if ( (pParms->hClass == NV01_MEMORY_LOCAL_USER) ||
             (pParms->hClass == NV01_MEMORY_LOCAL_PRIVILEGED) ||
              (pParms->hClass == NV01_MEMORY_LOCAL_PHYSICAL) ) {
         allocatedSize = pParms->limit + 1 ;
        }
    } else if( _IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
                _IOC_SIZE(cmd) == sizeof(NVOS64_PARAMETERS) ) {
        NVOS64_PARAMETERS *pArgs;
        pArgs = (NVOS64_PARAMETERS *)argv;
        pStatus = &pArgs->status;
        hRoot = pArgs->hRoot;
        hObjectParent = pArgs->hObjectParent;
        hObjectNew = pArgs->hObjectNew;
        if ( ((pArgs->hClass == NV01_MEMORY_LOCAL_USER) ||
             (pArgs->hClass == NV01_MEMORY_LOCAL_PRIVILEGED) ||
              (pArgs->hClass == NV01_MEMORY_LOCAL_PHYSICAL)) &&
              (pArgs->pAllocParms != NULL) &&
             ((pArgs->paramsSize == 0) ||
              (pArgs->paramsSize == sizeof(NV_MEMORY_ALLOCATION_PARAMS))) ) {
            NV_MEMORY_ALLOCATION_PARAMS *pAllocParams;
            pAllocParams = (NV_MEMORY_ALLOCATION_PARAMS *)pArgs->pAllocParms;
           allocatedSize = pAllocParams->size ;
        }
    } else if( _IOC_NR(cmd) == NV_ESC_RM_ALLOC &&
                _IOC_SIZE(cmd) == sizeof(NVOS21_PARAMETERS) ) {
        NVOS21_PARAMETERS *pArgs = (NVOS21_PARAMETERS *)argv;
        pStatus = &pArgs->status;
        hRoot = pArgs->hRoot;
        hObjectParent = pArgs->hObjectParent;
        hObjectNew = pArgs->hObjectNew;
        if ( ((pArgs->hClass == NV01_MEMORY_LOCAL_USER) ||
             (pArgs->hClass == NV01_MEMORY_LOCAL_PRIVILEGED) ||
              (pArgs->hClass == NV01_MEMORY_LOCAL_PHYSICAL)) &&
              (pArgs->pAllocParms != NULL) &&
             ((pArgs->paramsSize == 0) ||
              (pArgs->paramsSize == sizeof(NV_MEMORY_ALLOCATION_PARAMS))) ) {
            NV_MEMORY_ALLOCATION_PARAMS *pAllocParams;
            pAllocParams = (NV_MEMORY_ALLOCATION_PARAMS *)pArgs->pAllocParms;
           allocatedSize = pAllocParams->size ;
        }
    } else if( _IOC_NR(cmd) == NV_ESC_RM_ALLOC_OBJECT &&
                _IOC_SIZE(cmd) == sizeof(NVOS05_PARAMETERS) ) {
        NVOS05_PARAMETERS *pApi = (NVOS05_PARAMETERS *)argv;
        pStatus = &pApi->status;
        hRoot = pApi->hRoot;
        hObjectParent = pApi->hObjectParent;
        hObjectNew = pApi->hObjectNew;
        allocatedSize = 0 ;
    } else {
        goto fallback;
    }

     stackSize.set(stackSize.get()+1) ;
    used_memory = get_used_gpu_memory();
     stackSize.set(stackSize.get()-1) ;

    if (  ( (allocatedSize != 0) && ( memory_limit < used_memory + allocatedSize ) ) ||
            ( memory_limit < allocatedSize ) ) {
         *pStatus = NV_ERR_NO_MEMORY ;
         return 0;
    }

    retval = ioctl_ptr(fd,cmd,argv);
    if( retval || ( *pStatus != NV_OK ) ) {
         return retval;
    }

     stackSize.set(stackSize.get()+1) ;
    used_memory = get_used_gpu_memory();
     stackSize.set(stackSize.get()-1) ;

    if ( ( memory_limit < used_memory ) && rollback(fd, hRoot, hObjectParent, hObjectNew) ) {
         *pStatus = NV_ERR_NO_MEMORY ;
         return retval;
    }

    return retval;
#endif

    goto fallback;

fallback:

    return ioctl_ptr(fd,cmd,argv);
}
#endif  /* not __WIN__ */



#ifdef __cplusplus
}
#endif

// Initialization
void DetourFunctions() {
#if defined(__WIN__)
    CUDADRIVER CudaDrvLib;
    CUresult result = LOAD_LIBRARY(&CudaDrvLib);
    if (CUDA_SUCCESS != result) {
        return;
    }

    // fetch all function pointers
    cuInit_fn = (PFN_cuInit_v2000)GetProcAddress(CudaDrvLib, "cuInit");
    cuDriverGetVersion_fn = (PFN_cuDriverGetVersion_v2020)GetProcAddress(CudaDrvLib, "cuDriverGetVersion");
    cuDeviceGetCount_fn = (PFN_cuDeviceGetCount_v2000)GetProcAddress(CudaDrvLib, "cuDeviceGetCount");
    cuDeviceComputeCapability_fn = (PFN_cuDeviceComputeCapability_v2000)GetProcAddress(CudaDrvLib, "cuDeviceComputeCapability");
    cuDeviceGetAttribute_fn = (PFN_cuDeviceGetAttribute_v2000)GetProcAddress(CudaDrvLib, "cuDeviceGetAttribute");
    cuDevicePrimaryCtxRetain_fn = (PFN_cuDevicePrimaryCtxRetain_v7000)GetProcAddress(CudaDrvLib, "cuDevicePrimaryCtxRetain");
    cuDevicePrimaryCtxRelease_fn = (PFN_cuDevicePrimaryCtxRelease_v7000)GetProcAddress(CudaDrvLib, "cuDevicePrimaryCtxRelease");
    cuCtxSetCurrent_fn = (PFN_cuCtxSetCurrent_v4000)GetProcAddress(CudaDrvLib, "cuCtxSetCurrent");
    cuCtxGetCurrent_fn = (PFN_cuCtxGetCurrent_v4000)GetProcAddress(CudaDrvLib, "cuCtxGetCurrent");
    cuCtxGetDevice_fn = (PFN_cuCtxGetDevice_v2000)GetProcAddress(CudaDrvLib, "cuCtxGetDevice");
    cuCtxSynchronize_fn = (PFN_cuCtxSynchronize_v2000)GetProcAddress(CudaDrvLib, "cuCtxSynchronize");
    cuModuleLoad_fn = (PFN_cuModuleLoad_v2000)GetProcAddress(CudaDrvLib, "cuModuleLoad");
    cuModuleLoadData_fn = (PFN_cuModuleLoadData_v2000)GetProcAddress(CudaDrvLib, "cuModuleLoadData");
    cuModuleLoadDataEx_fn = (PFN_cuModuleLoadDataEx_v2010)GetProcAddress(CudaDrvLib, "cuModuleLoadDataEx");
    cuModuleLoadFatBinary_fn = (PFN_cuModuleLoadFatBinary_v2000)GetProcAddress(CudaDrvLib, "cuModuleLoadFatBinary");
    cuModuleUnload_fn = (PFN_cuModuleUnload_v2000)GetProcAddress(CudaDrvLib, "cuModuleUnload");
    cuModuleGetFunction_fn = (PFN_cuModuleGetFunction_v2000)GetProcAddress(CudaDrvLib, "cuModuleGetFunction");
    cuLibraryLoadData_fn = (PFN_cuLibraryLoadData_v12000)GetProcAddress(CudaDrvLib, "cuLibraryLoadData");
    cuLibraryGetKernel_fn = (PFN_cuLibraryGetKernel_v12000)GetProcAddress(CudaDrvLib, "cuLibraryGetKernel");
    cuLibraryGetModule_fn = (PFN_cuLibraryGetModule_v12000)GetProcAddress(CudaDrvLib, "cuLibraryGetModule");
    cuKernelGetFunction_fn = (PFN_cuKernelGetFunction_v12000)GetProcAddress(CudaDrvLib, "cuKernelGetFunction");
    cuMemGetInfo_fn = (PFN_cuMemGetInfo_v3020)GetProcAddress(CudaDrvLib, "cuMemGetInfo_v2");
    cuMemAlloc_fn = (PFN_cuMemAlloc_v3020)GetProcAddress(CudaDrvLib, "cuMemAlloc_v2");
    cuMemFree_fn = (PFN_cuMemFree_v3020)GetProcAddress(CudaDrvLib, "cuMemFree_v2");
    cuMemFreeHost_fn = (PFN_cuMemFreeHost_v2000)GetProcAddress(CudaDrvLib, "cuMemFreeHost");
    cuMemHostAlloc_fn = (PFN_cuMemHostAlloc_v2020)GetProcAddress(CudaDrvLib, "cuMemHostAlloc");
    cuFuncGetAttribute_fn = (PFN_cuFuncGetAttribute_v2020)GetProcAddress(CudaDrvLib, "cuFuncGetAttribute");
    cuFuncSetAttribute_fn = (PFN_cuFuncSetAttribute_v9000)GetProcAddress(CudaDrvLib, "cuFuncSetAttribute");
    cuFuncSetCacheConfig_fn = (PFN_cuFuncSetCacheConfig_v3000)GetProcAddress(CudaDrvLib, "cuFuncSetCacheConfig");
    cuLaunchKernel_fn = (PFN_cuLaunchKernel_v4000)GetProcAddress(CudaDrvLib, "cuLaunchKernel");
    cuLaunchKernelEx_fn = (PFN_cuLaunchKernelEx_v11060)GetProcAddress(CudaDrvLib, "cuLaunchKernelEx");
    cuLaunchHostFunc_fn = (PFN_cuLaunchHostFunc_v10000)GetProcAddress(CudaDrvLib, "cuLaunchHostFunc");
    cuGetProcAddress_fn = (PFN_cuGetProcAddress_v11030)GetProcAddress(CudaDrvLib, "cuGetProcAddress");
    cuGetProcAddress_v2_fn = (PFN_cuGetProcAddress_v12000)GetProcAddress(CudaDrvLib, "cuGetProcAddress_v2");
    cuStreamCreate_fn = (PFN_cuStreamCreate_v2000)GetProcAddress(CudaDrvLib, "cuStreamCreate");
    cuStreamWaitEvent_fn = (PFN_cuStreamWaitEvent_v3020)GetProcAddress(CudaDrvLib, "cuStreamWaitEvent");
    cuStreamAddCallback_fn = (PFN_cuStreamAddCallback_v5000)GetProcAddress(CudaDrvLib, "cuStreamAddCallback");
    cuStreamQuery_fn = (PFN_cuStreamQuery_v2000)GetProcAddress(CudaDrvLib, "cuStreamQuery");
    cuStreamSynchronize_fn = (PFN_cuStreamSynchronize_v2000)GetProcAddress(CudaDrvLib, "cuStreamSynchronize");
    cuStreamDestroy_fn = (PFN_cuStreamDestroy_v4000)GetProcAddress(CudaDrvLib, "cuStreamDestroy_v2");
    cuStreamIsCapturing_fn = (PFN_cuStreamIsCapturing_v10000)GetProcAddress(CudaDrvLib, "cuStreamIsCapturing");
    cuGetExportTable_fn = (PFN_cuGetExportTable_v3000)GetProcAddress(CudaDrvLib, "cuGetExportTable");

    DetourRestoreAfterWith();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if ( cuInit_fn != NULL ) {
        DetourAttach(&(PVOID&)cuInit_fn, (void *)cuInit);
    }
    if ( cuDeviceGetAttribute_fn != NULL ) {
        DetourAttach(&(PVOID&)cuDeviceGetAttribute_fn, (void *)cuDeviceGetAttribute);
    }
    if ( cuCtxSynchronize_fn != NULL ) {
        DetourAttach(&(PVOID&)cuCtxSynchronize_fn, (void *)cuCtxSynchronize);
    }
    if ( cuModuleLoad_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleLoad_fn, (void *)cuModuleLoad);
    }
    if ( cuModuleLoadData_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleLoadData_fn, (void *)cuModuleLoadData);
    }
    if ( cuModuleLoadDataEx_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleLoadDataEx_fn, (void *)cuModuleLoadDataEx);
    }
    if ( cuModuleLoadFatBinary_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleLoadFatBinary_fn, (void *)cuModuleLoadFatBinary);
    }
    if ( cuModuleUnload_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleUnload_fn, (void *)cuModuleUnload);
    }
    if ( cuModuleGetFunction_fn != NULL ) {
        DetourAttach(&(PVOID&)cuModuleGetFunction_fn, (void *)cuModuleGetFunction);
    }
    if ( cuLibraryLoadData_fn != NULL ) {
        DetourAttach(&(PVOID&)cuLibraryLoadData_fn, (void *)cuLibraryLoadData);
    }
    if ( cuLibraryGetKernel_fn != NULL ) {
        DetourAttach(&(PVOID&)cuLibraryGetKernel_fn, (void *)cuLibraryGetKernel);
    }
    if ( cuKernelGetFunction_fn != NULL ) {
        DetourAttach(&(PVOID&)cuKernelGetFunction_fn, (void *)cuKernelGetFunction);
    }
    if ( cuMemGetInfo_fn != NULL ) {
        DetourAttach(&(PVOID&)cuMemGetInfo_fn, (void *)cuMemGetInfo_v2);
    }
    if ( cuFuncSetAttribute_fn != NULL ) {
        DetourAttach(&(PVOID&)cuFuncSetAttribute_fn, (void *)cuFuncSetAttribute);
    }
    if ( cuLaunchKernel_fn != NULL ) {
        DetourAttach(&(PVOID&)cuLaunchKernel_fn, (void *)cuLaunchKernel);
    }
    if ( cuStreamCreate_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamCreate_fn, (void *)cuStreamCreate);
    }
    if ( cuStreamWaitEvent_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamWaitEvent_fn, (void *)cuStreamWaitEvent);
    }
    if ( cuStreamAddCallback_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamAddCallback_fn, (void *)cuStreamAddCallback);
    }
    if ( cuStreamQuery_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamQuery_fn, (void *)cuStreamQuery);
    }
    if ( cuStreamSynchronize_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamSynchronize_fn, (void *)cuStreamSynchronize);
    }
    if ( cuStreamDestroy_fn != NULL ) {
        DetourAttach(&(PVOID&)cuStreamDestroy_fn, (void *)cuStreamDestroy_v2);
    }
    if ( cuLaunchKernelEx_fn != NULL ) {
        DetourAttach(&(PVOID&)cuLaunchKernelEx_fn, (void *)cuLaunchKernelEx);
    }
    if ( cuGetProcAddress_fn != NULL ) {
        DetourAttach(&(PVOID&)cuGetProcAddress_fn, (void *)cuGetProcAddress);
    }
    if ( cuGetProcAddress_v2_fn != NULL ) {
        DetourAttach(&(PVOID&)cuGetProcAddress_v2_fn, (void *)cuGetProcAddress_v2);
    }
    if ( cuGetExportTable_fn != NULL ) {
        DetourAttach(&(PVOID&)cuGetExportTable_fn, (void *)cuGetExportTable);
    }
    LONG error = DetourTransactionCommit();

    if (error != NO_ERROR) {
        printf("simple" DETOURS_STRINGIFY(DETOURS_BITS) ".dll:"
                  " Error detouring function(): %ld\n", error);
    }
#else /* __WIN__ */
    Dl_info info;  
    int rc;      
    int i;
    char szTemp[256];
    void *dlhandle;
    CUDADRIVER CudaDrvLib;

    rc = dladdr((void *)dlvsym, &info);   
    if (!rc)  {    
        GCN_WARNING("Problem retrieving program information for %p:  %s\n", (void *)dlvsym, dlerror()); 
    }

    dlhandle = dlopen(info.dli_fname, RTLD_NOW);

    if (dlhandle == NULL)  {
        GCN_WARNING("%s Not Found:  %s\n", info.dli_fname, dlerror());
    }

    for ( i = 0 ; ((i<80) && (dlsym_ptr == NULL)) ; i ++ ) {
        sprintf ( szTemp , "GLIBC_2.%d" , i ) ;
        dlsym_ptr = (PFN_DLSYM)dlvsym(dlhandle, "dlsym", szTemp);   
    }

    for ( i = 0 ; ((i<1000) && (dlsym_ptr == NULL)) ; i ++ ) {
        sprintf ( szTemp , "GLIBC_2.%d.%d" , i / 16 , i % 16 ) ;
        dlsym_ptr = (PFN_DLSYM)dlvsym(dlhandle, "dlsym", szTemp);   
    }

    if ( dlsym_ptr == NULL ) {
        return;
    }

    if( LOAD_LIBRARY(&CudaDrvLib) != CUDA_SUCCESS ) {
        return;
    }

    // fetch all function pointers
    *(void **)&ioctl_ptr = dlsym_ptr(RTLD_NEXT, "ioctl");

    cuInit_fn = (PFN_cuInit_v2000)dlsym_ptr(CudaDrvLib, "cuInit");
    cuDriverGetVersion_fn = (PFN_cuDriverGetVersion_v2020)dlsym_ptr(CudaDrvLib, "cuDriverGetVersion");
    cuDeviceGetCount_fn = (PFN_cuDeviceGetCount_v2000)dlsym_ptr(CudaDrvLib, "cuDeviceGetCount");
    cuDeviceComputeCapability_fn = (PFN_cuDeviceComputeCapability_v2000)dlsym_ptr(CudaDrvLib, "cuDeviceComputeCapability");
    cuDeviceGetAttribute_fn = (PFN_cuDeviceGetAttribute_v2000)dlsym_ptr(CudaDrvLib, "cuDeviceGetAttribute");
    cuDevicePrimaryCtxRetain_fn = (PFN_cuDevicePrimaryCtxRetain_v7000)dlsym_ptr(CudaDrvLib, "cuDevicePrimaryCtxRetain");
    cuDevicePrimaryCtxRelease_fn = (PFN_cuDevicePrimaryCtxRelease_v7000)dlsym_ptr(CudaDrvLib, "cuDevicePrimaryCtxRelease");
    cuCtxSetCurrent_fn = (PFN_cuCtxSetCurrent_v4000)dlsym_ptr(CudaDrvLib, "cuCtxSetCurrent");
    cuCtxGetCurrent_fn = (PFN_cuCtxGetCurrent_v4000)dlsym_ptr(CudaDrvLib, "cuCtxGetCurrent");
    cuCtxGetDevice_fn = (PFN_cuCtxGetDevice_v2000)dlsym_ptr(CudaDrvLib, "cuCtxGetDevice");
    cuCtxSynchronize_fn = (PFN_cuCtxSynchronize_v2000)dlsym_ptr(CudaDrvLib, "cuCtxSynchronize");
    cuModuleLoad_fn = (PFN_cuModuleLoad_v2000)dlsym_ptr(CudaDrvLib, "cuModuleLoad");
    cuModuleLoadData_fn = (PFN_cuModuleLoadData_v2000)dlsym_ptr(CudaDrvLib, "cuModuleLoadData");
    cuModuleLoadDataEx_fn = (PFN_cuModuleLoadDataEx_v2010)dlsym_ptr(CudaDrvLib, "cuModuleLoadDataEx");
    cuModuleLoadFatBinary_fn = (PFN_cuModuleLoadFatBinary_v2000)dlsym_ptr(CudaDrvLib, "cuModuleLoadFatBinary");
    cuModuleUnload_fn = (PFN_cuModuleUnload_v2000)dlsym_ptr(CudaDrvLib, "cuModuleUnload");
    cuModuleGetFunction_fn = (PFN_cuModuleGetFunction_v2000)dlsym_ptr(CudaDrvLib, "cuModuleGetFunction");
    cuLibraryLoadData_fn = (PFN_cuLibraryLoadData_v12000)dlsym_ptr(CudaDrvLib, "cuLibraryLoadData");
    cuLibraryGetKernel_fn = (PFN_cuLibraryGetKernel_v12000)dlsym_ptr(CudaDrvLib, "cuLibraryGetKernel");
    cuLibraryGetModule_fn = (PFN_cuLibraryGetModule_v12000)dlsym_ptr(CudaDrvLib, "cuLibraryGetModule");
    cuKernelGetFunction_fn = (PFN_cuKernelGetFunction_v12000)dlsym_ptr(CudaDrvLib, "cuKernelGetFunction");
    cuMemGetInfo_fn = (PFN_cuMemGetInfo_v3020)dlsym_ptr(CudaDrvLib, "cuMemGetInfo_v2");
    cuMemAlloc_fn = (PFN_cuMemAlloc_v3020)dlsym_ptr(CudaDrvLib, "cuMemAlloc_v2");
    cuMemFree_fn = (PFN_cuMemFree_v3020)dlsym_ptr(CudaDrvLib, "cuMemFree_v2");
    cuMemFreeHost_fn = (PFN_cuMemFreeHost_v2000)dlsym_ptr(CudaDrvLib, "cuMemFreeHost");
    cuMemHostAlloc_fn = (PFN_cuMemHostAlloc_v2020)dlsym_ptr(CudaDrvLib, "cuMemHostAlloc");
    cuFuncGetAttribute_fn = (PFN_cuFuncGetAttribute_v2020)dlsym_ptr(CudaDrvLib, "cuFuncGetAttribute");
    cuFuncSetAttribute_fn = (PFN_cuFuncSetAttribute_v9000)dlsym_ptr(CudaDrvLib, "cuFuncSetAttribute");
    cuFuncSetCacheConfig_fn = (PFN_cuFuncSetCacheConfig_v3000)dlsym_ptr(CudaDrvLib, "cuFuncSetCacheConfig");
    cuLaunchKernel_fn = (PFN_cuLaunchKernel_v4000)dlsym_ptr(CudaDrvLib, "cuLaunchKernel");
    cuLaunchKernelEx_fn = (PFN_cuLaunchKernelEx_v11060)dlsym_ptr(CudaDrvLib, "cuLaunchKernelEx");
    cuLaunchHostFunc_fn = (PFN_cuLaunchHostFunc_v10000)dlsym_ptr(CudaDrvLib, "cuLaunchHostFunc");
    cuGetProcAddress_fn = (PFN_cuGetProcAddress_v11030)dlsym_ptr(CudaDrvLib, "cuGetProcAddress");
    cuGetProcAddress_v2_fn = (PFN_cuGetProcAddress_v12000)dlsym_ptr(CudaDrvLib, "cuGetProcAddress_v2");
    cuStreamCreate_fn = (PFN_cuStreamCreate_v2000)dlsym_ptr(CudaDrvLib, "cuStreamCreate");
    cuStreamWaitEvent_fn = (PFN_cuStreamWaitEvent_v3020)dlsym_ptr(CudaDrvLib, "cuStreamWaitEvent");
    cuStreamAddCallback_fn = (PFN_cuStreamAddCallback_v5000)dlsym_ptr(CudaDrvLib, "cuStreamAddCallback");
    cuStreamQuery_fn = (PFN_cuStreamQuery_v2000)dlsym_ptr(CudaDrvLib, "cuStreamQuery");
    cuStreamSynchronize_fn = (PFN_cuStreamSynchronize_v2000)dlsym_ptr(CudaDrvLib, "cuStreamSynchronize");
    cuStreamDestroy_fn = (PFN_cuStreamDestroy_v4000)dlsym_ptr(CudaDrvLib, "cuStreamDestroy_v2");
    cuStreamIsCapturing_fn = (PFN_cuStreamIsCapturing_v10000)dlsym_ptr(CudaDrvLib, "cuStreamIsCapturing");
    cuGetExportTable_fn = (PFN_cuGetExportTable_v3000)dlsym_ptr(CudaDrvLib, "cuGetExportTable");
#if defined(__linux__)
    uvm_load(CudaDrvLib);
#endif

#endif /* defined(__WIN__) */
}

void DetachFunctions() {
#if defined(__WIN__)
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if ( cuInit_fn != NULL ) {
        DetourDetach(&(PVOID&)cuInit_fn, (void *)cuInit);
    }
    if ( cuDeviceGetAttribute_fn != NULL ) {
        DetourDetach(&(PVOID&)cuDeviceGetAttribute_fn, (void *)cuDeviceGetAttribute);
    }
    if ( cuCtxSynchronize_fn != NULL ) {
        DetourDetach(&(PVOID&)cuCtxSynchronize_fn, (void *)cuCtxSynchronize);
    }
    if ( cuModuleLoad_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleLoad_fn, (void *)cuModuleLoad);
    }
    if ( cuModuleLoadData_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleLoadData_fn, (void *)cuModuleLoadData);
    }
    if ( cuModuleLoadDataEx_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleLoadDataEx_fn, (void *)cuModuleLoadDataEx);
    }
    if ( cuModuleLoadFatBinary_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleLoadFatBinary_fn, (void *)cuModuleLoadFatBinary);
    }
    if ( cuModuleUnload_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleUnload_fn, (void *)cuModuleUnload);
    }
    if ( cuModuleGetFunction_fn != NULL ) {
        DetourDetach(&(PVOID&)cuModuleGetFunction_fn, (void *)cuModuleGetFunction);
    }
    if ( cuLibraryLoadData_fn != NULL ) {
        DetourDetach(&(PVOID&)cuLibraryLoadData_fn, (void *)cuLibraryLoadData);
    }
    if ( cuLibraryGetKernel_fn != NULL ) {
        DetourDetach(&(PVOID&)cuLibraryGetKernel_fn, (void *)cuLibraryGetKernel);
    }
    if ( cuKernelGetFunction_fn != NULL ) {
        DetourDetach(&(PVOID&)cuKernelGetFunction_fn, (void *)cuKernelGetFunction);
    }
    if ( cuMemGetInfo_fn != NULL ) {
        DetourDetach(&(PVOID&)cuMemGetInfo_fn, (void *)cuMemGetInfo_v2);
    }
    if ( cuFuncSetAttribute_fn != NULL ) {
        DetourDetach(&(PVOID&)cuFuncSetAttribute_fn, (void *)cuFuncSetAttribute);
    }
    if ( cuLaunchKernel_fn != NULL ) {
        DetourDetach(&(PVOID&)cuLaunchKernel_fn, (void *)cuLaunchKernel);
    }
    if ( cuStreamCreate_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamCreate_fn, (void *)cuStreamCreate);
    }
    if ( cuStreamWaitEvent_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamWaitEvent_fn, (void *)cuStreamWaitEvent);
    }
    if ( cuStreamAddCallback_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamAddCallback_fn, (void *)cuStreamAddCallback);
    }
    if ( cuStreamQuery_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamQuery_fn, (void *)cuStreamQuery);
    }
    if ( cuStreamSynchronize_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamSynchronize_fn, (void *)cuStreamSynchronize);
    }
    if ( cuStreamDestroy_fn != NULL ) {
        DetourDetach(&(PVOID&)cuStreamDestroy_fn, (void *)cuStreamDestroy_v2);
    }
    if ( cuLaunchKernelEx_fn != NULL ) {
        DetourDetach(&(PVOID&)cuLaunchKernelEx_fn, (void *)cuLaunchKernelEx);
    }
    if ( cuGetProcAddress_fn != NULL ) {
        DetourDetach(&(PVOID&)cuGetProcAddress_fn, (void *)cuGetProcAddress);
    }
    if ( cuGetProcAddress_v2_fn != NULL ) {
        DetourDetach(&(PVOID&)cuGetProcAddress_v2_fn, (void *)cuGetProcAddress_v2);
    }
    if ( cuGetExportTable_fn != NULL ) {
        DetourDetach(&(PVOID&)cuGetExportTable_fn, (void *)cuGetExportTable);
    }
    DetourTransactionCommit();
#endif /* defined(__WIN__) */
}

#if defined(__WIN__)
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD dwReason, LPVOID reserved) {
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (dwReason == DLL_PROCESS_ATTACH) {
        DetourFunctions();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        DetachFunctions();
        bye();
    }
    return TRUE;
    (void)reserved;
    (void)hinst;
}
#endif /* defined(__WIN__) */
