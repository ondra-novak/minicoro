/*
Copyright (c) 2025 Ondrej Novak

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

*/

/**
 * @file coroutine.h bare minimum for coroutines
 *
 * Several classes defined here
 *
 * coroutine<T, Alloc=void> - allows to write coroutine
 *
 * @code
 * coroutine<int> my_first_coroutine() {
 *  co_return 42;
 * }
 * @endcode
 *
 * You can start coroutine by simple calling it. However you
 * can co_await coroutine from different coroutine
 *
 * awaitable<T> - generic object to exchange return value
 * asynchronously. You can co_await on result
 *
 * @code
 * awaitable<int> call_coroutine() {
 *      return my_first_coroutine()
 * }
 * @endcode
 *
 * Awaitable object can do more. You can associate it with a
 * callback function
 *
 * @code
 * awaitable<int> do_async_stuff() {
 *      return [](auto r) {
 *          r = 42;
 *      };
 * }
 * @endcode
 *
 * The main benefit is you can move 'r' (result) around as you wish.
 * You can assign final value outside scope. Until the result
 * is assigned, the awaiting coroutine must wait.
 *
 * the result (awaitable_result) can be also used as callback function
 *
 * r(42) - is same
 *
 * If you need to access awaitable outside of coroutine, you can:
 *
 * - read return value synchronously
 *
 * @code
 * int my_result = do_async_stuff();
 * @endcode
 *
 * Or you can attach a callbacl
 *
 * @code
 * do_async_stuff() >> [](auto &res) {
 *      int my_result = res;
 * }
 * @endcode
 *
 * the callback function can be called asynchronously outside of
 * active scope
 *
 *
 */


#pragma once
#include <coroutine>
#include <stdexcept>
#include <memory>
#include <utility>
#include <deque>
#include <optional>
#include <span>

#ifndef MINICORO_NAMESPACE
#define MINICORO_NAMESPACE minicoro
#endif


namespace MINICORO_NAMESPACE {


///definition of allocator interface
template<typename T>
concept coro_allocator = (requires(T &val, void *ptr, std::size_t sz, float b, char c) {
    ///static function alloc which should accept multiple arguments where one argument can be reference to its instance - returns void *
    /** the first argument must be size */
    {T::alloc(sz, val, b, c, ptr)} -> std::same_as<void *>;
    ///static function dealloc which should accept pointer and size of allocated space
    {T::dealloc(ptr, sz)};
} || std::is_void_v<T>);  //void can be specified in meaning of default allocator



template<typename T, typename Obj, typename Fn>
concept is_member_fn_call_for_result = requires(T val, Obj obj, Fn fn) {
    {((*obj).*fn)(std::move(val))};
};


template<typename T> class awaitable;
template<typename T, std::invocable<awaitable<T> &> _CB, typename Allocator = void> class awaiting_callback;
template<typename T, coro_allocator _Allocator = void> class coroutine;
template<typename T> class coro_frame;
template<typename T> class awaitable_result;


///await or co_await function has been canceled
/**
 * The cancelation can be due several reasons. For example, the coroutine has
 * been destroyed, result has not been delivered or attempt to await
 * on non-coroutine object
 *
 */
class canceled_exception: public std::exception {
public:
    virtual const char *what() const noexcept override {return "canceled exception";}
};

///object is in invalid state
class invalid_state: public std::exception {
public:
    virtual const char *what() const noexcept override {return "invalid state";}
};

///contains prepared coroutine.
/** If object is destroyed, coroutine is resumed. You can resume
 * coroutine manually
 */
class prepared_coro {
public:

    ///construct empty
    prepared_coro() = default;
    ///construct by handle
    prepared_coro(std::coroutine_handle<> h):_coro(h.address()) {}

    ///test if empty
    explicit operator bool() const {return static_cast<bool>(_coro);}

    ///resume
    void resume(){
        if (*this) std::coroutine_handle<>::from_address(_coro.release()).resume();
    }
    ///resume
    void operator()() {
        if (*this) std::coroutine_handle<>::from_address(_coro.release()).resume();
    }
    ///destroy coroutine
    void destroy(){
        if (*this) std::coroutine_handle<>::from_address(_coro.release()).destroy();
    }
    ///release handle and return it for symmetric transfer
    std::coroutine_handle<> symmetric_transfer(){
        if (!_coro) return std::noop_coroutine();
        return std::coroutine_handle<>::from_address(_coro.release());
    }

protected:
    struct deleter{
        void operator()(void *ptr) {
            std::coroutine_handle<>::from_address(ptr).resume();
        }
    };

    std::unique_ptr<void,deleter> _coro;
};


///minimum size required for awaiting_callback working with given T
/**
 * @tparam T type of awaitable passed to the callback (awaitable<T>)
 * @tparam _CB type of callback class/function/functor.
 * @return the constant cointains size of the callback frame in bytes. This value is available during compile time
 */
template<typename T, std::invocable<awaitable<T> &> _CB>
constexpr std::size_t awaiting_callback_size = sizeof(awaiting_callback<T, _CB, void>);

namespace _details {

///coroutine promise base - helper object
template<typename T>
class promise_type_base {
public:

    awaitable<T> *_target = {};

    template<typename X>
    void return_value(X &&v) {
        if (_target) _target->set_value(std::forward<X>(v));
    }
    void set_exception(std::exception_ptr e) {
        if (_target) _target->set_exception(std::move(e));
    }
    prepared_coro wakeup() {
        if (_target) return _target->wakeup();
        return {};
    }

};

}

///pointer to function, which is called when unhandled exception is caused by a coroutine (or similar function)
/**
 * Called when unhandled_exception() function cannot pass exception object to the result. This can 
 * happen when coroutine is started in detached mode and throws an exception.
 * 
 * You can change function and implement own version. Returning from the function ignores any futher
 * processing of the exception, so it is valid code to store or log the exception and return to 
 * resume normal execution.
 */
inline void (*async_unhandled_exception)() = []{std::terminate();};

///helper class modifying new and delete of the object to use minicoro's specific allocator
template<coro_allocator _Allocator>
class object_allocated_by_allocator {
public:
    void operator delete(void *ptr, std::size_t sz) {
        _Allocator::dealloc(ptr, sz);
    }
    template<typename ... Args>
    void *operator new(std::size_t sz, Args  && ... args) {
        void *ptr = _Allocator::alloc(sz, std::forward<Args>(args)... );
        return ptr;
    }
    template<typename ... Args>
    void operator delete(void *ptr, Args  && ... args) {
        if constexpr(sizeof...(Args) == 1) {
            (_Allocator::dealloc(ptr, args),...);
        } else {
            throw std::logic_error("unreachable");
        }
    }
};


template<>
class object_allocated_by_allocator<void> {};

namespace _details {
    template<int n>
    class alloc_in_buffer {
    public:
        alloc_in_buffer(char *buff):_buff(buff) {}
        template<typename ... Args>
        static void *alloc(std::size_t sz, alloc_in_buffer &inst, Args && ... ) {
            if (sz > n) throw std::bad_alloc();
            return inst._buff;
        }
        static void dealloc(auto, auto) {}
    protected:
        char *_buff;
    };
}

///construct coroutine
/**
 * @tparam T result type
 * @tparam _Allocator allocator. If it is void, it uses standard allocator (malloc)
 *
 * The coroutine can return anything which is convertible to T. It can
 * also return a function (lambda function) which returns T - this can achieve
 * RVO.
 *
 * @code
 * co_return [&]{ return 42;}
 * @endcode
 *
 * The returned value from the lambda function is returned with respect to RVO,
 * so the returned object can be immovable, and still can be returned from
 * the coroutine
 *
 * If the object is initialized and not awaited, the coroutine is
 * started in detached state. You need to call destroy() if you need
 * to destroy already initialized coroutine.
 *
 * You can destroy the coroutine anytime when you have a handle. In this
 * case, the awaiting coroutine receives exception canceled_exception
 */
template<typename T>
class coroutine<T, void>{
public:

    class promise_type: public _details::promise_type_base<T> {
    public:

        struct finisher {
            promise_type *me;
            constexpr bool await_ready() const noexcept {
                return !me->_target;
            }
            #if _MSC_VER && defined(_DEBUG) 
            //BUG in MSVC - in debug mode, symmetric transfer cannot be used
            //because return value of await_suspend is located in destroyed
            //coroutine context. This is not issue for release
            //build as the return value is stored in register
            constexpr void await_suspend(std::coroutine_handle<> h) noexcept {
                auto p = me->wakeup();
                h.destroy();
                p.resume();
            }
            #else
            constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
                auto p = me->wakeup();
                h.destroy();
                return p.symmetric_transfer();
            }
            #endif
            static constexpr void await_resume() noexcept {}

        };

        promise_type() = default;
        ~promise_type() {
            this->wakeup();
        }

        static constexpr std::suspend_always initial_suspend() noexcept {return {};}
        constexpr finisher final_suspend() noexcept {return {this};}
        void unhandled_exception() {
            if (this->_target) {
                this->set_exception(std::current_exception());
            } else {
                async_unhandled_exception();
            }
        }
        coroutine get_return_object()  {
            return this;
        }
    };

    ///construct empty object
    coroutine()  = default;

    template<typename Alloc>
    coroutine(coroutine<T, Alloc> &&other):_coro(other._coro) {other._coro = nullptr;}
    ///move object
    coroutine(coroutine &&other) :_coro(other._coro) {other._coro = nullptr;}
    ///move object
    coroutine &operator=(coroutine &&other) {
        if (this != &other) {
            std::destroy_at(this);
            std::construct_at(this, std::move(other));
        }
        return *this;
    }
    ///destroy object
    /**
     * When object is destroyed with the coroutine, the coroutine is started.
     * If the coroutine is suspended, it continues in detached mode.
     *
     */
    ~coroutine()  {
        if (_coro) {
            release().resume();
        }
    }

    ///start coroutine - set result object
    /**
     * @param res result object will be used to put result there. If
     * the result object is not initialized, the coroutine is started in
     * detached mode
     * @return prepared coroutine object. If result is ignored, the coroutine
     * is started immediately. However, you can store the result to
     * perform symmetric transfer for example
     */
    prepared_coro start(awaitable_result<T> &&res) {
        auto c = std::exchange(_coro, nullptr);
        if (c) {
            c->_target = res.release();
            return prepared_coro(std::coroutine_handle<promise_type>::from_promise(*c));
        }
        return {};
    }

    awaitable<T> operator co_await() {
        return std::move(*this);
    }

    ///await synchronously on result
    T await();

    ///retrieve result synchronously (conversion to result)
    operator decltype(auto)() {
        return await();
    }

    ///destroy initialized coroutine
    /**
     * By default, if coroutine object leaves scope, the coroutine
     * is resumed in detached mode. If you need to prevent this, you
     * need to explicitly call destroy().
     */
    void destroy() {
        if (_coro) {
            release().destroy();
        }
    }

    ///Release coroutine from the object, you get its handle for any usage
    auto release() {
        return std::coroutine_handle<promise_type>::from_promise(*std::exchange(_coro, nullptr));
    }

protected:

    promise_type *_coro = nullptr;

    coroutine(promise_type *pt):_coro(pt) {}

    std::coroutine_handle<promise_type> get_handle() const {
        return std::coroutine_handle<promise_type>::from_promise(*_coro);
    }
};


///Declaration of basic coroutine with allocator
/**
 * @tparam T type of result
 * @tparam _Allocator allocator. The allocator must define two static
 * function alloc(sz,...) and dealloc(ptr, sz). The alloc function receives
 * size and all arguments passed to the coroutine. It can pickup a
 * self instance from arguments. If it needs instance for deallocation, it
 * must store pointer to self within allocated block
 */
template<typename T, coro_allocator _Allocator>
class coroutine: public coroutine<T, void> {
public:

    using coroutine<T, void>::coroutine;

    class promise_type: public coroutine<T, void>::promise_type,  object_allocated_by_allocator<_Allocator> {
    public:
        coroutine<T, _Allocator> get_return_object() {
          return this;
        }

        using object_allocated_by_allocator<_Allocator>::operator new;
        using object_allocated_by_allocator<_Allocator>::operator delete;

    };
    coroutine(promise_type *p):coroutine<T, void>(p) {}
};

///Awatable object. Indicates asynchronous result
/**
 * To access value
 *
 * @code
 * co_await obj
 * obj >> callback(result)
 * type result = obj
 * @endcode
 *
 * To set value
 *
 * @code
 * return coroutine(args)
 * return [=](awaitable_result r) {
 *         //do async stuff
 *         r = result;
 *         // or r(result)
 *  };
 * @endcode
 *
 * @tparam T type of return value - can be void
 */
template<typename T>
class awaitable {
public:
    ///contains actually stored value type. It is T unless for void, it is bool
    using store_type = std::conditional_t<std::is_void_v<T>, bool, T>;
    ///contains alias for result object
    using result = awaitable_result<T>;
    ///allows to use awaitable to write coroutines
    using promise_type = coroutine<T>::promise_type;

    ///construct with no value
    awaitable(std::nullptr_t) {};
    ///dtor
    ~awaitable() {
        dtor();
    }
    ///construct containing result constructed by default constructor
    /**
     * @note if the result cannot be constructed by default constructor,
     * it is initialized with no value
     */
    awaitable() {
        if constexpr(std::is_default_constructible_v<store_type>) {
            std::construct_at(&_value);
            _state = value;
        } else {
            _state = no_value;
        }
    }

    ///construct containing result constructed by arguments
    template<typename ... Args>
    requires (std::is_constructible_v<store_type, Args...> && (!std::is_same_v<std::remove_reference_t<Args>, awaitable> && ...))
    awaitable(Args &&... args)
        :_state(value),_value(std::forward<Args>(args)...) {}

    ///construct by coroutine awaitable for its completion
    awaitable(coroutine<T, void> coroutine):_state(coro),_coro(std::move(coroutine)) {}

    ///construct containing result constructed by arguments
    template<typename ... Args>
    requires (std::is_constructible_v<store_type, Args...>)
    awaitable(std::in_place_t, Args &&... args)
        :_state(value),_value(std::forward<Args>(args)...) {}

    ///construct unresolved containing function which is after suspension of the awaiting coroutine
    template<std::invocable<result> Fn>
    awaitable(Fn &&fn) {
        if constexpr(sizeof(CallbackImpl<Fn>) <= callback_max_size) {
            new (_callback_space) CallbackImpl<Fn>(std::forward<Fn>(fn));
            _state = callback;
        } else {
            std::construct_at(&_callback_ptr, std::make_unique<CallbackImpl<Fn> >(std::forward<Fn>(fn)));
            _state = callback_ptr;
        }
    }

    ///construct containing result - in exception state
    awaitable(std::exception_ptr e):_state(exception),_exception(std::move(e)) {}

    ///construct unresolved containing member function which is after suspension of the awaiting coroutine
    /**
     * @param ptr a pointer or any object which defines dereference operator (*) which
     * returns reference to an instance of a class
     * @param fn pointer to member function, which is called on the instance of the class
     */
    template<typename ObjPtr, typename Fn>
    requires(is_member_fn_call_for_result<result, ObjPtr, Fn>)
    awaitable(ObjPtr ptr, Fn fn):awaitable([ptr, fn](result res){
        ((*ptr).*fn)(std::move(res));
    }) {}


    ///awaitable can be moved
    awaitable(awaitable &&other):_state(other._state) {
        switch (_state) {
            default: break;
            case value: std::construct_at(&_value, std::move(other._value));break;
            case exception: std::construct_at(&_exception, std::move(other._exception));break;
            case coro: std::construct_at(&_coro, std::move(other._coro));break;
            case callback: other.get_local_callback()->move_to(_callback_space);break;
            case callback_ptr: std::construct_at(&_callback_ptr, std::move(other._callback_ptr));break;
        }
        other.destroy_state();
        other._state = no_value;
    }

    ///awaitable can be assigned by move
    awaitable &operator=(awaitable &&other) {
        if (this != &other) {
            std::destroy_at(this);
            std::construct_at(this, std::move(other));
        }
        return *this;
    }

    template<bool b>
    class pending_awaiter {
    public:
        pending_awaiter(awaitable &owner):owner(owner) {}
        bool await_ready() const {return owner.await_ready();}
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
            return owner.await_suspend(h);
        }
        bool await_resume() const {
            return (owner._state == no_value) == b;
        }
        explicit operator bool() const {
            owner.wait();
            return await_resume();
        }
        pending_awaiter<!b> operator!() const {return owner;}

    protected:
        awaitable &owner;
    };

    ///returns true if the awaitable is no_value state
    /** If awaitable is still pending, function blocks. However you can co_await it */
    pending_awaiter<true> operator!() noexcept {
        return *this;
    }

    ///return true, if the awaitable is resolved
    bool await_ready() const noexcept {
        return _state == no_value || _state == value || _state == exception;
    }

    ///returns true if the awaitable is resolved
    bool is_ready() const {
        return await_ready();
    }

    ///returns value of resolved awaitable
    std::add_rvalue_reference_t<T> await_resume() {
        if (_state == value) {
            if constexpr(std::is_void_v<T>) {
                return;
            }
            else {
                return std::move(_value);
            }
        } else if (_state == exception) {
            std::rethrow_exception(_exception);
        }
        throw canceled_exception();
    }

    ///handles suspension
    /**
     * @param h coroutine currently suspended
     * @return coroutine being resumed
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
        _owner = h;
        if (_state == coro) {
            return _coro.start(result(this)).symmetric_transfer();
        } else if (_state == callback) {
            return get_local_callback()->call(result(this)).symmetric_transfer();
        } else if (_state == callback_ptr) {
            auto cb = std::move(_callback_ptr);
            return cb->call(result(this)).symmetric_transfer();
        } else {
            return h;
        }
    }

    ///set callback, which is called once the awaitable is resolved
    /**
     * @param cb callback function. The function receives reference
     * to awaitable in resolved state
     * @return prepared coroutine (if there is an involved one), you
     * can postpone its resumption by storing result and release it
     * later
     * 
     * @note you can set only one callback or coroutine
     */
    template<std::invocable<awaitable &> _Callback>
    prepared_coro operator >> (_Callback &&cb) {
        return set_callback_internal(std::forward<_Callback>(cb));
    }

    ///set callback, which is called once the awaitable is resolved
    /**
     * @param cb callback function. The function receives reference
     * to awaitable in resolved state
     * @return prepared coroutine (if there is an involved one), you
     * can postpone its resumption by storing result and release it
     * later
     * 
     * @note you can set only one callback or coroutine
     */
    template<std::invocable<awaitable &> _Callback>
    prepared_coro set_callback (_Callback &&cb) {
        return set_callback_internal(std::forward<_Callback>(cb));
    }

    ///set callback, which is called once the awaitable is resolved
    /**
     * @param cb callback function. The function receives reference
     * to awaitable in resolved state
     * @param a allocator instance, allows to allocate callback 
     * instance using this allocator
     * @return prepared coroutine (if there is an involved one), you
     * can postpone its resumption by storing result and release it
     * later
     * 
     * @note you can set only one callback or coroutine
     */
    template<std::invocable<awaitable &> _Callback, coro_allocator _Allocator>
    prepared_coro set_callback (_Callback &&cb, _Allocator &a) {
        return set_callback_internal(std::forward<_Callback>(cb), a);
    }

    ///set callback, which is called once the awaitable is resolved
    /**
     * @param cb callback function. The function receives reference
     * to awaitable in resolved state
     * @param buffer reference to a buffer space where callback will be
     * allocated. The buffer must be large enough to fit the callback. 
     * You can use template awaiting_callback_size to calculate minimum 
     * space required for the callback. This number is available during
     * compile time
     * 
     * @return prepared coroutine (if there is an involved one), you
     * can postpone its resumption by storing result and release it
     * later
     * 
     * @note you can set only one callback or coroutine
     */
    template<std::invocable<awaitable &> _Callback, int n>
    prepared_coro set_callback (_Callback &&cb, char (&buffer)[n]) {
            static_assert(awaiting_callback_size<T, _Callback> <= n, "Callback doesn't fit to buffer");
            _details::alloc_in_buffer<n> a(buffer);
            return set_callback_internal(std::forward<_Callback>(cb), a);
    }

    ///synchronous await
    decltype(auto) await() {
        return wait().await_resume();
    }

    ///synchronous await
    operator store_type&&() {
        return wait().await_resume();
    }

    ///evaluate asynchronous operation, waiting for result synchronously
    /**
     * Function blocks execution until the asynchronous operation is complete
     * @retval reference to this object, so you can chain additional operation
     */
    awaitable &&wait();


    ///copy evaluated awaitable object.
    /**
     * If the object has value or exception, returned object contains copy
     * of the value or exception. Otherwise default constructed object is
     * returned
     *
     * @return evaluated awaitable
     */
    awaitable copy_value() const  {
        switch (_state) {
            default: return {};
            case value:return awaitable(std::in_place, _value);
            case exception:return awaitable(_exception);
        }
    }

    bool is_pending() const {
        return _owner != std::coroutine_handle<>();
    }


protected:

    enum State {
        ///awaitable is resolved with no value
        no_value,
        ///awaitable is resolved with a value
        value,
        ///awaitable is resolved with exception
        exception,
        ///awaitable is not resolved, a coroutine is ready to generate result once awaited
        coro,
        ///awaitable is not resolved, locally constructed callback is ready to generate result once awaited
        callback,
        ///awaitable is not resolved, dynamically constructed callback is ready to generate result once awaited
        callback_ptr
    };

    ///virtual interface to execute callback for resolution
    class ICallback {
    public:
        virtual ~ICallback() = default;
        ///start resolution, call the callback
        virtual prepared_coro call(result) = 0;
        ///move support
        virtual void move_to(void *address) = 0;
    };



    template<std::invocable<result> Fn>
    class CallbackImpl: public ICallback {
    public:
        CallbackImpl(Fn &&fn):_fn(std::forward<Fn>(fn)) {}
        virtual prepared_coro call(result r) {
            if constexpr(std::convertible_to<std::invoke_result_t<Fn, result>, prepared_coro>) {
                return prepared_coro(_fn(std::move(r)));
            } else {
                _fn(std::move(r));
                return {};
            }
        }
        virtual void move_to(void *address) {
            new(address) CallbackImpl(std::move(_fn));
        }


    protected:
        Fn _fn;
    };

    static constexpr auto callback_max_size = std::max(sizeof(void *) * 4, sizeof(store_type));

    ///current state of object
    State _state = no_value;
    ///handle of owning coroutine. If not set, no coroutine owns, nobody awaiting
    /**@note the handle must not be destroyed in destructor. The awaitable instance
     * is always inside of coroutine's frame, so it will be only destroyed with the owner
     */
    std::coroutine_handle<> _owner;
    union {
        ///holds current value
        store_type _value;
        ///holds current exception
        std::exception_ptr _exception;
        ///holds coroutine registration (to start coroutine when awaited)
        coroutine<T> _coro;
        ///holds pointer to virtual interface of callback
        std::unique_ptr<ICallback> _callback_ptr;
        ///holds reserved space for local callback
        /**@see get_local_callback() */
        char _callback_space[callback_max_size];

    };

    ///retrieves pointer to local callback (instance in _callback_space)
    /**
     * @return pointer to instance
     * @note pointer is only valid when _state == callback
     */
    ICallback *get_local_callback() {
        return reinterpret_cast<ICallback *>(_callback_space);
    }

    void dtor() {
        if (is_pending()) throw invalid_state();
        destroy_state();
    }

    void destroy_state() {
        switch (_state) {
            default:break;
            case value: std::destroy_at(&_value);break;
            case exception: std::destroy_at(&_exception);break;
            case coro: _coro.destroy();std::destroy_at(&_coro);break;
            case callback: std::destroy_at(get_local_callback());break;
            case callback_ptr: std::destroy_at(&_callback_ptr);break;
        }
    }

    template<typename ... Args>
    void set_value(Args && ... args) {
        destroy_state();
        try {
            _state = value;
            if constexpr (sizeof...(Args) == 1 && (std::is_invocable_r_v<store_type, Args>  && ...)) {
                new(&_value) store_type((...,args()));
            } else {
                new(&_value) store_type(std::forward<Args>(args)...);
            }
        } catch (...) {
            _state = exception;
            std::construct_at(&_exception, std::current_exception());
        }
    }

    void set_exception(std::exception_ptr e) {
        destroy_state();
        _state = exception;
        std::construct_at(&_exception, std::move(e));
    }

    void drop() {
        destroy_state();
        _state = no_value;
    }

    prepared_coro wakeup() {
        if (_state != no_value && _state != value && _state != exception) drop();
        return prepared_coro(std::exchange(_owner, {}));
    }

    template<typename _Callback, typename ... _Allocator>
    requires(sizeof...(_Allocator)<2)
    prepared_coro set_callback_internal(_Callback &&cb, _Allocator & ... a);


    friend class awaitable_result<T>;
    friend class _details::promise_type_base<T>;
};


template<typename T>
class awaitable_result {
public:

    ///construct uninitialized
    /**
     * Uninitialized object can be used, however it will not invoke any
     * action and set result or exception is ignored
     */
    awaitable_result() = default;
    ///construct result using pointer to awaitable (which contains space to result)
    awaitable_result(awaitable<T> *ptr):_ptr(ptr) {}
    ///you can move
    awaitable_result(awaitable_result &&other) = default;
    ///you can move
    awaitable_result &operator=(awaitable_result &&other) = default;

    using const_reference = std::add_lvalue_reference_t<std::add_const_t<std::conditional_t<std::is_void_v<T>, bool, T> > >;
    using rvalue_reference = std::add_rvalue_reference_t<std::conditional_t<std::is_void_v<T>, bool, T> > ;
    ///set the result
    /**
     * @param val value to set
     * @return prepared coroutine. If the result is discarded, coroutine
     * is resumed immediately. You can store result and destroy it
     * later to postpone resumption.
     */
    prepared_coro operator=(const_reference val) {
        auto p = _ptr.release();
        if (p) {
            p->set_value(val);
            return p->wakeup();
        } else {
            return {};
        }
    }

    ///set the result
    /**
     * @param val value to set
     * @return prepared coroutine. If the result is discarded, coroutine
     * is resumed immediately. You can store result and destroy it
     * later to postpone resumption.
     */
    prepared_coro operator=(rvalue_reference val) {
        auto p = _ptr.release();
        if (p) {
            p->set_value(std::move(val));
            return p->wakeup();
        } else {
            return {};
        }
    }

    ///assign exception to result
    /**
     * @param e exception ptr
     * @return prepared coroutine. If the result is discarded, coroutine
     * is resumed immediately. You can store result and destroy it
     * later to postpone resumption.
     */
    prepared_coro operator=(std::exception_ptr e) {
        auto p = _ptr.release();
        if (p) {
            p->set_exception(std::move(e));
            return p->wakeup();
        } else {
            return {};
        }

    }

    ///set result by calling its constructor
    /**
     * @param args arguments required to construct the result
     * @return prepared coroutine. If the result is discarded, coroutine
     * is resumed immediately. You can store result and destroy it
     * later to postpone resumption.
     */
    template<typename ... Args>
    prepared_coro operator()(Args && ... args) {
        auto p = _ptr.release();
        if (p) {
            p->set_value(std::forward<Args>(args)...);
            return p->wakeup();
        } else {
            return {};
        }
    }

    ///drop the result, cancel the awaiting operation
    /**
     * the awaiting coroutine receives exception canceled_exception()
     */
    prepared_coro drop() {
        auto p = _ptr.release();
        if (p) {
            p->drop();
            return p->wakeup();
        } else {
            return {};
        }
    }

    ///Release internal pointer to be used elswhere
    /**
     * This is for special purpose, if you need carry the pointer to
     * result by other way, for example you need to cast it as uintptr_t.
     * Remember that you need it convert back later and initialize
     * awaitable_result with it to restore its purpose
     *
     * @return internal pointer.
     *
     * @note the object itself becomes unintialized
     */
    awaitable<T> *release() {
        return _ptr.release();
    }

protected:
    struct deleter {
        void operator()(awaitable<T> *ptr) const {
            auto e = std::current_exception();
            if (e) ptr->set_exception(std::move(e)); else ptr->drop();
            ptr->wakeup();
        };
    };
    std::unique_ptr<awaitable<T>, deleter> _ptr;
};


///coroutine promise base - helper object
template<>
class _details::promise_type_base<void> {
public:

    awaitable<void> *_target = {};

    void return_void() {
        if (_target) _target->set_value();
    }
    prepared_coro wakeup() {
        if (_target) return _target->wakeup();
        return {};
    }
    void set_exception(std::exception_ptr e) {
        if (_target) _target->set_exception(std::move(e));
    }
};


///A format of generic coroutine frame
/**
 * A valid coroutine frame can be converted to coroutine_handle even if it
 * is not coroutine. It mimics the coroutine. Implementation must inherid this
 * struct and specific self as template argument (CRTP)
 * @tparam FrameImpl name of class implementing frame. The class must
 * declared a function do_resume(), which is called when the handle is
 * used for resumption
 *
 * if someone calls destroy() delete is called
 */
template<typename FrameImpl>
class coro_frame {
protected:
    void (*resume)(std::coroutine_handle<>) = [](std::coroutine_handle<> h) {
        auto *me = reinterpret_cast<FrameImpl *>(h.address());
        me->do_resume();
    };
    void (*destroy)(std::coroutine_handle<>) = [](std::coroutine_handle<> h) {
        auto *me = reinterpret_cast<FrameImpl *>(h.address());
        me->do_destroy();
    };

    ///default implementation. Implement own version with a code to perform
    void do_resume() {}
    ///default implementation. It calls delete. You can overwrite this behaviour
    void do_destroy() {
        auto *me = static_cast<FrameImpl *>(this);
        delete me;
    }

public:
    ///convert the frame to coroutine_handle.
    std::coroutine_handle<> get_handle() {
        return std::coroutine_handle<>::from_address(this);
    }
    ///simulate done coroutine (done() return true)
    void set_done() {
        resume = nullptr;
    }
};


///an awaitable object which suspends current coroutine and calls a function with its handle
/**
 * @code
 * co_await suspend([](std::coroutine_handle<> my_handle) {
 *          //do anything with handle
 *          //eventually resume coroutine
 *          my_handle.resume();
 * });
 * @endcode
 * @tparam Fn
 */
template<std::invocable<std::coroutine_handle<> > Fn>
class suspend : public std::suspend_always{
public:
    suspend(Fn &&fn):_fn(std::forward<Fn>(fn)) {}
    void await_suspend(std::coroutine_handle<> h) {
        _fn(h);
    }

protected:
    Fn _fn;
};

template<typename T, std::invocable<awaitable<T> &> _CB, typename Allocator >
class awaiting_callback : public coro_frame<awaiting_callback<T, _CB, Allocator> >
                        , public object_allocated_by_allocator<Allocator>
{
public:
    static std::pair<std::coroutine_handle<>, awaitable<T> *> create(_CB &&cb) {
        awaiting_callback *n = new awaiting_callback(std::forward<_CB>(cb));
        return {n->get_handle(), &n->_awt};
    }

    template<std::convertible_to<Allocator &> A>
    static std::pair<std::coroutine_handle<>, awaitable<T> *> create(_CB &&cb, A &&a) {
        Allocator &alloc = a;
        awaiting_callback *n = new(alloc) awaiting_callback(std::forward<_CB>(cb));
        return {n->get_handle(), &n->_awt};
    }
protected:
    awaiting_callback(_CB &&cb):_cb(std::forward<_CB>(cb)) {}    

    _CB _cb;
    awaitable<T> _awt = {nullptr};

    void do_resume() {
        try {
            _cb(_awt);
            do_destroy();
        } catch (...) {
            async_unhandled_exception();
            do_destroy();
        }
    }
    void do_destroy() {
        delete this;
    }

    friend class coro_frame<awaiting_callback<T, _CB, Allocator> >;
};


template <typename T>
template <typename _Callback, typename... _Allocator>
    requires(sizeof...(_Allocator) < 2)
inline prepared_coro awaitable<T>::set_callback_internal(_Callback &&cb, _Allocator &...a)
{

    prepared_coro out = {};

    if (await_ready()) {
        cb(*this);
    } else {

        auto [h, awt] =  awaiting_callback<T, _Callback, _Allocator ... >::create(std::forward<_Callback>(cb), a ...);
        awt->_owner = h;
        if (_state == callback) {
            out = get_local_callback()->call(result(awt));
        } else if (_state == callback_ptr) {
            out =_callback_ptr->call(result(awt));
        } else if (_state == coro) {
            out = _coro.start(result(awt));
        } else {
            h.destroy();
            throw invalid_state();
        }
    }
    return out;
}


///a emulation of coroutine which sets atomic flag when it is resumed
class sync_frame : public coro_frame<sync_frame> {
public:

    sync_frame() = default;
    sync_frame (const sync_frame  &) = delete;
    sync_frame &operator = (const sync_frame  &) = delete;

    ///wait for synchronization
    void wait() {
        _signal.wait(false);
    }

    ///reset synchronization
    void reset() {
        _signal = false;
    }

protected:
    friend class coro_frame<sync_frame>;
    std::atomic<bool> _signal = {};
    void do_resume() {
        _signal = true;
        _signal.notify_all();
    }

};

template<typename T>
inline awaitable<T> && awaitable<T>::wait() {
    if (!await_ready()) {
        sync_frame sync;
        await_suspend(sync.get_handle()).resume();
        sync.wait();
    }
    return std::move(*this);
}



template<typename T>
inline T coroutine<T, void>::await() {
    awaitable<T> awt(std::move(*this));
   if constexpr(std::is_void_v<T>) {
       awt.await();
       return;
   } else {
       return awt.await();
   }
}

template<typename T, typename ... Us>
struct count_type;
template<typename T>
struct count_type<T> {
    static constexpr unsigned int value = 0;
};

template<typename T, typename Head, typename ... Tail>
struct count_type<T, Head, Tail...> {
    static constexpr unsigned int value = (std::is_same_v<T, Head> ? 1:0)
                + count_type<T, Tail...>::value;
};
template<typename T, typename ... Us>
constexpr auto count_type_v = count_type<T, Us ...>::value;


///holds a memory which can be reused for coroutine frame
/**
 * The allocator can hold one coroutine at time. You should avoid
 * multiple active coroutines. This is not checked, so result of
 * such action is undefined behaviour
 *
 * Main purpose of this function is to keep memory allocated if
 * the coroutines are called in a cycle. Allocation is costly, so
 * reusing existing memory increases performance
 *
 * To use this allocator, coroutine must be declared with  this allocator
 *
 * @code
 * coroutine<T, reusable_allocator> my_coro(reusable_allocator&, addition_args...)
 * @endcode
 *
 * The instance of the allocator MUST be included in argument list even
 * if the actuall instance is not used in the code of the coroutine. You
 * can freely choose argument position, but there must be exactly
 * one reference to the allocator. The reference points to
 * actuall instance of the allocator which holds the preallocated memory
 *
 *
 */
class reusable_allocator {
public:

    reusable_allocator() = default;
    reusable_allocator(const reusable_allocator &) = delete;
    reusable_allocator &operator=(const reusable_allocator &) = delete;
    ~reusable_allocator() {::operator delete(_buffer);}

    template<typename ... Args>
    static constexpr reusable_allocator *find_me(Args && ... args) {
        constexpr auto finder = [](reusable_allocator *& me, auto &&k) {
            if (me) return;
            if constexpr(std::is_same_v<decltype(k), reusable_allocator &>) {
                me = &k;
            }
        };
        reusable_allocator *me = nullptr;
        (finder(me, args), ...);
        return me;
    }

    template<typename ... Args>
    static void *alloc(std::size_t sz, Args && ... args) {
        static_assert(count_type_v<reusable_allocator &, Args ...> == 1,
                "The coroutine must include `reusable_allocator &` into argument list (once)");
        reusable_allocator *me = find_me(args...);
        if (me) {
            if (me->_buffer_size < sz) {
                ::operator delete(me->_buffer);
                me->_buffer = ::operator new(sz);
                me->_buffer_size = sz;
            }
            return me->_buffer;
        } else {
            throw invalid_state();
        }
    }

    static void dealloc(void *, std::size_t) {}


protected:
    void *_buffer = {};
    std::size_t _buffer_size = 0;
};

///Helps to await multiple awaitable until all of them are evaluated
/**
 * @code
 * auto awt1 = async_op_1(...); //initiate first operation
 * auto awt2 = async_op_2(...); //initiate second operation
 * auto awt3 = async_op_3(...); //initiate third operation
 * allof_set s; //initialize set
 * s.add(awt1);  //register first awaitable
 * s.add(awt2);  //register second awaitable
 * s.add(awt3);  //register third awaitable
 * co_await s;   //wait until all are evaluated
 * auto r1 = awt1.await_resume(); //retrieve value of first
 * auto r2 = awt2.await_resume(); //retrieve value of second
 * auto r3 = awt3.await_resume(); //retrieve value of third
 */
class allof_set {
public:

    allof_set() = default;

    allof_set(const allof_set &) = delete;
    allof_set &operator=(const allof_set &) = delete;

    template<typename X, typename ... Args>
    allof_set(awaitable<X> &first, Args & ... other) {
        add(first);
        (add(other),...);
    }

    template<typename X>
    allof_set(std::span<awaitable<X> > list) {
        for (auto &x: list) add(x);
    }

    ///add an awaitable object to set
    /** Intended way is to add multiple awaitables to the set
     * before you start to awaiting
     *
     * @param awt awaitable object
     * @return because result of operation can be resumption of an
     * coroutine, its prepared handle is returned now. You can ignore
     * return value in most of cases
     *
     * @note function only registers the awaitable object. It is now
     * in in pending state and you need to keep it until it is evaluated
     *
     */
    template<typename X>
    prepared_coro add(awaitable<X> &awt) {
        if (!awt.await_ready()) {
            ++_cnt.count;
            return awt.await_suspend(_cnt.get_handle());
        }
        return {};
    }

    ///start awaiting operation
    awaitable<void> operator co_await() {
        return start();
    }

    ///wait synchronously
    void wait() {
        start().wait();
    }

    ///reset state
    bool reset() {
        unsigned int need = 0;
        return _cnt.count.compare_exchange_strong(need,1);
    }

protected:

    //start co_await operation
    awaitable<void> start() {
        //this is asynchronous operation
        return [this](awaitable_result<void> r) {
            //publish result object
            _cnt.r = std::move(r);
            //check counter
            _cnt.do_resume();
        };
    }

    //mimics coroutine object is resumed for every complete result
    //we just count down all attempts until it is reached zero
    //then awaiting coroutine is resumed
    struct counter: coro_frame<counter> {
        std::atomic<unsigned int> count = {1};
        awaitable_result<void> r = {};
        void do_resume() {
            if (--count == 0) //if reached zero
                r();            //resume awaiting
        }

    };
    counter _cnt;

};

///Helps to select first evaluated of set of awaitable objects
class anyof_set {
public:

    ///construct empty set
    anyof_set() = default;
    ///cannot be copied
    anyof_set(const anyof_set &) = delete;
    ///cannot be copied
    anyof_set &operator=(const anyof_set &) = delete;


    ///add awaitable to the set assign an unique uid
    /**
     * @param awt awaitable object ready to be awaited
     * @param uid associated unique id, this id is returned from co_await
     * @return by adding awaitable causes that asynchronous operation is started
     * at background. If it is started as coroutine, this function returns its
     * prepared handle. You can schedule its resumption. The return
     * value can be ignored
     *
     * It is possible to add new items between awaiting
     */
    template<typename X>
    prepared_coro add(awaitable<X> &awt, unsigned int uid) {
        slot *sel = _first_free.load();
        if (sel) {
            while (!_first_free.compare_exchange_weak(sel, sel->_next));
            sel->reset(uid);
        } else {
            _sls.emplace_back(this, uid);
            sel = &_sls.back();
        }
        if (awt.await_ready()) {
            sel->do_resume();
            return {};
        } else {
            return awt.await_suspend(sel->get_handle());
        }
    }

    ///start awaiting on the set
    /**
     * @return unique identifier of first evaluated awaitable from the set.
     * The object is also removed from the set, so next call
     * returns next evaluated awaitable
     */
    awaitable<unsigned int> operator co_await() {
        return check();
    }

    ///start awaiting on the set
    /**
     * @return unique identifier of first evaluated awaitable from the set.
     * The object is also removed from the set, so next call
     * returns next evaluated awaitable
     */
    unsigned int wait() {
        return check().wait();
    }


protected:

    ///slot ready to accept finished async operation
    /** All slots can be ordered in linked list */
    struct slot: coro_frame<slot> {
        ///pointer to owner
        anyof_set *owner = nullptr;
        ///next in linked list
        slot *_next = nullptr;
        ///assigned uid
        unsigned int _uid = 0;

        ///construct
        slot(anyof_set *owner, unsigned int uid):owner(owner),_uid(uid) {}

        ///called when coroutine is resumed because operation is complete
        prepared_coro do_resume() {
            //init next pointer
            _next = nullptr;
            //put self into _done_stack atomically
            while (!owner->_done_stack.compare_exchange_weak(_next, this));
            //try to acquire current awaitable pointer (and disable it for others)
            auto w = owner->cur_awaiting.exchange(nullptr);
            //if success
            if (w) {
                //this path is processed by only one thread
                //pop first item from queue (don't need to be our item)
                auto x = owner->pop_queue();
                //send result
                auto r = awaitable_result(w);
                if (x) {
                    return r(*x);
                }

            }
            //this thread is finished
            return {};
        }
        void reset(unsigned int uid) {
            this->_uid = uid;
            this->_next = nullptr;
        }
    };
    std::atomic<awaitable<unsigned int> *> cur_awaiting = {};
    std::deque<slot> _sls;
    std::atomic<slot *> _done_stack = nullptr;
    slot *_done_queue = nullptr;
    std::atomic<slot *> _first_free = nullptr;

    ///pop item from queue
    /**
     * @return item in queue, or empty if queue is empty (atomically)
     */
    std::optional<unsigned int> pop_queue() {
        //test whether we have anything in queue
        if (!_done_queue) {
            //acquire stack
            auto p = _done_stack.exchange(nullptr);
            //reverse stack to queue
            while (p) {
                auto z = p;
                z = z->_next;
                p->_next = _done_queue;
                _done_queue = p;
                p = z;
            }
        }
        //we have queue?
        if (_done_queue) {
            //pick it
            auto r = _done_queue;
            //remove from queue
            _done_queue = r->_next;
            //get uid
            auto uid = r->_uid;
            r->_next = nullptr;
            //place slot to free list
            while (!_first_free.compare_exchange_weak(r->_next, r));
            //return uid
            return uid;
        }
        //return empty
        return {};
    }

    ///check state, return awaitable
    awaitable<unsigned int> check() {
        //try to pop from queue
        auto v = pop_queue();
        //if success,  return it synchronously
        if (v) {
            return *v;
        }
        //nothing in queue, start async operation.
        return [this](auto r) -> prepared_coro{
            //atomically publish current awaitable object
            cur_awaiting.exchange(r.release());
            //test race condition (someone finished, before publishing awaitable)
            auto st = _done_stack.load();
            //we found race condition,
            if (st) {
                //try get awaitable back
                auto w = cur_awaiting.exchange(nullptr);
                //we successed, so there is no parallel operation yet
                if (w) {
                    awaitable_result<unsigned int> r2(w);
                    //pop from queue
                    auto sr = pop_queue();
                    if (sr) {
                        //extract result and set the awaiter
                        return r2(*sr);
                    }
                }
                //if awaitable has been picked, the issue will be solved in other thread
            }
            return {};
        };
    }


};


}
