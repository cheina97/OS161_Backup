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

#include <current.h>
#include <lib.h>
#include <opt-sync1.h>
#include <opt-sync2.h>
#include <spinlock.h>
#include <synch.h>
#include <thread.h>
#include <types.h>
#include <wchan.h>

#include "opt-cvsem.h"
#include "opt-cvwc.h"

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count) {
    struct semaphore *sem;

    sem = kmalloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }

    sem->sem_name = kstrdup(name);
    if (sem->sem_name == NULL) {
        kfree(sem);
        return NULL;
    }

    sem->sem_wchan = wchan_create(sem->sem_name);
    if (sem->sem_wchan == NULL) {
        kfree(sem->sem_name);
        kfree(sem);
        return NULL;
    }

    spinlock_init(&sem->sem_lock);
    sem->sem_count = initial_count;

    return sem;
}

void sem_destroy(struct semaphore *sem) {
    KASSERT(sem != NULL);

    /* wchan_cleanup will assert if anyone's waiting on it */
    spinlock_cleanup(&sem->sem_lock);
    wchan_destroy(sem->sem_wchan);
    kfree(sem->sem_name);
    kfree(sem);
}

void P(struct semaphore *sem) {
    KASSERT(sem != NULL);

    /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
    KASSERT(curthread->t_in_interrupt == false);

    /* Use the semaphore spinlock to protect the wchan as well. */
    spinlock_acquire(&sem->sem_lock);
    while (sem->sem_count == 0) {
        /*
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
        wchan_sleep(sem->sem_wchan, &sem->sem_lock);
    }
    KASSERT(sem->sem_count > 0);
    sem->sem_count--;
    spinlock_release(&sem->sem_lock);
}

void V(struct semaphore *sem) {
    KASSERT(sem != NULL);

    spinlock_acquire(&sem->sem_lock);

    sem->sem_count++;
    KASSERT(sem->sem_count > 0);
    wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

    spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

#if OPT_SYNC1

struct lock *
lock_create(const char *name) {
    struct lock *lk = (struct lock *)kmalloc(sizeof(struct lock));
    spinlock_acquire(&lk->lk_lock);
    lk->sem = sem_create(name, 1);
    lk->owner = NULL;
    lk->lk_name = kstrdup(name);
    spinlock_release(&lk->lk_lock);
    return lk;
}

void lock_destroy(struct lock *lock) {
    KASSERT(lock != NULL);
    spinlock_acquire(&lock->lk_lock);
    sem_destroy(lock->sem);
    kfree(lock->lk_name);
    spinlock_release(&lock->lk_lock);
}

void lock_acquire(struct lock *lock) {
    KASSERT(lock != NULL);
    spinlock_acquire(&lock->lk_lock);
    P(lock->sem);
    lock->owner = curthread;
    spinlock_release(&lock->lk_lock);
}

void lock_release(struct lock *lock) {
    KASSERT(lock != NULL);
    spinlock_acquire(&lock->lk_lock);
    KASSERT(lock_do_i_hold(lock) == true);
    V(lock->sem);
    lock->owner = NULL;
    spinlock_release(&lock->lk_lock);
}

bool lock_do_i_hold(struct lock *lock) {
    return lock->owner == curthread;  // dummy until code gets written
}

#endif

#if OPT_SYNC2

struct lock *
lock_create(const char *name) {
    struct lock *lock;

    lock = kmalloc(sizeof(*lock));
    if (lock == NULL) {
        return NULL;
    }

    lock->lk_name = kstrdup(name);
    if (lock->lk_name == NULL) {
        kfree(lock);
        return NULL;
    }

    HANGMAN_LOCKABLEINIT(&lock->lk_hangman, lock->lk_name);

    lock->lk_wchan = wchan_create(lock->lk_name);
    if (lock->lk_wchan == NULL) {
        kfree(lock->lk_name);
        kfree(lock);
        return NULL;
    }

    spinlock_init(&lock->lk_lock);
    lock->lk_count = 1;

    return lock;
}

void lock_destroy(struct lock *lock) {
    KASSERT(lock != NULL);
    /* wchan_cleanup will assert if anyone's waiting on it */
    spinlock_cleanup(&lock->lk_lock);
    wchan_destroy(lock->lk_wchan);
    kfree(lock->lk_name);
    kfree(lock);
}

void lock_acquire(struct lock *lock) {
    KASSERT(lock != NULL);

    KASSERT(curthread->t_in_interrupt == false);

    spinlock_acquire(&lock->lk_lock);

    while (lock->lk_count == 0) {
        wchan_sleep(lock->lk_wchan, &lock->lk_lock);
    }
    KASSERT(lock->lk_count > 0);
    lock->lk_count--;
    //KASSERT(lock->owner == NULL);
    lock->owner = curthread;
    spinlock_release(&lock->lk_lock);
}

void lock_release(struct lock *lock) {
    KASSERT(lock != NULL);

    spinlock_acquire(&lock->lk_lock);

    KASSERT(lock_do_i_hold(lock) == true);
    lock->owner = NULL;
    lock->lk_count++;
    KASSERT(lock->lk_count > 0);
    wchan_wakeone(lock->lk_wchan, &lock->lk_lock);

    spinlock_release(&lock->lk_lock);
}

bool lock_do_i_hold(struct lock *lock) {
    return lock->owner == curthread;  // dummy until code gets written
}

#endif

////////////////////////////////////////////////////////////
//
// CV

#if OPT_CVSEM
struct cv *
cv_create(const char *name) {
    struct cv *cv;

    cv = kmalloc(sizeof(*cv));
    if (cv == NULL) {
        return NULL;
    }

    cv->cv_name = kstrdup(name);
    if (cv->cv_name == NULL) {
        kfree(cv);
        return NULL;
    }
    cv->wait_counter = 0;
    cv->lk->lk_name = cv->cv_name;
    cv->sem = sem_create(cv->cv_name, 1);

    return cv;
}

void cv_destroy(struct cv *cv) {
    KASSERT(cv != NULL);

    lock_destroy(cv->lk);
    sem_destroy(cv->sem);

    kfree(cv->cv_name);
    kfree(cv);
}

void cv_wait(struct cv *cv, struct lock *lock) {
    cv->wait_counter++;
    lock_release(lock);
    P(cv->sem);
    lock_acquire(lock);
    if (cv->wait_counter != 0) cv->wait_counter--;
}

void cv_signal(struct cv *cv, struct lock *lock) {
    KASSERT(lock->owner == curthread);
    V(cv->sem);
}

void cv_broadcast(struct cv *cv, struct lock *lock) {
    KASSERT(lock->owner == curthread);
    for (int i = 0; i < cv->wait_counter; i++) {
        V(cv->sem);
    }
    cv->wait_counter = 0;
}

#endif

#if OPT_CVWC
struct cv *
cv_create(const char *name) {
    struct cv *cv;

    cv = kmalloc(sizeof(*cv));
    if (cv == NULL) {
        return NULL;
    }

    cv->cv_name = kstrdup(name);
    if (cv->cv_name == NULL) {
        kfree(cv);
        return NULL;
    }
    cv->wc= wchan_create(cv->cv_name);
    cv->slk=kmalloc(sizeof(struct spinlock));
    spinlock_init(cv->slk);

    return cv;
}

void cv_destroy(struct cv *cv) {
    KASSERT(cv != NULL);

    spinlock_cleanup(cv->slk);
    kfree(cv->slk);
    wchan_destroy(cv->wc);

    kfree(cv->cv_name);
    kfree(cv);
}

void cv_wait(struct cv *cv, struct lock *lock) {
    spinlock_acquire(cv->slk);
    lock_release(lock);
    wchan_sleep(cv->wc,cv->slk);
    spinlock_release(cv->slk);
    lock_acquire(lock);

}

void cv_signal(struct cv *cv, struct lock *lock) {
    KASSERT(lock_do_i_hold(lock));
    spinlock_acquire(cv->slk);
    wchan_wakeone(cv->wc,cv->slk);
    spinlock_release(cv->slk);

}

void cv_broadcast(struct cv *cv, struct lock *lock) {
    KASSERT(lock_do_i_hold(lock));
    spinlock_acquire(cv->slk);
    wchan_wakeall(cv->wc,cv->slk);
    spinlock_release(cv->slk);
}
#endif