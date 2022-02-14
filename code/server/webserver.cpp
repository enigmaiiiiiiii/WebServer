
#include "webserver.h"

using namespace std;

WebServer::WebServer(
	int port, int trigMode, int timeoutMS, bool OptLinger,
	int sqlPort, const char* sqlUser, const char* sqlPwd,
	const char* dbName, int connPoolNum, int threadNum,
	bool openLog, int logLevel, int logQueSize) :
	port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
	timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)),
	epoller_(new Epoller())
{

	srcDir_ = getcwd(nullptr, 256);  // 当前工作目录
	assert(srcDir_);
	strncat(srcDir_, "/ServerPage/", 16);  // 设置web资源目录:"/resources/"
	HttpConn::userCount = 0;     // 原子类型变量, 记录http连接用户数量
	HttpConn::srcDir = srcDir_;  // 设置httpconn中静态变量srcDir_

	// 初始化Sql连接池
	SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);  //

	/// epoll触发模式 电平 or 边界
	InitEventMode_(trigMode);

	/*
	 * 创建socket,并绑定, 监听,
	 * 设置非阻塞, 延时close属性
	 * 加入epoll监听
	 */
	if (!InitSocket_())
	{ isClose_ = true; }  // 初始化失败, isClose_ = true

	std::cout << "start server" << std::endl;

	if (openLog)
	{  // 日志是否可用
		std::cout << "log available" << std::endl;
		Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
		if (isClose_)
		{ LOG_ERROR("========== Server init error!=========="); }
		else
		{
			LOG_INFO("========== Server init ==========");
			LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger ? "true" : "false");
			LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
				(listenEvent_ & EPOLLET ? "ET" : "LT"),
				(connEvent_ & EPOLLET ? "ET" : "LT"));
			LOG_INFO("LogSys level: %d", logLevel);
			LOG_INFO("srcDir: %s", HttpConn::srcDir);
			LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
		}
	}
}

WebServer::~WebServer()
{
	close(listenFd_);
	isClose_ = true;
	free(srcDir_);
	SqlConnPool::Instance()->ClosePool();
}

void WebServer::InitEventMode_(int trigMode)
{
	// epoll 触发模式
	listenEvent_ = EPOLLRDHUP;
	connEvent_ = EPOLLONESHOT | EPOLLRDHUP;
	// 设置 为 监听文件挂断，可读, 且设置为该文件描述符只可被一个线程处理
	switch (trigMode)
	{
	case 0:
		break;
	case 1:
		connEvent_ |= EPOLLET;
		break;
	case 2:
		listenEvent_ |= EPOLLET;
		break;
	case 3:
		listenEvent_ |= EPOLLET;
		connEvent_ |= EPOLLET;
		break;
	default:
		// 默认触发模式全部边界触发
		listenEvent_ |= EPOLLET;
		connEvent_ |= EPOLLET;
		break;
	}
	HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start()
{
	// 服务器start表示开始处理各种io事件(文件描述符)
	int timeMS = -1;  /* epoll wait timeout == -1 无事件将阻塞 */
	if (!isClose_)
	{ LOG_INFO("========== Server start =========="); }
	while (!isClose_)
	{
		if (timeoutMS_ > 0)
		{
			timeMS = timer_->GetNextTick();
		}
		int eventCnt = epoller_->Wait(timeMS);  // 就绪socket个数
		for (int i = 0; i < eventCnt; i++)
		{
			/* 处理事件 */
			int fd = epoller_->GetEventFd(i);
			uint32_t events = epoller_->GetEvents(i);
			if (fd == listenFd_)
			{
				/// 如果就绪的事件是listenfd可读，创建acceptfd,并加入epoll监听
				DealListen_();
			}
			else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
			{  //
				/// 如果监听到的事件是读写挂断 或 出错
				assert(users_.count(fd) > 0);  // fd数量
				CloseConn_(&users_[fd]);
			}
			else if (events & EPOLLIN)
			{
				/// 处理可读事件
				assert(users_.count(fd) > 0);
				DealRead_(&users_[fd]);  // read任务加入线程池
			}
			else if (events & EPOLLOUT)
			{
				/// 处理可写事件
				assert(users_.count(fd) > 0);
				DealWrite_(&users_[fd]);  // write任务加入线程池
			}
			else
			{
				LOG_ERROR("Unexpected event");
			}
		}
	}
}

void WebServer::SendError_(int fd, const char* info)
{
	assert(fd > 0);
	int ret = send(fd, info, strlen(info), 0);
	if (ret < 0)
	{
		LOG_WARN("send error to client[%d] error!", fd);
	}
	close(fd);
}

void WebServer::CloseConn_(HttpConn* client)
{
	assert(client);
	LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d",
		client->GetFd(),
		client->GetIP(),
		client->GetPort(),
		(int)client->userCount);
	epoller_->DelFd(client->GetFd());  // 停止监听clientfd
	client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr)
{
	/*
	 * 由地址和监听的文件描述符初始化一个HttpConn对象, 加入unordered_map
	 * acceptfd加入监听, 监听连接关闭事件
	*/
	assert(fd > 0);
	/// user_[fd]是webserver各项任务的client参数, 一个描述符和一个地址
	// 创建了一个匿名HttpConn对象, client的地址为addr
	users_[fd].init(fd, addr);  // HttpConn::init

	if (timeoutMS_ > 0)
	{
		timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
	}
	epoller_->AddFd(fd, EPOLLIN | connEvent_);  // fd加入epoll监听，监听事件可读
	SetFdNonblock(fd);  // 设置fd非阻塞
	LOG_INFO("Client[%d](%s:%d) in!", users_[fd].GetFd(), users_[fd].GetIP(), users_[fd].GetPort());
}

void WebServer::DealListen_()
{
	struct sockaddr_in addr;
	// 存储被接收的另一端地址
	socklen_t len = sizeof(addr);
	// do...while 先执行一次后判断
	do
	{
		int fd = accept(listenFd_, (struct sockaddr*)&addr, &len);
		/*
		 * 返回一个新文件描述符fd
		 * 每个进程都会打开三个标准文件描述符0，1，2
		 * 对于服务器程序还会打开另外三个文件描述符:
		 * 3 anon_inode:[eventpoll]
		 * 4 socket:[]
		 * 5 日志文件.log
		 */
		if (fd <= 0)
		{ return; }
		else if (HttpConn::userCount >= MAX_FD)
		{
			SendError_(fd, "Server busy!");
			LOG_WARN("Clients is full!");
			return;
		}
		// 用accept函数返回的新fd
		AddClient_(fd, addr);
	} while (listenEvent_ & EPOLLET);
}

void WebServer::DealRead_(HttpConn* client)
{
	/// 线程池中添加read任务
	assert(client);
	ExtentTime_(client);
	threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

void WebServer::DealWrite_(HttpConn* client)
{
	/// 线程池中添加write任务
	assert(client);
	ExtentTime_(client);
	threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client)
{
	assert(client);
	if (timeoutMS_ > 0)
	{ timer_->adjust(client->GetFd(), timeoutMS_); }
}

void WebServer::OnRead_(HttpConn* client)
{
	assert(client);
	int ret = -1;
	int readErrno = 0;
	ret = client->read(&readErrno);
	if (ret <= 0 && readErrno != EAGAIN)
	{
		CloseConn_(client);
		return;
	}
	/// 完成服务端读写转换的成员
	OnProcess(client);  // 解析请求, 设置iov结构体
}

void WebServer::OnProcess(HttpConn* client)
{
	if (client->process())
	{
		/*
		 * 已没有可读内容
		 * 添加监听事件: client socketfd 可写
		 */
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
	}
	else
	{
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
	}
}

void WebServer::OnWrite_(HttpConn* client)
{
	// 加入线程的任务
	assert(client);
	int ret = -1;
	int writeErrno = 0;
	ret = client->write(&writeErrno);
	if (client->ToWriteBytes() == 0)
	{
		// buffer is empty
		/* 传输完成 */
		if (client->IsKeepAlive())
		{
			OnProcess(client);  // 监听 client socketfd 可读
			return;
		}
	}
	else if (ret < 0)
	{
		if (writeErrno == EAGAIN)
		{
			/* 继续传输 */
			epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);  // 监听文件描述符可写和设置触发模式
			return;
		}
	}
	CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_()
{
	int ret;
	struct sockaddr_in addr;
	if (port_ > 65535 || port_ < 1024)
	{  // 端口范围1024 ~ 65535
		LOG_ERROR("Port:%d error!", port_);
		return false;
	}
	addr.sin_family = AF_INET;  // 协议
	addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 地址
	addr.sin_port = htons(port_);  // 地址结构体addr端口, 主机字节序 转 网络字节序
	struct linger optLinger = { 0 };
	if (openLinger_)
	{
		/* 优雅关闭: 直到所剩数据发送完毕或超时 */
		optLinger.l_onoff = 1;  // 设置close延时
		optLinger.l_linger = 1;  // 设置close等待事件
	}

	listenFd_ = socket(AF_INET, SOCK_STREAM, 0);  // 创建socket
	if (listenFd_ < 0)
	{
		LOG_ERROR("Create socket error!", port_);
		return false;
	}

	/// 设置close延时
	ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));

	if (ret < 0)
	{
		close(listenFd_);
		LOG_ERROR("Init linger error!", port_);
		return false;
	}

	int optval = 1;

	/* 端口复用 */
	/* 只有最后一个套接字会正常接收数据。 */
	/// 设置端口复用
	ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
	if (ret == -1)
	{
		LOG_ERROR("set socket setsockopt error !");
		close(listenFd_);
		return false;
	}

	ret = bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr));   // bind
	if (ret < 0)
	{
		LOG_ERROR("Bind Port:%d error!", port_);
		close(listenFd_);
		return false;
	}

	/// 监听socket
	ret = listen(listenFd_, 6);

	if (ret < 0)
	{
		LOG_ERROR("Listen port:%d error!", port_);
		close(listenFd_);
		return false;
	}

	/// 加入epoll监听列表
	ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);

	if (ret == 0)
	{
		LOG_ERROR("Add listen error!");
		close(listenFd_);
		return false;
	}
	// 设置监听socket非阻塞
	SetFdNonblock(listenFd_);
	LOG_INFO("Server port:%d", port_);
	return true;
}

int WebServer::SetFdNonblock(int fd)
{
	// 设置为O_NONBLOCK的原因，man 2 select BUGS
	assert(fd > 0);  // assert: 断言
	return fcntl(fd, F_SETFL,
		fcntl(fd, F_GETFD, 0) | O_NONBLOCK);  // 通过位或添加: NONBLOCK位
}


