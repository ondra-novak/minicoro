#pragma once

#include "coroutine.h"
#include <mutex>

namespace MINICORO_NAMESPACE {

///limited queue - helper class for coro_basic_queue
/**
 * @tparam T type of item in queue
 * @tparam count max count of items in queue

 */
template<typename T, unsigned int count>
struct limited_queue{
public:

    using value_type = T;

    ///determine whether queue is full
    constexpr bool is_full() const {
        return _front - _back >= count;
    }

    ///determine whether queue is empty
    constexpr bool is_empty() const {
        return _front - _back == 0;
    }

    ///push item
    /**
     * @param args arguments to construct item
     *
     * @note it doesn't check fullness, use is_full() before you call this function
     *
     */
    template<typename ... Args>
    constexpr void push(Args && ... args) {
        item &x = _items[_front % count];
        std::construct_at(&x.val, std::forward<Args>(args)...);
        ++_front;
    }

    ///pop item
    /**
     * @return item removed from queue
     *
     * @note it doesn't check for emptyness, use is_empty() before calling of this function
     */
    constexpr T pop() {
        item &x = _items[_back % count];
        T r = std::move(x.val);
        std::destroy_at(&x.val);
        ++_back;
        return r;
    }


protected:

    struct item {
        T val;
        item() {}
        ~item() {}
    };

    item _items[count];
    unsigned int _front = 0;
    unsigned int _back = 0;
};



///basic coroutine queue
/**
 *
 * @tparam Queue_Impl implementation of the queue = example limited_queue
 * @tparam Lock object to lock internals
 */
template<typename Queue_Impl, typename Lock = std::mutex>
class coro_basic_queue {
public:

    using value_type = typename Queue_Impl::value_type;

    ///Push to queue
    /**
     * @param args arguments to construct item
     * @return awaitable (co_await). If there is a space in the queue, the operation finishes
     * immediately and new item is pushed to the queue. If the queue is full,
     * returned object is set to pending state. Push operation continues
     * when someone removes an item from the queue
     *
     * @note if you doesn't co_await on result, the item is pushed only if it
     * can be pushed without blocking. You can check this state by testing
     * is_ready()
     *
     */
    template<std::convertible_to<value_type> ... Args >
    awaitable<void> push(Args && ... args) {
        prepared_coro resm;
        std::lock_guard _(_mx);
        if (_queue.is_full()) {
            return [this,slot = slot<val_and_result>(std::forward<Args>(args)...)]
                    (awaitable<void>::result r) mutable {
                if (!r) return;
                slot.payload.res =std::move(r);
                _push_queue.push(&slot);
            };
        } else if (_queue.is_empty()) {
            auto slot = _pop_queue.pop();
            if (slot) {
                resm = slot->payload(std::forward<Args>(args)...);
            } else {
                _queue.push(std::forward<Args>(args)...);
            }
        } else {
            _queue.push(std::forward<Args>(args)...);
        }
    }

    ///pop from queue
    /**
     * @return awaitable object which eventually receives the item. You
     * need co_await on result.
     */
    awaitable<value_type> pop() {
        prepared_coro resm;
        std::lock_guard _(_mx);
        if (_queue.is_empty()) {
            return [this, slot = slot<typename awaitable<value_type>::result>({})]
                    (typename awaitable<value_type>::result r) mutable {
             if (!r) return;
             if (_closed) {
                 r =  _closed;
                 return;
             }
             slot.payload = std::move(r);
             _pop_queue.push(&slot);
            };
        } else {
            return pop2(resm);
        }
    }

    ///clear whole queue. The function also resumes all stuck producers
    void clear() {
        while (pop().is_ready());
    }

    ///close queue / set exception
    /**
     * @param e when argument is empty, queue is opened. When argument is
     * an valid instance, the queue is closed and asynchronous operation pop
     * returns exception (not applicable for synchronous pop, so you still
     * can push values which will received by pop until the queue is empty)
     *
     * This function also resumes all currently pending consuments with the
     * same exception
     */
    void set_closed(std::exception_ptr e) {
        slot<typename awaitable<value_type>::result> *slots;
        {
            std::lock_guard _(_mx);
            _closed = std::move(e);
            if (!e) return;

            slots = _pop_queue.first;
            _pop_queue.first = _pop_queue.last;
        }
        while (slots) {
            auto s = slots;
            slots = s->next;
            s->payload = _closed;
        }
    }



protected:

    template<typename _Payload>
    struct slot {
        _Payload payload = {};
        slot *next = nullptr;

        slot(_Payload p):payload(p) {}
    };

    struct val_and_result {
        value_type val;
        awaitable<void>::result res = {};

        template<std::convertible_to<value_type> ... Args >
        val_and_result(Args && ... args):val(std::forward<Args>(args)...) {}
    };

    awaitable<value_type> pop2(prepared_coro &resm) {
        awaitable<value_type> r ( _queue.pop());
        slot<val_and_result> *s = _push_queue.pop();
        if (s) {
            _queue.push(std::move(s->payload.val));
            resm = s->payload.res();
        }
        return r;
    }

    template<typename X>
    struct link_list_queue {
        slot<X> *first = {};
        slot<X> *last = {};

        void push(slot<X> *s) {
            if (last) {
                last->next = s;
                last = s;
            } else {
                first = last = s;
            }
        }

        slot<X> *pop() {
            auto r = first;
            if (r == last) {
                last = first = nullptr;
            } else {
                first = first->next;
            }
            return r;
        }

    };

    Lock _mx;
    Queue_Impl _queue;
    link_list_queue<typename awaitable<value_type>::result> _pop_queue;
    link_list_queue<val_and_result> _push_queue;
    std::exception_ptr _closed = {};


};


template<typename T, unsigned int count, typename Lock = std::mutex>
class coro_queue : public coro_basic_queue<limited_queue<T, count>, Lock > {};

}
