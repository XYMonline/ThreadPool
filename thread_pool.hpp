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

class cancel_token {
public:
	cancel_token() : cancelled_(false) {}

	bool is_cancelled() const {
		return cancelled_.load();
	}

	void cancel() {
		cancelled_.store(true);
	}

	void check_cancel() const {
		if (is_cancelled()) {
			throw std::runtime_error("Operation was cancelled");
		}
	}

private:
	std::atomic<bool> cancelled_;
};

using cancel_token_ptr = std::shared_ptr<cancel_token>;

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
		chrono::milliseconds time_step = 1s,
		uint32_t max_size = std::thread::hardware_concurrency()
	)
		: thread_timeout_(timeout)
		, thread_step_(time_step)
		, max_threads_(max_size)
	{
		available_slots_.reserve(max_size);
		for (uint32_t i = max_size; i > 0; --i) {
			available_slots_.push_back(i);
		}
		available_slots_.push_back(0);

		if (pool_size > max_size) {
			pool_size = max_size;
		}

		if (pool_size == 0) {
			pool_size = std::thread::hardware_concurrency();
		}

		if constexpr (Policy & ThreadPoolPolicy::DYNAMIC) {
			threads_.reserve(std::max(pool_size, max_size));
		}

		for (uint32_t i = 0; i < pool_size; ++i) {
			auto t = create_thread();
			if (t) {
				t->start();
			}
		}
	}

	~thread_pool() { if (running_) destroy(); }

	thread_pool(const thread_pool&) = delete;
	thread_pool(thread_pool&&) = default;
	thread_pool& operator=(const thread_pool&) = delete;
	thread_pool& operator=(thread_pool&&) = default;

	template <typename Func, typename... Args>
	auto submit(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		return submit_impl(0, nullptr, std::forward<Func>(func), std::forward<Args>(args)...);
	}

	template <typename Func, typename... Args>
	auto submit(priority_type priority, Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
			return submit_impl(priority, nullptr, std::forward<Func>(func), std::forward<Args>(args)...);
		}
		else {
			return submit_impl(0, nullptr, std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}

	template <typename Func, typename... Args>
	auto submit_cancelable(cancel_token_ptr token, Func&& func, Args&&... args)
		-> std::future<decltype(func(args...))> {
		return submit_impl(0, std::move(token), std::forward<Func>(func), std::forward<Args>(args)...);
	}

	template <typename Func, typename... Args>
	auto submit_cancelable(priority_type priority, cancel_token_ptr token, Func&& func, Args&&... args)
		-> std::future<decltype(func(args...))> {
		if constexpr (Policy & ThreadPoolPolicy::PRIORITY) {
			return submit_impl(priority, std::move(token), std::forward<Func>(func), std::forward<Args>(args)...);
		}
		else {
			return submit_impl(0, std::move(token), std::forward<Func>(func), std::forward<Args>(args)...);
		}
	}

	cancel_token_ptr create_token() { return std::make_shared<cancel_token>(); }

	void destroy() {
		running_ = false;
		uint32_t worker_slot = 0;

		bool is_worker_thread = wrapped_thread::is_worker();

		if (is_worker_thread) {
			std::unique_lock<std::mutex> lock(pool_mtx_);
			threads_[worker_slot]->set_cleanup_status(true);

			for (auto& thread : threads_) {
				if (thread && thread->get_slot() != worker_slot) {
					thread->notify();
				}
			}
		}
		else {
			std::unique_lock<std::mutex> lock(pool_mtx_);

			for (auto& thread : threads_) {
				if (thread) {
					thread->notify();
				}
			}

			pool_cv_.wait(lock, [this] { return thread_count_.load() == 0; });
		}
	}

	auto create_thread() {
		std::unique_lock<std::mutex> lock(pool_mtx_);

		if (thread_count_ < max_threads_) {
			auto slot = available_slots_.back();
			available_slots_.pop_back();
			auto worker = std::make_unique<wrapped_thread>(
				std::bind(&thread_pool::worker_func, this, std::placeholders::_1),
				slot,
				this);
			threads_.emplace_back(std::move(worker));
			thread_count_.fetch_add(1);
			return threads_.back().get();
		}
		return (wrapped_thread*)nullptr;
	}

	size_t get_pool_size() const { return thread_count_; }

	void wait_all() {
		bool is_worker_thread = wrapped_thread::is_worker();
		if (is_worker_thread) {
			throw std::runtime_error("Cannot call wait_all from worker thread");
		}
		std::unique_lock<std::mutex> lock(pool_mtx_);
		pool_cv_.wait(lock, [this] { return remaining_tasks_ == 0; });
	}

private:
	struct warpped_task {
		std::function<void()> task_;
		priority_type priority_;
		cancel_token_ptr cancel_ptr_;

		explicit warpped_task(std::function<void()> t = nullptr, priority_type p = 0, cancel_token_ptr cancel_ptr = nullptr)
			: task_(std::move(t)), priority_(p), cancel_ptr_(cancel_ptr) { }

		bool operator<(const warpped_task& other) const { return priority_ < other.priority_; }

		bool operator>(const warpped_task& other) const { return priority_ > other.priority_; }

		bool is_cancelled() const { return cancel_ptr_ && cancel_ptr_->is_cancelled(); }

		void exec() { if (task_) task_(); }
	};

	template<typename Derived, ThreadPoolPolicy P>
	class queue_base {
	public:
		using task_type = typename thread_pool<P>::task_type;

		void push(task_type&& task) { static_cast<Derived*>(this)->push_impl(std::forward<task_type>(task)); }

		bool pop(task_type& task) { return static_cast<Derived*>(this)->pop_impl(task); }

		bool empty() const { return static_cast<Derived*>(this)->empty_impl(); }

		void wait(const std::atomic<bool>& running) { static_cast<Derived*>(this)->wait_impl(running); }

		void wait_for(chrono::milliseconds time, const std::atomic<bool>& running) { static_cast<Derived*>(this)->wait_for_impl(time, running); }

		void notify() { static_cast<Derived*>(this)->notify_impl(); }

		auto size() const { return static_cast<const Derived*>(this)->size_impl(); }
	};

	template<ThreadPoolPolicy P>
	class local_queue : public queue_base<local_queue<P>, P> {
	public:
		using task_type = typename thread_pool<P>::task_type;
		using queue_container_t = std::conditional_t<
			static_cast<bool>(P & ThreadPoolPolicy::PRIORITY),
			std::priority_queue<task_type, std::vector<task_type>, std::less<>>,
			std::queue<task_type>
		>;

		local_queue(const void* pool_ptr) {}

		~local_queue() {
			std::lock_guard<std::mutex> lock(mtx_);
			cv_.notify_all();
		}

		friend queue_base<local_queue<P>, P>;
	private:

		void push_impl(task_type&& task) {
			std::lock_guard<std::mutex> lock(mtx_);
			tasks_.push(std::forward<task_type>(task));
			cv_.notify_one();
		}

		bool pop_impl(task_type& task) {
			task.task_ = nullptr;
			std::lock_guard<std::mutex> lock(mtx_);

			if (!tasks_.empty()) {
				if constexpr (P & ThreadPoolPolicy::PRIORITY) {
					task = tasks_.top();
				} else {
					task = std::move(tasks_.front());
				}
				tasks_.pop();
			}

			return task.task_ != nullptr;
		}

		bool empty_impl() const {
			std::lock_guard<std::mutex> lock(mtx_);
			return tasks_.empty();
		}

		void wait_impl(const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(mtx_);
			cv_.wait(lock, [this, &running] { return !tasks_.empty() || !running; });
		}

		void wait_for_impl(chrono::milliseconds time, const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(mtx_);
			cv_.wait_for(lock, time, [this, &running] { return !tasks_.empty() || !running; });
		}

		void notify_impl() {
			std::lock_guard<std::mutex> lock(mtx_);
			cv_.notify_one();
		}

		auto size_impl() const {
			std::lock_guard<std::mutex> lock(mtx_);
			return tasks_.size();
		}

	private:
		queue_container_t tasks_;
		mutable std::mutex mtx_;
		std::condition_variable cv_;
	};

	template<ThreadPoolPolicy P>
	class global_queue : public queue_base<global_queue<P>, P> {
	public:
		using task_type = typename thread_pool<P>::task_type;
		using queue_container_t = std::conditional_t<
			static_cast<bool>(P & ThreadPoolPolicy::PRIORITY),
			std::priority_queue<task_type, std::vector<task_type>, std::less<>>,
			std::queue<task_type>
		>;

		struct shared_resources {
			std::shared_ptr<queue_container_t> tasks;
			std::shared_ptr<std::mutex> mtx;
			std::shared_ptr<std::condition_variable> cv;
			std::atomic<int> ref_count{0};

			shared_resources() 
				: tasks(std::make_shared<queue_container_t>())
				, mtx(std::make_shared<std::mutex>())
				, cv(std::make_shared<std::condition_variable>()) {}
		};

		static std::unordered_map<const void*, std::shared_ptr<shared_resources>>& get_resource_map() {
			static std::unordered_map<const void*, std::shared_ptr<shared_resources>> resource_map;
			return resource_map;
		}

		static std::mutex& get_resource_map_mutex() {
			static std::mutex resource_map_mutex;
			return resource_map_mutex;
		}

		static std::shared_ptr<shared_resources> create_or_get_resources(const void* pool_ptr) {
			std::lock_guard<std::mutex> lock(get_resource_map_mutex());
			auto& resource_map = get_resource_map();
			
			auto it = resource_map.find(pool_ptr);
			if (it == resource_map.end()) {
				auto resources = std::make_shared<shared_resources>();
				resource_map[pool_ptr] = resources;
				return resources;
			}
			return it->second;
		}

		static void remove_resources(const void* pool_ptr) {
			std::lock_guard<std::mutex> lock(get_resource_map_mutex());
			auto& resource_map = get_resource_map();
			resource_map.erase(pool_ptr);
		}

		explicit global_queue(const void* pool_ptr) 
			: pool_ptr_(pool_ptr) {
			resources_ = create_or_get_resources(pool_ptr);
			resources_->ref_count.fetch_add(1);
		}

		~global_queue() {
			if (resources_) {
				int prev_count = resources_->ref_count.fetch_sub(1);
				
				if (resources_->mtx && resources_->cv) {
					std::lock_guard<std::mutex> lock(*resources_->mtx);
					resources_->cv->notify_all();
				}

				if (prev_count == 1) {
					remove_resources(pool_ptr_);
				}
			}
		}

		global_queue(const global_queue& other) = delete;
		global_queue(global_queue&& other) = delete;
		global_queue& operator=(const global_queue& other) = delete;
		global_queue& operator=(global_queue&& other) = delete;

		friend queue_base<global_queue<P>, P>;
	private:
		void push_impl(task_type&& task) {
			std::lock_guard<std::mutex> lock(*resources_->mtx);
			resources_->tasks->push(std::forward<task_type>(task));
			resources_->cv->notify_one();
		}

		bool pop_impl(task_type& task) {
			task.task_ = nullptr;
			std::lock_guard<std::mutex> lock(*resources_->mtx);

			if (!resources_->tasks->empty()) {
				if constexpr (P & ThreadPoolPolicy::PRIORITY) {
					task = resources_->tasks->top();
				} else {
					task = std::move(resources_->tasks->front());
				}
				resources_->tasks->pop();
			}

			return task.task_ != nullptr;
		}

		bool empty_impl() const {
			std::lock_guard<std::mutex> lock(*resources_->mtx);
			return resources_->tasks->empty();
		}

		void wait_impl(const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(*resources_->mtx);
			resources_->cv->wait(lock, [this, &running] { return !resources_->tasks->empty() || !running; });
		}

		void wait_for_impl(chrono::milliseconds time, const std::atomic<bool>& running) {
			std::unique_lock<std::mutex> lock(*resources_->mtx);
			resources_->cv->wait_for(lock, time, [this, &running] { return !resources_->tasks->empty() || !running; });
		}

		void notify_impl() {
			std::lock_guard<std::mutex> lock(*resources_->mtx);
			resources_->cv->notify_one();
		}

		auto size_impl() const {
			std::lock_guard<std::mutex> lock(*resources_->mtx);
			return resources_->tasks->size();
		}

	private:
		const void* pool_ptr_;
		std::shared_ptr<shared_resources> resources_;
	};

	// 最终使用时的队列选择器
	template<ThreadPoolPolicy P>
	using wrapped_queue = std::conditional_t<
		static_cast<bool>(P & ThreadPoolPolicy::WORK_STEALING),
		local_queue<P>,
		global_queue<P>
	>;

	class wrapped_thread {
		using thread_work = std::function<void(wrapped_thread&)>;
		static inline thread_local wrapped_thread* current_worker = nullptr;
	public:
		explicit wrapped_thread(thread_work func, uint32_t slot, const void* pool_ptr)
			: func_(std::move(func)) 
			, slot_(slot)
			, cleanup_thread_(false)
			, queue_(pool_ptr)
		{ }

		~wrapped_thread() { current_worker = nullptr; }

		void start() {
			auto t = std::thread([this] { 
				current_worker = this;
				func_(*this); 
				});
			t.detach();
		}

		static bool is_worker() { return current_worker != nullptr; }

		static wrapped_thread* get_current() { return current_worker; }

		void push(task_type&& task) { queue_.push(std::forward<task_type>(task)); }

		bool try_pop(task_type& task) { return queue_.pop(task); }

		void wait(const std::atomic<bool>& running) { queue_.wait(running); }

		void wait_for(chrono::milliseconds time, const std::atomic<bool>& running) { queue_.wait_for(time, running); }

		void notify() { queue_.notify(); }

		auto queue_size() const { return queue_.size(); }

		auto get_slot() const { return slot_; }

		// provide a way to steal tasks from this thread
		bool try_steal(task_type& task) {
			if constexpr (Policy & ThreadPoolPolicy::WORK_STEALING) {
				return queue_.pop(task);
			}
			return false;
		}

		void set_cleanup_status(bool status) { cleanup_thread_ = status; }

		bool should_cleanup() const { return cleanup_thread_; }

	private:
		wrapped_queue<Policy> queue_;
		uint32_t slot_;
		thread_work func_;
		bool cleanup_thread_;
	};

private:
	bool try_acquire_task(wrapped_thread& worker, task_type& task) {
		static std::mt19937 rng(std::random_device{}());

		if (worker.try_pop(task)) {
			return true;
		}

		if constexpr (Policy & ThreadPoolPolicy::WORK_STEALING) {
			if (thread_count_ <= 1) {
				return false;
			}
			std::unique_lock lock(pool_mtx_);
			const size_t max_attempts = std::min<size_t>(thread_count_, 3);
			for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
				auto victim_id = std::uniform_int_distribution<size_t>(0, thread_count_ - 1)(rng);

				// avoid self
				if (threads_[victim_id] && threads_[victim_id]->get_slot() != worker.get_slot()) {
					if (threads_[victim_id]->try_steal(task)) {
						return true;
					}
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
					task.exec();
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
						if (thread_count_ > 1 && worker.queue_size() == 0) {
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
			task.exec();
			remaining_tasks_.fetch_sub(1);
			pool_cv_.notify_all();
			if constexpr (Policy & ThreadPoolPolicy::DYNAMIC) {
				idle_since = chrono::high_resolution_clock::now();
			}
			idle_threads_.fetch_add(1);
		}

		idle_threads_.fetch_sub(1);
		auto slot = worker.get_slot();

		if (worker.should_cleanup()) {
			std::unique_lock<std::mutex> lock(pool_mtx_);
			for (auto& th : threads_) {
				if (th && th->get_slot() != slot) {
					th->notify();
				}
			}

			pool_cv_.wait(lock, [this, slot]() {
				for (auto& th : threads_) {
					if (th && th->get_slot() != slot) {
						return false;
					}
				}
				return true;
				});

			threads_[slot].reset(nullptr);
			available_slots_.push_back(slot);
			thread_count_.fetch_sub(1);
			pool_cv_.notify_all();
		}
		else {
			std::unique_lock<std::mutex> lock(pool_mtx_);
			threads_[slot].reset(nullptr);
			available_slots_.push_back(slot);
			thread_count_.fetch_sub(1);
			if (thread_count_.load() == 0) {
				pool_cv_.notify_all();
			}
		}
	}

	template <typename Func, typename... Args>
	auto submit_impl(priority_type priority, cancel_token_ptr token, Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
		static std::mt19937 rng(std::random_device{}());
		using return_type = decltype(func(args...));
		std::shared_ptr<std::packaged_task<return_type()>> task;

		if (token) {
			task = std::make_shared<std::packaged_task<return_type()>>(
				[func = std::forward<Func>(func),
				args_tuple = std::make_tuple(std::forward<Args>(args)...),
				token]() -> return_type {
					if (token && token->is_cancelled()) {
						throw std::runtime_error("The task was canceled.");
					}
					return std::apply(func, args_tuple);
				}
			);
		}
		else {
			task = std::make_shared<std::packaged_task<return_type()>>(
				std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
			);
		}

		auto res = task->get_future();
		if (wrapped_thread::is_worker() && (idle_threads_.load() == 0)) { // if we are in a worker thread and no idle threads are available
			(*task)(); // execute the task immediately
			return res;
		}


		warpped_task wrapped{ [task]() { (*task)(); }, priority, token };
		bool need_new_thread = false;
		if constexpr (Policy & ThreadPoolPolicy::DYNAMIC) {
			need_new_thread = idle_threads_ == 0 && thread_count_ < std::thread::hardware_concurrency();
		}

		if (need_new_thread) {
			auto t = create_thread();
			if (t) {
				t->push(std::move(wrapped));
				t->start();
			}
		}
		else {
			if constexpr (Policy & ThreadPoolPolicy::WORK_STEALING) {
				if (wrapped_thread::is_worker()) {
					auto* current = wrapped_thread::get_current();
					if (current) {
						current->push(std::move(wrapped));
						remaining_tasks_.fetch_add(1);
						return res;
					}
				}

				std::unique_lock<std::mutex> lock(pool_mtx_);
				auto slot = std::uniform_int_distribution<size_t>(0, thread_count_ - 1)(rng);
				threads_[slot]->push(std::move(wrapped));
			}
			else {
				std::unique_lock<std::mutex> lock(pool_mtx_);
				auto slot = std::uniform_int_distribution<size_t>(0, thread_count_ - 1)(rng);
				threads_[slot]->push(std::move(wrapped));
			}
		}

		remaining_tasks_.fetch_add(1);
		return res;
	}

	auto queue_size() const {
		size_t total_size = 0;
		for (const auto& thread : threads_) {
			if (thread) {
				total_size += thread->queue_size();
			}
		}
		return total_size;
	}

private:
	std::vector<std::unique_ptr<wrapped_thread>>	threads_;
	std::mutex										pool_mtx_;
	std::condition_variable							pool_cv_;
	std::atomic<bool>								running_{ true };
	std::atomic<size_t>								remaining_tasks_{ 0 };
	std::atomic<uint32_t>							idle_threads_{ 0 };
	std::atomic<uint32_t>							thread_count_{ 0 };
	std::vector<uint32_t> 							available_slots_;
	const uint32_t									max_threads_;
	const chrono::milliseconds						thread_timeout_;
	const chrono::milliseconds						thread_step_;
};

}; // namespace leo
