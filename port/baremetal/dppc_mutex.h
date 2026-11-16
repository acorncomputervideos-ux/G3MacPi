// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/baremetal/dppc_mutex.h — recursive mutex type for DingusPPC's timer
// manager on bare metal (selected via -DDPPC_RECURSIVE_MUTEX_HEADER).
//
// The bare-metal C++ library has no thread support, so std::recursive_mutex
// does not exist. In the current design the whole emulated machine (CPU,
// devices, timer queue) runs on ONE core and the timer queue is only touched
// at task level on that core, never from interrupt handlers. Locking is
// therefore not required; this type only checks balanced use.
//
// MUST be revisited before any other core or interrupt handler touches the
// timer queue (plan step P7, multi-core split).

#ifndef G3_PORT_BAREMETAL_DPPC_MUTEX_H
#define G3_PORT_BAREMETAL_DPPC_MUTEX_H

#include <mutex>  // std::lock_guard (available without thread support)

class DppcRecursiveMutex {
public:
    void lock() { ++depth_; }
    bool try_lock()
    {
        ++depth_;
        return true;
    }
    void unlock() { --depth_; }
    unsigned depth() const { return depth_; }

private:
    unsigned depth_ = 0;
};

#endif // G3_PORT_BAREMETAL_DPPC_MUTEX_H
