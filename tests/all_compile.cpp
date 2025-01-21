#include "../coroutine.h"
#include "../generator.h"
#include "../coro_mutex.h"

template class minicoro::async_generator<int>;

int main() {
    return 0;
}
