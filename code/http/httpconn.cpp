/*
 * @Author       : mark
 * @Date         : 2020-06-15
 * @copyleft Apache 2.0
 */
#include "httpconn.h"
using namespace std;

const char *HttpConn::srcDir; // 初始化webserver时设置初始值
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;  // 边界触发

HttpConn::HttpConn() {
  fd_ = -1;
  addr_ = {0};
  isClose_ = true;
}

HttpConn::~HttpConn() {
  Close();
}

void HttpConn::init(int fd, const sockaddr_in &addr) {
  /*
   * 初始化客户端的缓存结构体buff,作为客户端的缓存空间
   */
  assert(fd > 0);
  userCount++;  // 统计user数量
  addr_ = addr;
  fd_ = fd;
  writeBuff_.RetrieveAll();  // 刷新缓存, 分配1024字节空间
  readBuff_.RetrieveAll();  // 缓存空间 : 1024 bytes
  isClose_ = false;
}

void HttpConn::Close() {
  response_.UnmapFile();
  if (isClose_==false) {
	isClose_ = true;
	userCount--;
	close(fd_);
  }
}

int HttpConn::GetFd() const {
  //
  return fd_;
}

struct sockaddr_in HttpConn::GetAddr() const {
  return addr_;
}

const char *HttpConn::GetIP() const {
  return inet_ntoa(addr_.sin_addr);  // 10进制转xxx.xxx.xxx.xxx
}

int HttpConn::GetPort() const {
  return addr_.sin_port;  // client的端口
}

ssize_t HttpConn::read(int *saveErrno) {
  ssize_t len = -1;
  do {
	len = readBuff_.ReadFd(fd_, saveErrno);
	// 读fd,拷贝到readbuff
	if (len <= 0) {
	  break;
	}
  } while (isET);
  return len;
}

ssize_t HttpConn::write(int *saveErrno) {
  // 向acceptfd写入数据
  ssize_t len = -1;
  do {
	len = writev(fd_, iov_, iovCnt_);
	/// 将响应头iov_[0], 响应体iov_[1]一起写出至accept()函数返回的fd_
	if (len <= 0) {
	  *saveErrno = errno;
	  break;
	}
	if (iov_[0].iov_len + iov_[1].iov_len==0) { break; } /* 传输结束 */
	else if (static_cast<size_t>(len) > iov_[0].iov_len) {
	  /// 响应头部分write完成, 响应体部分write未完成
	  iov_[1].iov_base = (uint8_t *)iov_[1].iov_base + (len - iov_[0].iov_len);
	  iov_[1].iov_len -= (len - iov_[0].iov_len);
	  if (iov_[0].iov_len) {
		writeBuff_.RetrieveAll();
		iov_[0].iov_len = 0;
	  }
	} else {
	  iov_[0].iov_base = (uint8_t *)iov_[0].iov_base + len;
	  iov_[0].iov_len -= len;
	  writeBuff_.Retrieve(len);
	}
  } while (isET || ToWriteBytes() > 10240);
  return len;
}

bool HttpConn::process() {
  // 解析请求(request)，生成响应(response)
  request_.Init();
  if (readBuff_.ReadableBytes() <= 0) {
	return false;
  } else if (request_.parse(readBuff_)) {
	// 解析请求
	LOG_DEBUG("%s", request_.path().c_str());
	response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
  } else {
	response_.Init(srcDir, request_.path(), false, 400);
  }

  response_.MakeResponse(writeBuff_);
  /*
   * 添加响应头至 Buffer writeBuff_
   * 设置响应内容映射区char *mmfile_
   */
  iov_[0].iov_base = const_cast<char *>(writeBuff_.Peek());
  iov_[0].iov_len = writeBuff_.ReadableBytes();  // 写位置(writePos_.) - 读位置(readPos_)
  iovCnt_ = 1;
  /* 将响应头加入,iov[0] */

  /* 文件 */
  if (response_.FileLen() > 0 && response_.File()) {
	// 有响应文件
	iov_[1].iov_base = response_.File();  // 返回httpresponse::mmfile_
	iov_[1].iov_len = response_.FileLen();  //
	iovCnt_ = 2;
  }
  LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen(), iovCnt_, ToWriteBytes());
  return true;
}
