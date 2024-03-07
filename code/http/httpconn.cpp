#include "httpconn.h"
using namespace std;

const char* HttpConn::srcDir;//静态变量资源路径
std::atomic<int> HttpConn::userCount;//静态变量用户数量，因为是原子操作，所以不需要加锁。监听文件描述符会增加用户数量，通信文件描述符会减少用户数量，所以需要原子操作保证线程安全
bool HttpConn::isET;//是否是边沿触发模式

HttpConn::HttpConn() {
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;//默认构造出的连接是默认关闭的
};

HttpConn::~HttpConn() { 
    Close(); 
};

void HttpConn::init(int fd, const sockaddr_in& addr) {//初始化连接
    assert(fd > 0);
    userCount++;//用户数量增加
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();//写缓冲区清空
    readBuff_.RetrieveAll();//读缓冲区清空
    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close() {//关闭连接
    response_.UnmapFile();//关闭内存映射
    if(isClose_ == false){//如果连接没有关闭
        isClose_ = true; 
        userCount--;
        close(fd_);
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const {//获取此连接的文件描述符
    return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const {//获取此连接的地址（IP地址和端口号）
    return addr_;
}

const char* HttpConn::GetIP() const {//获取此连接的IP地址
    return inet_ntoa(addr_.sin_addr);
}

int HttpConn::GetPort() const {//获取此连接的端口号
    return addr_.sin_port;
}

ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = readBuff_.ReadFd(fd_, saveErrno);
        if (len <= 0) {
            break;
        }
    } while (isET);
    return len;
}

ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iovCnt_);
        if(len <= 0) {
            *saveErrno = errno;
            break;
        }
        if(iov_[0].iov_len + iov_[1].iov_len  == 0) { break; } /* 传输结束 */
        else if(static_cast<size_t>(len) > iov_[0].iov_len) {
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            if(iov_[0].iov_len) {
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        }
        else {
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; 
            iov_[0].iov_len -= len; 
            writeBuff_.Retrieve(len);
        }
    } while(isET || ToWriteBytes() > 10240);
    return len;
}

bool HttpConn::process() {
    request_.Init();
    if(readBuff_.ReadableBytes() <= 0) {
        return false;
    }
    else if(request_.parse(readBuff_)) {
        LOG_DEBUG("%s", request_.path().c_str());
        response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    } else {
        response_.Init(srcDir, request_.path(), false, 400);
    }

    response_.MakeResponse(writeBuff_);
    /* 响应头 */
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    /* 文件 */
    if(response_.FileLen() > 0  && response_.File()) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }
    LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen() , iovCnt_, ToWriteBytes());
    return true;
}
