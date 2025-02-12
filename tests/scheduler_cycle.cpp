#include "../coro_scheduler.h"

#include "check.h"

#include <iostream>
#include <sstream>

using namespace minicoro;


awaitable<int> cycle_coro(scheduler &sch, std::atomic<bool> &flag) {
    int count_cycles = 0;
    while (!flag.load(std::memory_order_relaxed)) {
        count_cycles++;
        co_await sch.sleep_for_alertable(flag, std::chrono::milliseconds(100));
        co_await sch.sleep_for(std::chrono::milliseconds(100));
    }
    co_return count_cycles;
}


awaitable<int> main_coro(scheduler &sch, std::chrono::milliseconds ms) {
    std::atomic<bool> flag= {false};

    awaitable<int> c = cycle_coro(sch, flag);
    anyof_set s;
    s.add(c, 0);
    co_await sch.sleep_for(ms);
    sch.alert(flag);
    co_await s;
    co_return c.await_resume();

}

int main() {
    scheduler sch;
    int count = sch.await(main_coro(sch,std::chrono::milliseconds(950)));
    CHECK_EQUAL(count,5);
    count = sch.await(main_coro(sch,std::chrono::milliseconds(550)));
    CHECK_EQUAL(count,3);
    return 0;
}

