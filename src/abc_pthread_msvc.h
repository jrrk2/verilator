// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: the pthread subset abc's opt/ufar uses, for MSVC
//
// On Windows abc's opt/ufar includes abc's pthreads-win32 header and expects
// its pthreadVC2.dll, a 2010 binary that needs the MSVC 2010 runtime.  The
// CMake build on MSVC compiles that unit through abc_ufarpth_msvc.cpp with
// this header instead: the ten calls it makes, over the C++ thread library.
// pthread_exit unwinds the thread with an exception that pthread_create's
// wrapper catches.
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_ABC_PTHREAD_MSVC_H_
#define VERILATOR_ABC_PTHREAD_MSVC_H_

#define PTHREAD_H  // third_party/abc/lib/pthread.h is then empty

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

struct pthread_attr_t final {};
struct pthread_mutex_t final {
    std::mutex mutex;
};
struct pthread_cond_t final {
    std::condition_variable_any cond;
};
struct pthread_thread_t final {
    std::thread thread;
    void* result = nullptr;
};
using pthread_t = pthread_thread_t*;
struct pthread_exit_t final {
    void* value;
};

inline int pthread_create(pthread_t* threadp, const pthread_attr_t*, void* (*start)(void*),
                          void* arg) {
    pthread_thread_t* const tp = new pthread_thread_t;
    tp->thread = std::thread{[tp, start, arg]() {
        try {
            tp->result = start(arg);
        } catch (const pthread_exit_t& e) { tp->result = e.value; }
    }};
    *threadp = tp;
    return 0;
}
inline int pthread_join(pthread_t tp, void** resultp) {
    tp->thread.join();
    if (resultp) *resultp = tp->result;
    delete tp;
    return 0;
}
[[noreturn]] inline void pthread_exit(void* value) { throw pthread_exit_t{value}; }
inline int pthread_mutex_lock(pthread_mutex_t* mp) {
    mp->mutex.lock();
    return 0;
}
inline int pthread_mutex_unlock(pthread_mutex_t* mp) {
    mp->mutex.unlock();
    return 0;
}
inline int pthread_cond_signal(pthread_cond_t* cp) {
    cp->cond.notify_one();
    return 0;
}
inline int pthread_cond_timedwait(pthread_cond_t* cp, pthread_mutex_t* mp,
                                  const timespec* abstime) {
    const auto since
        = std::chrono::seconds{abstime->tv_sec} + std::chrono::nanoseconds{abstime->tv_nsec};
    const std::chrono::system_clock::time_point deadline{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(since)};
    return cp->cond.wait_until(mp->mutex, deadline) == std::cv_status::timeout ? ETIMEDOUT : 0;
}

#endif  // Guard
