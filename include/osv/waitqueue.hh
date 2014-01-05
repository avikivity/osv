/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef WAITQUEUE_HH_
#define WAITQUEUE_HH_

// A waitqueue is similar to a condition variable, but relies on the
// user supplied mutex for internal locking.

#include <sys/cdefs.h>
#include <sched.hh>
#include <osv/wait_record.hh>

class waitqueue {
private:
    struct {
        // A FIFO queue of waiters - a linked list from oldest (next in line
        // to be woken) towards newest. The wait records themselves are held
        // on the stack of the waiting thread - so no dynamic memory
        // allocation is needed for this list.
        struct wait_record *oldest = {};
        struct wait_record *newest = {};
    } _waiters_fifo;
public:
    /**
     * Wait on the wait queue
     *
     * Wait to be woken (with wake_one() or wake_all()), or the given time point
     * has passed, whichever occurs first.
     *
     * It is assumed that wait() is called with the given mutex locked.
     * This mutex is unlocked during the wait, and re-locked before wait()
     * returns.
     */
    void wait(mutex& mtx);
    /**
     * Wake one thread waiting on the condition variable
     *
     * Wake one of the threads currently waiting on the condition variable,
     * or do nothing if there is no thread waiting.
     *
     * wake_one() must be called with the mutex held.
     */
    void wake_one(mutex& mtx);
    /**
     * Wake all threads waiting on the condition variable
     *
     * Wake all of the threads currently waiting on the condition variable,
     * or do nothing if there is no thread waiting.
     *
     * If more than one thread is waiting, they will not all be woken
     * concurrently as all will need the same mutex and most will need to
     * go back to sleep (this is known as the "thundering herd problem").
     * Rather, only one thread is woken, and the rest are moved to the
     * waiting list of the mutex, to be woken up one by one as the mutex
     * becomes available. This optimization is known as "wait morphing".
     *
     * wake_all() must be called with the mutex held.
     */
    void wake_all(mutex& mtx);
private:
    void arm(mutex& mtx);
    bool poll() const;
    class waiter;
    friend class waiter;
    friend waiter wait_object(waitqueue& wq, mutex& mtx);
};

class waitqueue::waiter {
public:
    waiter(waitqueue& wq, mutex& mtx)
        : _wq(wq), _mtx(mtx), _wr(sched::thread::current()) {}
    waiter(const waiter&) = default;
    bool poll() const { return _wr.woken(); }
    void arm();
    void disarm();
private:
    waitqueue& _wq;
    mutex& _mtx;
    wait_record _wr;
};

waitqueue::waiter wait_object(waitqueue& wq, mutex& mtx)
{
    return waitqueue::waiter(wq, mtx);
}

#endif /* WAITQUEUE_HH_ */
