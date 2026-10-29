/*  src/thr-rthreads.c: Thread functions for Libretro
    Copyright 2019 barbudreadmon

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* sched_setaffinity */
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif
#ifdef __linux__
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

#include <stdlib.h>
#include "core.h"
#include "threads.h"
#include "rthreads/rthreads.h"

struct thd_s {
	int running;
	int wake_pending; // protected by mutex
	sthread_t *thd;
	slock_t *mutex;
	scond_t *cond;
};

static struct thd_s thread_handle[YAB_NUM_THREADS];

/* The calling thread's YAB_THREAD_* id (-1: not started by YabThreadStart).
   YabThreadSleep used to find it by scanning thread_handle, which raced
   with starting and joining other threads (a joined thread's sthread is
   freed while another thread's scan may be reading it). */
static __thread int yab_self_id = -1;
struct thd_id_start { void (*func)(void *); void *arg; int id; };
static void IdTrampoline(void *data)
{
	struct thd_id_start st = *(struct thd_id_start *)data;
	free(data);
	yab_self_id = st.id;
	st.func(st.arg);
}

/* YabFlagWait/YabFlagNotify: one condition for all flags, as only the
 * emulation thread waits */
static slock_t *flag_lock;
static scond_t *flag_cond;

//////////////////////////////////////////////////////////////////////////////

#ifdef __linux__
/* Render workers can run at a lower priority than the emulation thread,
 * which is the critical path: on two cores, waking several workers
 * otherwise preempts it. On the Miyoo, nice +10 gave Princess Crown
 * +9% game speed and a third more shown frames under frameskip. The RBG0
 * thread stays at normal priority: lowering it (with VDP1, before the
 * workers ran as SCHED_BATCH) made Panzer Dragoon Saga 20% slower. The VDP1
 * threads are niced too: on two cores the polygon thread otherwise takes
 * the core from the emulation thread while the SH-2s run (Sega Rally -3%). */
static int WorkerNiced(unsigned int id)
{
	return id >= YAB_THREAD_VIDSOFT_LAYER_NBG3 && id != YAB_THREAD_VIDSOFT_LAYER_RBG0;
}
/* The heavy 3D workers, which run as SCHED_BATCH (see batch_mode) */
static int WorkerBatch(unsigned int id)
{
	return id == YAB_THREAD_VIDSOFT_VDP1 || id == YAB_THREAD_VIDSOFT_VDP1B ||
	       id == YAB_THREAD_VIDSOFT_LAYER_RBG0;
}
struct thd_start { void (*func)(void *); void *arg; int nice; int cpu; int batch; int id; };
static int worker_nice;

/* Test knob YAB_AFFINITY (two-core devices): 1 = emulation thread on CPU 0,
 * all workers on CPU 1; 2 = emulation thread on CPU 0 only; 3 = emulation
 * thread on CPU 0, normal-priority workers (RBG0) on CPU 1;
 * 4 = normal-priority workers on CPU 1 only. */
static int affinity_mode = -1;

/* cpu < 0: any CPU (threads inherit the creator's mask, so workers started
 * by a pinned emulation thread must widen theirs) */
static void PinSelf(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	if (cpu >= 0)
		CPU_SET(cpu, &set);
	else
	{
		long n = sysconf(_SC_NPROCESSORS_CONF), i;
		for (i = 0; i < n && i < CPU_SETSIZE; i++)
			CPU_SET(i, &set);
	}
	sched_setaffinity(0, sizeof(set), &set);
}

static int WorkerCpu(unsigned int id, int niced)
{
	switch (affinity_mode)
	{
	case 1: return 1;
	case 3: case 4: return niced ? -1 : 1;
	default: return -1;
	}
}

/* With low worker priority the VDP1 and RBG0 workers also run as
 * SCHED_BATCH: they don't preempt the emulation thread when it
 * wakes them, and run on the other core or when it waits. +2-5% on 3D games
 * at a fixed draw rate. Test knob YAB_WORKER_BATCH: 0 = off, 1 = all render
 * workers, 2 = only VDP1/RBG0. */
static int batch_mode = -1;

static void ThreadTrampoline(void *data)
{
	struct thd_start st = *(struct thd_start *)data;
	free(data);
	yab_self_id = st.id;
	if (st.batch)
	{
		struct sched_param sp = { 0 };
		sched_setscheduler(0, SCHED_BATCH, &sp);
	}
	if (st.nice)
		setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), getpriority(PRIO_PROCESS, 0) + st.nice);
	if (affinity_mode > 0)
		PinSelf(st.cpu);
	st.func(st.arg);
}
#endif

void YabThreadSetWorkerNice(int nice)
{
#ifdef __linux__
	worker_nice = nice;
#else
	(void)nice;
#endif
}

int YabThreadStart(unsigned int id, void (*func)(void *), void *arg)
{
	if (!flag_lock)
	{
		flag_lock = slock_new();
		flag_cond = scond_new();
	}
	if (thread_handle[id].running == 1)
	{
		return -1;
	}

	thread_handle[id].mutex = slock_new();
	thread_handle[id].wake_pending = 0;

	if ((thread_handle[id].cond = scond_new()) == NULL)
	{
		return -1;
	}

#ifdef __linux__
	{
		const char *env = getenv("YAB_WORKER_NICE");
		const char *mask = getenv("YAB_WORKER_NICE_MASK"); /* test knob: bit n = thread id n */
		int niced = mask ? (int)((strtoul(mask, NULL, 0) >> id) & 1) : WorkerNiced(id);
		int nice = niced ? (env ? atoi(env) : worker_nice) : 0;
		int cpu;
		if (affinity_mode < 0)
		{
			const char *aff = getenv("YAB_AFFINITY");
			affinity_mode = aff ? atoi(aff) : 0;
			if (affinity_mode >= 1 && affinity_mode <= 3)
				PinSelf(0); /* the emulation thread starts the workers */
		}
		cpu = WorkerCpu(id, nice != 0);
		if (batch_mode < 0)
		{
			const char *b = getenv("YAB_WORKER_BATCH");
			batch_mode = b ? atoi(b) : worker_nice ? 2 : 0;
		}
		{
		int worker = id >= YAB_THREAD_VIDSOFT_LAYER_NBG3;
		int batch = worker && (batch_mode == 1 || (batch_mode == 2 && WorkerBatch(id)));
		if (nice || affinity_mode > 0 || batch)
		{
			struct thd_start *st = malloc(sizeof(*st));
			st->func = func; st->arg = arg; st->nice = nice; st->cpu = cpu; st->batch = batch; st->id = (int)id;
			if ((thread_handle[id].thd = sthread_create(ThreadTrampoline, st)) == NULL)
			{
				free(st);
				return -1;
			}
			thread_handle[id].running = 1;
			return 0;
		}
		}
	}
#endif
	{
		struct thd_id_start *st = malloc(sizeof(*st));
		st->func = func; st->arg = arg; st->id = (int)id;
		if ((thread_handle[id].thd = sthread_create(IdTrampoline, st)) == NULL)
		{
			free(st);
			return -1;
		}
	}

	thread_handle[id].running = 1;

	return 0;
}

void YabThreadWait(unsigned int id)
{
	if (thread_handle[id].running != 1)
		return;  // Thread wasn't running in the first place

	sthread_join(thread_handle[id].thd);

	thread_handle[id].running = 0;
}

int YabThreadSelfId(void)
{
	return yab_self_id;
}

void YabThreadYield(void)
{
#ifdef _WIN32
	SwitchToThread();
#else
	sched_yield();
#endif
}

void YabThreadSleep(void)
{
	int id = yab_self_id;

	if (id < 0 || id >= YAB_NUM_THREADS) return;

	// Wait until woken; a wake that happened since the last sleep counts,
	// otherwise it would be lost if it arrived before we got here.
	slock_lock(thread_handle[id].mutex);
	while (!thread_handle[id].wake_pending)
		scond_wait(thread_handle[id].cond, thread_handle[id].mutex);
	thread_handle[id].wake_pending = 0;
	slock_unlock(thread_handle[id].mutex);
}

void YabThreadRemoteSleep(unsigned int id)
{
}

void YabThreadWake(unsigned int id)
{
	if (thread_handle[id].running != 1)
		return;  // Thread wasn't running in the first place

	slock_lock(thread_handle[id].mutex);
	thread_handle[id].wake_pending = 1;
	scond_signal(thread_handle[id].cond);
	slock_unlock(thread_handle[id].mutex);
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

void YabFlagWait(volatile int *flag)
{
	int spin;

	// the work is often almost done: a short spin avoids a sleep and wake
	for (spin = 0; spin < 200; spin++)
	{
		if (YabFlagGet(*flag))
			return;
#if defined(__arm__) || defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	if (!flag_lock)
	{
		while (!YabFlagGet(*flag))
			YabThreadYield();
		return;
	}

	// The notifier takes the lock, so a flag set between our check and the
	// wait can't be missed: scond_wait releases the lock atomically.
	slock_lock(flag_lock);
	while (!YabFlagGet(*flag))
		scond_wait(flag_cond, flag_lock);
	slock_unlock(flag_lock);
}

void YabFlagNotify(void)
{
	if (!flag_lock)
		return;
	slock_lock(flag_lock);
	scond_broadcast(flag_cond);
	slock_unlock(flag_lock);
}
