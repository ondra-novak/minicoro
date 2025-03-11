#include "../coro_scheduler.h"

#include "check.h"

#include <iostream>
#include <sstream>

using namespace minicoro;


awaitable<unsigned int> coro_test(scheduler &sch, unsigned int ms, unsigned int id) {
    co_await sch.sleep_for(std::chrono::milliseconds(ms));
    co_return id;
}


awaitable<void> coro_test_master(scheduler &sch, std::ostream &out) {
    awaitable<unsigned int>lst[] = {
            coro_test(sch,1000,1),
            coro_test(sch,500,2),
            coro_test(sch,1500,3),
            coro_test(sch,700,4),
            coro_test(sch,825,5),
            coro_test(sch,225,6),
    };

    when_each s(lst);
    while (s) {
        auto r = co_await s;
        out << lst[r].await_resume() << "|";
    }
}



int main() {
    std::ostringstream buff;
    scheduler sch;
    sch.await(coro_test_master(sch,buff));
    if (buff.view() != "6|2|4|5|1|3|") return 1;
    return 0;
}

