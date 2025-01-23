#pragma once
#include "coroutine.h"
#include <array>

namespace MINICORO_NAMESPACE {

///implements concurrency mutex
/**
 * This mutex can be used inside of coroutines and can be held across co_await and across multiple threads
 *
 * There is difference of standard mutex. This mutex contains own guard, named ownership. Your code
 * holds mutex while it holds ownership. Releasing ownership causes releasing mutex. The
 * ownership object is movable, but not copyable. You can construct ownership without owning
 * the mutex (this can be tested)
 *
 * @code
 * auto own = co_await mx.lock();
 * co_await async_op();
 * own.release();
 * @endcode
 */
class coro_mutex {
public:

    ///ownership object - carries ownership of the locked mutex
    class ownership {
    public:
        ///default construct not owned
        ownership() = default;
        ///you can move
        ownership(ownership &&other):_owning(std::exchange(other._owning, nullptr)) {}
        ///you can move by assignment
        ownership &operator=(ownership &&other) {
            if (this != &other) {
                release();
                _owning = std::exchange(other._owning, nullptr);
            }
            return *this;
        }
        ///release ownership prematurely
        /** this allows to release ownership before the object expires. After execution this
         * function, the object loose ownership
         *
         * This also allows to schedule and resumption any awaiting coroutine
         *
         * @return prepared coroutine which received ownership (if any). You can schedule its resumption
         */
        prepared_coro release() {
            auto p = std::exchange(_owning, nullptr);
            if (p) return p->unlock();
            return {};
        }
        ///destructor releases ownership
        ~ownership() {
            release();
        }

        ///determine state
        /**
         * @retval true object does own the lock
         * @retval false object doesn't own the lock
         */
        bool owns_lock() const {return _owning != nullptr;}

        ///determine state
        /**
         * @retval true object does own the lock
         * @retval false object doesn't own the lock
         */
        explicit operator bool() const {return _owning != nullptr;}
    protected:
        ownership(coro_mutex *own):_owning(own) {}
        coro_mutex *_owning = nullptr;

        friend class coro_mutex;
    };

    ///try to lock without waiting
    /**
     * @return ownership object either owning lock, or not owning lock. you need to test it by method ownership::owns_lock()
     */
    ownership try_lock() {
        slot *need = nullptr;
        if (_requests.compare_exchange_strong(need, get_doorman())) return this;
        else return {};
    }

    ///attempt to lock, allow to co?await
    /**
     * @return returns awaitable. To acquire lock, you need to co_await on awaitable.
     *
     * @note The state of awaitable depends on whether try_lock successed. In successful
     * try_lock(), it immediately returns ownership and no waiting is required. Otherwise
     * it is set to pending state and you need to co_await on it to obtain ownership.
     *
     * The acquire process starts after co_await is initiated. You can discard return value
     * which cancels the operation (releasing ownership if has been acquired by try_lock())
     */
    awaitable<ownership> lock() {
        //try lock
        auto test = try_lock();
        //if success, return it directly
        if (test) return test;
        //otherwise create slot and add self to waiting queue
        return [s = slot(), this](awaitable<ownership>::result r) mutable {
            if (!r) return prepared_coro{};
            //retrieve awaitable as pointer
            s._resume = r.release();
            //add slot as request
            return add_request(&s);
        };
    }


protected:
    //item of linked list of the requests and queue
    struct slot {
        //next item in linked list
        slot *_next;
        //pointer to awaitable to be resolved when ownership is retrieved
        awaitable<ownership> *_resume;

    };

    constexpr static slot doorman = {};


    //stack of requests - added between unlocks
    std::atomic<slot *> _requests = {};
    //queue of requests - processed during unlocks
    slot *_queue = {};
    //retrieve pointer to doorman
    slot *get_doorman() {return const_cast<slot *>(&doorman);}

    //add slot to request stack
    prepared_coro add_request(slot *s) {
        //atomically add slot to _requests stack - linked list
        while (!_requests.compare_exchange_strong(s->_next, s));
        //this checks whether this slot was added as first
        if (s->_next == nullptr) {
            //in this case, lock is successful, we own the lock
            //prepare queue of possible other request and at bottom of request stack atomically
            make_queue(_requests.exchange(get_doorman()), s);
            //resume this slot
            return resume_slot(s);
        }
        //request added, nothing to resume
        return {};
    }

    //resume slot
    prepared_coro resume_slot(slot *s) {
        //convert pointer back to result
        awaitable<ownership>::result r(s->_resume);
        //set ownership to resume
        return r(ownership(this));
    }

    //converts stack oriented linked list to queue
    //linked list start at "from" item and must stop at "to" item
    //reversed list is put to _queue.
    void make_queue(slot *from, slot *to) {
        while (from != to) {
            auto n = from->_next;
            from->_next = _queue;
            _queue = from;
            from = n;
        }
    }

    //unlock and transfer ownership
    prepared_coro unlock() {
        //if queue is empty, probably nobody is waiting
        if (!_queue) {
            slot *d = get_doorman();
            slot *need = d;
            //try exchange doorman by nullptr
            if (_requests.compare_exchange_strong(need, nullptr)) {
                //if successed, lock is unlocked
                //nothing to resume
                return {};
            }
            //there are requests, make queue
            make_queue(_requests.exchange(d), d);
        }
        //pick first from the queue
        auto f = _queue;
        //advance queue
        _queue = f->_next;
        //resume picked slot
        return resume_slot(f);
    }
};


///implements multiple coro_mutex locking
/**
 * @tpatam n count of mutexes. This value is subject of CTAG.
 * You can set this value higher than actuall mutex count, but you must ensure, that extra space is filled by nullptrs.
 * (with exception, the first pointer must not be nullptr)
 */
template<int n>
class multi_lock {
public:

    ///ownership of multiple mutexes
    using ownership = std::array<coro_mutex::ownership, n>;

    ///initialize class with list of mutexes
    /**
     * @param list array of pointers to existing mutexes. Expect the first pointer, others can be set to nullptr
     */
    multi_lock(coro_mutex *(&list)[n]) {
        int p = 0;
        for (auto &x:locking) x = list[p++];
    }

    ///attempt lo lock on multiple mutexes
    /**
     * The function ensures that no deadlock happen as the unsuccessful lock of particular mutex
     * causes rollback of other locks and new attempt. This can cause repeatedly acquire and release all mutexes
     * during the process
     *
     * The function returns awaitable void, because the object holds the ownership (so you can release them by
     * destroying this object). It is possible to retrieve ownership by function get_ownership() which can
     * be useful, if you need move ownership around
     *
     */
    awaitable<void> lock() {
        //try lock first
        auto o = locking[0]->try_lock();
        //if success
        if (o) {
            //try lock others
            int x = lock_others();
            //if success (all locked);
            if (x == n) {
                //success returned (default constructed awaitable)
                return {};
            }
            //mark index of failed lock - we will start with them
            first = x;
        }
        //prepare asynchronous locking
        return [this](auto res) {
            //do not process when operation has been canceled
            if (!res) return prepared_coro{};
            //store result
            r = std::move(res);
            //attempt to lock first
            return lock_first();
        };
    }

    ///retrieve ownership
    /** Moves ownership from the object to the return value */
    ownership get_ownership() {
        ownership ret;
        int p = 0;
        for (auto &x: ret) x = std::move(owns[p++]);
        return ret;
    }

protected:

    prepared_coro lock_complete(awaitable<coro_mutex::ownership> &awt) {
        //when lock is complete, remeber ownership
        owns[first] = awt.await_resume();
        //attempt to lock others
        int x = lock_others();
        //if failed - ownership has been released
        if (x != n) {
            //update first - we attempt to lock it asynchronously
            first = x;
            //lock it now
            return lock_first();
        }
        //done, set result and resume
        return r();
    }


    ///a callback function intended to call this object if lock is complete
    using resolve_cb = awaitable<coro_mutex::ownership>::member_callback<multi_lock, &multi_lock::lock_complete>;

    prepared_coro lock_first() {
        //initiate lock - call the callback when done - use cb_buffer to store callback's internals
        return locking[first]->lock().set_callback(resolve_cb(this), cb_buffer);
    }

    int lock_others() {
        //start with first until n
        for (int i = 1; i < n; ++i) {
            //calculate index
            int idx = (i+first) % n;
            //if not null
            if (locking[idx]) {
                //try to lock
                auto o = locking[idx]->try_lock();
                if (!o) {
                    //if failed, release all ownerships
                    for (auto &x: owns) x.release();
                    //return failed index
                    return i;
                }
            }
        }
        //return max as no failure
        return n;

    }

    //list of mutexes
    coro_mutex *locking[n] = {};
    //list of ownership
    coro_mutex::ownership owns[n] = {};
    //result
    awaitable<void>::result r = {};
    //buffer to store callback's internals
    char cb_buffer[awaiting_callback_size<coro_mutex::ownership, resolve_cb>];
    //first mutex to lock asynchronously
    int first = 0;
};


template<int n>
multi_lock(coro_mutex *(&list)[n]) -> multi_lock<n>;

}
