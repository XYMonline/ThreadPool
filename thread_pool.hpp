#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <unordered_map>

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;

namespace leo {
namespace chrono = std::chrono;

enum ThreadPoolPolicy {
	DEFAULT							= 0,									// static
	DYNAMIC							= 1,									// use dynamic pool size
	PRIORITY						= 2,									// use task priority
	WORK_STEALING					= 4,									// use work stealing
	DYNAMIC_PRIORITY				= DYNAMIC | PRIORITY,
	WORK_STEALING_PRIORITY			= WORK_STEALING | PRIORITY,
	WORK_STEALING_DYNAMIC			= WORK_STEALING | DYNAMIC,
	ALL								= WORK_STEALING | DYNAMIC | PRIORITY,
};

using priority_type = int;

template<ThreadPoolPolicy Policy = ThreadPoolPolicy::DEFAULT>
class thread_pool {
	struct warpped_task;
public:
	using task_type = warpped_task;

	explicit thread_pool(uint32_t pool_size = std::thread::hardware_concurrency(), 
		chrono::milliseconds timeout = 60s, 
		chrono::milliseconds time_step = 1s
	)
		: thread_timeout_(timeout)
		, thread_step_(time_step)
	{
		pool_size = std::min(pool_size, std::thread::hardware_concurrency());

		for (uint32_t i = 0; i < pool_size; ++i) {
			auto id = thread_id_gen_.fetch_add(1);
			auto worker = std::make_unique<wrapped_thread>(
				std::bind(&thread_pool::worker_func, this, std::placeholders::_1),
				id);
			worker->start();
			threads_.emplace(id, std::move(worker));
			thread_count_.fetch_add(1);
		}
	}

	~thread_pool() {
		if (running_) {
			destroy();
		}
	}

	thread_pool(const thread_pool&) = delete;
	thread_pool(thread_pool&&) = default;
	thread_pool& operator=(const thread_pool&) = delete;
	thread_pool& operator=(thread_pool&&) = default;

	template <typename Func, typename... Args>
	auto submit(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		return submit_impl(0, std::forward<Func>(func), std::forward<Args>(args)...);
	}

	template <typename Func, typename... Args>
	auto submit(priority_type priority, Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
			return submit_impl(priority, std::forward<Func>(func), std::forward<Args>(args)...);
		}
		else {
			return submit_impl(0, std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}

	void destroy() {
		running_ = false;

		for (auto& [id, thread] : threads_) {
			thread->notify();
		}

		std::unique_lock<std::mutex> lock(pool_mtx_);
		pool_cv_.wait(lock, [this] { return threads_.empty(); });
	}

	size_t get_pool_size() const {
		return thread_count_;
	}

	void wait_all() {
		std::unique_lock<std::mutex> lock(pool_mtx_);
		pool_cv_.wait(lock, [this] { return remaining_tasks_ == 0; });
	}

private:
	struct warpped_task {
		std::function<void()> task;
		priority_type priority;

		explicit warpped_task(std::function<void()> t = nullptr, priority_type p = 0)
			: task(std::move(t)) , priority(p) { }

		bool operator<(const warpped_task& other) const {
			return priority < other.priority; 
		}

		bool operator>(const warpped_task& other) const {
			return priority > other.priority;
		}
	};

	class wrapped_queue {
	public:
		wrapped_queue()
			: normal_tasks_(nullptr)
			, priority_tasks_(nullptr)
		{
			if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
				priority_tasks_ = std::make_unique<std::priority_queue<task_type, std::vector<task_type>, std::less<>>>();
			}
			else {
				normal_tasks_ = std::make_unique<std::queue<task_type>>();
			}
		}

		void push(task_type&& task) {
			std::lock_guard<std::mutex> lock(queue_mtx_);
			if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
				priority_tasks_->push(std::forward<task_type>(task));
			}
			else {
				normal_tasks_->push(std::forward<task_type>(task));
			}
		}

		bool pop(task_type& task) {
			task.task = nullptr;
			std::lock_guard<std::mutex> lock(queue_mtx_);
			if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
				if (!priority_tasks_->empty()) {
					task = priority_tasks_->top();
					priority_tasks_->pop();
				}
			}
			else {
				if (!normal_tasks_->empty()) {
					task = std::move(normal_tasks_->front());
					normal_tasks_->pop();
				}
			}
			return task.task != nullptr;
		}

		bool empty() const {
			std::lock_guard<std::mutex> lock(queue_mtx_);
			if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
				return priority_tasks_->empty();
			}
			else {
				return normal_tasks_->empty();
			}
		}

		size_t size() const {
			std::unique_lock<std::mutex> lock(queue_mtx_);
			if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
				return priority_tasks_->size();
			}
			else {
				return normal_tasks_->size();
			}
		}

	private:
		std::unique_ptr<std::priority_queue<task_type, std::vector<task_type>, std::less<>>> priority_tasks_;
		std::unique_ptr<std::queue<task_type>> normal_tasks_;
		mutable std::mutex queue_mtx_;
	};

	class wrapped_thread {
		using thread_work = std::function<void(wrapped_thread&)>;
	public:
		explicit wrapped_thread(thread_work func, size_t id) 
			: func_(std::move(func)) 
			, id_(id)
		{ }

		void start() {
			auto t = std::thread([this] { func_(*this); });
			t.detach();
		}

		void push(task_type&& task) {
			std::lock_guard<std::mutex> lock(thread_mtx_);
			queue_.push(std::forward<task_type>(task));
			cv_.notify_one();
		}

		bool try_pop(task_type& task) {
			return queue_.pop(task);
		}

		void wait(const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(thread_mtx_);
			cv_.wait(lock, [this, &running] { return !queue_.empty() || !running; });
		}

		void wait_for(chrono::milliseconds time, const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(thread_mtx_);
			cv_.wait_for(lock, time, [this, &running] { return !queue_.empty() || !running; });
		}

		void notify() {
			std::lock_guard<std::mutex> lock(thread_mtx_);
			cv_.notify_one();
		}

		auto get_id() const {
			return id_;
		}

		auto size() const {
			return queue_.size();
		}

		// provide a way to steal tasks from this thread
		bool try_steal(task_type& task) {
			if constexpr (Policy & ThreadPoolPolicy::WORK_STEALING) {
				std::lock_guard<std::mutex> lock(thread_mtx_);
				if (queue_.empty()) {
					return false;
				}
				return queue_.pop(task);
			}
			return false;
		}

	private:
		wrapped_queue queue_;
		size_t id_;
		std::mutex thread_mtx_;
		std::condition_variable cv_;
		thread_work func_;
	};

private:
	bool try_acquire_task(wrapped_thread& worker, task_type& task) {
		static std::mt19937 rng(std::random_device{}());

		if (worker.try_pop(task)) {
			return true;
		}

		if constexpr (Policy & ThreadPoolPolicy::WORK_STEALING) {
			std::unique_lock lock(pool_mtx_);
			auto victim_id = std::uniform_int_distribution<size_t>(0, thread_count_ - 1)(rng);
			auto it = std::next(threads_.begin(), victim_id);
			if (it->first != worker.get_id()) {
				if (it->second->try_steal(task)) {
					return true;
				}
			}
		}

		return false;
	}

	void worker_func(wrapped_thread& worker) {
		auto idle_since = chrono::high_resolution_clock::now();
		idle_threads_.fetch_add(1);

		while (running_) {
			task_type task;
			bool got_task = try_acquire_task(worker, task);

			if (!running_) {
				// exit after task is done
				if (got_task) { 
					task.task();
					remaining_tasks_.fetch_sub(1);
					pool_cv_.notify_all();
				}
				break;
			}

			if (!got_task) {
				if constexpr (Policy & ThreadPoolPolicy::DYNAMIC) {
					worker.wait_for(thread_step_, running_);
					if (chrono::high_resolution_clock::now() - idle_since > thread_timeout_) {
						std::unique_lock<std::mutex> lock(pool_mtx_);
						if (thread_count_ > 1 && !worker.size()) {
							break;
						}
					}
				}
				else {
					worker.wait(running_);
				}
				continue;
			}

			idle_threads_.fetch_sub(1);
			task.task();
			remaining_tasks_.fetch_sub(1);
			pool_cv_.notify_all();
			idle_since = chrono::high_resolution_clock::now();
			idle_threads_.fetch_add(1);
		}

		std::unique_lock<std::mutex> lock(pool_mtx_);
		thread_count_.fetch_sub(1);
		threads_.erase(worker.get_id());
		idle_threads_.fetch_sub(1);
		if (threads_.empty()) {
			pool_cv_.notify_all();
		}
	}

	template <typename Func, typename... Args>
	auto submit_impl(priority_type priority, Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		static std::mt19937 rng(std::random_device{}());
		using return_type = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<return_type()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
		);
		warpped_task wrapped{ [task]() { (*task)(); }, priority };
		auto res = task->get_future();

		bool need_new_thread = false;
		if constexpr (Policy & ThreadPoolPolicy::DYNAMIC) {
			need_new_thread = idle_threads_ == 0 && thread_count_ < std::thread::hardware_concurrency();
		}

		if (need_new_thread) {
			auto id = thread_id_gen_.fetch_add(1);
			auto worker = std::make_unique<wrapped_thread>(
				std::bind(&thread_pool::worker_func, this, std::placeholders::_1),
				id);
			worker->push(std::move(wrapped));
			worker->start();
			{
				std::unique_lock<std::mutex> lock(pool_mtx_);
				threads_.emplace(id, std::move(worker));
			}
			thread_count_.fetch_add(1);
		}
		else {
			std::unique_lock<std::mutex> lock(pool_mtx_);
			auto it = std::next(threads_.begin(),
				std::uniform_int_distribution<size_t>(0, thread_count_ - 1)(rng));
			it->second->push(std::move(wrapped));
		}

		remaining_tasks_.fetch_add(1);
		return res;
	}


private:
	std::unordered_map<size_t, std::unique_ptr<wrapped_thread>> threads_;
	std::mutex													pool_mtx_;
	std::condition_variable										pool_cv_;
	std::atomic<bool>											running_{ true };
	std::atomic<size_t>											thread_id_gen_{ 0 };
	std::atomic<size_t>											idle_threads_{ 0 };
	std::atomic<size_t>											thread_count_{ 0 };
	std::atomic<size_t>											remaining_tasks_{ 0 };
	const chrono::milliseconds									thread_timeout_;
	const chrono::milliseconds									thread_step_;
};

}; // namespace leo

