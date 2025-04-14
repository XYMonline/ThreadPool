#include "../thread_pool.hpp"
#include <iostream>
#include <print>
#include <thread>
#include <vector>

// 简单的任务函数，带有可选的休眠时间
void simple_task(int id, std::chrono::milliseconds sleep_time = 100ms) {
    std::this_thread::sleep_for(sleep_time);
    std::println("Task {} completed on thread {}",
        id, std::this_thread::get_id());
}

// 示例1：基本用法 - 默认静态线程池
void basic_usage_example() {
    std::println("\n=== Basic ===\n");

    std::atomic_int counter{ 0 };

    {
        // 创建默认线程池
        leo::thread_pool<> pool(4); // 4个线程

        auto res = pool.submit([] {
            throw std::runtime_error("Error in task");
            });

        try {
			res.get(); // 等待任务完成并获取结果
		}
        catch (const std::exception& e) {
            std::println("Caught exception: {}", e.what());
        }

        // 提交不需要返回值的任务
        for (int i = 0; i < 10; ++i) {
            pool.submit([i, &counter]() {
                simple_task(i);
                counter.fetch_add(1);
                });
        }

        // 提交有返回值的任务
        auto future = pool.submit([&counter]() {
            std::this_thread::sleep_for(200ms);
            counter.fetch_add(1);
            return 42;
            });

        // 等待并获取结果
        int result = future.get();
        std::println("Task returned: {}", result);

        // 提交带参数的任务
        auto future2 = pool.submit([&counter](int a, int b) {
            std::this_thread::sleep_for(150ms);
            counter.fetch_add(1);
            return a + b;
            }, 10, 32);

        std::println("Sum task returned: {}", future2.get());

        // 等待所有任务完成
        pool.wait_all();
    }
    std::println("{} tasks finished", counter.load());
}

// 示例2：动态线程池
void dynamic_pool_example() {
    std::println("\n=== Dynamic ===\n");

    // 创建动态线程池，初始为2个线程，超时1秒，检查间隔100毫秒
    leo::thread_pool<leo::ThreadPoolPolicy::DYNAMIC> pool(2, 1000ms, 100ms);

    std::println("Initial pool size: {}", pool.get_pool_size());

    // 提交8个长时间运行的任务，迫使线程池扩容
    std::vector<std::future<void>> tasks;
    for (int i = 0; i < 8; ++i) {
        tasks.push_back(pool.submit([i]() {
            std::println("Started long task {} on thread {}",
                i, std::this_thread::get_id());
            std::this_thread::sleep_for(500ms);
            std::println("Completed long task {} on thread {}",
                i, std::this_thread::get_id());
            }));
    }

    // 等待一会儿让所有任务开始执行
    std::this_thread::sleep_for(200ms);

    // 检查线程池是否扩容
    std::println("Pool size after submitting tasks: {}", pool.get_pool_size());

    // 等待所有任务完成
    for (auto& task : tasks) {
        task.wait();
    }

    // 等待超时让多余线程退出
    std::println("Waiting for threads to timeout...");
    std::this_thread::sleep_for(2s);

    // 检查线程池是否收缩
    std::println("Final pool size after timeout: {}", pool.get_pool_size());
}

// 示例3：任务优先级
void priority_tasks_example() {
    std::println("\n=== Priority ===\n");

    // 创建支持优先级的线程池
    leo::thread_pool<leo::ThreadPoolPolicy::PRIORITY> pool(2);

    // 提交不同优先级的任务（数字越大优先级越高）
    auto low_task = pool.submit(1, []() {
        std::println("Low priority task started");
        std::this_thread::sleep_for(300ms);
        std::println("Low priority task completed");
        });

    auto high_task = pool.submit(10, []() {
        std::println("High priority task started");
        std::this_thread::sleep_for(300ms);
        std::println("High priority task completed");
        });

    auto normal_task = pool.submit(5, []() {
        std::println("Normal priority task started");
        std::this_thread::sleep_for(300ms);
        std::println("Normal priority task completed");
        });

    // 等待所有任务完成
    high_task.wait();
    normal_task.wait();
    low_task.wait();
}

// 示例4：工作窃取策略
void work_stealing_example() {
    std::println("\n=== Work Stealing ===\n");

    // 创建支持工作窃取的线程池
    leo::thread_pool<leo::ThreadPoolPolicy::WORK_STEALING> pool(4);

    // 提交大量短任务，使工作窃取机制显现出来
    std::vector<std::future<int>> results;

    for (int i = 0; i < 100; ++i) {
        results.push_back(pool.submit([i]() {
            // 模拟不均衡的工作负载
            if (i % 10 == 0) {
                std::this_thread::sleep_for(50ms);
            }
            else {
                std::this_thread::sleep_for(10ms);
            }
            return i;
            }));
    }

    // 获取并验证结果
    int sum = 0;
    for (auto& result : results) {
        sum += result.get();
    }

    std::println("Sum of all task results: {} (should be 4950)", sum);
}

// 示例5：组合策略 - 动态线程池 + 工作窃取 + 优先级
void combined_strategy_example() {
    std::println("\n=== Combined ===\n");

    // 创建使用所有策略的线程池
    leo::thread_pool<leo::ThreadPoolPolicy::ALL> pool(2, 1000ms, 100ms);

    // 提交不同优先级的任务
    pool.submit(10, []() {
        std::println("High priority task running");
        std::this_thread::sleep_for(300ms);
        });

    // 提交大量任务以触发动态扩容
    for (int i = 0; i < 6; ++i) {
        pool.submit(5, [i]() {
            std::println("Normal task {} started", i);
            std::this_thread::sleep_for(200ms);
            std::println("Normal task {} completed", i);
            });
    }

    pool.submit(10, []() {
        std::println("High priority task running");
        std::this_thread::sleep_for(300ms);
        });

    // 输出当前线程池大小
    std::this_thread::sleep_for(100ms);
    std::println("Current pool size: {}\n", pool.get_pool_size());

    // 等待任务完成并让线程池收缩
    std::this_thread::sleep_for(2s);
    std::println("Final pool size: {}\n", pool.get_pool_size());
}

// 示例6：使用线程池进行并行计算
void parallel_computation_example() {
    std::println("\n=== Parallel ===\n");

    // 创建线程池
    leo::thread_pool<> pool(std::thread::hardware_concurrency());

    // 并行计算大数组的总和
    constexpr size_t size = 10'000'000;
    std::vector<int> data(size, 1); // 全是1的数组

    // 分块计算和
    constexpr size_t num_tasks = 10;
    constexpr size_t block_size = size / num_tasks;

    std::vector<std::future<size_t>> partial_sums;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_tasks; ++i) {
        size_t start_idx = i * block_size;
        size_t end_idx = (i + 1) * block_size;

        partial_sums.push_back(pool.submit([&data, start_idx, end_idx]() {
            size_t sum = 0;
            for (size_t j = start_idx; j < end_idx; ++j) {
                sum += data[j];
            }
            return sum;
            }));
    }

    // 收集部分和
    size_t total = 0;
    for (auto& future : partial_sums) {
        total += future.get();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::println("Sum of {} elements: {} (computed in {}ms)\n",
        size, total, duration.count());
}

// 示例7：任务取消
void cancellation_example() {
    std::println("\n=== Task Cancellation ===\n");

    leo::thread_pool<> pool(2);

    // 基础取消示例
    {
        std::println("Basic cancellation example:");

        leo::thread_pool pool(1);

        // 创建取消令牌
        auto token1 = pool.create_token();
        auto token2 = pool.create_token();

        // 提交第一个任务但不马上取消
        auto res1 = pool.submit_cancelable(token1, [] {
            std::this_thread::sleep_for(200ms);
            std::println("Task1 complete");
            });

        // 让第一个任务有机会开始执行
        std::this_thread::sleep_for(50ms);

        // 提交第二个任务并立即取消（它应该还在队列中）
        auto res2 = pool.submit_cancelable(token2, [] {
            std::this_thread::sleep_for(200ms);
            std::println("Task2 complete");
            });
        token2->cancel(); // 取消第二个任务

        try {
            res1.get(); // 等待任务完成并获取结果
            std::println("Task1 completed successfully");
        }
        catch (const std::exception& e) {
            std::println("Task1 caught exception: {}", e.what());
        }

        try {
            res2.get(); // 等待任务完成并获取结果
        }
        catch (const std::exception& e) {
            std::println("Task2 caught exception: {}", e.what());
        }
    }

    // 批量任务取消示例
    {
        std::println("\nBatch cancellation example:\n");

        // 使用更多线程使示例更明确
        leo::thread_pool<> batch_pool(4);
        auto token = batch_pool.create_token();

        // 提交多个任务（前几个可能开始执行，后几个应该还在队列中）
        std::vector<std::future<int>> futures;
        for (int i = 0; i < 10; ++i) {
            futures.push_back(batch_pool.submit_cancelable(token, [i]() {
                std::println("Task {} started", i);
                std::this_thread::sleep_for(300ms);
                std::println("Task {} completed", i);
                return i * 10;
                }));
        }

        // 让一些任务有机会开始执行
        std::this_thread::sleep_for(50ms);

        // 取消所有剩余的任务
        std::println("Cancelling all pending tasks...");
        token->cancel();

        // 尝试获取所有结果
        for (int i = 0; i < futures.size(); ++i) {
            try {
                int result = futures[i].get();
                std::println("Task {} result: {}", i, result);
            }
            catch (const std::exception& e) {
                std::println("Task {} cancelled: {}", i, e.what());
            }
        }
    }

    // 优先级取消示例
    {
        std::println("\nPriority with cancellation example:\n");

        leo::thread_pool<leo::ThreadPoolPolicy::PRIORITY> priority_pool(2);
        auto token = priority_pool.create_token();

        // 先提交一些会执行的高优先级任务
        std::vector<std::future<void>> high_priority_tasks;
        for (int i = 0; i < 2; ++i) {
            high_priority_tasks.push_back(priority_pool.submit(10, [i]() {
                std::println("High priority task {} running", i);
                std::this_thread::sleep_for(200ms);
                std::println("High priority task {} completed", i);
                }));
        }

        // 让高优先级任务先开始执行
        std::this_thread::sleep_for(50ms);

        // 提交低优先级可取消任务（应该仍在队列中）
        std::vector<std::future<void>> low_priority_tasks;
        for (int i = 0; i < 5; ++i) {
            low_priority_tasks.push_back(priority_pool.submit_cancelable(1, token, [i]() {
                std::println("Low priority task {} running", i);
                std::this_thread::sleep_for(300ms);
                std::println("Low priority task {} completed", i);
                }));
        }

        // 立即取消低优先级任务（它们应该还在队列中）
        std::println("Cancelling pending low priority tasks...");
        token->cancel();

        // 等待所有高优先级任务完成
        for (auto& task : high_priority_tasks) {
            task.wait();
        }
        std::println("All high priority tasks completed");

        // 检查低优先级任务是否被取消
        for (int i = 0; i < low_priority_tasks.size(); ++i) {
            try {
                low_priority_tasks[i].get();
                std::println("Low priority task {} completed unexpectedly", i);
            }
            catch (const std::exception& e) {
                std::println("Low priority task {} cancelled as expected: {}", i, e.what());
            }
        }
    }

    // 协作式取消任务
    {
        leo::thread_pool pool(1);
		auto token = pool.create_token();
		auto res = pool.submit_cancelable(token, [token] {
			std::println("Task started");
			for (int i = 0; i < 10; ++i) {
				token->check_cancel(); // 检查取消状态
				std::this_thread::sleep_for(100ms);
				std::println("Working... {}0%", i);
			}
			std::println("Task completed");
			});
		// 让任务有机会开始执行
		std::this_thread::sleep_for(300ms);
		// 取消任务
		token->cancel();
		try {
			res.get(); // 等待任务完成并获取结果
		}
		catch (const std::exception& e) {
			std::println("Task cancelled: {}", e.what());
		}
    }
}

int main() {
    // 运行所有示例
    basic_usage_example();
    dynamic_pool_example();
    priority_tasks_example();
    work_stealing_example();
    combined_strategy_example();
    parallel_computation_example();
	cancellation_example();

    return 0;
}
