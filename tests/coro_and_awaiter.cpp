#include <iostream>
#include <sstream>
#include <thread>
#include "check.h"

#include "../coroutine.h"

using namespace minicoro;

static awaitable<int>::result prom;


awaitable<int> awt_fn() {
    return [=](auto p) {
        prom = std::move(p);
    };
}

coroutine<int> test2() {
    co_return co_await awt_fn();
}


coroutine<int> test1() {
    co_return co_await test2();
}

awaitable<int> test1_call() {
    return test1();
}

coroutine<void> test_void(std::ostream &out) {
    out << "In void coro/";
    co_return;
}

struct callback_by_member {
    void foo(awaitable<int>::result res) {
        res = 10;
    }
};

void test1(std::ostream &out) {
    auto cb = [&](awaitable<int> &result) {
        out << result.await_resume() << '/';
    };


    test_void(out);
    auto r = test1_call();
    r.set_callback(std::move(cb));
    prom=42;

    callback_by_member bar;
    int x = awaitable<int>(&bar, &callback_by_member::foo);
    out << x << '/';
}

coroutine<std::string> switch_thread() {

    auto id = co_await awaitable<std::string>([](auto r){
       std::thread thr([r = std::move(r)]()mutable{
           std::ostringstream buff;
           buff << std::this_thread::get_id();
           r = buff.str();
       }) ;
       thr.detach();
    });
    co_return id;
}

void test_awaitable_in_thread(std::ostream &out) {
    std::ostringstream buff;
    buff << std::this_thread::get_id();
    auto t1 = buff.str();
    auto t2 = switch_thread().await();
    if (t1 == t2) out << "same";
    else out << "different";
}

coroutine<int, reusable_allocator> test_alloc_coro(int a, reusable_allocator &) {
    co_return a*a;
}

void reusable_test() {
    reusable_allocator ra;
    int total = 0;
    for (int i = 0; i < 10; ++i) {
        total = total + test_alloc_coro(i, ra);
    }
    CHECK_EQUAL(total, 285);
}

struct test_struct {
    int val;
};

awaitable<test_struct> test_pointer_access_fn() {
    return [](auto prom){
        return prom(test_struct{42});
    };
}

awaitable<void> test_pointer_access_coro() {
    auto awt = test_pointer_access_fn();
    for (auto iter = co_await awt.begin(); iter != awt.end(); ++iter) {
        CHECK_EQUAL(iter->val, 42);
    }
}

awaitable<void> detach_test_coro(bool expect) {
    bool b = co_await awaitable<void>::is_detached();
    CHECK_EQUAL(b, expect);
}

void detached_test() {
    detach_test_coro(false).wait();
    detach_test_coro(true);
}



int main() {
    std::ostringstream s;
    test1(s);
    CHECK_EQUAL(s.view(),"In void coro/42/10/");
    s.str({});
    test_awaitable_in_thread(s);
    CHECK_EQUAL(s.view(),"different");
    reusable_test();
    test_pointer_access_coro().wait();
    detached_test();
    return 0;
}
