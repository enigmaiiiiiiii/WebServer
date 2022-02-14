
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>

class ThreadPool
{
 public:
	explicit ThreadPool(size_t threadCount = 8) : pool_(std::make_shared<Pool>())
	{
		assert(threadCount > 0);
		for (size_t i = 0; i < threadCount; i++)
		{
			std::thread([pool = pool_]
			{
			  std::unique_lock<std::mutex> locker(pool->mtx);
			  while (true)
			  {
				  if (!pool->tasks.empty())
				  {
					  auto task = std::move(pool->tasks.front());
					  pool->tasks.pop();
					  locker.unlock();
					  task();
					  locker.lock();
				  }
				  else if (pool->isClosed) break;
				  else pool->cond.wait(locker);
			  }
			}).detach();
		}
	}

	ThreadPool() = default;  // 默认构造函数

	ThreadPool(ThreadPool&&) = default;

	~ThreadPool()
	{  // 析构，
		if (static_cast<bool>(pool_))
		{
			{
				std::lock_guard<std::mutex> locker(pool_->mtx);
				pool_->isClosed = true;  // 关闭线程池
			}
			pool_->cond.notify_all();
		}
	}

	template<class F>
	void AddTask(F&& task)
	{
		{
			std::lock_guard<std::mutex> locker(pool_->mtx);
			pool_->tasks.emplace(std::forward<F>(task));
		}
		pool_->cond.notify_one();
	}

 private:
	struct Pool
	{
		std::mutex mtx;  // 互斥量
		std::condition_variable cond; // 条件量
		bool isClosed;  //
		std::queue<std::function<void()>> tasks;  // 任务
	};
	std::shared_ptr<Pool> pool_;
};

#endif //THREADPOOL_H