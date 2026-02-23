#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <bits/sigaction.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <libgen.h>
#include <stdbool.h>

#define MAX_EVENT_NUMBER 1024 //epoll_wait 返回的最大事件数。
static int pipefd[2];//用于信号传递的管道（由 socketpair 创建），pipefd[0] 读端，pipefd[1] 写端。

int setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);//获取文件描述符当前标志
    int new_option = old_option | O_NONBLOCK;//添加非阻塞状态
    fcntl(fd,F_SETFL,new_option);//设置非阻塞I/O
    return old_option;//返回旧标志，以便后续恢复
}

void addfd(int epollfd,int fd)//向epoll添加文件描述符
{
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN |EPOLLET;//监听可读事件，使用边缘触发(要求一次性读完所有数据)
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);//将目标fd设为非阻塞
}

//信号处理函数
void sig_handler(int sig)
{
    //保留原来的errno，在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;//保存errno，保证可重入性
    int msg = sig;
    send(pipefd[1],(char*)&msg,1,0);//向管道写端发送一个字节(信号值)
    errno = save_errno;//恢复errno，防止干扰被信号中断的系统调用
}

//设置信号处理函数
void addsig(int sig)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = sig_handler;//注册信号处理函数
    sa.sa_flags |= SA_RESTART;//自动重启被信号中断的系统调用
    sigfillset(&sa.sa_mask);//执行处理函数时，阻塞所有信号
    assert(sigaction(sig,&sa,NULL) != -1);
}

int main(int argc,char* argv[])
{
    if(argc<=2)
    {
        printf("usage:%s ip_address port_number\n",basename(argv[0]));
        return -1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET,ip,&address.sin_addr);
    address.sin_port = htons(port);

    //创建监听套接字listenfd
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    if(ret == -1)//绑定套接字失败
    {
        printf("errno is %d\n",errno);
        return -1;
    }
    ret = listen(listenfd,5);//监听套接字listenfd,最大等待accpet连接数为5
    assert(ret != -1);

    struct epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd,listenfd);//将监听socket加入epoll,监听可读事件(有新的连接)

    //使用socketpair创建管道，注册pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);//写端非阻塞，防止信号处理函数中因管道满而阻塞。
    addfd(epollfd,pipefd[0]);//读端加入epoll，以便当有信号写入时，主循环能检测到可读事件。

    //==== 新增：将标准输入加入epoll监听，实现在服务器终端输入命令控制服务器 ====
    setnonblocking(STDIN_FILENO);      // 将标准输入设为非阻塞
    addfd(epollfd, STDIN_FILENO);      // 将标准输入加入epoll，监听可读事件
    //===============================================================

    //设置一些信号的处理函数
    addsig(SIGHUP);//终端挂起
    addsig(SIGCHLD);//子进程结束
    addsig(SIGTERM);//终止
    addsig(SIGINT);//中断
    bool stop_server = false;//停止服务器为假，即不要停止

    while(!stop_server)//stop_server == false;
    {
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number<0)&& (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
    
        for(int i = 0;i< number;i++)
        {
            int sockfd = events[i].data.fd;
            //如果就绪的文件描述符是listenfd，则处理新的连接
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addresslength = sizeof(client_address);
                while(1)
                {
                    int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addresslength);
                    if(connfd==-1)
                    {
                        if(errno == EAGAIN||errno == EWOULDBLOCK)
                        {
                            break;//所有连接已经处理完毕
                        }
                        else{break;}//处理其他错误；
                    }
                    addfd(epollfd,connfd);//将新连接加入epoll
                    printf("new client connected, fd = %d\n", connfd);
                }
            }
            //如果就绪的文件描述符是pipefd[0]，则处理信号
            else if((sockfd == pipefd[0])&&(events[i].events&EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0],signals,sizeof(signals),0);
                if(ret == -1)//接收过程出现错误
                {
                    continue;
                }
                else if(ret == 0)//对端(管道的写端)关闭了连接
                {
                    continue;
                }
                else
                {
                    for(int i = 0;i<ret;i++)
                    {
                        switch(signals[i])
                        {
                            case SIGCHLD:
                            case SIGHUP:
                            {
                                continue;//忽略这些信号
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                stop_server = true;//停止服务器为真，即收到终止信号，退出循环，停止服务
                            }
                        }
                    }
                }
            }
            //==== 新增：处理标准输入事件（服务器终端输入）====
            else if (sockfd == STDIN_FILENO && (events[i].events & EPOLLIN))
            {
                char cmd[128];
                int n = read(STDIN_FILENO, cmd, sizeof(cmd) - 1);
                if (n > 0)
                {
                    cmd[n] = '\0';
                    // 去除末尾的换行符（如有）
                    if (cmd[n-1] == '\n')
                        cmd[n-1] = '\0';
                    if (strcmp(cmd, "exit") == 0)
                    {
                        printf("server shutdown requested from stdin\n");
                        stop_server = true;   // 设置退出标志，关闭服务器
                    }
                }
                else if (n == 0)
                {
                    // 标准输入已关闭（例如重定向结束），可忽略或视情况退出
                }
                else
                {
                    if (errno != EAGAIN)
                        perror("read stdin");
                }
            }
            //===============================================
            // 处理已连接客户端的可读事件
            else if (events[i].events & EPOLLIN)
            {
                char buf[1024];
                while (1)
                {
                    memset(buf, 0, sizeof(buf));
                    int n = recv(sockfd, buf, sizeof(buf) - 1, 0);
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;          // 数据已读完
                        else
                        {
                            // 读错误，关闭连接
                            printf("recv error on fd %d: %s\n", sockfd, strerror(errno));
                            close(sockfd);
                            epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                            break;
                        }
                    }
                    else if (n == 0)
                    {
                        // 客户端关闭连接
                        printf("client fd %d closed\n", sockfd);
                        close(sockfd);
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                        break;
                    }
                    else
                    {
                        // 检查客户端是否发送了 "exit" 命令（忽略可能存在的换行符）
                        if (strncmp(buf, "exit", 4) == 0) {
                            fprintf(stderr,"client requested to close, fd %d\n", sockfd);
                            close(sockfd);
                            epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                            break;  // 跳出 while(1) 读循环
                        }

                        // 成功读取数据，加上前缀后发回（不再是原样echo）
                        printf("received %d bytes from fd %d: %s", n, sockfd, buf);

                        // 构造带前缀的响应消息
                        const char *prefix = "来自服务器的数据：";
                        int prefix_len = strlen(prefix);
                        char response[2048];
                        // 确保缓冲区足够，若不足则截断原始数据
                        if (prefix_len + n > sizeof(response) - 1) {
                            n = sizeof(response) - prefix_len - 1;
                        }
                        memcpy(response, prefix, prefix_len);
                        memcpy(response + prefix_len, buf, n);
                        int to_send = prefix_len + n;

                        int total_sent = 0;
                        while (total_sent < to_send)
                        {
                            int sent = send(sockfd, response + total_sent, to_send - total_sent, 0);
                            if (sent < 0)
                            {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                {
                                    // 发送缓冲区满，简单处理：关闭连接（实际应缓存并注册EPOLLOUT）
                                    printf("send buffer full, close fd %d\n", sockfd);
                                    close(sockfd);
                                    epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                                    break;
                                }
                                else
                                {
                                    printf("send error on fd %d: %s\n", sockfd, strerror(errno));
                                    close(sockfd);
                                    epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                                    break;
                                }
                            }
                            total_sent += sent;
                        }
                        // 如果发送循环因错误退出，需要跳出外层读循环
                        if (total_sent < to_send)
                            break;
                    }
                }
            }
        }
    }
    printf("close fds\n");
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    close(epollfd);   // 建议关闭epoll文件描述符
    return 0;
}   