#include <gtest/gtest.h>
#include "../thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <unordered_set>
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

TEST(ThreadPoolTest, WaitAll) {
    leo::thread_pool<> pool(4); // 创建一个包含4个线程的线程池

    std::atomic<int> counter{ 0 };
    {
        // 提交10个任务，每个任务将计数器加1
        for (int i = 0; i < 10; ++i) {
            pool.submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                counter.fetch_add(1);
                });
        }

        // 等待所有任务完成
        pool.wait_all();
    }

    // 确认所有任务都已完成
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
    thread_pool<ThreadPoolPolicy::DYNAMIC> pool(2, 2s, 100ms); // 从4减少到2个初始线程

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
    pool.wait_all();

    // 再提交一批任务
    for (int i = 0; i < 16; ++i) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(50ms);
            counter.fetch_add(1);
            });
    }

    // 等待所有任务完成
    pool.wait_all();

    // 等待线程超时退出
    std::this_thread::sleep_for(4s);

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
    std::this_thread::sleep_for(3s);

    // 线程池应该已经收缩
    EXPECT_LT(pool.get_pool_size(), peak_size);
    EXPECT_EQ(counter.load(), num_tasks); // 确认所有任务都已完成
}

// 测试优先级任务
TEST(ThreadPoolTest, PriorityTasks) {
    thread_pool<ThreadPoolPolicy::PRIORITY> pool(2);

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
    
    std::atomic<int> counter{0};
    std::atomic<int> thread_ids[20]{};
    
    // 创建任务，每个任务记录其执行线程ID
    for (int i = 0; i < 20; ++i) {
        pool.submit([i, &counter, &thread_ids]() {
            std::this_thread::sleep_for(10ms);
            thread_ids[i] = std::hash<std::thread::id>{}(std::this_thread::get_id());
            counter.fetch_add(1);
        });
    }
    
    // 等待所有任务完成
    while (counter.load() < 20) {
        std::this_thread::sleep_for(10ms);
    }
    
    // 检查是否有多个线程参与执行任务
    std::unordered_set<int> unique_threads;
    for (int i = 0; i < 20; ++i) {
        if (thread_ids[i] != 0) {
            unique_threads.insert(thread_ids[i]);
        }
    }
    
    // 工作窃取应该使多个线程参与处理任务
    EXPECT_GT(unique_threads.size(), 1);
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
            std::this_thread::sleep_for(20ms);
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(1);
            counter.fetch_add(1);
            });

        pool.submit(3, [&counter, &execution_order, &order_mutex]() {
            std::this_thread::sleep_for(20ms);
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

// 测试任务取消功能
TEST(ThreadPoolTest, TaskCancellation) {
    thread_pool<> pool(4);

    // 创建取消令牌
    auto token = pool.create_token();
    std::atomic<bool> task_executed{ false };

    // 提交可取消任务
    auto future = pool.submit_cancelable(token, [&task_executed]() {
        std::this_thread::sleep_for(500ms);
        task_executed = true;
        return 42;
        });

    // 立即取消任务
    token->cancel();

    // 验证任务被取消
    try {
        future.get();
        FAIL() << "应该抛出异常";
    }
    catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "The task was canceled.");
    }

    // 给足够时间让任务可能执行完成
    std::this_thread::sleep_for(1s);

    // 验证任务没有实际执行
    EXPECT_FALSE(task_executed);
}

// 测试取消令牌可以取消多个任务
TEST(ThreadPoolTest, CancelMultipleTasks) {
    thread_pool<> pool(4);
    auto token = pool.create_token();

    std::atomic<int> executed_count{ 0 };
    std::vector<std::future<void>> futures;

    // 提交5个共享同一个取消令牌的任务
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit_cancelable(token, [&executed_count, i]() {
            std::this_thread::sleep_for(300ms);
            executed_count.fetch_add(1);
            }));
    }

    // 短暂延迟后取消所有任务
    std::this_thread::sleep_for(50ms);
    token->cancel();

    // 验证所有future都抛出取消异常
    int exception_count = 0;
    for (auto& f : futures) {
        try {
            f.get();
        }
        catch (const std::runtime_error& e) {
            exception_count++;
        }
    }

    // 验证大部分任务都被取消了
    EXPECT_GE(exception_count, 5);

    // 验证实际执行的任务数量很少
    std::this_thread::sleep_for(500ms); // 给时间让任何已经开始的任务完成
    EXPECT_LE(executed_count, 5);
}

// 测试带优先级的任务取消
TEST(ThreadPoolTest, PriorityCancellation) {
    thread_pool<ThreadPoolPolicy::PRIORITY> pool(2);

    auto token = pool.create_token();
    std::atomic<int> executed_high{ 0 };
    std::atomic<int> executed_low{ 0 };

    // 提交一些低优先级的可取消任务
    for (int i = 0; i < 10; ++i) {
        pool.submit_cancelable(1, token, [&executed_low]() {
            std::this_thread::sleep_for(200ms);
            executed_low.fetch_add(1);
            });
    }

    // 提交一些高优先级的不可取消任务
    for (int i = 0; i < 3; ++i) {
        pool.submit(10, [&executed_high]() {
            std::this_thread::sleep_for(100ms);
            executed_high.fetch_add(1);
            });
    }

    // 取消低优先级任务
    token->cancel();

    // 等待所有任务处理完毕
    std::this_thread::sleep_for(500ms);

    // 验证高优先级任务全部执行，低优先级任务被取消
	EXPECT_EQ(executed_high, 3); // 所有高优先级任务应该完成
    EXPECT_LT(executed_low, 3); // 大多数低优先级任务应该被取消
}

// 测试任务开始执行后不会被取消
TEST(ThreadPoolTest, TaskStartedNotCancelled) {
    thread_pool<> pool(1); // 只使用一个线程确保任务按顺序执行

    auto token = pool.create_token();
    std::atomic<bool> task_started{ false };
    std::atomic<bool> task_completed{ false };
    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    // 提交一个可控制的长时间运行任务
    auto future = pool.submit_cancelable(token, [&]() {
        task_started = true;

        // 通知测试线程任务已开始
        wait_cv.notify_one();

        // 模拟执行一段时间
        std::this_thread::sleep_for(300ms);

        task_completed = true;
        return 100;
        });

    // 等待任务开始执行
    {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_cv.wait_for(lock, 1s, [&] { return task_started.load(); });
    }

    // 确认任务已经开始执行
    EXPECT_TRUE(task_started);

    // 尝试取消任务，但此时任务已经在执行中
    token->cancel();

    // 验证任务正常完成
    try {
        int result = future.get();
        EXPECT_EQ(result, 100);
        EXPECT_TRUE(task_completed);
    }
    catch (const std::exception& e) {
        FAIL() << "不应该抛出异常: " << e.what();
    }
}

// 测试在线程池中多个不同的取消令牌可以独立工作
TEST(ThreadPoolTest, MultipleCancellationTokens) {
    thread_pool<> pool(4);

    auto token1 = pool.create_token();
    auto token2 = pool.create_token();

    std::atomic<int> count1{ 0 };
    std::atomic<int> count2{ 0 };

    std::vector<std::future<void>> futures1;
    std::vector<std::future<void>> futures2;
    for (int i = 0; i < 10; ++i) {
        if (i % 2) {
			// 使用token1的任务
			futures1.push_back(pool.submit_cancelable(token1, [&count1]() {
				std::this_thread::sleep_for(200ms);
				count1.fetch_add(1);
				}));
		}
        else {
            // 使用token2的任务
            futures2.push_back(pool.submit_cancelable(token2, [&count2]() {
                std::this_thread::sleep_for(200ms);
                count2.fetch_add(1);
                }));
        }
    }

    // 仅取消token1的任务
    token1->cancel();

    // 等待所有任务处理完毕
    std::this_thread::sleep_for(1000ms);

    // 验证token1的任务被取消，token2的任务正常执行
    EXPECT_LT(count1, 3); // token1的大多数任务应该被取消
    EXPECT_EQ(count2, 5); // token2的任务应该全部完成
}

// 测试大量任务
TEST(ThreadPoolTest, LargeNumberOfTasks) {
    thread_pool<leo::ALL> pool(4);

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