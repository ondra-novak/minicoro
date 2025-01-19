#pragma once

#include "coroutine.h"
namespace MINICORO_NAMESPACE {


template<typename T, typename Allocator = void>
class async_generator {
public:

    using value_type = T;

    class promise_type {
    public:

        promise_type_base<T> _prom;

        struct yield_awaiter: std::suspend_always {
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                promise_type &me =h.promise();
                auto r =  me._prom.wakeup();
                me._prom._target = nullptr;
                return r.symmetric_transfer();
            }
        };

        template<typename X>
        requires(std::is_constructible_v<T, X> || std::is_invocable_r_v<T, X>)
        yield_awaiter yield_value(X &&val) {
            _prom.return_value(std::forward<X>(val));
            return {};
        }
        yield_awaiter final_suspend() noexcept {
            return {};
        }
        std::suspend_always initial_suspend() noexcept {return {};}
        void return_void() {
            //empty
        }
        void unhandled_exception() {
            _prom.set_exception(std::current_exception());
        }
        async_generator get_return_object() {
            return this;
        }
        prepared_coro next(typename awaitable<T>::result r) {
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            if (h.done()) return {};
            _prom._target = r.release();
            return prepared_coro(h);
        }
    };

    async_generator() = default;

    awaitable<T> operator()() {
        return [this](auto r){
            return _g->next(std::move(r));
        };
    }



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
