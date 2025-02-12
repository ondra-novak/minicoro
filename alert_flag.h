#pragma once
#include "coroutine.h"

namespace MINICORO_NAMESPACE {

    class alert_flag_type {
        public:
        
            explicit operator bool() const {return _flag.load(std::memory_order_relaxed);}
            void set() {_flag.store(true, std::memory_order_relaxed);}
            bool test_and_reset() {return _flag.exchange(false, std::memory_order_relaxed);}
            void reset() {_flag.store(false, std::memory_order_relaxed);}
            alert_flag_type() = default;
            alert_flag_type(bool v):_flag(v) {};
            alert_flag_type(const alert_flag_type &) = delete;
            alert_flag_type &operator=(const alert_flag_type &) = delete;
        
        protected:
            std::atomic<bool> _flag = {false};
        
    };
        
}