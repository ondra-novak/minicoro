#pragma once
#include "coroutine.h"
#include <atomic>
#include <mutex>

namespace MINICORO_NAMESPACE {

    
    ///helps to unlock a mutex during suspended operation.
    /**
     * @code
     * std::unique_lock lock(mx);
     * int result = co_await unlock_on_suspend(get_async_result(), lock);
     * @endcode
     *
     * @tparam _Awt an awaiter which is monitored
     * @tparam _Lock lock object which is controlled, expected std::unique_lock
     *
     * The awaiter monitors other awaiter. If the awaiter enters to await_suspend, it
     * performs to unlock of the mutex as the very last action of suspend operation.
     * The lock is acquired back before resume, so the code continues with locked owned
     *
     * @note the object need owned lock, otherwise it throws an exception
     *
     */
    template<typename _Awt, typename _Lock>
    class unlock_on_suspend {
    public:
        unlock_on_suspend(_Awt &&awt, _Lock &lock):_awt(awt), _lock(lock) {
            check_lock();
        }
        bool await_ready() {return _awt.await_ready();}
        decltype(auto) await_suspend(std::coroutine_handle<> h) {
            check_lock();
            //release mutex - we will unlock it later
            auto raw_mutex = _lock.release();
            //consider mutex is no longer owned by this thread,
            //construct lock defering lock operation
            _lock = _Lock(raw_mutex,std::defer_lock);
            //define call, which unlocks the mutex after suspend
            auto unlock = on_destroy([&]{
                //unlock, when suspend is done
                raw_mutex.unlock();
            });
            //forward to awaiter
            return _awt.await_suspend(h);
        }
        decltype(auto) await_resume() {
            //if lock is not owner, we returning from suspend
            //so acquire lock now - so it will look unchanged during operation
            if (!_lock) _lock.lock();
            return _awt.await_resume();
        }
    protected:
        _Awt &&_awt;
        _Lock &&_lock;
        void check_lock() {
            if (!_lock) throw std::invalid_argument("unlock_on_suspend: the lock is not owned");
        }
    };
    

    
    
}
