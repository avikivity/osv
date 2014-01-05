/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/waitqueue.hh>
#include <osv/trace.hh>
#include <osv/wait_record.hh>

TRACEPOINT(trace_waitqueue_wait, "%p", waitqueue *);
TRACEPOINT(trace_waitqueue_wake_one, "%p", waitqueue *);
TRACEPOINT(trace_waitqueue_wake_all, "%p", waitqueue *);

void waitqueue::waiter::arm()
{
    auto& fifo = _wq._waiters_fifo;
    if (!fifo.oldest) {
        fifo.oldest = &_wr;
    } else {
        fifo.newest->next = &_wr;
    }
    fifo.newest = &_wr;
}

void waitqueue::waiter::disarm()
{
    auto& fifo = _wq._waiters_fifo;
    if (_wr.woken()) {
        return;
    }
    // wr is still in the linked list, so remove it:
    wait_record** pp = &fifo.oldest;
    while (*pp) {
        if (&_wr == *pp) {
            *pp = (*pp)->next;
            if (!(*pp)->next) {
                fifo.newest = *pp;
            }
            break;
        }
        pp = &(*pp)->next;
    }
}

void waitqueue::wait(mutex& mtx)
{
    trace_waitqueue_wait(this);
    sched::thread::wait_for(mtx, *this);
}

void waitqueue::wake_one(mutex& mtx)
{
    trace_waitqueue_wake_one(this);
    wait_record *wr = _waiters_fifo.oldest;
    if (wr) {
        _waiters_fifo.oldest = wr->next;
        if (wr->next == nullptr) {
            _waiters_fifo.newest = nullptr;
        }
        // Rather than wake the waiter here (wr->wake()) and have it wait
        // again for the mutex, we do "wait morphing" - have it continue to
        // sleep until the mutex becomes available.
        wr->thread()->wake_lock(&mtx, wr);
    }
}

void waitqueue::wake_all(mutex& mtx)
{
    trace_waitqueue_wake_all(this);
    wait_record *wr = _waiters_fifo.oldest;
    _waiters_fifo.oldest = _waiters_fifo.newest = nullptr;
    while (wr) {
        auto next_wr = wr->next; // need to save - *wr invalid after wake
        // FIXME: splice the entire chain at once?
        wr->thread()->wake_lock(&mtx, wr);
        wr = next_wr;
    }
}

