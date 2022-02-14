
#include "sqlconnpool.h"
using namespace std;

SqlConnPool::SqlConnPool()
{
	useCount_ = 0;
	freeCount_ = 0;
}

SqlConnPool* SqlConnPool::Instance()
{
	// 单例模式
	static SqlConnPool connPool;  // 声明一个静态SqlConnPool对象
	return &connPool;
}

void SqlConnPool::Init(const char* host, int port,  // 服务器ip, 服务器的sql端口
	const char* user, const char* pwd, // mysqlserver用户名,密码
	const char* dbName, // 仓库名称
	int connSize = 10)
{
	assert(connSize > 0);  // sql链接池, 实现链接重用
	for (int i = 0; i < connSize; i++)
	{
		MYSQL* sql = nullptr;
		sql = mysql_init(sql);
		if (!sql)
		{
			LOG_ERROR("MySql init error!");
			assert(sql);
		}
		sql = mysql_real_connect(sql, host,
			user, pwd,
			dbName, port, nullptr, 0);
		if (!sql)
		{
			LOG_ERROR("MySql Connect error!");
		}
		connQue_.push(sql);  // 创建connSize个MYSQL*对象,加入队列
	}
	MAX_CONN_ = connSize;  // 限制最大连接数
	sem_init(&semId_, 0, MAX_CONN_);  // 创建匿名信号量
	// MAX_CONN_作为信号量的初始值，用来限制最大连接数
}

MYSQL* SqlConnPool::GetConn()
{
	MYSQL* sql = nullptr;
	if (connQue_.empty())
	{
		LOG_WARN("SqlConnPool busy!");
		return nullptr;
	}
	sem_wait(&semId_);
	/*
	 * 信号量上锁, 信号量值-1
	 * 连接数+1
	 * 信号量值为0时阻塞
	 */
	{
		lock_guard<mutex> locker(mtx_);
		sql = connQue_.front();
		connQue_.pop();
	}
	return sql;
}

void SqlConnPool::FreeConn(MYSQL* sql)
{
	assert(sql);
	lock_guard<mutex> locker(mtx_);
	connQue_.push(sql);
	sem_post(&semId_);  // 信号量值加1
}

void SqlConnPool::ClosePool()
{
	lock_guard<mutex> locker(mtx_);
	while (!connQue_.empty())
	{
		auto item = connQue_.front();
		connQue_.pop();
		mysql_close(item);
	}
	mysql_library_end();
}

int SqlConnPool::GetFreeConnCount()
{
	lock_guard<mutex> locker(mtx_);
	return connQue_.size();
}

SqlConnPool::~SqlConnPool()
{
	ClosePool();
}
