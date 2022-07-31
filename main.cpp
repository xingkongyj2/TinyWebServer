#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>

#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
//pipefd是定时器通信使用的
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//注册这个要捕捉的信号，当内核给这个进程发这个信号的时候，就能捕捉到了。然后去指令自己指定的函数。
/**
 *
 * @param sig 要注册的信号
 * @param handler 信号捕捉后的的处理函数
 * @param restart todo
 */
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    //可能在处理这里信号的过程中又有其他信号来，所以临时屏蔽掉其他信号。
    //将某个信号集置1
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}


void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif
    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = 10000;//atoi(argv[1]);
    printf("port %d\n",port);

    addsig(SIGPIPE, SIG_IGN);
    printf("创建数据库连接池\n");
    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "@Xingkong22", "webserver", 3306, 8);
    printf("创建线程池\n");
    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    // 预先为每个可能的客户连接分配一个http_conn对象
    http_conn *users = new http_conn[MAX_FD];
    //如果它的条件返回错误，则终止程序执行。
    assert(users);
    printf("初始化数据库读取表\n");
    //初始化数据库读取表
    //只是使用第一个对象的initmysql_result读取用户数据，放入一个全局变量中（需要修改）。
    users->initmysql_result(connPool);

    //创建一个socket的文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //端口复用
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    //开始监听
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    printf("bind finish\n");
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建管道
    //[socketpair的用法和理解](https://blog.csdn.net/weixin_40039738/article/details/81095013)
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    //每隔TIMESLOT秒alarm就会发出一个信号，定时去检查是不活跃的连接
    alarm(TIMESLOT);

    while (!stop_server)
    {

        LOG_INFO("%s", "epoll_wait等待连接");
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //客户端请求连接
            if (sockfd == listenfd)
            {
                printf("socket connection\n");
                LOG_INFO("%s", "处理新到的客户连接");
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                //doing
                /* 并将connfd注册到内核事件表中 */
                //与客户端建立连接后，将连接数据保存到对应的数组元素中
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            //处理异常事件,服务器端关闭连接
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 如有异常，则直接关闭客户连接，并删除该用户的timer
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
            //处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //doing
                printf("deal data\n");
                util_timer *timer = users_timer[sockfd].timer;
                /* 当这一sockfd上有可读事件时，epoll_wait通知主线程。*/
                if (users[sockfd].read_once())/* 主线程从这一sockfd循环读取数据, 直到没有更多数据可读 */
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    cout<< "deal with the client(%s)"<<inet_ntoa(users[sockfd].get_address()->sin_addr)<<endl;
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);/* 然后将读取到的数据封装成一个请求对象并插入请求队列 */

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                /* 当这一sockfd上有可写事件时，epoll_wait通知主线程。主线程往socket上写入服务器处理客户请求的结果 */
                cout<<"return data"<<endl;
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    cout<<"send data to the client(%s)"<<inet_ntoa(users[sockfd].get_address()->sin_addr)<<endl;
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
