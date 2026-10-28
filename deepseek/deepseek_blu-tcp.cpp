#define LOG_TAG "BT_PROBE_BRIDGE"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
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

#define H4_CMD   1
#define H4_ACL   2
#define H4_SCO   3
#define H4_EVENT 4

static android::sp<IBluetoothHci> g_hci;
static int g_sock = -1;
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
        printf("[HAL] initializationComplete status=%d ok=%d\n", (int)s, g_init_ok);
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
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void rx_loop() {
    printf("[TCP] rx thread started\n");
    while (true) {
        int fd;
        { std::lock_guard<std::mutex> lk(g_sock_mtx); fd = g_sock; }
        if (fd < 0) { usleep(100000); continue; }

        uint8_t type;
        if (!read_exact(fd, &type, 1)) {
            std::lock_guard<std::mutex> lk(g_sock_mtx);
            if (g_sock == fd) { close(g_sock); g_sock = -1; }
            printf("[TCP] peer closed\n");
            continue;
        }

        hidl_vec<uint8_t> pkt;
        if (type == H4_CMD) {
            uint8_t h[3];
            if (!read_exact(fd, h, 3)) goto drop;
            uint32_t n = h[2];
            std::vector<uint8_t> b(3 + n);
            memcpy(b.data(), h, 3);
            if (n && !read_exact(fd, b.data() + 3, n)) goto drop;
            printf("[PC→HAL] CMD opcode=0x%02x%02x len=%u\n", h[1], h[0], n);
            pkt.setToExternal(b.data(), b.size());
            { auto r = g_hci->sendHciCommand(pkt);
              if (!r.isOk()) printf("[HAL] sendHciCommand fail\n"); }
        } else if (type == H4_ACL || type == H4_SCO) {
            uint8_t h[4];
            if (!read_exact(fd, h, 4)) goto drop;
            uint32_t n = h[2] | (h[3] << 8);
            std::vector<uint8_t> b(4 + n);
            memcpy(b.data(), h, 4);
            if (n && !read_exact(fd, b.data() + 4, n)) goto drop;
            printf("[PC→HAL] %s len=%u\n", type == H4_ACL ? "ACL" : "SCO", n);
            pkt.setToExternal(b.data(), b.size());
            auto r = (type == H4_ACL) ? g_hci->sendAclData(pkt)
                                      : g_hci->sendScoData(pkt);
            if (!r.isOk()) printf("[HAL] send%s fail\n",
                                  type == H4_ACL ? "Acl" : "Sco");
        } else {
            printf("[TCP] unknown H4 type=%u\n", type);
            goto drop;
        }
        continue;
    drop:
        {
            std::lock_guard<std::mutex> lk(g_sock_mtx);
            if (g_sock == fd) { close(g_sock); g_sock = -1; }
        }
    }
}

int main() {
    printf("===== BT Probe Bridge (standard getService path) =====\n");
    android::hardware::ProcessState::self()->startThreadPool();

    // ★ 关键：标准接口，不硬编码 .so 路径
    g_hci = IBluetoothHci::getService();
    if (!g_hci) {
        printf("[-] IBluetoothHci::getService() = nullptr\n");
        printf("    (VINTF 未声明 / HAL 未启动 / 已迁移 AIDL)\n");
        return 1;
    }
    printf("[+] getService() OK, isRemote=%d (%s)\n",
           g_hci->isRemote(), g_hci->isRemote() ? "Binderized" : "Passthrough");

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

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(9002);
    if (bind(sfd, (sockaddr*)&a, sizeof(a)) < 0) {
        printf("[-] bind 9002: %s\n", strerror(errno)); return 1;
    }
    listen(sfd, 1);
    printf("[+] listening on :9002, waiting for PC...\n");

    std::thread(rx_loop).detach();

    while (true) {
        int fd = accept(sfd, nullptr, nullptr);
        if (fd < 0) continue;
        std::lock_guard<std::mutex> lk(g_sock_mtx);
        if (g_sock >= 0) close(g_sock);
        g_sock = fd;
        printf("[+] PC connected\n");
    }
}
