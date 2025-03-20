#pragma once

#include "coroutine.h"
#include "alert_flag.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <vector>
#include <thread>
namespace MINICORO_NAMESPACE {

class stop_source_coro_frame: public coro_frame<stop_source_coro_frame>,
                              public std::stop_source {
public:

protected:
    friend class coro_frame<stop_source_coro_frame>;
    void do_resume() {
        this->request_stop();
    }
};


template<typename T, typename _TP, typename _Ident = const void *>
class generic_scheduler {
public:

    void schedule_at(T x, _TP timestamp, _Ident ident) {
        _heap.push_back({timestamp, std::move(x), ident});
        std::push_heap(_heap.begin(), _heap.end(), compare);
    }

    std::optional<_TP> get_first_scheduled_time() const {
        if (_heap.empty()) return {};
        return _heap.front().timestamp;
    }

    T remove_first() {
        T out;
        if (!_heap.empty()) {
            std::pop_heap(_heap.begin(), _heap.end(), compare);
            out = std::move(_heap.back().res);
            _heap.pop_back();
        }
        return out;
    }
    T remove_by_ident(_Ident ident) {
        T r;
        for (std::size_t i = 0, cnt = _heap.size();i<cnt; ++i) {
            if (_heap[i].ident == ident) {
                r = std::move(_heap[i].res);
                if (i == 0) {
                    remove_first();
                } else if (i == cnt-1) {
                    _heap.pop_back();
                } else {
                    update_heap_element(i, std::move(_heap.back()));
                    _heap.pop_back();
                }
                break;
            }
        }
        return r;
    }

    ///set task time, update its position in the heap
    bool set_time(_Ident ident, _TP new_tp) {
        bool ok = false;
        for (std::size_t i = 0, cnt = _heap.size();i<cnt; ++i) {
            if (_heap[i].ident == ident) {
                auto r=std::move(_heap[i].res);
                update_heap_element(i, {new_tp, std::move(r), ident});
                ok = true;
            }
        }
        return ok;
    }

    bool empty() const {
        return _heap.empty();
    }

protected:

    struct HeapItem {
        _TP timestamp;
        T res;
        _Ident ident;
    };

    static bool compare(const HeapItem &a,const HeapItem &b) {
        return a.timestamp > b.timestamp;
    }

    std::vector<HeapItem> _heap;


    void update_heap_element(std::size_t pos, HeapItem &&new_value) {
        bool shift_up = compare(_heap[pos], new_value);
        _heap[pos] = std::move(new_value);

        if (shift_up) {
            while (pos > 0) {
                size_t parent = (pos - 1) / 2;
                if (compare(_heap[parent], _heap[pos])) {
                    std::swap(_heap[parent], _heap[pos]);
                    pos = parent;
                } else {
                    break;
                }
            }
        }
        else {
            size_t n = _heap.size();
            while (true) {
                size_t left = 2 * pos + 1;
                size_t right = 2 * pos + 2;
                size_t largest = pos;

                if (left < n && compare(_heap[largest], _heap[left])) {
                    largest = left;
                }
                if (right < n && compare(_heap[largest], _heap[right])) {
                    largest = right;
                }
                if (largest != pos) {
                    std::swap(_heap[pos], _heap[largest]);
                    pos = largest;
                } else {
                    break;
                }
            }
        }
    }
};


template<typename _TP = std::chrono::system_clock::time_point>
class manual_scheduler {
public:
    using _Ident = const void *;
    using result_object = awaitable<void>::result;

    awaitable<void> sleep_until(_TP tp, _Ident ident = {}) {
        return [this,tp=std::move(tp),ident](result_object r) mutable {
            _sch.schedule_at(std::move(r), std::move(tp), ident);
        };
    }
    awaitable<void> sleep_until_alertable(alert_flag_type alert_flag, _TP tp) {
        return [this,tp=std::move(tp),&alert_flag](result_object r) mutable -> prepared_coro {
            if (alert_flag) return r();
            _sch.schedule_at(std::move(r), std::move(tp), &alert_flag);
            return prepared_coro();
        };
    }

    template<typename Dur>
    requires(requires(_TP tp, Dur dur){{tp+dur}->std::convertible_to<_TP>;})
    awaitable<void> sleep_for(Dur dur, _Ident ident = {}) {
        return sleep_until(get_current_time() + dur, ident);
    }
    template<typename Dur>
    requires(requires(_TP tp, Dur dur){{tp+dur}->std::convertible_to<_TP>;})
    awaitable<void> sleep_for_alertable(alert_flag_type &alert_flag, Dur dur) {
        return sleep_until_alertable(alert_flag,  get_current_time()+dur);
    }
    ///retrive first scheduled time
    std::optional<std::chrono::system_clock::time_point> get_first_scheduled_time() const {
        return _sch.get_first_scheduled_time();
    }
    ///remove first scheduled coroutine
    /**
     * @return result object of this coroutine, or empty if none
     */
    result_object remove_first() {
        return _sch.remove_first();
    }
    ///remove sleeping coroutine by identity
    /**
     * @param ident identity
     * @return result object of this coroutine, or empty, if not found
     */
    result_object remove_by_ident(_Ident ident) {
        return _sch.remove_by_ident(ident);
    }
    ///cancel sleep
    /** cancels sleep and resolves awaitable with given value
     *
     * @param ident identity
     * @param arg value
     * @return prepared coroutine. If empty, then nothing has been canceled
     */

     template<std::convertible_to<void> Arg>
     prepared_coro cancel(_Ident ident, Arg &&arg) {
         result_object r = remove_by_ident(ident);
         return r(std::forward<Arg>(arg));
     }
     ///cancel sleep with exception
     /** cancels sleep and resolves awaitable with exception
      *
      * @param ident identity
      * @param e exception
      * @return prepared coroutine. If empty, then nothing has been canceled
      */

     prepared_coro cancel(_Ident ident, std::exception_ptr e) {
         result_object r = remove_by_ident(ident);
         return r = e;
     }
     ///cancel sleep with exception
     /** cancels sleep and resolves awaitable with no-value
      *
      * @param ident identity
      * @return prepared coroutine. If empty, then nothing has been canceled
      */
     prepared_coro cancel(_Ident ident) {
         result_object r = remove_by_ident(ident);
         return r = std::nullopt;
     }
     ///send alert to alertable sleeping coroutine and wake it up
     /**
      * @param ident identity of sleeping coroutine
      * @param alert_flag reference to a shared flag, which serves as alert notification. The
      * function sets this flag to true
      *
      * if the coroutine is not sleeping, it will not sleep until the flag is cleared. If the
      * coroutine is sleeping, it is waken up immediately. Note that the coroutine is still
      * executed by the scheduler (in this thread)
      */
     void alert(alert_flag_type &alert_flag) {
         alert_flag.set();
         _sch.set_time(&alert_flag, std::chrono::system_clock::now());
     }

     ///retrieves current time
     /**
      * @return current simmulation time
      */
     _TP get_current_time() const {return _current_time;}

     ///advances time until givne time point
     /** If there is scheduled coroutine, it removes it and returns it
      * to be resumed. The current time is set to time where coroutine was scheduled
      * If there is no coroutine before given target_time, returns empty object
      */
     prepared_coro advance_time_until(_TP target_time) {
        _TP n = _sch.get_first_scheduled_time();
        if (!n || *n>target_time) return {};
        _current_time = std::max(target_time,n);
        result_object r = _sch.remove_first();
        return r();
     }

protected:
    _TP _current_time = {};
    generic_scheduler<result_object, std::chrono::system_clock::time_point,_Ident> _sch;
};

class scheduler {
public:

    using _Ident = const void *;
    using result_object = typename awaitable<void>::result;

    ///sleep until given time
    /**
     * @param tp time point
     * @param ident optional identity
     * @return awaitable, coroutine must co_await to perform sleep
     */
    awaitable<void> sleep_until(std::chrono::system_clock::time_point tp, _Ident ident = {}) {
        return [this, tp, ident = std::move(ident)](result_object r) mutable {
            std::lock_guard _(_mx);
            auto n = _sch.get_first_scheduled_time();
            if (!n || tp < *n) _cv.notify_all();
            _sch.schedule_at(std::move(r),std::move(tp),std::move(ident));
        };
    }

    ///sleep coroutine but enable alert() feature
    /**
     * @param alert_flag a reference to flag which must be set to true for alert. If this flag is
     * set, the sleep immediately returns, because there is an active alert. If this flag is false,
     * sleep_until function performs sleep as normal
     * @param tp time point when sleep is waken up
     * @return awaitable object
     *
     * @see alert
     */
    awaitable<void> sleep_until_alertable(alert_flag_type &alert_flag, std::chrono::system_clock::time_point tp) {
        return [this, tp, &alert_flag](result_object r) mutable -> prepared_coro {
            std::lock_guard _(_mx);
            if (alert_flag) {
                return r();
            }
            if (tp < _sch.get_first_scheduled_time()) _cv.notify_all();
            _sch.schedule_at(std::move(r),std::move(tp),&alert_flag);
            return prepared_coro{};
        };
    }

    ///sleep for given time
    /**
     * @param dur duration
     * @param ident optional identity
     * @return
     */
    template<typename A, typename B>
    awaitable<void> sleep_for(std::chrono::duration<A,B> dur, _Ident ident = {}) {
        return sleep_until(std::chrono::system_clock::now()+dur, std::move(ident));
    }

    ///sleep coroutine but enable alert() feature
    /**
     * @param alert_flag a reference to flag which must be set to true for alert. If this flag is
     * set, the sleep immediately returns, because there is an active alert. If this flag is false,
     * sleep_until function performs sleep as normal
     * @param duration time duration
     * @param ident identity of the coroutine. This field is mandatory (otherwise alert() can't work). This
     * value must be unique.
     * @return awaitable object
     *
     * @see alert
     */
    template<typename A, typename B>
    awaitable<void> sleep_for_alertable(alert_flag_type &alert_flag, std::chrono::duration<A,B> dur) {
        return sleep_until_alertable(alert_flag,  std::chrono::system_clock::now()+dur);
    }

    ///retrive first scheduled time
    std::optional<std::chrono::system_clock::time_point> get_first_scheduled_time() const {
        std::lock_guard _(_mx);
        return _sch.get_first_scheduled_time();
    }
    ///remove first scheduled coroutine
    /**
     * @return result object of this coroutine, or empty if none
     */
    result_object remove_first() {
        std::lock_guard _(_mx);
        return _sch.remove_first();
    }
    ///remove sleeping coroutine by identity
    /**
     * @param ident identity
     * @return result object of this coroutine, or empty, if not found
     */
    result_object remove_by_ident(_Ident ident) {
        std::lock_guard _(_mx);
        return _sch.remove_by_ident(ident);
    }
    ///run scheduler's thread
    /**
     * @param executor executor - function which resolved result object and executes it
     * @param tkn stop token, signal this token to stop operation
     */
    template<std::invocable<result_object> Executor>
    void run_thread(Executor &&executor, std::stop_token tkn) {
        std::unique_lock lk(_mx);
        std::stop_callback __(tkn,[this]{
            _cv.notify_all();
        });
        while (!tkn.stop_requested()) {
            auto tm = _sch.get_first_scheduled_time();
            if (tm) {
                auto now =std::chrono::system_clock::now();
                if (now > *tm) {
                    auto r = _sch.remove_first();
                    lk.unlock();
                    executor(r);
                    lk.lock();
                } else{
                    _cv.wait_until(lk, *tm);
                }
            } else {
                _cv.wait(lk);
            }
        }
    }
    ///run scheduler's thread, execute scheduled coroutines in this thread
    /**
     * @param tkn stop token to stop thread
     */
    void run_thread(std::stop_token tkn) {
        run_thread([](auto &&x){x();}, std::move(tkn));
    }

    ///run scheduler while awaiting for given awaiter
    /**
     * @param awt coroutine's compatibile awaiter (must have await_ready, await_suspend and await_resume)
     * @return value returned by await_resume()
     *
     * function similar as co_await, however runs scheduler while awaiting
     */
    template<is_awaiter Awt>
    auto await(Awt &&awt) {

        if (!awt.await_ready()) {
            stop_source_coro_frame stpsrc;
            call_await_suspend(awt, stpsrc.get_handle());
            run_thread(stpsrc.get_token());
        }
        return awt.await_resume();
    }
    template<has_co_await Awt>
    auto await(Awt &&awt) {
        return await(awt.operator co_await());
    }
    template<has_global_co_await Awt>
    auto await(Awt &&awt) {
        return await(operator co_await(awt));
    }


    ///create thread and run scheduler
    /**
     * @param executor executor (see run_thread)
     * @return running thread. Ensure that you destroy thread before destuction of scheduler
     */
    template<std::invocable<result_object> Executor>
    std::jthread create_thread(Executor executor) {
        return std::jthread([this,executor = std::move(executor)]
                             (std::stop_token tkn)mutable{
            run_thread(std::move(executor), std::move(tkn));
        });
    }
    ///create thread and run scheduler
    /**
     * @return running thread. Ensure that you destroy thread before destuction of scheduler
     */
    std::jthread create_thread() {
        return std::jthread([this](std::stop_token tkn)mutable{
            run_thread(std::move(tkn));
        });
    }

    ///cancel sleep
    /** cancels sleep and resolves awaitable with given value
     *
     * @param ident identity
     * @param arg value
     * @return prepared coroutine. If empty, then nothing has been canceled
     */

    template<std::convertible_to<void> Arg>
    prepared_coro cancel(_Ident ident, Arg &&arg) {
        result_object r = remove_by_ident(ident);
        return r(std::forward<Arg>(arg));
    }
    ///cancel sleep with exception
    /** cancels sleep and resolves awaitable with exception
     *
     * @param ident identity
     * @param e exception
     * @return prepared coroutine. If empty, then nothing has been canceled
     */

    prepared_coro cancel(_Ident ident, std::exception_ptr e) {
        result_object r = remove_by_ident(ident);
        return r = e;
    }
    ///cancel sleep with exception
    /** cancels sleep and resolves awaitable with no-value
     *
     * @param ident identity
     * @return prepared coroutine. If empty, then nothing has been canceled
     */
    prepared_coro cancel(_Ident ident) {
        result_object r = remove_by_ident(ident);
        return r = std::nullopt;
    }
    ///send alert to alertable sleeping coroutine and wake it up
    /**
     * @param ident identity of sleeping coroutine
     * @param alert_flag reference to a shared flag, which serves as alert notification. The
     * function sets this flag to true
     *
     * if the coroutine is not sleeping, it will not sleep until the flag is cleared. If the
     * coroutine is sleeping, it is waken up immediately. Note that the coroutine is still
     * executed by the scheduler (in this thread)
     */
    void alert(alert_flag_type &alert_flag) {
        std::lock_guard _(_mx);
        alert_flag.set();
        _sch.set_time(&alert_flag, std::chrono::system_clock::now());
        _cv.notify_all();
    }

protected:
    mutable std::mutex _mx;
    std::condition_variable _cv;
    generic_scheduler<result_object, std::chrono::system_clock::time_point,_Ident> _sch;
};


}
