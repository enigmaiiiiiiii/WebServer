#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H

#include <mutex>
#include <deque>
#include <condition_variable>
#include <sys/time.h>

// 线程安全队列

template<class T>
class BlockDeque
{
 public:
	explicit BlockDeque(size_t MaxCapacity = 1000);

	~BlockDeque();

	void clear();

	bool empty();

	bool full();

	void Close();

	size_t size();

	size_t capacity();

	T front();

	T back();

	void push_back(const T& item);

	void push_front(const T& item);

	bool pop(T& item);

	bool pop(T& item, int timeout);

	void flush();

 private:
	std::deque<T> deq_;

	size_t capacity_;

	std::mutex mtx_;  // 条件变量要求同一个mtx_

	bool isClose_;

	std::condition_variable condConsumer_;  // pop()操作

	std::condition_variable condProducer_;  // push()操作
};

template<class T>
BlockDeque<T>::BlockDeque(size_t MaxCapacity) :capacity_(MaxCapacity)
{
	assert(MaxCapacity > 0);
	isClose_ = false;
}

template<class T>
BlockDeque<T>::~BlockDeque()
{
	Close();
}

template<class T>
void BlockDeque<T>::Close()
{
	{
		std::lock_guard<std::mutex> locker(mtx_);
		deq_.clear();
		isClose_ = true;
	}
	condProducer_.notify_all();  // 通知所有生产者
	condConsumer_.notify_all();  // 通知所有消费者
}

template<class T>
void BlockDeque<T>::flush()
{
	condConsumer_.notify_one();  // 通知可进行pop()操作
}

template<class T>
void BlockDeque<T>::clear()
{
	std::lock_guard<std::mutex> locker(mtx_);
	deq_.clear();
}

template<class T>
T BlockDeque<T>::front()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return deq_.front();
}

template<class T>
T BlockDeque<T>::back()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return deq_.back();
}

template<class T>
size_t BlockDeque<T>::size()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return deq_.size();
}

template<class T>
size_t BlockDeque<T>::capacity()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return capacity_;
}

template<class T>
void BlockDeque<T>::push_back(const T& item)
{
	std::unique_lock<std::mutex> locker(mtx_);
	while (deq_.size() >= capacity_)
	{
		// 队列长度超过限制，阻塞等待
		condProducer_.wait(locker);
	}
	deq_.push_back(item);  // string push to deque
	condConsumer_.notify_one();
}

template<class T>
void BlockDeque<T>::push_front(const T& item)
{
	std::unique_lock<std::mutex> locker(mtx_);
	while (deq_.size() >= capacity_)
	{
		condProducer_.wait(locker);
	}
	deq_.push_front(item);
	condConsumer_.notify_one();
}

template<class T>
bool BlockDeque<T>::empty()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return deq_.empty();
}

template<class T>
bool BlockDeque<T>::full()
{
	std::lock_guard<std::mutex> locker(mtx_);
	return deq_.size() >= capacity_;
}

template<class T>
bool BlockDeque<T>::pop(T& item)
{
	// pop队列中日志字符串,保存到变量item
	std::unique_lock<std::mutex> locker(mtx_);
	while (deq_.empty())
	{
		// 如果队列为空
		condConsumer_.wait(locker);  // 阻塞等待locker
		if (isClose_)
		{
			return false;
		}
	}
	item = deq_.front();
	deq_.pop_front();
	condProducer_.notify_one();
	return true;
}

template<class T>
bool BlockDeque<T>::pop(T& item, int timeout)
{
	std::unique_lock<std::mutex> locker(mtx_);
	while (deq_.empty())
	{
		if (condConsumer_.wait_for(locker, std::chrono::seconds(timeout))
			== std::cv_status::timeout)
		{
			// 超时，返回false
			return false;
		}
		if (isClose_)
		{
			return false;
		}
	}
	item = deq_.front();
	deq_.pop_front();
	condProducer_.notify_one();
	return true;
}

#endif // BLOCKQUEUE_H