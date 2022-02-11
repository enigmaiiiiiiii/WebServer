/*
 * @Author       : mark
 * @Date         : 2020-06-27
 * @copyleft Apache 2.0
 */
#include "httpresponse.h"

using namespace std;

const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
	{".html", "text/html"},
	{".xml", "text/xml"},
	{".xhtml", "application/xhtml+xml"},
	{".txt", "text/plain"},
	{".rtf", "application/rtf"},
	{".pdf", "application/pdf"},
	{".word", "application/nsword"},
	{".png", "image/png"},
	{".gif", "image/gif"},
	{".jpg", "image/jpeg"},
	{".jpeg", "image/jpeg"},
	{".au", "audio/basic"},
	{".mpeg", "video/mpeg"},
	{".mpg", "video/mpeg"},
	{".avi", "video/x-msvideo"},
	{".gz", "application/x-gzip"},
	{".tar", "application/x-tar"},
	{".css", "text/css "},
	{".js", "text/javascript "},
};

const unordered_map<int, string> HttpResponse::CODE_STATUS = {
	{200, "OK"},
	{400, "Bad Request"},
	{403, "Forbidden"},
	{404, "Not Found"},
};

const unordered_map<int, string> HttpResponse::CODE_PATH = {
	{400, "/400.html"},
	{403, "/403.html"},
	{404, "/404.html"},
};

HttpResponse::HttpResponse() {
  code_ = -1;
  path_ = srcDir_ = "";
  isKeepAlive_ = false;
  mmFile_ = nullptr;
  mmFileStat_ = {0};
};

HttpResponse::~HttpResponse() {
  UnmapFile();
}

void HttpResponse::Init(const string &srcDir, string &path, bool isKeepAlive, int code) {
  assert(srcDir!="");
  if (mmFile_) { UnmapFile(); }  // mmFile被设置为true,则删除映射
  code_ = code;
  isKeepAlive_ = isKeepAlive;
  path_ = path;
  srcDir_ = srcDir;
  mmFile_ = nullptr;
  mmFileStat_ = {0};
}

void HttpResponse::MakeResponse(Buffer &buff) {
  if (stat((srcDir_ + path_).data(), &mmFileStat_) < 0 || S_ISDIR(mmFileStat_.st_mode)) {
	/* 判断请求的资源文件是否存在，是否有可访问权限 */
	code_ = 404;  // 文件不存在
  } else if (!(mmFileStat_.st_mode & S_IROTH)) {
	code_ = 403;  // 请求的文件不具有 other read(00004) 权限
  } else if (code_==-1) {  //
	code_ = 200;  // 正常返回
  }
  ErrorHtml_();
  AddStateLine_(buff);  // 响应报文状态行
  AddHeader_(buff);     // 响应报文首部行
  AddContent_(buff);    // 响应内容
}

char *HttpResponse::File() {
  return mmFile_;
}

size_t HttpResponse::FileLen() const {
  return mmFileStat_.st_size;
}

void HttpResponse::ErrorHtml_() {
  // 处理错误返回页面
  if (CODE_PATH.count(code_)==1) {
	path_ = CODE_PATH.find(code_)->second;
	stat((srcDir_ + path_).data(), &mmFileStat_);
  }
}

void HttpResponse::AddStateLine_(Buffer &buff) {
  /*
   * 添加状态行, 比如HTTP/1.1 200 OK
   * 添加到httpconn类的writeBuff_(Buffer结构体)成员
  */
  string status;
  if (CODE_STATUS.count(code_)==1) {
	status = CODE_STATUS.find(code_)->second;
  } else {
	code_ = 400;
	status = CODE_STATUS.find(400)->second;
  }
  buff.Append("HTTP/1.1 " + to_string(code_) + " " + status + "\r\n");
}

void HttpResponse::AddHeader_(Buffer &buff) {
  // 添加首部行，比如Connection: keep-alive
  buff.Append("Connection: ");
  if (isKeepAlive_) {
	buff.Append("keep-alive\r\n");
	buff.Append("keep-alive: max=6, timeout=120\r\n");
  } else {
	buff.Append("close\r\n");
  }
  buff.Append("Content-type: " + GetFileType_() + "\r\n");
}

void HttpResponse::AddContent_(Buffer &buff) {
  // 添加文件数据
  int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
  LOG_DEBUG("response source path: %s", srcDir_ + path_);
  if (srcFd < 0) {  // open 失败
	ErrorContent(buff, "File NotFound!");
	return;
  }
  /* 将文件映射到内存提高文件的访问速度
	  MAP_PRIVATE 建立一个写入时拷贝的私有映射*/
  LOG_DEBUG("file path %s", (srcDir_ + path_).data());
  int *mmRet = (int *)mmap(0, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);  // 本身返回void*
  if (*mmRet==-1) {
	ErrorContent(buff, "File NotFound!");
	return;
  }
  mmFile_ = (char *)mmRet;  // 文件映射地址赋值给mmFile_(char *)成员
  close(srcFd);  // 关闭文件
  buff.Append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}

void HttpResponse::UnmapFile() {
  // 删除映射数据
  if (mmFile_) {
	munmap(mmFile_, mmFileStat_.st_size);
	mmFile_ = nullptr;
  }
}

string HttpResponse::GetFileType_() {
  // 生成Content-type报文段
  string::size_type idx = path_.find_last_of('.');
  /* 判断文件类型
    idx值path_最后一个.符号的位置 */
  if (idx==string::npos) {
	// 没找到字符".", 则Content-Type: text/plain
	return "text/plain";
  }
  string suffix = path_.substr(idx);  // 字符"."到结尾, suffix表示文件名后缀
  if (SUFFIX_TYPE.count(suffix)==1) {
	// 查找unordered中的key对应的value作为Content-type
	return SUFFIX_TYPE.find(suffix)->second;
  }
  return "text/plain";
}

void HttpResponse::ErrorContent(Buffer &buff, string message) {
  string body;
  string status;
  body += "<html><title>Error</title>";
  body += "<body bgcolor=\"ffffff\">";
  if (CODE_STATUS.count(code_)==1) {
	status = CODE_STATUS.find(code_)->second;
  } else {
	status = "Bad Request";
  }
  body += to_string(code_) + " : " + status + "\n";
  body += "<p>" + message + "</p>";
  body += "<hr><em>WebServer</em></body></html>";

  buff.Append("Content-length: " + to_string(body.size()) + "\r\n\r\n");
  buff.Append(body);
}
