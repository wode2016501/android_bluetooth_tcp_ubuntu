#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>

// 🌟 补全 Linux 蓝牙内核专用的 ioctl 控制命令宏定义
#ifndef N_HCI
#define N_HCI 15
#endif

#ifndef HCIUARTSETPROTO
#define HCIUARTSETPROTO _IOW('U', 200, int)
#endif

#ifndef HCI_UART_H4
#define HCI_UART_H4 0  // 0 代表标准的 H4 协议，等同于 btattach -P h4
#endif

int tcp_fd = -1;
int pty_master = -1;
int pty_slave = -1;

void tcp_to_pty_thread() {
    std::vector<uint8_t> buffer(4096);
    while (true) {
        ssize_t len = recv(tcp_fd, buffer.data(), buffer.size(), 0);
        if (len <= 0) {
            printf("\n[-] 手机端断开连接。\n");
            break;
        }
        if (write(pty_master, buffer.data(), len) < 0) break;
    }
}

void pty_to_tcp_thread() {
    std::vector<uint8_t> buffer(4096);
    while (true) {
        ssize_t len = read(pty_master, buffer.data(), buffer.size());
        if (len <= 0) break;
        if (send(tcp_fd, buffer.data(), len, 0) < 0) break;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("使用方法: sudo %s <手机IP>\n", argv[0]);
        return -1;
    }
    if (getuid() != 0) {
        printf("[-] 错误: 必须使用 sudo 权限运行！\n");
        return -1;
    }
    //int port=9002;
    const char* phone_ip = argv[1];
    printf("[*] 正在连接手机蓝牙代理 [%s:9000]...\n", phone_ip);

    // 1. 连接手机 TCP
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9002);
    if (inet_pton(AF_INET, phone_ip, &server_addr.sin_addr) <= 0 || 
        connect(tcp_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] 无法连接到手机蓝牙 HAL 代理");
        return -1;
    }
    printf("[+] 成功连接到手机蓝牙 HAL 代理！\n");

    // 2. 创建本地伪终端 PTY
    char slave_name[64];
    if (openpty(&pty_master, &pty_slave, slave_name, nullptr, nullptr) < 0) {
        perror("[-] 创建虚拟串口失败");
        return -1;
    }
    printf("[+] 成功开辟本地虚拟串口通道: %s\n", slave_name);

    // 3. 【核心黑魔法第一步】执行 ioctl 切换线路规程到蓝牙
    int ldisc = N_HCI;
    if (ioctl(pty_slave, TIOCSETD, &ldisc) < 0) {
        perror("[-] 强行挂载 Linux 蓝牙线路规程 (N_HCI) 失败");
        return -1;
    }

    // 4. 🌟【核心黑魔法第二步：完美代替 btattach 的关键】
    // 强行告诉 Linux 内核：这个虚拟串口使用的硬件协议是 H4！
    int proto = HCI_UART_H4;
    if (ioctl(pty_slave, HCIUARTSETPROTO, proto) < 0) {
        perror("[-] 绑定蓝牙 H4 硬件传输协议失败！");
        return -1;
    }
    printf("[+] 成功激活 Linux 内核 H4 协议规程，并成功绑定 H4 传输协议！\n");

    // 5. 启动双向透明传输线程
    std::thread t1(tcp_to_pty_thread);
    std::thread t2(pty_to_tcp_thread);
    t1.detach();
    t2.detach();

    // 6. 刷新服务，让内核与 BlueZ 协议栈接管全新生成的网卡
    printf("[*] 正在刷新系统蓝牙服务并拉起网卡...\n");
    sleep(1); 
       sleep(1); 
    
    // 激活生成的网卡节点
    system("hciconfig -a");
        printf("\n[+] 全线打通！本地网卡已就绪。\n");
    printf("[*] 代理程序正在保持运行，按 [Ctrl + C] 退出...\n");

    while (true) {
        sleep(1);
    }

    close(tcp_fd);
    close(pty_master);
    close(pty_slave);
    return 0;
}

