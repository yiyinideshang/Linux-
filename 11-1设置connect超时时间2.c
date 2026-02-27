#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>

#define BUFFER_SIZE 1024

/**
 * 带超时的connect函数（非阻塞 + select）
 * @param ip   服务器IP地址
 * @param port 服务器端口
 * @param timeout_sec 超时秒数
 * @return 成功返回已连接套接字描述符，失败返回-1
 */

int timeout_connect(const char* ip, int port, int timeout_sec) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    // 1. 将套接字设置为非阻塞
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        close(sockfd);
        return -1;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(sockfd);
        return -1;
    }

    // 2. 构造服务器地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    // 3. 发起非阻塞连接
    int ret = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        // 极少数情况下连接立即成功（例如连接到本地回环地址且服务器在监听）
        // 此时直接返回套接字，但最好恢复阻塞模式（可选）
        fcntl(sockfd, F_SETFL, flags);   // 恢复原始标志
        return sockfd;
    }

    if (ret == -1 && errno != EINPROGRESS) {
        // 真正的连接错误（如网络不可达、拒绝连接等）
        perror("connect");
        close(sockfd);
        return -1;
    }

    // 4. 使用 select 等待连接完成或超时
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(sockfd + 1, NULL, &write_fds, NULL, &tv);
    if (ret == 0) {
        // 超时
        fprintf(stderr, "connect timeout (%d seconds)\n", timeout_sec);
        close(sockfd);
        return -1;
    } else if (ret < 0) {
        // select 出错
        perror("select");
        close(sockfd);
        return -1;
    }

    // 5. select 返回 >0，检查套接字是否有错误
    int error;
    socklen_t len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        perror("getsockopt");
        close(sockfd);
        return -1;
    }
    if (error != 0) {
        // 连接失败（例如服务器未监听、连接被拒绝等）
        fprintf(stderr, "connection failed: %s\n", strerror(error));
        close(sockfd);
        return -1;
    }

    // 6. 连接成功，恢复套接字为阻塞模式（可选，便于后续使用）
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        perror("fcntl restore blocking");
        // 即使恢复失败，套接字仍可用，只是可能仍为非阻塞
        // 这里不视为致命错误，继续返回sockfd
    }

    return sockfd;
}

int main(int argc, char* argv[])
{
    if (argc <= 2)
    {
        printf("Usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    int timeout = 10;  // 设置超时时间为10秒

    int sockfd = timeout_connect(ip, port, timeout);
    if (sockfd < 0) 
    {
        return -1;
    }

    printf("connect to %s:%d successfully (timeout=%d)\n", ip, port, timeout);
    // 此处可进行后续通信，
    char send_buf[BUFFER_SIZE];
    char recv_buf[BUFFER_SIZE];
    while (1) 
    {
    // 读取一行输入
        if (fgets(send_buf, sizeof(send_buf), stdin) == NULL) {
            break; // 遇到 EOF
        }

        // 去掉末尾换行符
        size_t len = strlen(send_buf);
        if (len > 0 && send_buf[len-1] == '\n') {
            send_buf[len-1] = '\0';
            len--;
        }

        // 发送给服务器
        if (send(sockfd, send_buf, len, 0) < 0) {
            perror("send");
            break;
        }

        // 如果是 "exit"，则退出循环
        if (strcmp(send_buf, "exit") == 0) {
            printf("Exiting...\n");
            break;
        }

        // 接收服务器回显
        ssize_t n = recv(sockfd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n > 0) {
            recv_buf[n] = '\0';
            printf("Echo: %s\n", recv_buf);
        } else if (n == 0) {
            printf("Server closed connection.\n");
            break;
        } else {
            perror("recv");
            break;
        }
    }
    close(sockfd);
    return 0;
}