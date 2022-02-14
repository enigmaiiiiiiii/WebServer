#include "log.h"

using namespace std;

Log::Log()
{
	lineCount_ = 0;
	isAsync_ = false;
	writeThread_ = nullptr;
	deque_ = nullptr;
	toDay_ = 0;
	fp_ = nullptr;
}

Log::~Log()
{
	if (writeThread_ && writeThread_->joinable())
	{
		while (!deque_->empty())
		{
			deque_->flush();
		};
		deque_->Close();
		writeThread_->join();
	}
	if (fp_)
	{
		lock_guard<mutex> locker(mtx_);
		flush();
		fclose(fp_);
	}
}

int Log::GetLevel()
{
	lock_guard<mutex> locker(mtx_);
	return level_;
}

void Log::SetLevel(int level)
{
	lock_guard<mutex> locker(mtx_);
	level_ = level;
}

void Log::init(int level = 1,
	const char* path,
	const char* suffix,
	int maxQueueSize)
{
	isOpen_ = true;
	level_ = level;
	if (maxQueueSize > 0)
	{
		isAsync_ = true;  // 异步日志
		if (!deque_)
		{
			/*
			 * deque_为nullptr
			 * 为deque_分配空间
			 * 创建writeThread_线程
			 * 刷新缓冲
			 * 关闭现有文件指针
			 * 创建和打开文件指针
			 */
			unique_ptr<BlockDeque<std::string>> newDeque(new BlockDeque<std::string>);
//	  deque_ = move(newDeque);
			deque_.reset(newDeque.release());

			std::unique_ptr<std::thread> NewThread(new thread(FlushLogThread));
//	  writeThread_ = move(NewThread);
			writeThread_.reset(NewThread.release());
		}
	}
	else
	{
		isAsync_ = false;
	}

	lineCount_ = 0;

	// 设置文件名信息
	time_t timer = time(nullptr);
	struct tm* sysTime = localtime(&timer);
	struct tm t = *sysTime;
	path_ = path;
	suffix_ = suffix;
	char fileName[LOG_NAME_LEN] = { 0 };
	snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s",
		path_, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, suffix_);
	// 日志文件名： 路径/年_月_日
	toDay_ = t.tm_mday;

	{
		lock_guard<mutex> locker(mtx_);
		buff_.RetrieveAll();
		if (fp_)
		{
			// 将缓冲写入现有文件指针,
			flush();  // 通知deque_的pop操作, 将字符串写入fp_
			fclose(fp_);
		}

		fp_ = fopen(fileName, "a");
		// 已append模式打开fileName
		if (fp_ == nullptr)
		{
			// 文件目录不存在，则创建目录
			mkdir(path_, 0777);  // 权限，所有人可读可写可执行
			fp_ = fopen(fileName, "a");
		}
		printf("log file init path : %s\n", fileName);
		assert(fp_ != nullptr);  // fp创建失败，退出程序
	}
}

void Log::write(int level, const char* format, ...)
{
	// 写入deque_, 指向阻塞队列
	struct timeval now = { 0, 0 };
	gettimeofday(&now, nullptr);
	time_t tSec = now.tv_sec;
	struct tm* sysTime = localtime(&tSec);
	struct tm t = *sysTime;
	va_list vaList;

	/* 日志日期 日志行数 */
	if (toDay_ != t.tm_mday || (lineCount_ && (lineCount_ % MAX_LINES == 0)))
	{
		/* 是否需要创建新文件
		 * 系统日期改变到下一天, 或日志条数超过限制
		*/
		unique_lock<mutex> locker(mtx_);
		locker.unlock();

		char newFile[LOG_NAME_LEN];
		char tail[36] = { 0 };
		snprintf(tail, 36, "%04d_%02d_%02d",
			t.tm_year + 1900,  // 自1900年起
			t.tm_mon + 1,  // tm_mon表示月份, [0,11]
			t.tm_mday);  //

		if (toDay_ != t.tm_mday)
		{
			//
			snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s",
				path_,  // log
				tail,   // xxxx_xx_xx
				suffix_);  // .log
			toDay_ = t.tm_mday;
			lineCount_ = 0;
		}
		else
		{
			snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s",
				path_,  // log
				tail,   // xxxx_xx_xx
				(lineCount_ / MAX_LINES),  // 500001/500000 = 1
				suffix_);  // .log
		}

		locker.lock();  // 访问buff_和fp_的程序，需要加锁
		flush();  // 刷新buff_.buffer_, 强制写入fp_
		fclose(fp_);  // 关闭
		fp_ = fopen(newFile, "a");
		assert(fp_ != nullptr);
	}

	{
		unique_lock<mutex> locker(mtx_);
		lineCount_++;
		// print to buff_.BeginWrite(),readPos_
		// 将时间信息输入buff_
		int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
			t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);

		buff_.HasWritten(n);  // 更新buff_的writePos_位置
		AppendLogLevelTitle_(level);

		va_start(vaList, format);  //  format表示日志条目的内容格式, 初始化valist
		// snprintf的va_list版本
		int m = vsnprintf(buff_.BeginWrite(),  // print位置
			buff_.WritableBytes(),  // 最大空间 - writePos_
			format,
			vaList);
		// 日志, 主体信息(除时间，等级以外的内容)
		va_end(vaList);  // 回收vaList

		buff_.HasWritten(m);  // 更新buff_的writePos_位置
		buff_.Append("\n\0", 2);  /// 尾端 + "\n\0"，用来刷新缓冲区

		if (isAsync_ && deque_ && !deque_->full())
		{
			// 异步日志开启，将日志内容push_back到deque_
			deque_->push_back(buff_.RetrieveAllToStr());
		}
		else
		{
			fputs(buff_.Peek(), fp_);
		}
		buff_.RetrieveAll();
	}
}

void Log::AppendLogLevelTitle_(int level)
{
	switch (level)
	{
	case 0:
		buff_.Append("[debug]: ", 9);
		break;
	case 1:
		buff_.Append("[info] : ", 9);
		break;
	case 2:
		buff_.Append("[warn] : ", 9);
		break;
	case 3:
		buff_.Append("[error]: ", 9);
		break;
	default:
		buff_.Append("[info] : ", 9);
		break;
	}
}

void Log::flush()
{
	if (isAsync_)
	{
		deque_->flush();  // 通知deque_中条件等待的pop
	}
	fflush(fp_);  //  从缓冲强制写入fp_
}

void Log::AsyncWrite_()
{
	// 执行异步写日志动作
	string str = "";
	while (deque_->pop(str))
	{
		lock_guard<mutex> locker(mtx_);
		printf("%s", str.c_str());
		fputs(str.c_str(), fp_);
	}
}

Log* Log::Instance()
{
	/*
	 * Log对象，单例模式
	 * 多线程中每次Instance只需要调用创建好的对象
	 */
	static Log inst;
	return &inst;
}

void Log::FlushLogThread()
{
	// 属于回调函数
	Log::Instance()->AsyncWrite_();
}