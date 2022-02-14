#include "buffer.h"

Buffer::Buffer(int initBuffSize) : buffer_(initBuffSize), readPos_(0), writePos_(0)
{
} //

// read fd to buffer, write buffer to fd

size_t Buffer::ReadableBytes() const
{
	// 当前写的位置 - 当前读位置为当前可读空间
	return writePos_ - readPos_;  //
}
size_t Buffer::WritableBytes() const
{
	return buffer_.size() - writePos_;  // 总空间 - 写到的位置 = 剩余可写空间
}

size_t Buffer::PrependableBytes() const
{
	return readPos_;
}

const char* Buffer::Peek() const
{
	/// read_position的位置读到位置
	return BeginPtr_() + readPos_;
}

void Buffer::Retrieve(size_t len)
{
	// 设置可读位置向前len个字符
	assert(len <= ReadableBytes());
	readPos_ += len;
}

void Buffer::RetrieveUntil(const char* end)
{
	assert(Peek() <= end);
	Retrieve(end - Peek());
}

void Buffer::RetrieveAll()
{
	// 用0填充缓存, 设置writePos_位置到起点
	bzero(&buffer_[0], buffer_.size());
	readPos_ = 0;
	writePos_ = 0;
}

std::string Buffer::RetrieveAllToStr()
{
	// 取出buffer中内容到字符串str,返回str,复位buffer对象
	std::string str(Peek(), ReadableBytes());
	RetrieveAll();
	return str;
}

const char* Buffer::BeginWriteConst() const
{
	return BeginPtr_() + writePos_;
}

char* Buffer::BeginWrite()
{
	return BeginPtr_() + writePos_;
}

void Buffer::HasWritten(size_t len)
{
	/// 该成员函数会修改当前写入位置
	writePos_ += len;
}

void Buffer::Append(const std::string& str)
{
	Append(str.data(), str.length());
}

void Buffer::Append(const void* data, size_t len)
{
	/// 调用重载函数
	/// 按照数据大小分配内存
	assert(data);
	Append(static_cast<const char*>(data), len);  // static_cast<const char*> data类型转化为char*
}

void Buffer::Append(const char* str, size_t len)
{
	/// 所有Append函数最终调用的函数,
	assert(str);
	EnsureWriteable(len);
	std::copy(str, str + len, BeginWrite());
	HasWritten(len);  // 将writePos_指向当前内容末尾
}

void Buffer::Append(const Buffer& buff)
{
	Append(buff.Peek(), buff.ReadableBytes());
}

void Buffer::EnsureWriteable(size_t len)
{
	if (WritableBytes() < len)
	{
		MakeSpace_(len);
	}
	assert(WritableBytes() >= len);
}

ssize_t Buffer::ReadFd(int fd, int* saveErrno)
{
	char buff[65535]; // 2^16字节
	struct iovec iov[2];
	const size_t writable = WritableBytes();
	/*
	 * 分散读， 保证数据全部读完
	 */
	iov[0].iov_base = BeginPtr_() + writePos_;
	iov[0].iov_len = writable;
	iov[1].iov_base = buff;
	iov[1].iov_len = sizeof(buff);

	const ssize_t len = readv(fd, iov, 2);  // 分散读fd到iov
	if (len < 0)
	{
		*saveErrno = errno;
	}
	else if (static_cast<size_t>(len) <= writable)
	{
		writePos_ += len;
	}
	else
	{
		writePos_ = buffer_.size();
		Append(buff, len - writable);
	}
	return len;
}

ssize_t Buffer::WriteFd(int fd, int* saveErrno)
{
	size_t readSize = ReadableBytes();
	ssize_t len = write(fd, Peek(), readSize);
	if (len < 0)
	{
		*saveErrno = errno;
		return len;
	}
	readPos_ += len;
	return len;
}

char* Buffer::BeginPtr_()
{
	return &*buffer_.begin();
}

const char* Buffer::BeginPtr_() const
{
	return &*buffer_.begin();  // &表示取地址
}

void Buffer::MakeSpace_(size_t len)
{
	/// 重新分配空间
	if (WritableBytes() + PrependableBytes() < len)
	{
		buffer_.resize(writePos_ + len + 1);
	}
	else
	{
		size_t readable = ReadableBytes();
		std::copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
		readPos_ = 0;
		writePos_ = readPos_ + readable;
		assert(readable == ReadableBytes());
	}
}