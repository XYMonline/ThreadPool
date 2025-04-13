# ThreadPool

`ThreadPool` 是一个高效、灵活的 C++ 线程池实现，支持多种线程池策略，包括动态线程池大小、任务优先级和工作窃取等特性。它旨在简化多线程任务的管理，提高程序的并发性能。

## 特性

- **多种线程池策略**：
  - `DEFAULT`：静态线程池。
  - `DYNAMIC`：动态调整线程池大小。
  - `PRIORITY`：支持任务优先级。
  - `WORK_STEALING`：支持线程间的任务窃取。
  - 组合策略：如 `DYNAMIC_PRIORITY`、`WORK_STEALING_PRIORITY` 等。

- **任务提交**：
  - 支持通过 `submit` 方法提交任务。
  - 可选任务优先级（在启用 `PRIORITY` 策略时）。

- **动态线程管理**：
  - 在启用 `DYNAMIC` 策略时，线程池会根据负载动态创建或销毁线程。

- **工作窃取**：
  - 在启用 `WORK_STEALING` 策略时，线程可以从其他线程队列中窃取任务以提高资源利用率。

- **杂项**:
  - 支持通过 `submit_cancelable` 提交可取消的任务，允许在任务开始执行之前取消任务。
  - 提供 `wait_all` 等待所有任务完成。

## 使用方法

[示例代码](example/example.cpp) 

[测试](test/thread_pool_tests.cpp)

## TODO

- 优化任务入队逻辑
- 实现更高效的工作窃取功能
- 提供更多的测试
- 添加`module`版本