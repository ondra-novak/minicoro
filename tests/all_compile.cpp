#include "../coroutine.h"
#include "../generator.h"

template class minicoro::async_generator<int>;

int main() {
    return 0;
}
