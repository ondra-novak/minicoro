#include "../coroutine.h"
#include "../generator.h"
#include "../coro_mutex.h"
#include "../coro_queue.h"


template class minicoro::async_generator<int>;
template class minicoro::coro_queue<int, 128>;

int main() {
    return 0;
}
