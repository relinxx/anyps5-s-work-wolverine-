#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace AgcDriver::DriverDetail {

// Bounds waiting work. Workers own popped tasks; their memory is controlled
// separately by the worker count. An oversized task can enter an empty queue
// so a shader larger than the byte budget still makes progress.
class PreparationQueue {
public:
    struct State { std::size_t entries, bytes, submitted; };
    PreparationQueue(std::size_t entries, std::size_t bytes)
        : entryLimit(entries), byteLimit(bytes) {}

    State Push(std::function<void()> task, std::size_t bytes) {
        std::unique_lock lock(mutex);
        space.wait(lock, [&] {
            return (entryLimit == 0 || tasks.size() < entryLimit) &&
                (byteLimit == 0 || tasks.empty() ||
                 (queuedBytes <= byteLimit && bytes <= byteLimit - queuedBytes));
        });
        tasks.emplace_back(std::move(task), bytes);
        queuedBytes += bytes;
        const State result{tasks.size(), queuedBytes, ++submitted};
        lock.unlock();
        ready.notify_one();
        return result;
    }

    std::function<void()> Pop() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return !tasks.empty(); });
        auto task = std::move(tasks.front().first);
        queuedBytes -= tasks.front().second;
        tasks.pop_front();
        lock.unlock();
        space.notify_all();
        return task;
    }

private:
    const std::size_t entryLimit, byteLimit;
    std::mutex mutex;
    std::condition_variable ready, space;
    std::deque<std::pair<std::function<void()>, std::size_t>> tasks;
    std::size_t queuedBytes = 0, submitted = 0;
};
}
