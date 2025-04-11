#include "../thread_pool.hpp"
#include <iostream>
#include <format>
#include <thread>
#include <vector>

// 简单的任务函数，带有可选的休眠时间
void simple_task(int id, std::chrono::milliseconds sleep_time = 100ms) {
    std::this_thread::sleep_for(sleep_time);
    std::cout << std::format("Task {} completed on thread {}\n",
        id, std::this_thread::get_id());
}

// 示例1：基本用法 - 默认静态线程池
void basic_usage_example() {
    std::cout << "\n=== Basic ===\n";

    // 创建默认线程池
    leo::thread_pool<> pool(4); // 4个线程
    std::atomic_int counter{ 0 };

    {
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
        std::cout << std::format("Task returned: {}\n", result);

        // 提交带参数的任务
        auto future2 = pool.submit([&counter](int a, int b) {
            std::this_thread::sleep_for(150ms);
            counter.fetch_add(1);
            return a + b;
            }, 10, 32);

        std::cout << std::format("Sum task returned: {}\n", future2.get());

        // 等待所有任务完成
        pool.wait_all();
    }
    std::cout << std::format("{} tasks finished", counter.load());
}

// 示例2：动态线程池
void dynamic_pool_example() {
    std::cout << "\n=== Dynamic ===\n";

    // 创建动态线程池，初始为2个线程，超时1秒，检查间隔100毫秒
    leo::thread_pool<leo::ThreadPoolPolicy::DYNAMIC> pool(2, 1000ms, 100ms);

    std::cout << std::format("Initial pool size: {}\n", pool.get_pool_size());

    // 提交8个长时间运行的任务，迫使线程池扩容
    std::vector<std::future<void>> tasks;
    for (int i = 0; i < 8; ++i) {
        tasks.push_back(pool.submit([i]() {
            std::cout << std::format("Started long task {} on thread {}\n",
                i, std::this_thread::get_id());
            std::this_thread::sleep_for(500ms);
            std::cout << std::format("Completed long task {} on thread {}\n",
                i, std::this_thread::get_id());
            }));
    }

    // 等待一会儿让所有任务开始执行
    std::this_thread::sleep_for(200ms);

    // 检查线程池是否扩容
    std::cout << std::format("Pool size after submitting tasks: {}\n", pool.get_pool_size());

    // 等待所有任务完成
    for (auto& task : tasks) {
        task.wait();
    }

    // 等待超时让多余线程退出
    std::cout << "Waiting for threads to timeout...\n";
    std::this_thread::sleep_for(2s);

    // 检查线程池是否收缩
    std::cout << std::format("Final pool size after timeout: {}\n", pool.get_pool_size());
}

// 示例3：任务优先级
void priority_tasks_example() {
    std::cout << "\n=== Priority ===\n";

    // 创建支持优先级的线程池
    leo::thread_pool<leo::ThreadPoolPolicy::PRIORITY> pool(2);

    // 提交不同优先级的任务（数字越大优先级越高）
    auto low_task = pool.submit(1, []() {
        std::cout << "Low priority task started\n";
        std::this_thread::sleep_for(300ms);
        std::cout << "Low priority task completed\n";
        });

    auto high_task = pool.submit(10, []() {
        std::cout << "High priority task started\n";
        std::this_thread::sleep_for(300ms);
        std::cout << "High priority task completed\n";
        });

    auto normal_task = pool.submit(5, []() {
        std::cout << "Normal priority task started\n";
        std::this_thread::sleep_for(300ms);
        std::cout << "Normal priority task completed\n";
        });

    // 等待所有任务完成
    high_task.wait();
    normal_task.wait();
    low_task.wait();
}

// 示例4：工作窃取策略
void work_stealing_example() {
    std::cout << "\n=== Work Stealing ===\n";

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

    std::cout << std::format("Sum of all task results: {} (should be 4950)\n", sum);
}

// 示例5：组合策略 - 动态线程池 + 工作窃取 + 优先级
void combined_strategy_example() {
    std::cout << "\n=== Combined ===\n";

    // 创建使用所有策略的线程池
    leo::thread_pool<leo::ThreadPoolPolicy::ALL> pool(2, 1000ms, 100ms);

    // 提交不同优先级的任务
    pool.submit(10, []() {
        std::cout << "High priority task running\n";
        std::this_thread::sleep_for(300ms);
        });

    // 提交大量任务以触发动态扩容
    for (int i = 0; i < 6; ++i) {
        pool.submit(5, [i]() {
            std::cout << std::format("Normal task {} started\n", i);
            std::this_thread::sleep_for(200ms);
            std::cout << std::format("Normal task {} completed\n", i);
            });
    }

    pool.submit(10, []() {
        std::cout << "High priority task running\n";
        std::this_thread::sleep_for(300ms);
        });

    // 输出当前线程池大小
    std::this_thread::sleep_for(100ms);
    std::cout << std::format("Current pool size: {}\n", pool.get_pool_size());

    // 等待任务完成并让线程池收缩
    std::this_thread::sleep_for(2s);
    std::cout << std::format("Final pool size: {}\n", pool.get_pool_size());
}

// 示例6：使用线程池进行并行计算
void parallel_computation_example() {
    std::cout << "\n=== Parallel ===\n";

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

    std::cout << std::format("Sum of {} elements: {} (computed in {}ms)\n",
        size, total, duration.count());
}

int main() {
    // 运行所有示例
    basic_usage_example();
    dynamic_pool_example();
    priority_tasks_example();
    work_stealing_example();
    combined_strategy_example();
    parallel_computation_example();

    return 0;
}
