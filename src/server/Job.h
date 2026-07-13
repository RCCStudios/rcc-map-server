#pragma once

#include <functional>
#include <condition_variable>

struct Job {
    std::function<int()> fun;
    std::condition_variable *cv = nullptr;
    int *code = nullptr;
    bool *done = nullptr;
};

