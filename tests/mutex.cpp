#include "../coro_mutex.h"
#include "check.h"
#include <vector>

using namespace MINICORO_NAMESPACE;

void test1() {
    coro_mutex mx;

    auto l1 = mx.lock();
    auto l2 = mx.lock();
    auto l3 = mx.lock();
    CHECK(l1.is_ready());
    CHECK(!l2.is_ready());
    CHECK(!l3.is_ready());
    std::vector<int> res;
    l2 >> [&](awaitable<coro_mutex::ownership> &r){
        coro_mutex::ownership own = r;
        res.push_back(2);
    };
    l3 >> [&](awaitable<coro_mutex::ownership> &r){
        coro_mutex::ownership own = r;
        res.push_back(3);
    };
    coro_mutex::ownership own = l1;
    res.push_back(1);
    own.release();
    CHECK_EQUAL(res.size(),3);
    CHECK_EQUAL(res[0],1);
    CHECK_EQUAL(res[1],2);
    CHECK_EQUAL(res[2],3);

}


int main() {
    test1();
    return 0;
}