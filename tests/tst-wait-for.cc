/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#define BOOST_TEST_MODULE tst-wait-for

#include <boost/test/unit_test.hpp>
#include <sched.hh>
#include <osv/waitqueue.hh>
#include <drivers/clock.hh>
#include <cstdlib>

BOOST_AUTO_TEST_CASE(test_wait_for_one_timer)
{
    auto now = clock::get()->time();
    sched::timer tmr(*sched::thread::current());
    tmr.set(now + 1_s);
    sched::thread::wait_for(tmr);
    auto later = clock::get()->time();
    BOOST_REQUIRE(std::abs(later - (now + 1_s)) < 20_ms);
    BOOST_REQUIRE(tmr.expired());
}

BOOST_AUTO_TEST_CASE(test_wait_for_two_timers)
{
    auto now = clock::get()->time();
    sched::timer tmr1(*sched::thread::current());
    sched::timer tmr2(*sched::thread::current());
    tmr1.set(now + 2_s);
    tmr2.set(now + 1_s);
    sched::thread::wait_for(tmr1, tmr2);
    BOOST_REQUIRE(!tmr1.expired() && tmr2.expired());
    tmr2.cancel();
    sched::thread::wait_for(tmr1, tmr2);
    BOOST_REQUIRE(tmr1.expired() && !tmr2.expired());
}

#include <debug.hh>

BOOST_AUTO_TEST_CASE(test_waitqueue)
{
    waitqueue wq;
    mutex mtx;
    int counter = 0;
    debug("entry\n");
    WITH_LOCK(mtx) {
        sched::thread waker([&] {
            debug("waker thread\n");
            WITH_LOCK(mtx) {
                debug("waker thread: acquired lock\n");
                ++counter;
                wq.wake_one(mtx);
                debug("waker thread: wake_one() done\n");
            }
        });
        waker.start();
        debug("waiting\n");
        wq.wait(mtx);
    }
    BOOST_REQUIRE(counter == 1);
}
