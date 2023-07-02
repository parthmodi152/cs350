/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, int initial_count)
{
        struct semaphore *sem;

        KASSERT(initial_count >= 0);

        sem = kmalloc(sizeof(struct semaphore));
        if (sem == NULL)
        {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL)
        {
                kfree(sem);
                return NULL;
        }

        sem->sem_wchan = wchan_create(sem->sem_name);
        if (sem->sem_wchan == NULL)
        {
                kfree(sem->sem_name);
                kfree(sem);
                return NULL;
        }

        spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /* wchan_cleanup will assert if anyone's waiting on it */
        spinlock_cleanup(&sem->sem_lock);
        wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

        spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0)
        {
                /*
                 * Bridge to the wchan lock, so if someone else comes
                 * along in V right this instant the wakeup can't go
                 * through on the wchan until we've finished going to
                 * sleep. Note that wchan_sleep unlocks the wchan.
                 *
                 * Note that we don't maintain strict FIFO ordering of
                 * threads going through the semaphore; that is, we
                 * might "get" it on the first try even if other
                 * threads are waiting. Apparently according to some
                 * textbooks semaphores must for some reason have
                 * strict ordering. Too bad. :-)
                 *
                 * Exercise: how would you implement strict FIFO
                 * ordering?
                 */
                wchan_lock(sem->sem_wchan);
                spinlock_release(&sem->sem_lock);
                wchan_sleep(sem->sem_wchan);

                spinlock_acquire(&sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
        spinlock_release(&sem->sem_lock);
}

void V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
        wchan_wakeone(sem->sem_wchan);

        spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
        struct lock *newlock;

        newlock = kmalloc(sizeof(struct lock));
        if (newlock == NULL)
        {
                return NULL;
        }

        newlock->lk_name = kstrdup(name);
        if (newlock->lk_name == NULL)
        {
                kfree(newlock);
                return NULL;
        }

        newlock->lk_wchan = wchan_create(newlock->lk_name);
        if (newlock->lk_wchan == NULL)
        {
                kfree(newlock->lk_name);
                kfree(newlock);
                return NULL;
        }

        spinlock_init(&newlock->lk_lock);
        newlock->lk_held = false;
        newlock->lk_owner = NULL;

        return newlock;
}

void lock_destroy(struct lock *newlock)
{
        KASSERT(newlock != NULL);

        newlock->lk_owner = NULL;
        spinlock_cleanup(&newlock->lk_lock);
        wchan_destroy(newlock->lk_wchan);
        kfree(newlock->lk_name);
        kfree(newlock);
}

void lock_acquire(struct lock *newlock)
{
        KASSERT(newlock != NULL);
        KASSERT(!lock_do_i_hold(newlock));
        KASSERT(curthread->t_in_interrupt == false);

        spinlock_acquire(&newlock->lk_lock);
        while (newlock->lk_held)
        {
                wchan_lock(newlock->lk_wchan);
                spinlock_release(&newlock->lk_lock);
                wchan_sleep(newlock->lk_wchan);
                spinlock_acquire(&newlock->lk_lock);
        }
        KASSERT(newlock->lk_held == false);
        newlock->lk_held = true;
        newlock->lk_owner = curthread;
        spinlock_release(&newlock->lk_lock);
}

void lock_release(struct lock *newlock)
{
        KASSERT(newlock != NULL);
        KASSERT(lock_do_i_hold(newlock));
        spinlock_acquire(&newlock->lk_lock);
        newlock->lk_held = false;
        newlock->lk_owner = NULL;
        wchan_wakeone(newlock->lk_wchan);
        spinlock_release(&newlock->lk_lock);
}

bool lock_do_i_hold(struct lock *newlock)
{
        KASSERT(newlock != NULL);
        return (newlock->lk_owner == curthread);
}

////////////////////////////////////////////////////////////
//
// CV

struct cv *
cv_create(const char *name)
{
        struct cv *newcv;

        newcv = kmalloc(sizeof(struct cv));
        if (newcv == NULL)
        {
                return NULL;
        }

        newcv->cv_name = kstrdup(name);
        if (newcv->cv_name == NULL)
        {
                kfree(newcv);
                return NULL;
        }

        newcv->cv_wchan = wchan_create(newcv->cv_name);
        if (newcv->cv_wchan == NULL)
        {
                kfree(newcv->cv_name);
                kfree(newcv);
                return NULL;
        }

        return newcv;
}

void cv_destroy(struct cv *newcv)
{
        KASSERT(newcv != NULL);

        wchan_destroy(newcv->cv_wchan);
        kfree(newcv->cv_name);
        kfree(newcv);
}

void cv_wait(struct cv *newcv, struct lock *newlock)
{
        KASSERT(newcv != NULL);
        KASSERT(newlock != NULL);
        KASSERT(lock_do_i_hold(newlock));

        wchan_lock(newcv->cv_wchan);
        lock_release(newlock);
        wchan_sleep(newcv->cv_wchan);
        lock_acquire(newlock);
}

void cv_signal(struct cv *newcv, struct lock *newlock)
{
        KASSERT(newcv != NULL);
        KASSERT(newlock != NULL);

        wchan_wakeone(newcv->cv_wchan);
}

void cv_broadcast(struct cv *newcv, struct lock *newlock)
{
        KASSERT(newcv != NULL);
        KASSERT(newlock != NULL);

        wchan_wakeall(newcv->cv_wchan);
}
