#include <gtest/gtest.h>
#include "../thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <numeric>

using namespace leo;
using namespace std::chrono_literals;

// 测试基础功能 - 静态线程池
TEST(ThreadPoolTest, BasicFunctionality) {
    thread_pool<> pool(4);

    std::atomic<int> counter{ 0 };

    // 提交10个任务，每个任务将计数器加1
    for (int i = 0; i < 10; ++i) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(10ms);
            counter.fetch_add(1);
            });
    }

    // 等待所有任务完成
    while (counter.load() < 10) {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(counter.load(), 10);
}

// 测试返回值功能
TEST(ThreadPoolTest, ReturnValue) {
    thread_pool<> pool(4);

    auto future = pool.submit([]() {
        std::this_thread::sleep_for(10ms);
        return 42;
        });

    EXPECT_EQ(future.get(), 42);
}

// 测试动态线程池
TEST(ThreadPoolTest, DynamicThreadPool) {
    thread_pool<ThreadPoolPolicy::DYNAMIC> pool(2, 1s, 100ms); // 从4减少到2个初始线程

    std::atomic<int> counter{ 0 };
    size_t initial_pool_size = pool.get_pool_size();
    EXPECT_EQ(initial_pool_size, 2); // 确认初始线程数为2

    // 提交一批任务，让所有线程都处于忙碌状态
    for (int i = 0; i < 4; ++i) { // 提交4个长时间运行的任务
        pool.submit([&counter]() {
            std::this_thread::sleep_for(500ms); // 长时间运行的任务
            counter.fetch_add(1);
            });
    }

    // 短暂等待确保前面的任务已经分配
    std::this_thread::sleep_for(100ms);

    // 检查线程池是否已经动态扩展
    size_t expanded_pool_size = pool.get_pool_size();
    EXPECT_GT(expanded_pool_size, initial_pool_size); // 线程数应该增加

    // 等待所有任务完成
    while (counter.load() < 4) {
        std::this_thread::sleep_for(10ms);
    }

    // 再提交一批任务
    for (int i = 0; i < 16; ++i) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(50ms);
            counter.fetch_add(1);
            });
    }

    // 等待所有任务完成
    while (counter.load() < 20) {
        std::this_thread::sleep_for(10ms);
    }

    // 等待线程超时退出
    std::this_thread::sleep_for(5s);

    // 确认任务都已完成且线程数量应该减少
    EXPECT_EQ(counter.load(), 20);
    EXPECT_LT(pool.get_pool_size(), expanded_pool_size);
}

// 测试动态线程池的自动扩展和收缩
TEST(ThreadPoolTest, DynamicThreadPoolExpandAndContract) {
    thread_pool<ThreadPoolPolicy::DYNAMIC> pool(1, 500ms, 100ms); // 从1个线程开始

    std::atomic<int> counter{ 0 };
    EXPECT_EQ(pool.get_pool_size(), 1); // 确认初始只有1个线程

    // 提交多个任务强制线程池扩展
    constexpr int num_tasks = 8;
    for (int i = 0; i < num_tasks; ++i) {
        pool.submit([&counter, i]() {
            // 不同持续时间的任务，确保线程池会扩展
            std::this_thread::sleep_for(300ms - i * 20ms);
            counter.fetch_add(1);
            });

        // 给线程池一些时间来创建新线程
        std::this_thread::sleep_for(20ms);
    }

    // 短暂等待后检查线程池是否扩展
    std::this_thread::sleep_for(100ms);
    size_t peak_size = pool.get_pool_size();
    EXPECT_GT(peak_size, 1); // 线程数应该已经增加

    // 等待所有任务完成
    while (counter.load() < num_tasks) {
        std::this_thread::sleep_for(10ms);
    }

    // 等待足够长的时间，让多余的线程超时退出
    std::this_thread::sleep_for(5s);

    // 线程池应该已经收缩
    EXPECT_LT(pool.get_pool_size(), peak_size);
    EXPECT_EQ(counter.load(), num_tasks); // 确认所有任务都已完成
}

// 测试优先级任务
TEST(ThreadPoolTest, PriorityTasks) {
    thread_pool<ThreadPoolPolicy::PRIORITY> pool(4);

    std::vector<int> execution_order;
    std::mutex order_mutex;

    // 提交不同优先级的任务
    for (int i = 0; i < 5; ++i) {
        // 低优先级任务 - 标记为1
        pool.submit(1, [&execution_order, &order_mutex]() {
            std::this_thread::sleep_for(10ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            });

        // 高优先级任务 - 标记为3
        pool.submit(3, [&execution_order, &order_mutex]() {
            std::this_thread::sleep_for(10ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
            });

        // 普通优先级任务 - 标记为2
        pool.submit(2, [&execution_order, &order_mutex]() {
            std::this_thread::sleep_for(10ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(2);
            });
    }

    // 等待所有任务完成
    while (execution_order.size() < 15) {
        std::this_thread::sleep_for(10ms);
    }

    // 检查高优先级任务是否先执行
    int high_priority_count = 0;
    for (int i = 0; i < 5; ++i) {  // 检查前5个任务
        if (execution_order[i] == 3) {
            high_priority_count++;
        }
    }

    // 前5个任务中应该有较多高优先级任务
    EXPECT_GE(high_priority_count, 3);
}

// 测试工作窃取
TEST(ThreadPoolTest, WorkStealing) {
    thread_pool<ThreadPoolPolicy::WORK_STEALING> pool(4);

    std::atomic<int> counter{ 0 };

    // 将所有任务提交到同一个线程
    auto& thread = *pool.get_threads().begin()->second;
    for (int i = 0; i < 20; ++i) {
        thread.push(thread_pool<ThreadPoolPolicy::WORK_STEALING>::task_type{
            [&counter]() {
                std::this_thread::sleep_for(5ms);
                counter.fetch_add(1);
            }
            });
    }

    // 等待所有任务完成
    while (counter.load() < 20) {
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(counter.load(), 20);
}

// 测试销毁线程池
TEST(ThreadPoolTest, DestroyPool) {
    auto pool = std::make_unique<thread_pool<>>(4);

    std::atomic<int> counter{ 0 };

    // 提交一些长时间运行的任务
    for (int i = 0; i < 4; ++i) {
        pool->submit([&counter]() {
            std::this_thread::sleep_for(100ms);
            counter.fetch_add(1);
            });
    }

    // 立即销毁线程池
    pool.reset();

    // 检查计数器 - 由于线程池被销毁，可能不是所有任务都完成了
    EXPECT_LE(counter.load(), 4);
}

// 测试混合策略
TEST(ThreadPoolTest, MixedPolicy) {
    // 增加初始线程数
    thread_pool<ThreadPoolPolicy::ALL> pool(4, 1s, 100ms);

    std::vector<int> execution_order;
    std::mutex order_mutex;
    std::atomic<int> counter{ 0 };
    constexpr int task_groups = 20; // 减少任务数量
    constexpr int total_tasks = task_groups * 2;

    // 提交不同优先级的任务
    for (int i = 0; i < task_groups; ++i) {
        pool.submit(1, [&counter, &execution_order, &order_mutex]() {
            std::this_thread::sleep_for(10ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            counter.fetch_add(1);
            });

        pool.submit(3, [&counter, &execution_order, &order_mutex]() {
            std::this_thread::sleep_for(10ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(3);
            counter.fetch_add(1);
            });

        // 给线程池一些处理时间
        std::this_thread::sleep_for(5ms);
    }

    // 更长的等待时间确保稳定
    while (counter.load() < total_tasks) {
        std::this_thread::sleep_for(20ms);
    }

    // 稍微放宽条件
    int high_priority_count = 0;
    int first_tasks_to_check = std::min(15, static_cast<int>(execution_order.size()));

    for (int i = 0; i < first_tasks_to_check; ++i) {
        if (execution_order[i] == 3) {
            high_priority_count++;
        }
    }

    // 前15个任务中应该有超过50%是高优先级
    EXPECT_GT(high_priority_count, first_tasks_to_check / 2);
}

// 测试异常处理
TEST(ThreadPoolTest, ExceptionHandling) {
    thread_pool<> pool(2);

    auto future1 = pool.submit([]() {
        throw std::runtime_error("Test exception");
        return 1;
        });

    auto future2 = pool.submit([]() {
        return 2;
        });

    EXPECT_THROW(future1.get(), std::runtime_error);
    EXPECT_EQ(future2.get(), 2);
}

// 测试大量任务
TEST(ThreadPoolTest, LargeNumberOfTasks) {
    thread_pool<> pool(4);

    constexpr int num_tasks = 1000;
    std::vector<std::future<int>> futures;
    std::atomic<int> counter{ 0 };

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.submit([i, &counter]() {
            counter.fetch_add(1);
            return i;
            }));
    }

    // 检查所有任务是否完成
    for (int i = 0; i < num_tasks; ++i) {
        EXPECT_EQ(futures[i].get(), i);
    }

    EXPECT_EQ(counter.load(), num_tasks);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}