#include "../coroutine.h"
#include "../async_generator.h"
#include "../coro_mutex.h"
#include "../coro_queue.h"
#include "../coro_distributor.h"
#include "../coro_scheduler.h"
#include <iostream>


template class minicoro::async_generator<int>;
template class minicoro::coro_queue<int, 128>;
template class minicoro::multi_lock<10>;
template class minicoro::awaitable<const int &>;
template class minicoro::distributor<const int>;

int main() {
    std::cout << sizeof(minicoro::awaitable<int>) << std::endl;
    return 0;
}

