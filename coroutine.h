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
#include <atomic>

#ifndef MINICORO_NAMESPACE
#define MINICORO_NAMESPACE minicoro
#endif


namespace MINICORO_NAMESPACE {

template<typename T>
concept IsAwaitSuspendResult = std::is_void_v<T> || std::is_convertible_v<T, bool> || std::is_convertible_v<T, std::coroutine_handle<> >;

template<typename T>
concept is_awaiter = requires(T a, std::coroutine_handle<> h) {
    {a.await_ready()} -> std::same_as<bool>;
    {a.await_suspend(h)} -> IsAwaitSuspendResult;
    {a.await_resume()};
};

template<typename T>
concept has_co_await = requires(T a) {
    { a.operator co_await() } -> is_awaiter;
};

template<typename T>
concept has_global_co_await = requires(T a) {
    { operator co_await(a) } -> is_awaiter;
};

template<typename T>
concept is_awaitabe = is_awaiter<T> || has_global_co_await<T> || has_co_await<T>;

template<typename T>
concept range_for_iterable = requires(T t) {
    { std::begin(t) } -> std::input_or_output_iterator;
    { std::end(t) } -> std::input_or_output_iterator;
    requires std::sentinel_for<decltype(std::end(t)), decltype(std::begin(t))>;
};

///definition of allocator interface
template<typename T>
concept coro_allocator = (requires(T &val, void *ptr, std::size_t sz, float b, char c) {
    ///static function alloc which should accept multiple arguments where one argument can be reference to its instance - returns void *
    /** the first argument must be size */
    {T::overrides::operator new(sz, val, b, c, ptr)} -> std::same_as<void *>;
    ///static function dealloc which should accept pointer and size of allocated space
    {T::overrides::operator delete(ptr, sz)};
});  //void can be specified in meaning of default allocator

///represents standard allocator
/**
 * Some templates uses this class as placeholder for standard allocation, however it can be used
 * as any other allocator
 */
class objstdalloc {
public:
    struct overrides {
        template<typename ... Args>
        void *operator new(std::size_t sz, Args && ...) {
            return ::operator new(sz);
        }
        template<typename ... Args>
        void operator delete(void *ptr, Args && ...) {
            ::operator delete(ptr);
        }
        void operator delete(void *ptr, std::size_t) {
            ::operator delete(ptr);
        }
    };
};

///replaces simple void everywhere valid type is required
struct void_type {};


template<typename T> using voidless_type = std::conditional_t<std::is_void_v<T>, void_type, T>;

///tests whether object T  can be used as member function pointer with Obj as pointer
template<typename T, typename Obj, typename Fn>
concept is_member_fn_call_for_result = requires(T val, Obj obj, Fn fn) {
    {((*obj).*fn)(std::move(val))};
};

template<typename T> class awaitable;
template<typename T, std::invocable<awaitable<T> &> _CB, coro_allocator _Allocator = objstdalloc> class awaiting_callback;
template<typename T, coro_allocator _Allocator = objstdalloc> class coroutine;
template<typename T> class coro_frame;
template<typename T> class awaitable_result;


///await or co_await function has been canceled
/**
 * The cancelation can be due several reasons. For example, the coroutine has
 * been destroyed, result has not been delivered or attempt to await
 * on non-coroutine object
 *
 */
class await_canceled_exception: public std::exception {
public:
    virtual const char *what() const noexcept override {return "await canceled exception";}
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
constexpr std::size_t awaiting_callback_size = sizeof(awaiting_callback<T, _CB, objstdalloc>);

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
class coroutine<T, objstdalloc>{
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

        bool is_detached() const {
            return this->_target == nullptr;
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
    void cancel() {
        if (_coro) {
            release().destroy();
        }
    }

    ///Release coroutine from the object, you get its handle for any usage
    auto release() {
        return std::coroutine_handle<promise_type>::from_promise(*std::exchange(_coro, nullptr));
    }

    ///struct that helps to detect detached mode
    struct detached_test_awaitable : std::suspend_always {
        bool _detached = false;
        bool await_resume() noexcept {return _detached;}
        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::coroutine_handle<promise_type> ph =
                    std::coroutine_handle<promise_type>::from_address(h.address());
            promise_type &p = ph.promise();
            _detached = p.is_detached();
            return false;
        }
    };

    ///determines whether coroutine is running in detached mode
    /**
     * This can optimize processing when coroutine knows, that no result
     * is requested, so it can skip certain parts of its code. It still
     * needs to generate result, but it can return inaccurate result or
     * complete invalid result
     *
     * to use this function, you need call it inside of coroutine body
     * with co_await
     *
     * @code
     * coroutine<int> foo() {
     *      bool detached = co_await coroutine<int>::is_detached();
     *      std::cout << detached?"detached":"not detached" << std::endl;
     *      co_return 42;
     * }
     *
     * @return awaitable which returns true - detached, false - not detached
     */
    static detached_test_awaitable is_detached() {return {};}
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
class coroutine: public coroutine<T, objstdalloc> {
public:

    using coroutine<T, objstdalloc>::coroutine;

    class promise_type: public coroutine<T, objstdalloc>::promise_type,  public _Allocator::overrides {
    public:
        coroutine<T, _Allocator> get_return_object() {
          return this;
        }

        using _Allocator::overrides::operator new;
        using _Allocator::overrides::operator delete;

    };
    coroutine(promise_type *p):coroutine<T, objstdalloc>(p) {}
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
    using store_type = std::conditional_t<std::is_reference_v<T>,std::reference_wrapper<voidless_type<std::remove_reference_t<T> > >, voidless_type<T> >;
    ///contains alias for result object
    using result = awaitable_result<T>;
    ///allows to use awaitable to write coroutines
    using promise_type = coroutine<T>::promise_type;

    ///virtual interface to execute callback for resolution
    class ICallback {
    public:
        virtual ~ICallback() = default;
        ///start resolution, call the callback
        virtual prepared_coro call(result) = 0;
        ///move support
        virtual void move_to(void *address) = 0;
    };


    ///object which implements lambda callback
    /** This symbol is public to allow calculation of the size in bytes of this object */
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


    ///construct with no value
    awaitable(std::nullptr_t) {};
    ///destructor
    /**
     * @note if there is prepared asynchronous operation, it is started
     * in detached mode. If you need to cancel such operation, use cancel()
     */
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
    awaitable(coroutine<T> coroutine):_state(coro),_coro(std::move(coroutine)) {}

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


    ///returns whether the object has a value
    /**
     * @return the result is awaitable, so you can co_await to retrieve
     * whether the awaitable receives a value (without throwing an exception)
     */
    awaitable<bool> has_value();

    ///returns whether the object has no value
    /**
     * @return the result is awaitable, so you can co_await to retrieve
     * whether the awaitable receives a value (without throwing an exception)
     */
    awaitable<bool> operator!() ;

    ///returns true if the awaitable is resolved
    bool is_ready() const {
        return await_ready();
    }

    ///returns iterator a value
    /**
     * @return iterator to value if value is stored there, otherwise returns end
     * @note the function is awaitable, you can co_await if the value is not yet available.
     */
    awaitable<store_type *> begin();

    ///returns iterator to end
    /**
     * @return iterator to end
     * @note this function is not awaitable it always return valid end()
     */
    store_type *end()  {
        return &_value+1;
    }


    ///returns value of resolved awaitable
    std::add_rvalue_reference_t<T> await_resume() {
        if (_state == value) {
            if constexpr(std::is_void_v<T>) {
                return;
            } else if constexpr(std::is_reference_v<T>) {
                return _value.get();
            } else {
                return std::move(_value);
            }
        } else if (_state == exception) {
            std::rethrow_exception(_exception);
        }
        throw await_canceled_exception();
    }


    ///return true, if the awaitable is resolved
    bool await_ready() const noexcept {
        return _state == no_value || _state == value || _state == exception;
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
        objstdalloc a;
        return set_callback_internal(std::forward<_Callback>(cb), a);
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
        objstdalloc alloc;
        return set_callback_internal(std::forward<_Callback>(cb), alloc);
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


    ///synchronous await
    decltype(auto) await() {
        wait();
        return await_resume();
    }

    ///synchronous await
    operator voidless_type<T> &&() {
        wait();
        return await_resume();
    }

    ///evaluate asynchronous operation, waiting for result synchronously
    void wait();


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
    ///return if there is someone awaiting on object
    /**
     * @retval true someone is awaiting, do not destroy the object
     * @retval false nobody awaiting
     */
    bool is_awaiting() const {
        return _owner != std::coroutine_handle<>();
    }

    ///Create result object from existing awaitable object
    /**
     * This function is used by awaiting_callback to create result object
     * on its internal awaiter. However it allows you to create result from
     * coroutine handle which is awaiting on result and awaitable object itself
     * where the coroutine expects the result. The awaitable object is set
     * to is_awaiting() state
     *
     * @param h handle of coroutine which is resumed once the value is set
     * @return result object
     */
    result create_result(std::coroutine_handle<> h) {
        if (_owner) throw invalid_state();
        _owner = h;
        return result(this);
    }

    ///cancel futher execution
    /** This prevents to execute prepared asynchronous operation. You need
     * to invoke this function if you requested only non-blocking part of
     * operation and don't want to contine asynchronously
     */
    void cancel() {
        if (_owner) throw invalid_state();
        destroy_state();
        _state = no_value;
    }

    ///determines whether coroutine is running in detached mode
    /**
     * This can optimize processing when coroutine knows, that no result
     * is requested, so it can skip certain parts of its code. It still
     * needs to generate result, but it can return inaccurate result or
     * complete invalid result
     *
     * to use this function, you need call it inside of coroutine body
     * with co_await
     *
     * @code
     * awaitable<int> foo() {
     *      bool detached = co_await awaitable<int>::is_detached();
     *      std::cout << detached?"detached":"not detached" << std::endl;
     *      co_return 42;
     * }
     *
     * @return awaitable which returns true - detached, false - not detached
     */
    static typename coroutine<T>::detached_test_awaitable is_detached() {return {};}


    ///Retrieve pointer to temporary state
    /**
     * Temporary state is a user defined object which is allocated inside of awaitable
     *  during performing an asynchronous operation. It can be allocated
     * at the beginning of the asynchronous operation and must be released before the
     * result is set (or exception);
     *
     * This function returns pointer to such temporary state
     *
     * @tparam X cast the memory to given type
     * @param result valid result object
     * @return if the result variable is not set, return is nullptr. This can happen
     * if the asynchronous operation is run in detached mode. Otherwise it
     * returns valid pointer to X.
     *
     * @note When called for the first time, returned pointer points to
     * uninitialized memory. Accessing this object is UB. You need
     * to start lifetime of this object  by calling std::cosntruct_at.
     * Don't also forget to destroy this object by calling
     * std::destroy_at before you set the result.
     *
     * @note When called for the first time, it destroys a closure
     * of the callback function started to initiated asynchronous
     * function. The destruction is performed by calling
     * destructor of the closure. Ensure, that your function
     * no longer need the closure before you call this function.
     *
     * @note space reserved for the state is equal to
     * size of T (result), but never less than
     * 4x size of pointer. The function checks in compile
     * time whether the type X fits to the buffer
     */
    template<typename X>
    static X * get_temp_state(awaitable_result<T> &result) {
        auto me = result._ptr.get();
        if (!me) return nullptr;
        static_assert(sizeof(X) <= callback_max_size);
        if (me->_state != no_value) {
            me->destroy_state();
            me->_state = no_value;
        }
        return reinterpret_cast<X *>(me->_callback_space);
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
        if (is_awaiting()) throw invalid_state();
        switch (_state) {
            default:break;
            case value: std::destroy_at(&_value);break;
            case exception: std::destroy_at(&_exception);break;
            case coro: std::destroy_at(&_coro);break;
            case callback:
                get_local_callback()->call({});
                std::destroy_at(get_local_callback());
                break;
            case callback_ptr:
                _callback_ptr->call({});
                std::destroy_at(&_callback_ptr);
                break;
        }
    }

    void destroy_state() {
        switch (_state) {
            default:break;
            case value: std::destroy_at(&_value);break;
            case exception: std::destroy_at(&_exception);break;
            case coro: _coro.cancel();std::destroy_at(&_coro);break;
            case callback: std::destroy_at(get_local_callback());break;
            case callback_ptr: std::destroy_at(&_callback_ptr);break;
        }
    }

    template<typename ... Args>
    void set_value(Args && ... args) {
        destroy_state();
        try {
            _state = value;
            void *trg = const_cast<std::remove_const_t<store_type> *>(&_value);
            if constexpr (sizeof...(Args) == 1 && (std::is_invocable_r_v<store_type, Args>  && ...)) {
                new(trg) store_type((...,args()));
            } else {
                new(trg) store_type(std::forward<Args>(args)...);
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
        if (!is_ready()) drop();
        return prepared_coro(std::exchange(_owner, {}));
    }

    template<typename _Callback, typename _Allocator>
    prepared_coro set_callback_internal(_Callback &&cb, _Allocator &a);



    template<bool test>
    struct read_state_frame: coro_frame<read_state_frame<test> >{
        awaitable *src;
        awaitable<bool> *result = {};

        read_state_frame(awaitable *src):src(src) {}

        void do_resume();
        void do_destroy() {}
    };

    struct read_ptr_frame: coro_frame<read_ptr_frame>{
        awaitable *src;
        awaitable<store_type *> *result = {};

        read_ptr_frame(awaitable *src):src(src) {}

        void do_resume();
        void do_destroy() {}
    };

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

    using const_reference = std::add_lvalue_reference_t<std::add_const_t<std::conditional_t<std::is_void_v<T>, void_type, T> > >;
    using rvalue_reference = std::add_rvalue_reference_t<std::conditional_t<std::is_void_v<T>, void_type, T> > ;

    ///set the result
    /**
     * @param val value to set
     * @return prepared coroutine. If the result is discarded, coroutine
     * is resumed immediately. You can store result and destroy it
     * later to postpone resumption.
     */

    template<typename _Val>
    prepared_coro operator=(_Val && val) {
        static_assert(std::is_convertible_v<_Val, T> || std::is_same_v<_Val, awaitable_result> || std::is_convertible_v<_Val, std::exception_ptr>);
        if constexpr(std::is_same_v<_Val, awaitable_result>) {
            auto p = _ptr.release();
            if (p) {
                p->drop();
                return p->wakeup();
            } else {
                return {};
            }
        } else if constexpr(std::is_convertible_v<_Val, std::exception_ptr>) {
            return this->set_exception(std::forward<_Val>(val));
        } else {
            auto p = _ptr.release();
            if (p) {
                p->set_value(std::forward<_Val>(val));
                return p->wakeup();
            } else {
                return {};
            }
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

    prepared_coro operator=(std::nullopt_t) {
        auto p = _ptr.release();
        if (p) {
            p->drop();
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
        static_assert(sizeof...(Args) != 1 || (!std::is_same_v<std::decay_t<Args>, std::exception_ptr> && ...),
                "To set exception, use operator=, or set_exception()");
        static_assert(std::is_constructible_v<voidless_type<T>, Args...>,
                "Result is not constructible from arguments");
        auto p = _ptr.release();
        if (p) {
            p->set_value(std::forward<Args>(args)...);
            return p->wakeup();
        } else {
            return {};
        }
    }

    prepared_coro set_exception(std::exception_ptr e) {
        auto p = _ptr.release();
        if (p) {
            p->set_exception(std::move(e));
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

    ///returns true if the result is expected
    /**
     * @retval true result is expected
     * @retval false result is not excpected (detached mode?)
     */
    explicit operator bool() const {return static_cast<bool>(_ptr);}

protected:
    struct deleter {
        void operator()(awaitable<T> *ptr) const {
            auto e = std::current_exception();
            if (e) ptr->set_exception(std::move(e)); else ptr->drop();
            ptr->wakeup();
        };
    };
    std::unique_ptr<awaitable<T>, deleter> _ptr;

    friend class awaitable<T>;
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

///a class that mimics coroutine allowing to callback a member function with arguments
/**
 * @tparam T type of result the callback accepts
 * @tparam Instance pointer class (raw pointer, or shared or unique ptr), to an instance
 * @tparam member_fn pointer to member function to call. The member function must have
 * the first argument awaitable<T> & (reference) and other arguments mus be references
 * to Args...
 * @tparam Args list of optional args passed to the function when callback is called.
 *
 *
 * @code
 *  //function
 * void Foo::on_complete(awaitable<int> &awt, int &bar, bool &baz);
 *  //declaration of instance
 * await_member_callback<int, Foo *, &Foo::on_complete, int, bool> _callback_instance;
 *  //usage
 *  awaitable<int> awt = do_async();
 *  _callback_instance.await(awt, this, 42,false);
 * @endcode
 *
 */
template<typename T, typename Instance, auto member_fn, typename ... Args>
requires(requires(awaitable<T> &awt, Instance instance, Args &... args){
    {((*instance).*member_fn)(awt, args...)};
})
class await_member_callback :
        public coro_frame<await_member_callback<T, Instance, member_fn, Args...> >{
public:
    await_member_callback() {}

    ///Register this instance to await on a an awaitable
    /**
     * @param awt awaitable object
     * @param instance a pointer which points to an instance of the callback object
     * @param args arguments
     * @return prepared_coro of asynchronous operation. If operation is already
     * done, returns prepared_coro of this callback
     */
    prepared_coro await(awaitable<T> &&awt, Instance instance, Args ... args) {
        return await(awt,instance, std::forward<Args>(args)...);
    }
    ///Register this instance to await on a an awaitable
    /**
     * @param awt awaitable object
     * @param instance a pointer which points to an instance of the callback object
     * @param args arguments
     * @return prepared_coro of asynchronous operation. If operation is already
     * done, returns prepared_coro of this callback
     */
    prepared_coro await(awaitable<T> &awt, Instance instance, Args ... args) {
        _instance = std::move(instance);
        _args.emplace(std::move(args)...);
        return await_cont(awt);
    }

    ///Continue in await operation
    /** When asynchronous operation must be processed per-partes, this allows
     * to await on asynchronous operation while keeping initial setup unchanged.
     * This function is intended to be used inside of the callback function to
     * continue awaiting
     *
     * @param awt awaiter
     *
     * @return prepared_coro of asynchronous operation. If operation is already
     * done, returns prepared_coro of this callback
     *
     */
    prepared_coro await_cont(awaitable<T> &&awt) {
        return await_cont(awt);
    }
    ///Continue in await operation
    /** When asynchronous operation must be processed per-partes, this allows
     * to await on asynchronous operation while keeping initial setup unchanged.
     * This function is intended to be used inside of the callback function to
     * continue awaiting
     *
     * @param awt awaiter
     *
     * @return prepared_coro of asynchronous operation. If operation is already
     * done, returns prepared_coro of this callback
     */
    prepared_coro await_cont(awaitable<T> &awt) {
        _awt = std::move(awt);
        if (_awt.await_ready()) {
            return this->get_handle();
        } else {
            return _awt.await_suspend(this->get_handle());
        }
    }

protected:
    Instance _instance = {};
    std::optional<std::tuple<Args...> >_args;
    awaitable<T> _awt;

    friend class coro_frame<await_member_callback<T, Instance, member_fn, Args...> >;
    auto do_resume() {
        return std::apply([&](auto & ... args){
            return ((*_instance).*member_fn)(_awt, args...);
        }, *_args);
    }
    void do_destroy() {
        //empty
    }
};


///Contains function which can be called through awaitable<T>::result object
/**
 * The function works as a coroutine associated with existing awaitable, but
 * the awaitable instance is not visible for the user. You can only
 * call this function through the result object
 *
 *
 * @tparam T type of return value (can be void)
 * @tparam _CB callback function. It must accept reference to internal awaitable
 * object, where it can retrieve value
 * @tparam _Allocator can specify allocator used to allocate the function (similar to coroutine)
 *
 * @note main benefit of this class is that you can calculate size of the occupied
 * memory during compile time. This is not possible for standard coroutines. Knowing
 * the occupied size allows to reserve buffers for its allocation.
 *
 */
template<typename T, std::invocable<awaitable<T> &> _CB, coro_allocator _Allocator >
class awaiting_callback : public coro_frame<awaiting_callback<T, _CB, _Allocator> >
                        , public _Allocator::overrides
{
public:
    ///Create result object to call specified callback function
    /**
     * @param cb callback function
     * @return result object
     */
    static typename awaitable<T>::result create(_CB &&cb) {
        awaiting_callback *n = new awaiting_callback(std::forward<_CB>(cb));
        return n->_awt.create_result(n->get_handle());
    }

    ///Create result object to call specified callback function
    /**
     * @param cb callback function
     * @param alloc reference allocator instance which is used to allocate this object
     * @return result object
     */
    static typename awaitable<T>::result create(_CB &&cb, _Allocator &alloc) {
        awaiting_callback *n = new(alloc) awaiting_callback(std::forward<_CB>(cb));
        return n->_awt.create_result(n->get_handle());
    }
protected:
    ///constructor is not visible on the API
    awaiting_callback(_CB &&cb):_cb(std::forward<_CB>(cb)) {}

    ///callback function itself
    _CB _cb;
    ///awaitable object associated with the function
    awaitable<T> _awt = {nullptr};

    ///called when resume is triggered
    void do_resume() {
        try {
            _cb(_awt);
        } catch (...) {
            async_unhandled_exception();
        }
        do_destroy();
    }
    void do_destroy() {
        delete this;
    }

    friend class coro_frame<awaiting_callback<T, _CB, _Allocator> >;
};

template <typename T>
template <typename _Callback, typename _Allocator>
inline prepared_coro awaitable<T>::set_callback_internal(_Callback &&cb, _Allocator &a)
{
    prepared_coro out = {};


    if (await_ready()) {
        cb(*this);
    } else {

        auto res = awaiting_callback<T, _Callback, _Allocator>::create(std::forward<_Callback>(cb), a);
        if (_state == callback) {
            out = get_local_callback()->call(std::move(res));
        } else if (_state == callback_ptr) {
            out =_callback_ptr->call(std::move(res));
        } else if (_state == coro) {
            out = _coro.start(std::move(res));
        }
        cancel();
    }
    return out;
}

template<typename T>
inline T coroutine<T>::await() {
    awaitable<T> awt(std::move(*this));
   if constexpr(std::is_void_v<T>) {
       awt.await();
       return;
   } else {
       return awt.await();
   }
}

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

   struct overrides {

        template<typename ... Args>
        requires((std::is_same_v<reusable_allocator &, Args> ||...))
        void *operator new(std::size_t sz, Args && ... args) {

            reusable_allocator *me;
            auto finder = [&](auto &&k) {
                if constexpr(std::is_same_v<decltype(k),reusable_allocator &>) me = &k;
            };
            (finder(args),...);
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

        void operator delete (void *, std::size_t) {}
    };

protected:
    void *_buffer = {};
    std::size_t _buffer_size = 0;
};



///start and wait for one or multiple tasks
/**
 * This class can be used to wait for multiple awaitables
 *
 * @code
 * co_await when_all(awt1,awt2,awt3);
 * @endcode
 *
 * Note that all arguments are passed as reference. The co_await doesn't
 * return value, you need to extract results from
 * each awaitable after wait is complete
 *
 * The class can be used to run concurrently with a background coroutine, and
 * to join its awaitable at the end of the section.
 *
 * @code
 * auto awt1 = coro1();
 * when_all c(awt1); //coroutine is started here
 *  //any code here
 * co_await c; //join
 * @endcode
 *
 */
class when_all {
public:

    ///construct empty
    when_all() = default;

    ///no copy
    when_all(const when_all &) = delete;
    ///no copy
    when_all &operator=(const when_all &) = delete;

    ///construc using multiple awaiters
    /**
     * @param other list of awaiters. This can be awaitable<> but it
     * also supports any direct awaiters. If the awaiter needs co_await
     * operator, you need to create instance of its result before it
     * can be used here
     *
     * @code
     * auto need_op1 = async_op1();
     * auto need_op2 = async_op2();
     * auto awt1 = operator co_await(need_op1);
     * auto awt2 = operator co_await(need_op2);
     * co_await when_all(awt1,awt2);
     * @endcode
     */
    template<is_awaiter... Awts>
    when_all(Awts & ... other) {
        (add(other),...);
    }

    ///construct from iteratable container
    template<range_for_iterable X>
    when_all(X &list) {
        for (auto &x: list) add(x);
    }

    ///construct from array
    template<is_awaiter Awt, int n>
    when_all(Awt (&list)[n]) {
        for (auto &x: list) add(x);
    }


    ///add an awaitable object to set
    /** Intended way is to add multiple awaitables to the set
     * before you start to awaiting
     *
     * @param awt awaitable object or direct awaiter
     * @return because result of operation can be resumption of an
     * coroutine, its prepared handle is returned now. You can ignore
     * return value in most of cases
     *
     * @note function only registers the awaitable object. It is now
     * in in pending state and you need to keep it until it is evaluated
     *
     */
    template<is_awaiter X>
    prepared_coro add(X &awt) {
        if (!awt.await_ready()) {
            _cnt.count.fetch_add(1, std::memory_order_relaxed);

            if constexpr (std::is_void_v<decltype(awt.await_suspend(_cnt.get_handle()))>) {
                awt.await_suspend(_cnt.get_handle());
                return {};
            } else {
                auto r = awt.await_suspend(_cnt.get_handle());
                if constexpr(std::is_convertible_v<decltype(r), bool>) {
                    if (!r) {
                        _cnt.count.fetch_sub(1, std::memory_order_relaxed);
                    }
                    return {};
                } else {
                    return r;
                }
            }
        }
        return {};
    }

    ///start awaiting operation
    awaitable<void> operator co_await() {
        return start();
    }

    ///retrieve awaitable for this object
    awaitable<void> get_awaitable() {
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
        if (_cnt.count.load(std::memory_order_acquire) == 1) {
            return {};
        }
        //this is asynchronous operation
        return [this](awaitable_result<void> r) {
            //publish result object
            _cnt.r = std::move(r);
            //check counter
            _cnt.do_resume();
        };
    }

    struct counter: coro_frame<counter> {
        std::atomic<unsigned int> count = {1};
        awaitable_result<void> r = {};
        prepared_coro do_resume() {
            //test whether we reached zero
            if (count.fetch_sub(1, std::memory_order_relaxed)  == 1) {
                //ensure that all results are visible
                count.load(std::memory_order_acquire);
                //resume awaiting coroutine
                return r();
            }
            //still pending
            return {};
        }

    };
    counter _cnt;

};

///Wait and iterate over completed results
/**
 * @tparam count count of awaited objects, This value is often deduced
 */
template<unsigned int count>
class when_each {
public:

    ///constructs from an array of awaitable objects
    /**
     * @param awts array of awaitable object
     *
     * @note when constructor finishes, the awaitable objects are
     * evaluated and eventually await_suspend is called if the
     * awaitable is not ready yet. This can cause that
     * evaluation can run on background (in different thread)
     */
    template<is_awaiter Awt>
    when_each(Awt (&awts)[count]):_cnt(count){
        for (std::size_t i = 0; i < count; ++i) {
            add(awts[i], i);
        }
    }
    ///constructs from multiple arguments
    /**
     * @param awts list of awaitable/awaiters, each can be different type
     *
     * @note when constructor finishes, the awaitable objects are
     * evaluated and eventually await_suspend is called if the
     * awaitable is not ready yet. This can cause that
     * evaluation can run on background (in different thread)
     */
    template<is_awaiter ... Awts>
    requires(sizeof...(Awts) <= count)
    when_each(Awts &... awts):_cnt(sizeof...(Awts)) {
        std::size_t idx = 0;
        (add(awts, idx++),...);
    }

    ///construct from the list
    /** This constructor cannot use deduction guide. You need to
     * specify count above expected size of the list. The actual
     * list can be smaller, but not larger than count
     * @param list object which is iteratable by ranged-for
     */
    template<range_for_iterable X>
    when_each(X &list) {
        std::size_t idx = 0;
        for (auto &x: list) {
            add(x, idx++);
            if (idx == count) break;
        }
        _cnt = idx;
    }

    ///cannot copy
    when_each(const when_each &) = delete;
    ///cannot copy
    when_each &operator=(const when_each &) = delete;

    ///destructor ensures that all awaitables are serialized (join)
    /** @note destructor can't use co_await, it joins synchronously
     */
    ~when_each() {
        while (_nx < _cnt) wait();
    }


    ///retrieve awaitable object for next complete awaitable
    /**
     * @return the awaitable object. The result itself is
     * zero base index of awaitable which is ready for result
     * retrieval.
     *
     * To retrieve result of the awaitable, you need just call
     * await_resume() of complete awaitable (which index is returned
     * by this function)
     *
     */
    awaitable<unsigned int> get_awaitable() {
        if (_nx == _cnt) return nullptr;
        unsigned int v = _finished[_nx].load(std::memory_order_acquire);
        if (v != 0) {
            ++_nx;
            return v - 2;
        }
        return [this](awaitable<unsigned int>::result r) {
            _r = std::move(r);
            std::size_t v = _finished[_nx].exchange(1, std::memory_order_acquire);
            if (v != 0) {
                ++_nx;
                _r(v-2);
            }
        };
    }

    ///co_await to wait until next awaitable is complete
    /**
     * @return is co_awaited, it returns index of awaitable object
     * which is complete. To retrieve result, you need to call
     * await_resume of this awaitable
     */
    awaitable<unsigned int> operator co_await() {return get_awaitable();}

    ///Wait synchronously
    /**
     Useful to wait in non-coroutine function
     @return index of complete awaitable
     */
    unsigned int wait() {
        return get_awaitable();
    }

    ///determines, whether there are still pending awaitables
    /**
     * @retval true still pending
     * @retval false no more pending, you can destroy this object
     */
    explicit operator bool() const{
        return _nx < _cnt;
    }



protected:


    ///contains fake-coroutine which is called when real coroutine would be resumed
    struct Slot: coro_frame<Slot> { // @suppress("Miss copy constructor or assignment operator")
        when_each *_parent;

        prepared_coro do_resume() {
            return _parent->resumed(this);
        }
        void do_destroy() {

        }
    };

    ///list of prepared fake-coroutines to catch resume attempt
    Slot _slots[count];
    ///contains indexes of complete awaitables
    /**
     * The actual value is not index directly, value is increased by 2
     * - value 0 - not complete yet
     * - value 1 - not complete yet but awaitin
     * - other - index of complete + 2
     */
    std::atomic<unsigned int> _finished[count] ={};
    ///contains index of free slot
    std::atomic<unsigned int> _free_slot = {};
    ///contains index of next tested slot
    unsigned int _nx = 0;
    ///contains count of slots
    unsigned int _cnt = 0;
    awaitable<unsigned int>::result  _r = {};

    ///register to slot
    /**
     * @param awt awaitable
     * @param idx index of slot
     * @return prepared coroutine if resume happens
     */
    template<is_awaiter Awt>
    prepared_coro add(Awt &awt, std::size_t idx) {

        //activate slot
        _slots[idx]._parent = this;
        //test whether awaiter is ready
        if (awt.await_ready()) {
            //if ready, we already resumed
            resumed(&_slots[idx]);
            //nothing to resume
            return {};
        } else {
            //if not - depend of type await_suspend
            //void return
            if constexpr (std::is_void_v<decltype(awt.await_suspend(_slots[idx].get_handle()))>) {
                //suspend
                awt.await_suspend(_slots[idx].get_handle());
                //return nothing
                return {};
            } else {
                //suspend and retrieve value
                auto r = awt.await_suspend(_slots[idx].get_handle());
                //if value is bool
                if constexpr(std::is_convertible_v<decltype(r), bool>) {
                    //if bool is false - suspension did not happened
                    if (!r) {
                        //mark resumed
                        return resumed(&_slots[idx]);
                    }
                    //suspend ok
                    return {};
                } else {
                    //if std::coroutine_handle is returned return it as prepared_coro
                    return r;
                }
            }
        }
    }

    ///called when slot is resumed
    /**
     * @param nd pointer to slot
     * @return prepared coroutine if resumption happened
     */
    prepared_coro resumed(Slot *nd) {
        //calculate index
        unsigned int idx = nd - _slots;
        //calculate value
        unsigned int v = idx + 2;
        //retrieve next result slot
        unsigned int wridx = _free_slot.fetch_add(1, std::memory_order_relaxed);
        //exchange value
        unsigned int st = _finished[wridx].exchange(v, std::memory_order_release);
        //if there is 1, somebody already awaiting
        if (st == 1) {
            //advance _nx (we can, nobody else will be there)
            ++_nx;
            //resume awaiting coroutine
            return _r(idx);
        }
        //nothing to resume
        return {};
    }
};

template<typename Awt, unsigned int N>
when_each(Awt (&)[N]) -> when_each<N>;

template<is_awaiter... Awts>
when_each(Awts&...) -> when_each<sizeof...(Awts)>;

///this class makes that callback function is called during destruction
/**
 * This is useful feature for coroutines. The function is called when coroutine
 * frame is destroyed regardless on how it has been destroyed.
 *
 * It is called even if the frame is destroyed
 * by destroy() function.
 *
 * Outside of the coroutines, the callback
 * is called when associated variable is destroyed
 * when excetuion leaves current scope.
 */
template<typename _CB>
class on_destroy{
public:
    on_destroy(_CB &&cb):_cb(std::forward<_CB>(cb)) {}
    ~on_destroy() {_cb();}
protected:
    _CB _cb;
};

template<typename T>
concept can_be_unlocked = requires(T v) {
    {v.unlock()};
};



template<typename T>
template<bool test>
void awaitable<T>::read_state_frame<test>::do_resume() {
           static_assert(std::is_trivially_destructible_v<read_state_frame>);
           bool n = src->_state != no_value;
           awaitable<bool>::result(this->result)(n == test);
}

template<typename T>
void awaitable<T>::read_ptr_frame::do_resume() {
    static_assert(std::is_trivially_destructible_v<read_ptr_frame>);
    typename awaitable<store_type *>::result r(this->result);
    if (src->_state == value) {
        r(&src->_value);
    } else if (src->_state == exception) {
        r = src->_exception;
    } else {
        r(&src->_value+1);
    }
}

template<typename T>
awaitable<bool> awaitable<T>::operator!()  {
    if (await_ready()) {return _state == no_value;}
    return [this](awaitable<bool>::result r) mutable -> prepared_coro {
        auto frm =awaitable<bool>::get_temp_state<read_state_frame<false> >(r);
        if (!frm) return {};
        std::construct_at(frm, this);
        frm->result = r.release();
        return frm->src->await_suspend(frm->get_handle());
    };

}
template<typename T>
awaitable<bool> awaitable<T>::has_value() {
    if (await_ready()) {return _state != no_value;}
    return [this](awaitable<bool>::result r) mutable -> prepared_coro {
        auto frm =awaitable<bool>::get_temp_state<read_state_frame<true> >(r);
        if (!frm) return {};
        std::construct_at(frm, this);
        frm->result = r.release();
        return frm->src->await_suspend(frm->get_handle());
    };
}

template<typename T>
awaitable<typename awaitable<T>::store_type *> awaitable<T>::begin() {
    if (await_ready()) {
        if (_state == exception) std::rethrow_exception(_exception);
        return &_value;
    }
    return [this](awaitable<store_type *>::result r) mutable -> prepared_coro{
        auto frm = awaitable<store_type* >::template get_temp_state<read_ptr_frame>(r);
        if (!frm) return {};
        std::construct_at(frm, this);
        frm->result = r.release();
        return frm->src->await_suspend(frm->get_handle());
    };
}

template<typename _Awt>
prepared_coro call_await_resume(_Awt &&awt, std::coroutine_handle<> handle) {
    using Ret = decltype(awt.await_suspend(handle));
    static_assert(std::is_void_v<Ret>
                || std::is_convertible_v<Ret, std::coroutine_handle<> >
                || std::is_convertible_v<Ret, bool>);

    if constexpr(std::is_convertible_v<Ret, std::coroutine_handle<> >) {
        return prepared_coro(awt.await_suspend(handle));
    } else if constexpr(std::is_convertible_v<Ret, bool>) {
        bool b = awt.await_suspend(handle);
        return b?prepared_coro():prepared_coro(handle);
    } else {
        return {};
    }
}

template<typename T>
concept basic_lockable = requires(T v) {
    {v.lock()};
    {v.unlock()};
};

class empty_lockable {
public:
    void lock() {};
    void unlock() {};
    bool try_lock() {return true;}
};

template<basic_lockable _LK>
class lock_guard {
public:
    lock_guard(_LK &lk):_lk(lk) {_lk.lock();}
    ~lock_guard() {_lk.unlock();}
    lock_guard(const lock_guard &) = delete;
    lock_guard &operator=(const lock_guard &) = delete;

protected:
    _LK &_lk;

};


///a emulation of coroutine which sets atomic flag when it is resumed
class sync_frame: public coro_frame<sync_frame> {
public:

    sync_frame() = default;
    sync_frame(const sync_frame&) = delete;
    sync_frame& operator =(const sync_frame&) = delete;

    ///wait for synchronization
    void wait() {
        _signal.wait(false);
    }

    ///reset synchronization
    void reset() {
        _signal = false;
    }

protected:
    friend class coro_frame<sync_frame> ;
    std::atomic<bool> _signal = { };
    void do_resume() {
        _signal = true;
        _signal.notify_all();
    }

};

template<typename T>
inline void awaitable<T>::wait() {
    if (!await_ready()) {
        sync_frame sync;
        await_suspend(sync.get_handle()).resume();
        sync.wait();
    }
}

}
