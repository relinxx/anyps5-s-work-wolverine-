#include "prx/libSceAgcDriver/Execution/include/PreparationQueue.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using AgcDriver::DriverDetail::PreparationQueue;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Admission(std::size_t entries, std::size_t bytes, std::size_t firstBytes) {
    PreparationQueue queue(entries, bytes);
    std::atomic<unsigned> executed{0};
    const auto first = queue.Push([&] { ++executed; }, firstBytes);
    Require(first.entries == 1 && first.bytes == firstBytes, "first admission accounting differs");
    std::promise<void> entering;
    auto producer = std::async(std::launch::async, [&] {
        entering.set_value();
        return queue.Push([&] { ++executed; }, 1);
    });
    entering.get_future().wait();
    Require(producer.wait_for(20ms) == std::future_status::timeout, "producer crossed a full queue");
    queue.Pop()();
    Require(producer.wait_for(2s) == std::future_status::ready, "pop did not wake blocked admission");
    const auto second = producer.get();
    Require(second.entries == 1 && second.bytes == 1 && second.submitted == 2, "pop did not release the retained byte budget");
    queue.Pop()();
    Require(executed == 2, "admitted work was lost");
}

void ConcurrentProducers() {
    PreparationQueue queue(4, 16);
    std::array<std::atomic<unsigned>, 400> executed{};
    std::atomic<bool> crossedBudget{false};
    std::vector<std::thread> workers, producers;
    for (unsigned i = 0; i < 2; ++i) workers.emplace_back([&] {
        for (unsigned j = 0; j < 200; ++j) queue.Pop()();
    });
    for (unsigned i = 0; i < 4; ++i) producers.emplace_back([&, i] {
        for (unsigned j = 0; j < 100; ++j) {
            const auto id = i * 100 + j;
            const auto state = queue.Push([&, id] { ++executed[id]; }, 7);
            if (state.entries > 4 || state.bytes > 16) crossedBudget = true;
        }
    });
    for (auto& producer : producers) producer.join();
    for (auto& worker : workers) worker.join();
    Require(!crossedBudget, "concurrent admission crossed a budget");
    for (const auto& count : executed) Require(count == 1, "concurrent work was dropped or duplicated");
}

int main() {
    try {
        Admission(1, 0, 1); // Entry limit with unlimited bytes.
        Admission(0, 16, 16); // Byte limit with unlimited entries.
        Admission(4, 16, 32); // An oversized shader progresses alone.
        ConcurrentProducers();
        std::cout << "preparation queue admission and concurrency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
