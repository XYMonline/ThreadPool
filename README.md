# ThreadPool

`ThreadPool` 是一个高效、灵活的 C++ 线程池实现，支持多种线程池策略，包括动态线程池大小、任务优先级和工作窃取等特性。它旨在简化多线程任务的管理，提高程序的并发性能。

## 特性

- **多种线程池策略**：
  - `DEFAULT`：静态线程池。
  - `DYNAMIC`：动态调整线程池大小。
  - `PRIORITY`：支持任务优先级。
  - `WORK_STEALING`：支持线程间的任务窃取。
  - 组合策略：如 `DYNAMIC_PRIORITY`、`WORK_STEALING_PRIORITY` 等。

- **异步等待**
  - 将任务提交到线程池时，线程池会返回一个future，可以通过`future::get`等待任务完成并获取结果。
  - 支持使用`thread_pool::wait_all`方法等待线程池中的所有任务完成。

- **任务取消**：
  - 创建可取消的任务：通过`thread_pool::create_token`方法生成一个token并使用`thread_pool::submit_cancelable`提交任务，在任务执行时会先检查任务是否被提交方取消。
  - 取消任务后的处理：当任务被标记为取消时，包装后的任务会抛出`runtime_error`。任务提交方使用`future::get`获取任务结果时通过捕获异常的方式做任务被取消后的处理。
  - 协作式取消：任务提交方可以通过捕获`cancel_token`并在任务执行过程中使用`cancel_token::check_cancel`检查任务是否被取消，并进行后续的处理。

## 使用方法

[示例代码](example/example.cpp) 

[测试](test/thread_pool_tests.cpp) 部分测试可能出现失败的情况，原因可能是测试写的太严格或者线程池的实现问题，有空再仔细看看，大部分时候可以全部Pass。

## TODO

- 待修复bug
	- 无法在任务中提交子任务，暂时关闭相关的测试项
	- 不使用`WORK_STEALING`策略时可能出现任务阻塞队列，导致队列中的任务无法被执行
	- `wait_all`跟`destroy`的行为设置不一致，前者在任务中会直接抛出异常，后者会正常执行销毁线程池的逻辑。后续考虑修改
- 优化任务入队逻辑
- 实现更高效的工作窃取功能
- 提供更多的测试
- 添加`module`版本