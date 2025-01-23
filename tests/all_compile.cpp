#include "../coroutine.h"
#include "../async_generator.h"
#include "../coro_mutex.h"
#include "../coro_queue.h"
#include <iostream>


template class minicoro::async_generator<int>;
template class minicoro::coro_queue<int, 128>;
template class minicoro::multi_lock<10>;

int main() {
    std::cout << sizeof(minicoro::awaitable<int>) << std::endl;
    return 0;
}
