# minicoro

bare minimum to support coroutines for C++20 and newer

## classes

### coroutine<T>, coroutine<T, Allocator>

- specify this class as return value from coroutine. 
- T is return value. 
- T can be **void**
- if return value is discarded, coroutine runs in detached mode
- you can **co_await** to return value in other coroutine
- you need to call **.await()** to retrieve return value in normal function
- you can also cast class to return value with the same effect as **.await()**

### awaitable<T>

- represents anything awaitable which you can return from function
- you can use co_await and co_return inside function returning awaitable as
coroutine<T>
- however you can associate awaitable with a callback, which is called
when anybody starts awaiting to awaitable
- you can **co_await** on awaitable
- you need to call **.await()** to retrieve return value in normal function
- you can also cast class to return value with the same effect as **.await()**
- you can attach a callback which is called once the awaitable is evaluated
- **awaitable** can return no-value state, which is reported as **canceled_exception**. You can use operator ! to test it before evaluation
- you can **co_await !awaitable** to do this in coroutine


#### using awaitable: function

```
awaitable<int> async_foo(int jobid);
```

#### using awaitable: coroutine

```
int result = co_await async_foo(42);
```


#### using awaitable: sync

```
int result = async_foo(42);
```

or

```
auto result = async_foo(42).await();
```

#### using awaitable: callback

```
async_foo(42) >> [=](awaitable<int> &res) {
    int result = res;
    //...
};
```

#### constructing awaitable: coroutine

```
awaitable<int> async_foo(int jobid) {
    return run_job_coro(jobid);
}
```

#### constructing awaitable: callback

```
awaitable<int> async_foo(int jobid) {
    return [=](auto result) {
        //do something async
        result = 56;
    };
}
```

#### constructing awaitable: synchronous result

```
awaitable<int> async_foo(int jobid) {
    std::optional<int> jobres =  try_job_sync(jobid);
    if (jobres) return *jobres;
    else return [=](auto result) {
        //continue async with result
    };
}
```

#### working with result

Asynchronous result is carried by `awaitable_result<T>` or `awaitable<T>::result`.
The result can be assigned to result object or you can call result object
as callback

```
void set_result(awaitable<int>::result r) {
    r = 42;
    //r(42);
}
```


### prepared_coro

The class `prepared_coro` often carries a suspended coroutine, which is
ready to run. You can find this class returned from various functions. If
the instance of this class is destroyed, suspended coroutine is resumed.
The main purpose of this class is to postpone resumption and move resumption
out of context where resumption should be unsuitable

```
void set_result(awaitable<int>::result r) {
    prepared_coro c;
    std::unique_lock lk(_mx);
    c = r(get_result());        //set result but left coroutine suspended
    lk.unlock()                 //unlock the lock
    //dtor resumes coroutine
}
```

### allof_set, anyof_set

These sets allows to wait for multiple awaitables, 

- **allof_set** wait for all of awaitables
- **anyof_set** wait for any of awaitables

```
awaitable<int> a = async_foo();
awaitable<float> b = async_bar();
allof_set s;
s.add(a);
s.add(b);
co_await s;
int a_res = a;
float b_res = b;
```
