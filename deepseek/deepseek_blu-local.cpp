#define LOG_TAG "BT_PROBE_BRIDGE"

#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>

#include <android/hardware/bluetooth/1.0/IBluetoothHci.h>
#include <android/hardware/bluetooth/1.0/IBluetoothHciCallbacks.h>
#include <hwbinder/ProcessState.h>

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::bluetooth::V1_0::IBluetoothHci;
using ::android::hardware::bluetooth::V1_0::IBluetoothHciCallbacks;
using ::android::hardware::bluetooth::V1_0::Status;

#ifndef N_HCI
#define N_HCI 15
#endif
#ifndef HCIUARTSETPROTO
#define HCIUARTSETPROTO _IOW('U', 200, int)
#endif
#ifndef HCI_UART_H4
#define HCI_UART_H4 0
#endif

#define H4_CMD   1
#define H4_ACL   2
#define H4_SCO   3
#define H4_EVENT 4

static android::sp<IBluetoothHci> g_hci;
static int g_sock = -1;                 // 本地 pty 方案里指向 pty master
static std::mutex g_sock_mtx;

static std::mutex g_init_mtx;
static std::condition_variable g_init_cv;
static bool g_init_done = false;
static bool g_init_ok = false;

static void send_h4(uint8_t type, const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(g_sock_mtx);
    if (g_sock < 0) return;
    if (write(g_sock, &type, 1) != 1) return;
    if (n && write(g_sock, p, n) != (ssize_t)n) return;
}

class Cb : public IBluetoothHciCallbacks {
public:
    Return<void> initializationComplete(Status s) override {
        {
            std::lock_guard<std::mutex> lk(g_init_mtx);
            g_init_done = true;
            g_init_ok = (s == Status::SUCCESS);
        }
        printf("[HAL] initializationComplete status=%d ok=%d\n",
               (int)s, (int)g_init_ok);
        g_init_cv.notify_all();
        return Void();
    }
    Return<void> hciEventReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL→PC] EVENT len=%zu\n", v.size());
        send_h4(H4_EVENT, v.data(), v.size());
        return Void();
    }
    Return<void> aclDataReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL→PC] ACL len=%zu\n", v.size());
        send_h4(H4_ACL, v.data(), v.size());
        return Void();
    }
    Return<void> scoDataReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL→PC] SCO len=%zu\n", v.size());
        send_h4(H4_SCO, v.data(), v.size());
        return Void();
    }
};

static bool read_exact(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void rx_loop() {
    printf("[PTY] rx thread started\n");
    while (true) {
        int fd;
        { std::lock_guard<std::mutex> lk(g_sock_mtx); fd = g_sock; }
        if (fd < 0) { usleep(100000); continue; }

        uint8_t type;
        if (!read_exact(fd, &type, 1)) {
            std::lock_guard<std::mutex> lk(g_sock_mtx);
            if (g_sock == fd) { close(g_sock); g_sock = -1; }
            printf("[PTY] master closed (pty_slave gone? main exited?)\n");
            return;
        }

        if (type == H4_CMD) {
            uint8_t h[3];
            if (!read_exact(fd, h, 3)) return;
            uint32_t n = h[2];
            std::vector<uint8_t> b(3 + n);
            memcpy(b.data(), h, 3);
            if (n && !read_exact(fd, b.data() + 3, n)) return;
            printf("[PC→HAL] CMD opcode=0x%02x%02x len=%u\n", h[1], h[0], n);
            hidl_vec<uint8_t> pkt(b);          // 拷贝，不用 setToExternal
            auto r = g_hci->sendHciCommand(pkt);
            if (!r.isOk()) printf("[HAL] sendHciCommand fail\n");

        } else if (type == H4_ACL) {
            uint8_t h[4];                      // ACL: 2 handle + 2 length
            if (!read_exact(fd, h, 4)) return;
            uint32_t n = h[2] | (h[3] << 8);
            std::vector<uint8_t> b(4 + n);
            memcpy(b.data(), h, 4);
            if (n && !read_exact(fd, b.data() + 4, n)) return;
            printf("[PC→HAL] ACL len=%u\n", n);
            hidl_vec<uint8_t> pkt(b);
            auto r = g_hci->sendAclData(pkt);
            if (!r.isOk()) printf("[HAL] sendAclData fail\n");

        } else if (type == H4_SCO) {
            uint8_t h[3];                      // SCO: 2 handle + 1 length
            if (!read_exact(fd, h, 3)) return;
            uint32_t n = h[2];
            std::vector<uint8_t> b(3 + n);
            memcpy(b.data(), h, 3);
            if (n && !read_exact(fd, b.data() + 3, n)) return;
            printf("[PC→HAL] SCO len=%u\n", n);
            hidl_vec<uint8_t> pkt(b);
            auto r = g_hci->sendScoData(pkt);
            if (!r.isOk()) printf("[HAL] sendScoData fail\n");

        } else {
            printf("[PTY] unknown H4 type=%u\n", type);
            return;
        }
    }
}

int main() {
    printf("===== BT Local PTY Bridge (standard getService) =====\n");
    android::hardware::ProcessState::self()->startThreadPool();

    // 1) 获取 HAL（标准接口，不硬编码 .so 路径）
    g_hci = IBluetoothHci::getService();
    if (!g_hci) {
        printf("[-] IBluetoothHci::getService() = nullptr\n");
        return 1;
    }
    printf("[+] getService() OK, isRemote=%d (%s)\n",
           g_hci->isRemote(),
           g_hci->isRemote() ? "Binderized" : "Passthrough");

    // 2) 初始化并等 initializationComplete
    android::sp<IBluetoothHciCallbacks> cb = new Cb();
    auto r = g_hci->initialize(cb);
    if (!r.isOk()) { printf("[-] initialize transport error\n"); return 1; }
    {
        std::unique_lock<std::mutex> lk(g_init_mtx);
        if (!g_init_cv.wait_for(lk, std::chrono::seconds(10),
                                []{ return g_init_done; })) {
            printf("[-] HAL init timeout\n"); return 1;
        }
        if (!g_init_ok) { printf("[-] HAL init failed\n"); return 1; }
    }
    printf("[+] HAL initialized\n");

    // 3) 创建 pty（slave 一直保持打开，直到 main 退出）
    int fdd = -1, pty_slave = -1;
    char slave_name[64];
    if (openpty(&fdd, &pty_slave, slave_name, nullptr, nullptr) < 0) {
        perror("openpty"); return -1;
    }
    printf("[+] pty master=%d slave=%s (pty_slave=%d kept open)\n",
           fdd, slave_name, pty_slave);

    // 4) 设置 g_sock，锁作用域仅覆盖这一行
    {
        std::lock_guard<std::mutex> lk(g_sock_mtx);
        g_sock = fdd;
    }

    // 5) 先起 rx_loop，让它进入 read 阻塞
    std::thread(rx_loop).detach();
    usleep(100000);

    // 6) 再挂 N_HCI 和 H4 协议（顺序不能反，否则内核 Reset 无人转发）
    int ldisc = N_HCI;
    if (ioctl(pty_slave, TIOCSETD, &ldisc) < 0) {
        perror("TIOCSETD(N_HCI)"); return -1;
    }
    int proto = HCI_UART_H4;
    if (ioctl(pty_slave, HCIUARTSETPROTO, proto) < 0) {
        perror("HCIUARTSETPROTO(H4)"); return -1;
    }
    printf("[+] H4 ldisc attached; kernel will send HCI_Reset now\n");
    printf("    验证: hciconfig -a | hcitool -i hci0 cmd 0x03 0x0c 0x00\n");

    // 7) 保持运行，pty_slave 不关闭
    while (true) sleep(1);
    return 0;
}
