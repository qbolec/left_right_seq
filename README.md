# left_right_seq
C++ header-only lib for synchronizing single writer with many readers

# What if Left-Right and Seq-Lock had a baby?
There are two cool ways of synchronizing single writer with many readers.

A Left-Right<T> maintains internally two copies of T, and lets the readers use the other when one is being modified.
The readers are never blocked by the writer, but the writer might need to wait for a reader (say if it got preempted from CPU in the middle of read, leaving non-zero refcount).
One downside of Left-Right is that you need space for 2 copies and time for 2 updates.

A Seq-lock<T> maintains a counter, which the writer bumps before and after modification, so a reader can detect the write is happening by checking parity or that it happened during its read by comparing the counter before and after the read.
The writer is never blocked, but the readers might have to wait for the writer (say, if it got preempted from CPU in the middle of write, leaving odd counter).
Interestingly the readers do not announce their presense in any way, so they never write to memory, which is nice for performance.
Also, the class itself only uses memory_order_release and memory_order_acquire atomic operations, which on x64 transalate to simple `mov`s - no extra memory barriers.
(Which is cool, but perhaps causes MACHINE_CLEAR events in readers, though?)
One downside of Seq-lock is that T has to be safe to read even while other threads modify it - ideally, is just a composition of atomic members.

So, what would happen if you combined the two approaches?
Neither the writer would have to wait for readers, nor readers for the writer!

The left_right_seq<T> maintains two copies of T internally and a counter, which it bumps before and after updating each of them.
The readers look at the counter before the read to figure out which of the two instances is safe to access, based on last two bits:

|bits|meaning|
|----|-------------------------|
| 00 | both instances are ready, but [0] is the latest one to use |
| 01 | the [1] is under construction, so use [0]                  |
| 10 | both instances are ready, but [1] is the latest one to use |
| 11 | the [0] is under construction, so use [1]                  |

# Semantics

So what are really the semantics of a data structure like that, given not only that nobody is waiting for anyone, but also (like in Seq-lock) there are no strong memory barriers?
I think it's similar to atomic<T> used only with memory_order_release store()s and memory_order_acquire load()s.
That is, there is no guaranteed single order of all the operations.
But, there is an order of modifications (actually, that's the requirement which the user has to satisfy: at most one writer at a time implies happens-before relation between all writes).
Each read(..) has to read one of the values which was write(..)n (as opposed to: something random, out of thin air).
And *if* a given read(..) observed value write(..)en by another thread, *then* the write(..) synchronizes-with the read(..) and thus establishes happens-before relation.

# Usage
```c++
#include "left_right_seq.hpp"
#include <atomic>

// An example data structure you want to protect.
// It's individual fields are atomic, but there's also some invariant: sum == x + y.
struct Information {
    std::atomic<uint64_t> x;
    std::atomic<uint64_t> y;
    std::atomic<uint64_t> sum;
    Information() = default;
    // Only needed if you want to use load()
    Information(const Information& other)
        : x(other.x.load(std::memory_order_relaxed))
        , y(other.y.load(std::memory_order_relaxed))
        , sum(other.sum.load(std::memory_order_relaxed))
    {
    }
    // Only needed if you want to use store()
    Information& operator=(const Information& other)
    {
        x.store(other.x.load(std::memory_order_relaxed));
        y.store(other.y.load(std::memory_order_relaxed));
        sum.store(other.sum.load(std::memory_order_relaxed));
        return *this;
    }
};

left_right_seq<Information> information;

void test()
{
    const auto res = information.write([](Information& info) {
        // relaxed stores are good enough - the left_right_seq adds the std::atomic_thread_fence for you
        info.x.store(13, std::memory_order_relaxed);
        // you can read existing info, if you want
        const auto prev_y = info.y.load(std::memory_order_relaxed);
        info.y.store(prev_y + 2, std::memory_order_relaxed);
        // some invariant:
        info.sum.store(13 + prev_y + 2, std::memory_order_relaxed);
        // you can return something, too
        return prev_y + 2;
    });
    const auto zero = information.read([](const Information& info) {
        // you can use arbitrary logic here, and relaxed loads
        if (info.x.load(std::memory_order_relaxed) > 10) {
            // This function might be retried many times if modification during call was detected.
            // this means that you can assume that info.x has the same value here as in the if condition in
            // and that the x+y==sum, but only in the final, successful run.
            // Do not, for example, try to divide info.x, assumming it can't be 0 due to above if.
            return info.x.load(std::memory_order_relaxed) + info.y.load(std::memory_order_relaxed) - info.sum.load(std::memory_order_relaxed);
        } else {
            return 0ULL;
        }
    });
    // There are also convinience wrappers for read and write which are just assigning values:
    Information info = information.load();
    information.store(info);
}
```
