// 注意：HIDL 是 C++ 接口，本文件仍需用 C++ 编译（.cpp）
// 但除 HIDL 交互外，全部改为 C 风格（pthread + POSIX）

#define LOG_TAG "BT_PROBE_BRIDGE"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

#define LISTEN_PORT       9002
#define INIT_TIMEOUT_SEC  10

static android::sp<IBluetoothHci> g_hci;

static int              g_sock     = -1;
static pthread_mutex_t  g_sock_mtx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t  g_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_init_cv  = PTHREAD_COND_INITIALIZER;
static int              g_init_done = 0;
static int              g_init_ok   = 0;

static void send_h4(uint8_t type, const uint8_t *p, size_t n)
{
    pthread_mutex_lock(&g_sock_mtx);
    if (g_sock >= 0) {
        if (write(g_sock, &type, 1) == 1) {
            if (n == 0 || write(g_sock, p, n) == (ssize_t)n) {
                /* ok */
            }
        }
    }
    pthread_mutex_unlock(&g_sock_mtx);
}

/* HIDL 回调接口只能是 C++ 类 */
class Cb : public IBluetoothHciCallbacks {
public:
    Return<void> initializationComplete(Status s) override {
        pthread_mutex_lock(&g_init_mtx);
        g_init_done = 1;
        g_init_ok   = (s == Status::SUCCESS);
        pthread_cond_broadcast(&g_init_cv);
        pthread_mutex_unlock(&g_init_mtx);
        printf("[HAL] initializationComplete status=%d ok=%d\n",   (int)s, g_init_ok);
        return Void();
    }
    Return<void> hciEventReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL->PC] EVENT len=%zu\n", v.size());
        send_h4(H4_EVENT, v.data(), v.size());
        return Void();
    }
    Return<void> aclDataReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL->PC] ACL len=%zu\n", v.size());
        send_h4(H4_ACL, v.data(), v.size());
        return Void();
    }
    Return<void> scoDataReceived(const hidl_vec<uint8_t>& v) override {
        printf("[HAL->PC] SCO len=%zu\n", v.size());
        send_h4(H4_SCO, v.data(), v.size());
        return Void();
    }
};

static int read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static void drop_fd(int fd)
{
    pthread_mutex_lock(&g_sock_mtx);
    if (g_sock == fd) {
        close(g_sock);
        g_sock = -1;
    }
    pthread_mutex_unlock(&g_sock_mtx);
}

static void *rx_loop(void *arg)
{
    (void)arg;
    printf("[TCP] rx thread started\n");

    for (;;) {
        int fd;
        pthread_mutex_lock(&g_sock_mtx);
        fd = g_sock;
        pthread_mutex_unlock(&g_sock_mtx);

        if (fd < 0) { usleep(100000); continue; }

        uint8_t type;
        if (!read_exact(fd, &type, 1)) goto peer_closed;

        if (type == H4_CMD) {
            uint8_t h[3];
            if (!read_exact(fd, h, 3)) goto peer_closed;
            uint32_t n = h[2];
            uint8_t *b = (uint8_t *)malloc(3 + (size_t)n);
            if (!b) goto peer_closed;
            memcpy(b, h, 3);
            if (n > 0 && !read_exact(fd, b + 3, n)) { free(b); goto peer_closed; }

            printf("[PC->HAL] CMD opcode=0x%02x%02x len=%u\n", h[1], h[0], n);
            {
                hidl_vec<uint8_t> pkt;
                pkt.setToExternal(b, 3 + (size_t)n);
                auto r = g_hci->sendHciCommand(pkt);
                if (!r.isOk()) printf("[HAL] sendHciCommand fail\n");
            }
            free(b);

        } else if (type == H4_ACL || type == H4_SCO) {
            uint8_t h[4];
            if (!read_exact(fd, h, 4)) goto peer_closed;
            uint32_t n = (uint32_t)h[2] | ((uint32_t)h[3] << 8);
            uint8_t *b = (uint8_t *)malloc(4 + (size_t)n);
            if (!b) goto peer_closed;
            memcpy(b, h, 4);
            if (n > 0 && !read_exact(fd, b + 4, n)) { free(b); goto peer_closed; }

            printf("[PC->HAL] %s len=%u\n",
                   type == H4_ACL ? "ACL" : "SCO", n);
            {
                hidl_vec<uint8_t> pkt;
                pkt.setToExternal(b, 4 + (size_t)n);
                auto r = (type == H4_ACL) ? g_hci->sendAclData(pkt)
                                          : g_hci->sendScoData(pkt);
                if (!r.isOk())
                    printf("[HAL] send%s fail\n",
                           type == H4_ACL ? "Acl" : "Sco");
            }
            free(b);

        } else {
            printf("[TCP] unknown H4 type=%u\n", type);
            goto peer_closed;
        }
        continue;

    peer_closed:
        drop_fd(fd);
        printf("[TCP] peer closed\n");
    }
    return NULL;
}

/* 返回：1=成功，0=失败，-1=超时 */
static int wait_init(int timeout_sec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_sec;

    pthread_mutex_lock(&g_init_mtx);
    while (!g_init_done) {
        int rc = pthread_cond_timedwait(&g_init_cv, &g_init_mtx, &ts);  //阻塞等待{ pthread_cond_broadcast 唤醒 /超时（ts) 到了）唤醒 }
        if (rc == ETIMEDOUT) break;
    }
    int done = g_init_done;
    int ok   = g_init_ok;
    pthread_mutex_unlock(&g_init_mtx);

    if (!done) return -1;
    return ok ? 1 : 0;
}

int main(void)
{
    printf("===== BT Probe Bridge (standard getService path, C-style) =====\n");

    /* ProcessState::self()->startThreadPool() 是 C++ 接口，无法避免 */
    android::hardware::ProcessState::self()->startThreadPool();

    /* 标准接口，不硬编码 .so 路径 */
    g_hci = IBluetoothHci::getService();
    if (g_hci == nullptr) {
        printf("[-] IBluetoothHci::getService() = nullptr\n");
        printf("    (VINTF 未声明 / HAL 未启动 / 已迁移 AIDL)\n");
        return 1;
    }
    printf("[+] getService() OK, isRemote=%d (%s)\n",
           (int)g_hci->isRemote(),
           g_hci->isRemote() ? "Binderized" : "Passthrough");

    android::sp<IBluetoothHciCallbacks> cb = new Cb();
    auto r = g_hci->initialize(cb);
    if (!r.isOk()) {
        printf("[-] initialize transport error\n");
        return 1;
    }

    int st = wait_init(INIT_TIMEOUT_SEC);
    if (st < 0) { printf("[-] HAL init timeout\n"); return 1; }
    if (st == 0) { printf("[-] HAL init failed\n");  return 1; }
    printf("[+] HAL initialized\n");

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        printf("[-] socket: %s\n", strerror(errno));
        return 1;
    }
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(LISTEN_PORT);

    if (bind(sfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        printf("[-] bind %d: %s\n", LISTEN_PORT, strerror(errno));
        return 1;
    }
    if (listen(sfd, 1) < 0) {
        printf("[-] listen: %s\n", strerror(errno));
        return 1;
    }
    printf("[+] listening on :%d, waiting for PC...\n", LISTEN_PORT);

    pthread_t th;
    if (pthread_create(&th, NULL, rx_loop, NULL) != 0) {
        printf("[-] pthread_create rx_loop fail\n");
        return 1;
    }
    pthread_detach(th);

    for (;;) {
        int fd = accept(sfd, NULL, NULL);
        if (fd < 0) continue;

        pthread_mutex_lock(&g_sock_mtx);
        if (g_sock >= 0) close(g_sock);
        g_sock = fd;
        pthread_mutex_unlock(&g_sock_mtx);

        printf("[+] PC connected\n");
    }

    return 0; /* unreachable */
}