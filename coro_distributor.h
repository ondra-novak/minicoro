#pragma once

#include "coroutine.h"
#include <mutex>
#include <vector>
#include <algorithm>

namespace MINICORO_NAMESPACE {


template<typename T, typename Lock = std::mutex>
class distributor {
public:
    using value_type = std::add_lvalue_reference<T>;

    using awaitable = MINICORO_NAMESPACE::awaitable<value_type>;
    using result_object = typename awaitable::result;
    using prepared = std::vector<prepared_coro>;
    using ident = const void *;

    struct awaiting_info {
        result_object r;
        ident i;
    };

    ///register coroutine to receive broadcast
    /**
     * @return awaitable, which returns reference to broadcasted value.
     * @note the function is MT-Safe if the Lock is std::mutex
     */
    awaitable operator()(ident id = {}) {
        return [this,id](result_object r){
            std::lock_guard _(_mx);
            _results.push_back({std::move(r), id});
        };
    }

    ///register coroutine to receive broadcast with alert support
    /**
     * @param alert_flag reference to alert flag. This must be set to false to register. If
     * it is set to true, the co_await immediately returns
     * @param id identity of coroutine, must be unique
     * 
     * @see alert
     */
    awaitable operator()(std::atomic<bool> &alert_flag, ident id) {
        return [this,id,&alert_flag](result_object r){
            std::lock_guard _(_mx);
            if (alert_flag.load(std::memory_order_relaxed)) return;
            _results.push_back({std::move(r), id});
        };
    }

    ///broadcast the value
    /**
     * @param v value to broadcast
     * @param buffer (preallocated) buffer to store prepared_coroutines. You
     * need to clear the buffer to resume all these coroutines. You can
     * use thread pool to enqueue coroutines to run
     *
     * @note This function is MT-Safe if the Lock is std::mutex
     */
    void broadcast(value_type v, prepared &buffer) {
        std::lock_guard _(_mx);
        for (auto &r: _results) {
            buffer.push_back(r.r(v));
        }
    }

    ///broadcast the value and resume awaiting coroutines in current thread
    /**
     * @param v value to broadcast
     * @note This function is not MT-Safe for this function, only one
     * thread can call broadcast() at the same time. For other functions
     * this function is MT-Safe.
     */
    void broadcast(value_type v) {
        broadcast(v, _ready_to_run);
        _ready_to_run.clear();
    }

    ///kicks out awaiting coroutine
    /**
     * @param id identification of coroutine (used during registration). If the id
     * is not unique, a random coroutine with equal id is kicked out - but only one (not recommended)
     * @param resolver function receives result object. It should set result to wake up the coroutine
     *  the function should return prepared_coro, which is returned from the function.
     * @return Returns prepared_coro instance of kicked out coroutine. If there is no such
     * coroutine, empty is returned. The result also depends on return value of the resolver
     */
    template<std::invocable<result_object> Resolver>
    prepared_coro kick_out(ident id, Resolver &&resolver) {
        std::lock_guard _(_mx);
        prepared_coro out;
        auto iter = std::find_if(_results.begin(), _results.end(), [&](const awaiting_info &x){
            return x.i == id;
        });
        if (iter != _results.end()) {
            if constexpr(std::is_void_v<std::invoke_result_t<Resolver, result_object> >) {
                resolver(std::move(iter->r));
            } else {
                out = resolver(std::move(iter->r));
            }
            auto last = _results.end();
            --last;
            if (iter != last) std::swap(*iter, *last);
            _results.pop_back();
        }
        return out;
    }

    ///kicks out awaiting coroutine sending out an exception
    /**
     * @param id identification
     * @param e exception
     * @return prepared coroutine for resumption or empty, if none
     */
    prepared_coro kick_out(ident id, std::exception_ptr e) {
        return kick_out(id, [e = std::move(e)](result_object obj) mutable {
            return obj.set_exception(std::move(e));
        });
    }

    ///kicks out awaiting coroutine setting result to "no-value"
    /**
     * @param id identification
     * @return prepared coroutine
     */
    prepared_coro kick_out(ident id) {
        return kick_out(id, [](result_object obj) mutable {
            return obj = std::nullopt;
        });
    }

    ///send alert to prevent a coroutine to receive broadcast
    /**
     * @param alert_flag shared flag. This flag is set to true to prevent registration
     * @param id ident of coroutine, it causes that if coroutine is already registered, it
     * is removed
     * @return prepared coro object. It is filled when registered coroutine was removed. You
     * need to resume it (just drop the object) to give coroutine chance to process
     * this situation. The coroutine can check shared flag. 
     * 
     * @note the co_await always throws exception await_canceled_exception.
     */
    prepared_coro alert(std::atomic<bool> &alert_flag,ident id) {
        prepared_coro out;
        std::lock_guard _(_mx);
        alert_flag.store(true, std::memory_order_relaxed);
        auto iter = std::find_if(_results.begin(), _results.end(), [&](const awaiting_info &x){
            return x.i == id;
        });
        if (iter != _results.end()) {
            out = iter->r = std::nullopt;

            auto last = _results.end();
            --last;
            if (iter != last) std::swap(*iter, *last);
            _results.pop_back();   
        }
        return out;

    }


protected:
    Lock _mx;
    std::vector<awaiting_info> _results;
    std::vector<prepared_coro> _ready_to_run;

};


}
