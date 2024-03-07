#include "webserver.h"

using namespace std;

WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
    {
    srcDir_ = getcwd(nullptr, 256);//获取当前路径
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);//拼接路径到资源目录
    HttpConn::userCount = 0;//连接类中静态变量用户数量初始化为0
    HttpConn::srcDir = srcDir_;//连接类中静态变量资源路径初始化
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);//单例模式初始化数据库连接池

    InitEventMode_(trigMode);//初始化epoll树的event即监听的事件
    if(!InitSocket_()) { isClose_ = true;}//初始化socket，以及监听文件描述符上树

    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);//单例模式初始化日志
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

void WebServer::InitEventMode_(int trigMode) {
    listenEvent_ = EPOLLRDHUP;//设置监听文件描述符为监听socket关闭连接事件
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;//设置通信文件描述符为监听socket关闭连接事件以及只监听一次
    switch (trigMode)//选择监听模式，即边沿触发和水平触发
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
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {//主函数中调用此函数进入事件循环
    /* epoll wait timeout == -1 无事件将阻塞 */
    int timeMS = -1;  //最初无连接，即无需处理超时连接，因此Wait()可以保持阻塞
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    while(!isClose_) {
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick();//清理超时连接并更新下一次阻塞时间
        }
        int eventCnt = epoller_->Wait(timeMS);//epoll_wait的简化封装，只需传入阻塞时间，返回活跃事件数
        for(int i = 0; i < eventCnt; i++) {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);//获取事件的文件描述符
            uint32_t events = epoller_->GetEvents(i);//获取事件的类型
            if(fd == listenFd_) {//监听文件描述符上有事件发生
                DealListen_();//处理监听文件描述符上的事件
            }
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {//连接文件描述符上有错误发生或者连接关闭
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);//关闭连接
            }
            else if(events & EPOLLIN) {
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);//处理连接文件描述符上的读事件
            }
            else if(events & EPOLLOUT) {
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);//处理连接文件描述符上的写事件
            } else {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info) {//发送错误信息并关闭连接
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);//向客户端发送错误信息
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn* client) {//关闭连接
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr) {//添加连接
    assert(fd > 0);
    users_[fd].init(fd, addr);//初始化连接，并将连接加入到哈希表中
    if(timeoutMS_ > 0) {//如果设置了超时时间，则将连接加入到定时器中
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);//将连接加入到epoll树上
    SetFdNonblock(fd);//设置文件描述符设置为非阻塞，方便一次性读出所有数据
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

void WebServer::DealListen_() {//处理监听文件描述符上的事件
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    do {
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        if(fd <= 0) { return;}
        else if(HttpConn::userCount >= MAX_FD) {//如果连接数超过最大连接数，则向对方发送服务器正忙并关闭连接
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);//添加连接，即初始化连接，将连接加入到哈希表中，并将连接文件描述符加入到epoll树上
    } while(listenEvent_ & EPOLLET);//ET模式下需要一次性将连接池中的连接全部取出
}

void WebServer::DealRead_(HttpConn* client) {//处理连接文件描述符的读事件
    assert(client);
    ExtentTime_(client);//更新连接的超时时间
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));//将读事件加入到线程池中，回调函数为OnRead_，并绑定this指针和client指针
}

void WebServer::DealWrite_(HttpConn* client) {//处理连接文件描述符的写事件
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));//将读事件加入到线程池中，回调函数为OnRead_，并绑定this指针和client指针
}

void WebServer::ExtentTime_(HttpConn* client) {//更新连接的超时时间
    assert(client);
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}

void WebServer::OnRead_(HttpConn* client) {//将读事件加入到线程池中的回调函数
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);//读取数据
    if(ret <= 0 && readErrno != EAGAIN) {//如果读取数据出错或者读取数据为空
        CloseConn_(client);
        return;
    }
    OnProcess(client);//处理数据
}

void WebServer::OnProcess(HttpConn* client) {
    if(client->process()) {//处理数据
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);//如果处理成功，则将监听连接文件描述符可写事件加入到epoll树上
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);//如果处理失败，则将监听连接文件描述符可读事件加入到epoll树上，重新等待读取数据
    }
}

void WebServer::OnWrite_(HttpConn* client) {//将写事件加入到线程池中的回调函数
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);//写数据
    if(client->ToWriteBytes() == 0) {//如果写数据为空
        /* 传输完成 */
        if(client->IsKeepAlive()) {
            OnProcess(client);
            return;
        }
    }
    else if(ret < 0) {//如果写数据出错
        if(writeErrno == EAGAIN) {//如果试的写操作由于资源暂时不可用而未能立即完成，则重新等待写事件
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_() {//初始化socket，以及监听文件描述符上树
    int ret;
    struct sockaddr_in addr;
    if(port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    struct linger optLinger = { 0 };
    if(openLinger_) {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));//设置优雅关闭
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    ret = listen(listenFd_, 6);
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN);//将监听文件描述符加入到epoll树上
    if(ret == 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_);//设置文件描述符设置为非阻塞，方便一次性取出所有新的连接
    LOG_INFO("Server port:%d", port_);
    return true;
}

int WebServer::SetFdNonblock(int fd) {//设置文件描述符设置为非阻塞
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}


