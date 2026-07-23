#include <string>
#include <thread>
#include <vector>

#include "common/concurrency/BoundedQueue.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;

bool TestPushAndTryPop() {
    sclient::BoundedQueue<int> queue(4);

    if (!Expect(queue.PushOrDropOldest(10), "PushOrDropOldest should succeed")) {
        return false;
    }
    if (!Expect(queue.PushOrDropOldest(20), "PushOrDropOldest should succeed")) {
        return false;
    }

    int value = 0;
    if (!Expect(queue.TryPop(&value), "TryPop should succeed")) {
        return false;
    }
    if (!Expect(value == 10, "expected first value to be 10")) {
        return false;
    }
    if (!Expect(queue.TryPop(&value), "TryPop should succeed again")) {
        return false;
    }
    if (!Expect(value == 20, "expected second value to be 20")) {
        return false;
    }
    if (!Expect(!queue.TryPop(&value), "TryPop on empty queue should return false")) {
        return false;
    }
    return true;
}

bool TestPushOrDropOldestAtCapacity() {
    sclient::BoundedQueue<int> queue(3);

    if (!Expect(queue.PushOrDropOldest(1), "push 1")) {
        return false;
    }
    if (!Expect(queue.PushOrDropOldest(2), "push 2")) {
        return false;
    }
    if (!Expect(queue.PushOrDropOldest(3), "push 3")) {
        return false;
    }
    if (!Expect(queue.PushOrDropOldest(4), "push 4 should drop oldest")) {
        return false;
    }

    int value = 0;
    if (!Expect(queue.TryPop(&value), "TryPop should succeed")) {
        return false;
    }
    if (!Expect(value == 2, "expected dropped oldest, first value should be 2")) {
        return false;
    }
    if (!Expect(queue.TryPop(&value), "TryPop should succeed")) {
        return false;
    }
    if (!Expect(value == 3, "expected second value to be 3")) {
        return false;
    }
    if (!Expect(queue.TryPop(&value), "TryPop should succeed")) {
        return false;
    }
    if (!Expect(value == 4, "expected third value to be 4")) {
        return false;
    }
    return true;
}

bool TestTryPopWithNullPointer() {
    sclient::BoundedQueue<int> queue(4);
    if (!Expect(!queue.TryPop(nullptr), "TryPop with null should return false")) {
        return false;
    }
    return true;
}

bool TestWaitPopWithNullPointer() {
    sclient::BoundedQueue<int> queue(4);
    if (!Expect(!queue.WaitPop(nullptr), "WaitPop with null should return false")) {
        return false;
    }
    return true;
}

bool TestCloseWakesWaitPop() {
    sclient::BoundedQueue<int> queue(4);
    bool wait_returned = false;
    bool wait_result = true;

    std::thread consumer([&]() {
        int value = 0;
        wait_result = queue.WaitPop(&value);
        wait_returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!Expect(!wait_returned, "WaitPop should be blocking")) {
        queue.Close();
        consumer.join();
        return false;
    }

    queue.Close();
    consumer.join();

    if (!Expect(wait_returned, "WaitPop should return after Close")) {
        return false;
    }
    if (!Expect(!wait_result, "WaitPop on closed empty queue should return false")) {
        return false;
    }
    return true;
}

bool TestCloseReturnsPendingData() {
    sclient::BoundedQueue<int> queue(4);
    queue.PushOrDropOldest(42);
    queue.Close();

    int value = 0;
    if (!Expect(queue.TryPop(&value), "TryPop on closed queue with data should succeed")) {
        return false;
    }
    if (!Expect(value == 42, "expected value 42")) {
        return false;
    }
    if (!Expect(!queue.TryPop(&value), "TryPop on drained closed queue should return false")) {
        return false;
    }
    return true;
}

bool TestPushFailsAfterClose() {
    sclient::BoundedQueue<int> queue(4);
    queue.Close();
    if (!Expect(!queue.PushOrDropOldest(1), "PushOrDropOldest on closed queue should return false")) {
        return false;
    }
    return true;
}

bool TestWaitPopBlocksUntilDataAvailable() {
    sclient::BoundedQueue<int> queue(4);
    int received = 0;

    std::thread consumer([&]() {
        int value = 0;
        if (queue.WaitPop(&value)) {
            received = value;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.PushOrDropOldest(99);
    consumer.join();

    if (!Expect(received == 99, "expected WaitPop to receive 99")) {
        return false;
    }
    return true;
}

bool TestMultiThreadedProducerConsumer() {
    sclient::BoundedQueue<int> queue(16);
    const int item_count = 1000;
    std::vector<int> consumed;
    consumed.reserve(item_count);

    std::thread producer([&]() {
        for (int i = 0; i < item_count; ++i) {
            queue.PushOrDropOldest(i);
        }
        queue.Close();
    });

    std::thread consumer([&]() {
        int value = 0;
        while (queue.WaitPop(&value)) {
            consumed.push_back(value);
        }
    });

    producer.join();
    consumer.join();

    if (!Expect(!consumed.empty(), "expected to consume some items")) {
        return false;
    }
    if (!Expect(consumed.front() >= 0, "expected valid first item")) {
        return false;
    }
    if (!Expect(consumed.back() == item_count - 1, "expected last item to be 999")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!TestPushAndTryPop()) return 1;
    if (!TestPushOrDropOldestAtCapacity()) return 1;
    if (!TestTryPopWithNullPointer()) return 1;
    if (!TestWaitPopWithNullPointer()) return 1;
    if (!TestCloseWakesWaitPop()) return 1;
    if (!TestCloseReturnsPendingData()) return 1;
    if (!TestPushFailsAfterClose()) return 1;
    if (!TestWaitPopBlocksUntilDataAvailable()) return 1;
    if (!TestMultiThreadedProducerConsumer()) return 1;
    return 0;
}
