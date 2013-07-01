//
// single-producer / single-consumer lockless ring buffer of fixed size.
//
#ifndef __LF_RING_HH__
#define __LF_RING_HH__

#include <atomic>
#include <sched.hh>

#define CACHELINE_ALIGNED __attribute__((aligned(64)))

//
// spsc ring of fixed size
//
template<class T, unsigned MaxSize>
class ring_spsc {
public:
    ring_spsc(): _begin(0), _end(0) { }

    bool push(const T& element)
    {
        unsigned end = _end.load(std::memory_order_relaxed);
        unsigned beg = _begin.load(std::memory_order_relaxed);

        if (end - beg >= MaxSize) {
            return false;
        }

        _ring[end % MaxSize] = element;
        _end.store(end + 1, std::memory_order_release);

        return true;
    }

    bool pop(T& element)
    {
        unsigned beg = _begin.load(std::memory_order_relaxed);
        unsigned end = _end.load(std::memory_order_acquire);

        if (beg >= end) {
            return false;
        }

        element = _ring[beg % MaxSize];
        _begin.store(beg + 1, std::memory_order_relaxed);

        return true;
    }

private:
    std::atomic<unsigned> _begin CACHELINE_ALIGNED;
    std::atomic<unsigned> _end CACHELINE_ALIGNED;
    T _ring[MaxSize];
};

//
// A spsc ring that has a blocking push()
//
template<class T, unsigned MaxSize>
class wait_ring_spsc {
public:

    wait_ring_spsc(): _ring(), _waiter(nullptr) { }

    void push(const T& element)
    {
        if (!_ring.push(element)) {
            _waiter = sched::thread::current();
            sched::thread::wait_until([&] { return (_waiter == nullptr); });
            bool rc = _ring.push(element);
            assert(rc);
        }
    }

    bool pop(T& element)
    {
        bool rc = _ring.pop(element);
        if (rc) {
            if (_waiter) {
                _waiter->wake_with([&] { _waiter = nullptr; });
            }
        }

        return rc;
    }

private:
    ring_spsc<T, MaxSize> _ring;
    sched::thread* _waiter CACHELINE_ALIGNED;
};


#endif // !__LF_RING_HH__
