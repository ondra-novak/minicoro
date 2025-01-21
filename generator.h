#pragma once

#include "coroutine.h"
namespace MINICORO_NAMESPACE {


///generator (with asynchronous support)
/**
 * @tparam T type accepted by co_yield and returned from call operator
 * @tparam Allocator specifies allocator for coroutine frame
 * 
 * @note the generator is allowed to use co_await for awaiting on asynchronous operations. 
 */
template<typename T, typename Allocator = void>
class async_generator {
public:

    //yield value type
    using value_type = T;

    ///contains promise type of coroutine
    class promise_type {
    public:
        ///promise
        _details::promise_type_base<T> _prom;
        

        ///awaiter for yield
        struct yield_awaiter: std::suspend_always {
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                promise_type &me =h.promise();
                auto r =  me._prom.wakeup();
                me._prom._target = nullptr;
                return r.symmetric_transfer();
            }
        };

        ///yield value
        /**
         * @param val a value convertible to T, or a invocable (functor) which returns T. If
         * the invocable is passed, it is called to construct T at place where awaitable is
         * located (RVO is used). 
         * 
         * @return the function returns awaiter which returns void
         */
        template<typename X>
        requires(std::is_constructible_v<T, X> || std::is_invocable_r_v<T, X>)
        yield_awaiter yield_value(X &&val) {
            _prom.return_value(std::forward<X>(val));
            return {};
        }

        ///final suspend - when generator is finished
        yield_awaiter final_suspend() noexcept {
            return {};
        }
        ///always starts suspended
        std::suspend_always initial_suspend() noexcept {return {};}

        ///generator doesn't return value
        void return_void() {
            //empty
        }
        void unhandled_exception() {
            _prom.set_exception(std::current_exception());
        }
        async_generator get_return_object() {
            return this;
        }
        ///resume for next cycle 
        /**
         * @param r variable that receives co_yield value
         * @return holds handle to generator which will be resumed once the return value is destroyed
         */
        prepared_coro next(typename awaitable<T>::result r) {
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            if (h.done()) return {};
            _prom._target = r.release();
            return prepared_coro(h);
        }
    };

    ///construct unitialized generator
    async_generator() = default;

    ///call the generator
    /**
     * @return the generator returns awaitable, you can co_await on result, or you can assign result to 
     * a variable to perform synchronous wait.
     * 
     * @note If the generator fhishes its operation, return value is no-value awaitable. You can test return
     * value by using operator! or operator!! if this is case. 
     * 
     * @code
     * awaitable<int> gen = finite_generator();
     * for (auto iter = gen(); co_await !!iter; iter = gen()) {
     *      int r = val;    //
     *      //work with r
     * }
     * @endcode
     */
    awaitable<T> operator()() {
        //if generator is not initialized, return no-value
        if (!_g) return nullptr;
        return [this](auto r){
            return _g->next(std::move(r));
        };
    }

    ///call the generator indirectly
    /**
     * @param yield_result an awaitable<>::result variable which receives next co_yield value.
     * @return handle of generator's coroutine as prepared_coro, you can discard the return value
     * to resume the generator.
     */
    prepared_coro operator()(awaitable<T>::result yield_result) {
        if (_g) return _g->next(std::move(yield_result));
        return {};
    }

    ///input iterator - converts generator to iteratable object
    class iterator {
    public:

        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using reference = std::add_lvalue_reference_t<value_type>;
        using pointer = std::add_pointer_t<value_type>;

        ///construct iterator set to end position
        iterator() = default;

        ///construct iterator from the generator and fetch first result
        /**
         * @param gen pointer to generator
         */
        iterator(async_generator *gen):_gen(gen) {fetch();}

        ///handle copy
        iterator(const iterator &other):_gen(other._gen),_awt(other._awt.copy_value()) {}

        ///handle assignment
        iterator &operator=(const iterator &other) {
            if (this != &other) {
                _gen = other._gen;
                _awt = other._awt.copy_value();
            }
            return *this;
        }
        ///move
        iterator(iterator &&) = default;
        ///move
        iterator &operator=(iterator &&) = default;

        ///comparison only returns true, if both iterators points to end
        bool operator==(const iterator &other) const {
            return !_awt && !other._awt;
        }
        ///returns current value
        reference operator *() const {
            value_type &&v = _awt.await_resume();
            return v;
        }

        ///returns pointer to current value
        pointer operator->() const {
            value_type &&v = _awt.await_resume();
            return &v;
        }

        ///advance to next item
        iterator &operator++() {
            fetch();
            return *this;
        }

    protected:
        async_generator<T> *_gen = {};
        mutable awaitable<T> _awt = {nullptr};

        void fetch() {
            _awt = (*_gen)();
            _awt.wait();

        }
    };

    ///start iterating
    /**
     * @return iterator
     * @note the function always fetch the first item
     * @note as the iterator is input_iterator, you can only iterate once
     */
    iterator begin() {return this;}
    ///returns end iterator
    iterator end() {return {};}

protected:

    async_generator(promise_type *p):_g(p) {}

    struct deleter {
        void operator()(promise_type *p) {
            std::coroutine_handle<promise_type>::from_promise(*p).destroy();
        }
    };
    std::unique_ptr<promise_type, deleter> _g;


};

template<typename T, typename Alloc = void>
using generator = async_generator<T, Alloc>;


}
