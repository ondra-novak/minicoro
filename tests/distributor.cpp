#include "../coro_distributor.h"
#include "check.h"


using namespace minicoro;

static int count_resume = 0;


awaitable<void> coro_test(distributor<int, empty_lockable> &dist, const void *ident) {

    int i = co_await dist(ident);
    count_resume++;
    CHECK_EQUAL(i, 10);
    i = co_await dist(ident);
    count_resume++;
    CHECK_EQUAL(i, 20);
    i = co_await dist(ident);
    count_resume++;
    CHECK_EQUAL(i, 30);
    i = co_await dist(ident);
    count_resume++;
    CHECK_EQUAL(i, 40);
}

awaitable<void> coro_alert_test(distributor<int, empty_lockable> &dist, std::atomic<bool> &b) {

    int p = 10;
    while (true) {
        int i = co_await dist(b);
        count_resume++;
        CHECK_EQUAL(i, p);
        p+=10;
    }
}


int main() {
    bool ident_a;
    bool ident_b;
    bool ident_c;
    std::atomic<bool> alt = {false};
    std::atomic<bool> alt2 = {false};
    distributor<int, empty_lockable> dist;
    awaitable<void> a = coro_test(dist, &ident_a);
    awaitable<void> b = coro_test(dist, &ident_b);
    awaitable<void> c = coro_test(dist, &ident_c);
    awaitable<void> d = coro_alert_test(dist, alt);
    awaitable<void> e = coro_alert_test(dist, alt2);
    anyof_set as;
    as.add(a, 0);
    as.add(b, 1);
    as.add(c, 2);
    as.add(d, 3);
    as.add(e, 4);

    std::vector<prepared_coro> buff;
    dist.broadcast(buff,10);
    dist.alert(alt2);
    buff.clear();
    int n = as.wait();
    CHECK_EQUAL(n,4);
    dist.broadcast(20);
    dist.kick_out(&ident_b);
    n = as.wait();
    CHECK_EQUAL(n,1);
    dist.broadcast(30);
    dist.alert(alt);
    CHECK_EQUAL(alt.load(), true);
    n = as.wait();
    CHECK_EQUAL(n,3);
    dist.broadcast(40);
    as.wait();
    as.wait();
    CHECK_EQUAL(count_resume, 14);



}
