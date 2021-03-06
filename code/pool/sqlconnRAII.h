
#ifndef SQLCONNRAII_H
#define SQLCONNRAII_H
#include "sqlconnpool.h"

/* 资源在对象构造初始化 资源在对象析构时释放*/
class SqlConnRAII
{
 public:
	SqlConnRAII(MYSQL** sql, SqlConnPool* connpool)
	{
		assert(connpool);
		*sql = connpool->GetConn();
		sql_ = *sql;  // sql是指向MYSQL*对象的指针, 解引用得到MYSQL*
		connpool_ = connpool;
	}

	~SqlConnRAII()
	{
		if (sql_)
		{ connpool_->FreeConn(sql_); }
	}

 private:
	MYSQL* sql_;
	SqlConnPool* connpool_;
};

#endif //SQLCONNRAII_H