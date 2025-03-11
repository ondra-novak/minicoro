#pragma once

#include "coroutine.h"
#include <vector>
namespace MINICORO_NAMESPACE {


///generator (with asynchronous support)
/**
 * @tparam T type accepted by co_yield and returned from call operator
 * @tparam Param parameter which is passed to the generator with every invocation. If this
 * argument is void, then no parameter is passed. Otherwise you must construct
 * the argument by every invocation. The parameter is then retrieved by the generator
 * as result of operator co_yield. See also start()
 * @tparam Allocator specifies allocator for coroutine frame
 *
 * @note the generator is allowed to use co_await for awaiting on asynchronous operations.
 */
template<typename T, typename Param = void, coro_allocator Allocator = objstdalloc>
class async_generator;

template<typename T, typename Param>
class async_generator<T, Param, objstdalloc> {
public:

    //yield value type
    using value_type = T;

    ///contains promise type of coroutine
    class promise_type {
    public:
        ///promise
        _details::promise_type_base<T> _prom;
        std::optional<voidless_type<Param> > _param;
        bool _started = false;


        ///awaiter for yield
        struct yield_awaiter: std::suspend_always {
            promise_type *me;
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                me =&h.promise();
                auto r =  me->_prom.wakeup();
                me->_prom._target = nullptr;
                return r.symmetric_transfer();
            }
            std::add_rvalue_reference_t<Param> await_resume() noexcept {
                if constexpr(std::is_void_v<Param>) return;
                else return std::move(*me->_param);
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
            _started = true;
            _prom.return_value(std::forward<X>(val));
            return {};
        }
        yield_awaiter yield_value(std::exception_ptr e) {
            _started = true;
            _prom.set_exception(std::move(e));
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
        }
        void unhandled_exception() {
            _prom.set_exception(std::current_exception());
        }
        async_generator get_return_object() {
            return this;
        }

        template<typename ... Args>
        void set_param(Args &&... args) {
            _param.emplace(std::forward<Args>(args)...);
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

        bool did_started() const {return _started;}

    };

    ///construct unitialized generator
    async_generator() = default;

    async_generator(async_generator &&) = default;
    async_generator &operator=(async_generator &&) = default;

    template<typename Alloc>
    async_generator(async_generator<T, Alloc> &&other):_g(std::move(other._g)) {}

    ///call the generator
    /**
     * @return the generator returns awaitable, you can co_await on result, or you can assign result to
     * a variable to perform synchronous wait.
     *
     *
     * @code
     * awaitable<int> gen = finite_generator();
     * for (auto iter = gen(); co_await iter.has_value(); iter = gen()) {
     *      int r = val;    //
     *      //work with r
     * }
     * @endcode
     *
     * @note if the operator is used for not yet started generator, the
     * arguments are ignored. If you need to start generator without passing
     * arguments, use start() method
     */
    template<typename ... Args>
    awaitable<T> operator()(Args && ... args) {
        static_assert(std::is_constructible_v<voidless_type<Param>, Args...>, "Parameter of generator is not constructible from arguments");
        //if generator is not initialized, return no-value
        if (!_g) return nullptr;
        _g->set_param(std::forward<Args>(args)...);
        return [this](auto r)->prepared_coro{
            if (!r) return {};
            return _g->next(std::move(r));
        };
    }

    ///start the generator
    /**
     * This function is useful for generators with the parameter. The very first
     * invocation of the generator causes to execution of initial part of the
     * generator until the first co_yield is reached. This function allows
     * to perfrom intial invocation where the parameter is not used. The
     * function can be called only once.
     * @return if the generator already started, returns no-value avaitable.
     * Otherways it returns pending awaitable which is eventually resolved with
     * a value returned by the first co_yield.
     *
     * @note for non-parameter generator, there is no difference between start()
     * and standard invocation operator. The start function can be called only
     * once at the beginning, standard invocation operator can be called anytime
     * regadless of state
     *
     */
    awaitable<T> start() {
        if (!_g || _g->did_started()) return nullptr;
        return [this](auto r)->prepared_coro{
            if (!r) return {};
            return _g->next(std::move(r));
        };
    }


    ///input iterator - converts generator to iteratable object
    /** Iterator is not supported for generators with parameter */
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

template<typename T, typename Param , coro_allocator Allocator>
class async_generator : public async_generator<T, Param, objstdalloc> {
public:
    using async_generator<T, Param, objstdalloc>::async_generator;

    class promise_type : public async_generator<T, Param, objstdalloc>::promise_type,
                         public Allocator::overrides{
    };
};


template<typename T, typename Param = void, coro_allocator Alloc = objstdalloc>
using generator = async_generator<T, Param, Alloc>;


template<typename T, typename Param, typename Allocator>
async_generator<T, Param, Allocator> generator_agregator(Allocator &, std::vector<generator<T, Param> > g) {
/*
    std::vector<awaitable<T> > awts;
    for (auto &x: g) awts.emplace(x.start());
    when_each_dynamic s;
    unsigned int cnt = 0;
    for (auto &x: awts) s.add(x, cnt++);
    while (cnt) {
        unsigned int n = co_await s;
        awaitable<T> &a = awts[n];
        if (a.has_value()) {
            std::exception_ptr e;
            try {
                co_yield a.await_resume();
                a = g[n]();
                s.add(a,n);
                continue;
            } catch (...) {
                e = std::current_exception();
            }
            co_yield std::move(e);
        }
        --cnt;
    }
    */
}



template<typename T, typename Param>
async_generator<T> generator_agregator(std::vector<generator<T, Param> > g) {
    objstdalloc a;
    return generator_agregator(std::move(g), a);

}

}
