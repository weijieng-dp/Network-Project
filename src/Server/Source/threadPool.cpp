#include "types.h"

threadPool::threadPool(uint32_t count) : maxThreads{ count }, updateThread(std::bind(&threadPool::update, this)) {}

threadPool::~threadPool() {
    stopped = true;
    updateThread.join();
}

void threadPool::stopAll() {
    stopped = true;
}

void threadPool::update() {
    while (!stopped) {
        std::vector<std::set<threadWrapper>::iterator> removeList;
        for (auto it{ threadpool.begin() }; it != threadpool.end(); it++) {
            if ((*it).finishSignal) {
                removeList.push_back(it);
            }
        }

        for (auto& it : removeList) {
            threadpool.erase(it);
            if (!threadQueue.empty()) {
                threadpool.emplace(threadQueue.front());
                threadQueue.pop();
            }
        }
        // Check for finished threads every second
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

