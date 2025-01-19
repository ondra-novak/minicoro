#include "../coroutine.h"
#include <sstream>
#include <iostream>
#include <vector>
#include <thread>

using namespace minicoro;

awaitable<void> thread_sleep(unsigned int ms) {
    return [ms](auto p) {
        std::thread thr([ms, p = std::move(p)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            p();
        });
        thr.detach();
    };
}

awaitable<unsigned int> coro_test(unsigned int ms, unsigned int id) {
    co_await thread_sleep(ms);
    co_return id;
}


awaitable<void> coro_test_master(std::ostream &out) {
    awaitable<unsigned int>lst[] = {
            coro_test(1000,1),
            coro_test(500,2),
            coro_test(1500,3),
            coro_test(700,4),
            coro_test(825,5),
            coro_test(225,6),
    };

    anyof_set s;
    unsigned int idx = 0;
    for (auto &x: lst) s.add(x, idx++);
    while(idx) {
        --idx;
        auto r = co_await s;
        out << lst[r].await_resume() << "|";
    }
}

awaitable<void> coro_test_master_all_off() {
    awaitable<unsigned int>lst[] = {
            coro_test(1000,1),
            coro_test(500,2),
            coro_test(1500,3),
            coro_test(700,4),
            coro_test(825,5),
            coro_test(225,6),
    };

    allof_set s;
    for (auto &x: lst) s.add(x);
    co_await s;
    unsigned int idx = 1;
    for (auto &x: lst) {
        if (x.await_resume() != idx) {
            exit(2);
        }
        ++idx;
    }
}

int main() {
    std::ostringstream buff;
    coro_test_master(buff).await();
    if (buff.view() != "6|2|4|5|1|3|") return 1;
    buff.str({});
    coro_test_master_all_off().await();
    return 0;
}
