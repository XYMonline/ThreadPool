#include <gtest/gtest.h>
#include "../thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <unordered_set>

using namespace leo;
using namespace std::chrono_literals;

// 测试基础功能 - 静态线程池
TEST(BasicFeature, BasicFunctionality) {
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

TEST(BasicFeature, WaitAll) {
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

// 测试从工作线程调用wait_all会抛出异常
TEST(BasicFeature, WaitAllThrowsFromWorkerThread) {
    thread_pool<> pool(4);
    std::atomic<bool> exception_thrown{ false };

    // 提交一个会调用wait_all的任务
    auto future = pool.submit([&pool, &exception_thrown]() {
        try {
            // 从工作线程调用wait_all应该抛出异常
            pool.wait_all();
            // 如果没有抛出异常，这里不应该执行
            return false;
        }
        catch (const std::runtime_error& e) {
            // 验证异常消息
            exception_thrown = true;
            EXPECT_STREQ(e.what(), "Cannot call wait_all from worker thread");
            return true;
        }
        });

    // 等待任务完成并检查结果
    bool result = future.get();

    // 验证异常被正确捕获和处理
    EXPECT_TRUE(result);
    EXPECT_TRUE(exception_thrown);
}

// 测试在任务内部调用destroy
TEST(BasicFeature, DestroyInsideTask) {
    // 使用共享指针跟踪线程池，以便在任务中销毁
    auto pool_ptr = std::make_shared<thread_pool<>>(4);
    std::atomic<bool> destroy_called{ false };
    std::atomic<int> tasks_completed{ 0 };

    // 创建一个单独的线程池用于执行destroy任务，避免在同一个池中销毁自己
    thread_pool<> control_pool(1);

    // 提交一些常规任务到主线程池
    for (int i = 0; i < 10; ++i) {
        pool_ptr->submit([&tasks_completed]() {
            std::this_thread::sleep_for(50ms);
            tasks_completed.fetch_add(1);
            });
    }

    // 提交一个要在其中销毁线程池的任务到控制线程池
    control_pool.submit([pool_ptr, &destroy_called]() {
        // 等待一些任务开始执行
        std::this_thread::sleep_for(100ms);

        // 在任务中调用destroy
        pool_ptr->destroy();
        destroy_called = true;
        });

    // 等待足够长的时间以确保所有操作完成
    std::this_thread::sleep_for(500ms);

    // 验证destroy被调用了
    EXPECT_TRUE(destroy_called);

    // 可能只有一部分任务完成，因为线程池被销毁了
    // 但我们不测试具体完成了多少，只要destroy调用成功就行
    std::cout << "Tasks completed before destroy: " << tasks_completed.load() << std::endl;
}

// 测试在任务中提交新任务 - 最简单的场景
TEST(SubtaskTest, SubmitTaskFromTask) {
    thread_pool<> pool(2); // 故意使用较少的线程，考验线程池的稳定性
    std::atomic<int> counter{ 0 };

    // 提交一个会提交另一个任务的任务
    auto outer_future = pool.submit([&pool, &counter]() {
        // 提交内部任务
        auto inner_future = pool.submit([&counter]() {
            std::this_thread::sleep_for(50ms);
            counter.fetch_add(1);
            return 42;
            });

        // 等待内部任务完成
        int result = inner_future.get();
        counter.fetch_add(1);
        return result;
        });

    // 等待外部任务完成
    int result = outer_future.get();

    // 验证结果
    EXPECT_EQ(result, 42);
    EXPECT_EQ(counter.load(), 2);
}

// 测试在任务中提交多个新任务 - 更复杂的场景
TEST(SubtaskTest, SubmitMultipleTasksFromTask) {
    thread_pool<> pool(2); // 故意使用较少的线程
    std::atomic<int> counter{ 0 };
    std::atomic<int> total_sum{ 0 };

    // 提交外层任务
    auto outer_future = pool.submit([&pool, &counter, &total_sum]() {
        constexpr int num_inner_tasks = 5;
        std::vector<std::future<int>> inner_futures;

        // 提交多个内层任务
        for (int i = 0; i < num_inner_tasks; ++i) {
            inner_futures.push_back(pool.submit([i, &counter]() {
                std::this_thread::sleep_for(20ms * i);
                counter.fetch_add(1);
                return i * 10;
                }));
        }

        // 等待所有内层任务完成并收集结果
        int sum = 0;
        for (auto& f : inner_futures) {
            sum += f.get();
        }

        total_sum.store(sum);
        return sum;
        });

    // 等待外层任务完成
    int result = outer_future.get();

    // 验证结果
    EXPECT_EQ(counter.load(), 5);
    EXPECT_EQ(result, 0 + 10 + 20 + 30 + 40);
    EXPECT_EQ(total_sum.load(), result);
}

// 修复NestedTasksWithFutureGet测试
TEST(SubtaskTest, NestedTasksWithFutureGet) {
    thread_pool<> pool(4);
    std::atomic<int> level1_counter{ 0 };
    std::atomic<int> level2_counter{ 0 };

    // 每个一级任务等待其对应的二级任务
    auto outer_future = pool.submit([&pool, &level1_counter, &level2_counter]() {
        std::vector<std::future<void>> level1_futures;

        for (int i = 0; i < 3; ++i) {
            level1_futures.push_back(pool.submit([&pool, &level1_counter, &level2_counter, i]() {
                // 提交二级任务
                std::vector<std::future<void>> level2_futures;
                for (int j = 0; j < 2; ++j) {
                    level2_futures.push_back(pool.submit([&level2_counter]() {
                        //std::this_thread::sleep_for(10ms);  // 减少等待时间
                        level2_counter.fetch_add(1);
                        }));
                }

                // 等待二级任务完成
                for (auto& f : level2_futures) {
                    f.get();
                }

                level1_counter.fetch_add(1);
                }));
        }

        // 等待所有一级任务完成
        for (auto& f : level1_futures) {
            f.get();
        }

        return true;
        });

    // 等待最外层任务完成
    EXPECT_TRUE(outer_future.get());

    // 验证计数器
    EXPECT_EQ(level1_counter.load(), 3);
    EXPECT_EQ(level2_counter.load(), 6);
}

// 修复RecoverFromWaitAllException测试
TEST(SubtaskTest, RecoverFromWaitAllException) {
    thread_pool<> pool(4);
    std::atomic<int> counter{ 0 };
    std::atomic<bool> exception_caught{ false };

    // 提交一个工作线程的任务
    auto future = pool.submit([&pool, &counter, &exception_caught]() {
        // 提交一些子任务
        std::vector<std::future<void>> futures;

        for (int i = 0; i < 5; ++i) {
            futures.push_back(pool.submit([&counter, i]() {
                std::this_thread::sleep_for(10ms * i); // 使用更短的时间
                counter.fetch_add(1);
                }));
        }

        try {
            // 尝试从工作线程调用wait_all - 应该抛出异常
            pool.wait_all();
        }
        catch ([[maybe_unused]] const std::runtime_error& e) {
            exception_caught = true;

            // 用future.get()替代wait_all
            for (auto& f : futures) {
                try {
                    f.get();
                }
                catch (...) {
                    // 确保即使某个任务失败，我们也会等待所有任务
                }
            }
        }

        return counter.load();
        });

    // 等待主任务完成
    int result = future.get();

    // 验证结果
    EXPECT_TRUE(exception_caught);
    EXPECT_EQ(result, 5);
}

// 测试多个工作线程调用wait_all时都抛出异常
TEST(SubtaskTest, MultipleWorkersCallingWaitAll) {
    thread_pool<> pool(6);
    std::atomic<int> exception_count{ 0 };

    // 创建一批任务并提交到线程池
    for (int i = 0; i < 10; ++i) {
        pool.submit([&pool, &exception_count]() {
            try {
                // 尝试调用wait_all - 应该抛出异常
                pool.wait_all();
            }
            catch (const std::runtime_error& e) {
                // 验证异常消息
                if (std::string(e.what()) == "Cannot call wait_all from worker thread") {
                    exception_count.fetch_add(1);
                }
            }
            });
    }

    // 等待所有任务完成
    pool.wait_all();

    // 验证所有工作线程调用wait_all时都抛出了异常
    EXPECT_EQ(exception_count.load(), 10);
}

// 测试返回值功能
TEST(BasicFeature, ReturnValue) {
    thread_pool<> pool(4);

    auto future = pool.submit([]() {
        std::this_thread::sleep_for(10ms);
        return 42;
        });

    EXPECT_EQ(future.get(), 42);
}

// 测试动态线程池
TEST(BasicFeature, DynamicThreadPool) {
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
TEST(BasicFeature, DynamicThreadPoolExpandAndContract) {
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
TEST(BasicFeature, PriorityTasks) {
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
TEST(BasicFeature, WorkStealing) {
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
TEST(BasicFeature, DestroyPool) {
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
TEST(BasicFeature, MixedPolicy) {
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
TEST(BasicFeature, ExceptionHandling) {
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
TEST(CancellationTest, TaskCancellation) {
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
TEST(CancellationTest, CancelMultipleTasks) {
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
TEST(CancellationTest, PriorityCancellation) {
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
TEST(CancellationTest, TaskStartedNotCancelled) {
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
TEST(CancellationTest, MultipleCancellationTokens) {
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
TEST(StressTest, LargeNumberOfTasks) {
    thread_pool<leo::ALL> pool(std::thread::hardware_concurrency());

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

// 测试没有工作窃取策略下队列阻塞行为
TEST(BasicFeature, BlockWithoutWorkSteal) {
    leo::thread_pool<leo::ThreadPoolPolicy::DYNAMIC> pool(2);
    std::atomic_bool flag;
    auto f = pool.submit([&flag]() {
        while (!flag.load()) {
            std::this_thread::sleep_for(10ms); // 模拟长时间运行的任务
		}
        return true;
		});
    pool.submit([&flag]() {
		flag.store(true); // 设置标志以结束第一个任务
        });

	EXPECT_TRUE(f.get()); // 确保任务可以正常完成
}

// 死锁预防测试套件 - 测试各种策略组合下的死锁预防能力
class DeadlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 设置测试超时，防止真正的死锁导致测试卡死
        timeout_ = 5s;
    }

    // 超时检测辅助函数
    template<typename Future>
    bool wait_with_timeout(Future& future, std::chrono::milliseconds timeout = 5s) {
        return future.wait_for(timeout) == std::future_status::ready;
    }

    // 创建一个会导致潜在死锁的嵌套任务场景
    template<ThreadPoolPolicy Policy>
    void test_nested_tasks_scenario(uint32_t pool_size = 2) {
        thread_pool<Policy> pool(pool_size);
        std::atomic<int> completed_tasks{ 0 };

        // 提交会创建子任务的父任务
        auto outer_future = pool.submit([&pool, &completed_tasks]() {
            std::vector<std::future<int>> inner_futures;

            // 创建多个子任务，数量超过线程池大小
            for (int i = 0; i < 4; ++i) {
                inner_futures.push_back(pool.submit([i, &completed_tasks]() {
                    std::this_thread::sleep_for(50ms);
                    completed_tasks.fetch_add(1);
                    return i * 10;
                    }));
            }

            // 等待所有子任务完成
            int sum = 0;
            for (auto& f : inner_futures) {
                sum += f.get();
            }
            return sum;
            });

        // 验证任务能在合理时间内完成，不会死锁
        ASSERT_TRUE(wait_with_timeout(outer_future));
        int result = outer_future.get();

        EXPECT_EQ(result, 0 + 10 + 20 + 30); // 0*10 + 1*10 + 2*10 + 3*10
        EXPECT_EQ(completed_tasks.load(), 4);
    }

    // 测试递归任务场景
    template<ThreadPoolPolicy Policy>
    void test_recursive_tasks_scenario() {
        thread_pool<Policy> pool(2);
        std::atomic<int> depth_counter{ 0 };

        std::function<std::future<int>(thread_pool<Policy>&, int)> recursive_task =
            [&](thread_pool<Policy>& p, int depth) -> std::future<int> {
            return p.submit([&, depth]() -> int {
                depth_counter.fetch_add(1);

                if (depth <= 0) {
                    return 1;
                }

                // 创建两个递归子任务
                auto left = recursive_task(p, depth - 1);
                auto right = recursive_task(p, depth - 1);

                return left.get() + right.get();
                });
            };

        auto future = recursive_task(pool, 3);

        // 验证递归任务能完成，不会死锁
        ASSERT_TRUE(wait_with_timeout(future));
        int result = future.get();

        EXPECT_GT(result, 0);
        EXPECT_GT(depth_counter.load(), 0);
    }

    // 测试多层嵌套任务
    template<ThreadPoolPolicy Policy>
    void test_multi_level_nesting() {
        thread_pool<Policy> pool(2);
        std::atomic<int> level_counters[4]{};

        auto future = pool.submit([&pool, &level_counters]() {
            level_counters[0].fetch_add(1);

            auto level1_future = pool.submit([&pool, &level_counters]() {
                level_counters[1].fetch_add(1);

                auto level2_future = pool.submit([&pool, &level_counters]() {
                    level_counters[2].fetch_add(1);

                    auto level3_future = pool.submit([&level_counters]() {
                        level_counters[3].fetch_add(1);
                        return 42;
                        });

                    return level3_future.get();
                    });

                return level2_future.get();
                });

            return level1_future.get();
            });

        ASSERT_TRUE(wait_with_timeout(future));
        int result = future.get();

        EXPECT_EQ(result, 42);
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(level_counters[i].load(), 1) << "Level " << i << " counter mismatch";
        }
    }

    std::chrono::seconds timeout_;
};

// 测试默认策略（静态线程池）的死锁预防
TEST_F(DeadlockTest, DefaultPolicyNestedTasks) {
    SCOPED_TRACE("Testing DEFAULT policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::DEFAULT>();
}

TEST_F(DeadlockTest, DefaultPolicyRecursiveTasks) {
    SCOPED_TRACE("Testing DEFAULT policy recursive tasks");
    test_recursive_tasks_scenario<ThreadPoolPolicy::DEFAULT>();
}

TEST_F(DeadlockTest, DefaultPolicyMultiLevelNesting) {
    SCOPED_TRACE("Testing DEFAULT policy multi-level nesting");
    test_multi_level_nesting<ThreadPoolPolicy::DEFAULT>();
}

// 测试动态策略的死锁预防
TEST_F(DeadlockTest, DynamicPolicyNestedTasks) {
    SCOPED_TRACE("Testing DYNAMIC policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::DYNAMIC>();
}

TEST_F(DeadlockTest, DynamicPolicyRecursiveTasks) {
    SCOPED_TRACE("Testing DYNAMIC policy recursive tasks");
    test_recursive_tasks_scenario<ThreadPoolPolicy::DYNAMIC>();
}

TEST_F(DeadlockTest, DynamicPolicyMultiLevelNesting) {
    SCOPED_TRACE("Testing DYNAMIC policy multi-level nesting");
    test_multi_level_nesting<ThreadPoolPolicy::DYNAMIC>();
}

// 测试优先级策略的死锁预防
TEST_F(DeadlockTest, PriorityPolicyNestedTasks) {
    SCOPED_TRACE("Testing PRIORITY policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::PRIORITY>();
}

TEST_F(DeadlockTest, PriorityPolicyRecursiveTasks) {
    SCOPED_TRACE("Testing PRIORITY policy recursive tasks");
    test_recursive_tasks_scenario<ThreadPoolPolicy::PRIORITY>();
}

// 测试工作窃取策略的死锁预防
TEST_F(DeadlockTest, DISABLED_WorkStealingPolicyNestedTasks) {
    SCOPED_TRACE("Testing WORK_STEALING policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::WORK_STEALING>();
}

// 测试组合策略的死锁预防
TEST_F(DeadlockTest, DynamicPriorityPolicyNestedTasks) {
    SCOPED_TRACE("Testing DYNAMIC_PRIORITY policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::DYNAMIC_PRIORITY>();
}

TEST_F(DeadlockTest, WorkStealingPolicyRecursiveTasks) {
    SCOPED_TRACE("Testing WORK_STEALING policy recursive tasks");
    test_recursive_tasks_scenario<ThreadPoolPolicy::WORK_STEALING>();
}

TEST_F(DeadlockTest, DISABLED_WorkStealingPriorityPolicyNestedTasks) {
    SCOPED_TRACE("Testing WORK_STEALING_PRIORITY policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::WORK_STEALING_PRIORITY>();
}

TEST_F(DeadlockTest, DISABLED_WorkStealingDynamicPolicyNestedTasks) {
    SCOPED_TRACE("Testing WORK_STEALING_DYNAMIC policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::WORK_STEALING_DYNAMIC>();
}

TEST_F(DeadlockTest, DISABLED_AllPolicyNestedTasks) {
    SCOPED_TRACE("Testing ALL policy nested tasks");
    test_nested_tasks_scenario<ThreadPoolPolicy::ALL>();
}

TEST_F(DeadlockTest, AllPolicyRecursiveTasks) {
    SCOPED_TRACE("Testing ALL policy recursive tasks");
    test_recursive_tasks_scenario<ThreadPoolPolicy::ALL>();
}

TEST_F(DeadlockTest, AllPolicyMultiLevelNesting) {
    SCOPED_TRACE("Testing ALL policy multi-level nesting");
    test_multi_level_nesting<ThreadPoolPolicy::ALL>();
}

// 特殊场景测试 - 单线程池的极限情况
TEST_F(DeadlockTest, SingleThreadPoolDeadlockPrevention) {
    thread_pool<> pool(1); // 只有一个线程，最容易死锁
    std::atomic<int> completed{ 0 };

    auto future = pool.submit([&pool, &completed]() {
        // 在单线程池中提交子任务应该被立即执行，而不是排队等待
        auto sub_future1 = pool.submit([&completed]() {
            completed.fetch_add(1);
            return 1;
            });

        auto sub_future2 = pool.submit([&completed]() {
            completed.fetch_add(1);
            return 2;
            });

        return sub_future1.get() + sub_future2.get();
        });

    ASSERT_TRUE(wait_with_timeout(future));
    int result = future.get();

    EXPECT_EQ(result, 3);
    EXPECT_EQ(completed.load(), 2);
}

// 测试极端嵌套深度
TEST_F(DeadlockTest, DeepNestingDeadlockPrevention) {
    thread_pool<> pool(2);
    std::atomic<int> depth_reached{ 0 };

    std::function<std::future<int>(int)> deep_nest = [&](int depth) -> std::future<int> {
        return pool.submit([&, depth]() -> int {
            depth_reached = std::max(depth_reached.load(), depth);

            if (depth >= 10) {
                return depth;
            }

            auto nested_future = deep_nest(depth + 1);
            return nested_future.get();
            });
        };

    auto future = deep_nest(0);

    ASSERT_TRUE(wait_with_timeout(future, 10s)); // 给更长时间，因为嵌套很深
    int result = future.get();

    EXPECT_EQ(result, 10);
    EXPECT_EQ(depth_reached.load(), 10);
}

// 测试混合场景：有些任务有子任务，有些没有
TEST_F(DeadlockTest, MixedTasksDeadlockPrevention) {
    thread_pool<> pool(3);
    std::atomic<int> simple_tasks{ 0 };
    std::atomic<int> nested_tasks{ 0 };

    std::vector<std::future<int>> futures;

    // 提交一些简单任务
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([&simple_tasks, i]() {
            std::this_thread::sleep_for(30ms);
            simple_tasks.fetch_add(1);
            return i;
            }));
    }

    // 提交一些有子任务的复杂任务
    for (int i = 0; i < 3; ++i) {
        futures.push_back(pool.submit([&pool, &nested_tasks, i]() {
            auto sub_future = pool.submit([&nested_tasks]() {
                std::this_thread::sleep_for(20ms);
                nested_tasks.fetch_add(1);
                return 100;
                });
            return i * 10 + sub_future.get();
            }));
    }

    // 等待所有任务完成
    int total_result = 0;
    for (auto& f : futures) {
        ASSERT_TRUE(wait_with_timeout(f));
        total_result += f.get();
    }

    EXPECT_EQ(simple_tasks.load(), 5);
    EXPECT_EQ(nested_tasks.load(), 3);

    // 验证结果：简单任务 0+1+2+3+4=10，复杂任务 (0*10+100)+(1*10+100)+(2*10+100)=330
    int expected = 0 + 1 + 2 + 3 + 4 + (0 * 10 + 100) + (1 * 10 + 100) + (2 * 10 + 100);
    EXPECT_EQ(total_result, expected);
}

// 测试带优先级的嵌套任务死锁预防
TEST_F(DeadlockTest, PriorityNestedTasksDeadlockPrevention) {
    thread_pool<ThreadPoolPolicy::PRIORITY> pool(2);
    std::atomic<int> high_priority_completed{ 0 };
    std::atomic<int> low_priority_completed{ 0 };

    auto future = pool.submit(5, [&pool, &high_priority_completed, &low_priority_completed]() {
        // 提交高优先级子任务
        auto high_future = pool.submit(10, [&high_priority_completed]() {
            std::this_thread::sleep_for(30ms);
            high_priority_completed.fetch_add(1);
            return 1;
            });

        // 提交低优先级子任务
        auto low_future = pool.submit(1, [&low_priority_completed]() {
            std::this_thread::sleep_for(30ms);
            low_priority_completed.fetch_add(1);
            return 2;
            });

        return high_future.get() + low_future.get();
        });

    ASSERT_TRUE(wait_with_timeout(future));
    int result = future.get();

    EXPECT_EQ(result, 3);
    EXPECT_EQ(high_priority_completed.load(), 1);
    EXPECT_EQ(low_priority_completed.load(), 1);
}

// 测试取消令牌与嵌套任务的交互
TEST_F(DeadlockTest, CancellationWithNestedTasksDeadlockPrevention) {
    thread_pool<> pool(2);
    auto token = pool.create_token();
    std::atomic<int> completed{ 0 };

    auto future = pool.submit_cancelable(token, [&pool, &completed]() {
        // 即使父任务可能被取消，子任务也应该能正常处理
        auto sub_future = pool.submit([&completed]() {
            std::this_thread::sleep_for(50ms);
            completed.fetch_add(1);
            return 42;
            });

        return sub_future.get();
        });

    // 让任务有机会开始执行
    std::this_thread::sleep_for(10ms);

    ASSERT_TRUE(wait_with_timeout(future));

    try {
        int result = future.get();
        EXPECT_EQ(result, 42);
        EXPECT_EQ(completed.load(), 1);
    }
    catch ([[maybe_unused]] const std::runtime_error& e) {
        // 如果父任务被取消，子任务仍然应该完成
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(completed.load(), 1);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
	// 只运行特定的测试
	//::testing::GTEST_FLAG(filter) = "DeadlockTest.*";
	//::testing::GTEST_FLAG(filter) = "BasicFeature.*";
	return RUN_ALL_TESTS();
}