#pragma once
#include "drivelink/session.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>

namespace drivelink_test {
struct ManualDispatcher {
    std::mutex mutex;std::condition_variable cv;std::deque<drivelink::Task> queue;
    drivelink::Dispatcher dispatcher() {return [this](drivelink::Task task) {{std::lock_guard lock(mutex);queue.push_back(std::move(task));}cv.notify_one();};}
    bool run() {
        drivelink::Task task;
        {std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return !queue.empty();})) {return false;}
         task=std::move(queue.front());queue.pop_front();}task();return true;
    }
};
// Deterministic scheduler fixture: copying an armed dispatcher pauses exactly
// one dispatch on the selected thread, before the task can reach the UI queue.
// It exercises the real public API without adding a production scheduling hook.
struct CopyGate {
    ManualDispatcher output;
    std::mutex mutex;
    bool armed=false;
    std::thread::id target{};
    std::promise<void> entered,release;
    std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> opened{false};
    void arm(std::thread::id thread={}) {std::lock_guard lock(mutex);target=thread;armed=true;}
    void onCopy() {
        {std::lock_guard lock(mutex);if(!armed || (target!=std::thread::id{} && target!=std::this_thread::get_id())) {return;}armed=false;}
        entered.set_value();released.wait();
    }
    void open() {if(!opened.exchange(true)) {release.set_value();}}
};
struct GatedDispatcher {
    std::shared_ptr<CopyGate> gate;
    explicit GatedDispatcher(std::shared_ptr<CopyGate> g):gate(std::move(g)) {}
    GatedDispatcher(const GatedDispatcher &other):gate(other.gate) {gate->onCopy();}
    GatedDispatcher(GatedDispatcher &&)=default;
    void operator()(drivelink::Task task) const {gate->output.dispatcher()(std::move(task));}
};
struct OpenGateOnExit {std::shared_ptr<CopyGate> gate;~OpenGateOnExit(){gate->open();}};
}
