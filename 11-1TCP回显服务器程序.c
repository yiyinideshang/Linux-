#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

#define BUFFER_SIZE 1024

/**
 * TCP 回显服务器
 * 监听指定端口，接受连接，并回显收到的数据
 */
int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("Usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }

    // 1. 创建监听套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return 1;
    }

    // 2. 设置地址重用，避免程序退出后端口被占用
    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listenfd);
        return 1;
    }

    // 3. 绑定地址和端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网络接口
    addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return 1;
    }

    // 4. 开始监听
    if (listen(listenfd, 5) < 0) {
        perror("listen");
        close(listenfd);
        return 1;
    }

    printf("Echo server listening on port %d...\n", port);

    while (1) {
        // 5. 接受客户端连接
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
        if (connfd < 0) {
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("New connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        // 6. 处理客户端数据（回显）
        char buffer[BUFFER_SIZE];
        ssize_t n;
        int exit_flag = 0;  // 是否收到 "exit" 命令

        while ((n = recv(connfd, buffer, sizeof(buffer) - 1, 0)) > 0) 
        {
            buffer[n] = '\0';

            // 打印接收到的内容（调试用）
            printf("Received from %s:%d: %s\n", client_ip, ntohs(client_addr.sin_port), buffer);
            // 将收到的数据回显给客户端
            if (send(connfd, buffer, n, 0) != n) 
            {
                perror("send");
                break;
            }
            // 检查是否是 "exit" 命令
            if (strcmp(buffer, "exit") == 0) {
                printf("Exit command received. Shutting down server.\n");
                exit_flag = 1;
                break;
            }
        }
        if (n < 0) 
        {
            perror("recv");
        }
        close(connfd);
        printf("Connection from %s:%d closed.\n", client_ip, ntohs(client_addr.sin_port));
        if (exit_flag) 
        {
            break;  // 退出 while 循环，结束服务器
        }
    }
    close(listenfd);
    printf("Server terminated.\n");
    return 0;
}