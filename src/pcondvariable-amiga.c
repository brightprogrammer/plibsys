/*
 * The MIT License
 *
 * Copyright (C) 2017 Alexander Saprykin <saprykin.spb@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "pcondvariable.h"
#include "patomic.h"
#include "pmem.h"
#include "pspinlock.h"

#include <stdlib.h>

#include "proto/exec.h"

typedef struct _PCondThread {
	struct Task		*thread;
	struct _PCondThread	*next;
	ULONG			sigmask;
} PCondThread;

struct PCondVariable_ {
	PSpinLock 	*lock;
	PCondThread	*wait_head;
	pint		wait_count;
};

P_LIB_API PCondVariable *
p_cond_variable_new (void)
{
	PCondVariable *ret;

	if (P_UNLIKELY ((ret = p_malloc0 (sizeof (PCondVariable))) == NULL)) {
		P_ERROR ("PCondVariable::p_cond_variable_new: failed to allocate memory");
		return NULL;
	}

	if ((ret->lock = p_spinlock_new ()) == NULL) {
		P_ERROR ("PCondVariable::p_cond_variable_new: failed to initialize");
		p_free (ret);
		return NULL;
	}

	return ret;
}

P_LIB_API void
p_cond_variable_free (PCondVariable *cond)
{
	if (P_UNLIKELY (cond == NULL))
		return;

	if ((cond->wait_count > 0) || (cond->wait_head != NULL))
		P_WARNING ("PCondVariable::p_cond_variable_free: destroying while threads are waiting");

	p_spinlock_free (cond->lock);
	p_free (cond);
}

P_LIB_API pboolean
p_cond_variable_wait (PCondVariable	*cond,
		      PMutex		*mutex)
{
	PCondThread	*wait_thread;
	BYTE		signal;
	ULONG		wait_singnals;

	if (P_UNLIKELY (cond == NULL || mutex == NULL))
		return FALSE;

	if ((wait_thread = p_malloc0 (sizeof (PCondThread))) == NULL) {
		P_ERROR ("PCondVariable::p_cond_variable_wait: failed to allocate memory");
		return FALSE;
	}

	wait_thread->thread = IExec->FindTask (NULL);
	wait_thread->next   = NULL;
	
	signal = IExec->AllocSignal (-1);
	
	if (signal == -1) {
		P_WARNING ("PCondVariable::p_cond_variable_wait: no free signal slot left");
		return FALSE;
	}

	wait_thread->sigmask = 1 << signal;

	if (p_spinlock_lock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_wait: failed to lock internal spinlock");
		return FALSE;
	}

	if (cond->wait_head != NULL)
		cond->wait_head->next = wait_thread;
	else
		cond->wait_head = wait_thread;

	p_atomic_int_inc ((volatile pint *) &cond->wait_count);
	
	if (p_spinlock_unlock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_wait: failed to unlock internal spinlock");
		return FALSE;
	}

	if (p_mutex_unlock (mutex) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_wait: failed to unlock mutex");
		return FALSE;
	}

	wait_singnals = IExec->Wait (wait_thread->sigmask);

	if (p_mutex_lock (mutex) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_wait: failed to lock mutex");
		return FALSE;
	}

	IExec->FreeSignal (signal);

	return TRUE;
}

P_LIB_API pboolean
p_cond_variable_signal (PCondVariable *cond)
{
	PCondThread	*wait_thread;

	if (P_UNLIKELY (cond == NULL))
		return FALSE;

	if (p_spinlock_lock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_signal: failed to lock internal spinlock");
		return FALSE;
	}

	if (cond->wait_head == NULL) {
		if (p_spinlock_unlock (cond->lock) != TRUE) {
			P_ERROR ("PCondVariable::p_cond_variable_signal(1): failed to unlock internal spinlock");
			return FALSE;
		} else
			return TRUE;
	}

	wait_thread = cond->wait_head;
	cond->wait_head = wait_thread->next;

	p_atomic_int_add ((volatile pint *) &cond->wait_count, -1);

	if (p_spinlock_unlock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_signal(2): failed to unlock internal spinlock");
		return FALSE;
	}

	IExec->Signal (wait_thread->thread, wait_thread->sigmask);

	p_free (wait_thread);
	return TRUE;
}

P_LIB_API pboolean
p_cond_variable_broadcast (PCondVariable *cond)
{
	if (P_UNLIKELY (cond == NULL))
		return FALSE;

	PCondThread	*cur_thread;
	PCondThread	*next_thread;

	if (p_spinlock_lock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_broadcast: failed to lock internal spinlock");
		return FALSE;
	}

	if (cond->wait_head == NULL) {
		if (p_spinlock_unlock (cond->lock) != TRUE) {
			P_ERROR ("PCondVariable::p_cond_variable_broadcast(1): failed to unlock internal spinlock");
			return FALSE;
		} else
			return TRUE;
	}

	cur_thread = cond->wait_head;

	do {
		IExec->Signal (cur_thread->thread, cur_thread->sigmask);

		next_thread = cur_thread->next;
		p_free (cur_thread);

		cur_thread = next_thread;
	} while (cur_thread != NULL);

	cond->wait_head  = NULL;
	cond->wait_count = 0;

	if (p_spinlock_unlock (cond->lock) != TRUE) {
		P_ERROR ("PCondVariable::p_cond_variable_broadcast(2): failed to unlock internal spinlock");
		return FALSE;
	}

	return TRUE;
}

void
p_cond_variable_init (void)
{
}

void
p_cond_variable_shutdown (void)
{
}
