#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdnoreturn.h>

#include "panic.h"

/**
 * Called by the inline assembly in arch_context_switch to send a SIGUSR1 in order to restore a previously preempted
 * thread. The only way to restore all of the mcontext registers of a preempted sandbox is to send ourselves a signal,
 * then update the registers we should return to, then sigreturn (by returning from the handler). This returns to the
 * control flow restored from the mcontext
 */
noreturn void __attribute__((noinline)) arch_context_restore_preempted(void)
{
	pthread_t tid = pthread_self();
    	pthread_kill(tid, SIGUSR1);
	panic("Unexpectedly reached code after sending self SIGUSR1\n");
}
