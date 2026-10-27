#define LOG_TAG "gps_tcp_proxy"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <mutex>
#include <vector>

#include <android/hardware/gnss/1.0/IGnss.h>
#include <android/hardware/gnss/1.0/IGnssCallback.h>
#include <hwbinder/ProcessState.h>
#include <utils/Log.h>

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_bitfield;
using ::android::hardware::gnss::V1_0::IGnss;
using ::android::hardware::gnss::V1_0::IGnssCallback;

android::sp<IGnss> gnssHal = nullptr;
int client_socket = -1;
std::mutex socket_mutex;

class GnssCallbacks : public IGnssCallback {
public:
    GnssCallbacks() {}

    // 1. 拦截 NMEA 原始文本数据流
    Return<void> gnssNmeaCb(int64_t timestamp, const hidl_string& nmea) override {
        std::lock_guard<std::mutex> lock(socket_mutex);
        if (client_socket >= 0) {
            ssize_t ret = write(client_socket, nmea.c_str(), nmea.size());
            if (ret < 0) {
                ALOGE("Failed to write NMEA data to TCP");
            } else {
                printf("%s", nmea.c_str()); 
                fflush(stdout);
            }
        }
        return Void();
    }

    // 2. 接收经纬度位置更新
    Return<void> gnssLocationCb(const ::android::hardware::gnss::V1_0::GnssLocation& location) override {
        ALOGV("Location updated: Lat: %f, Lon: %f", location.latitudeDegrees, location.longitudeDegrees);
        return Void();
    }

    // 3. 接收 GNSS 状态改变
    Return<void> gnssStatusCb(IGnssCallback::GnssStatusValue status) override {
        ALOGI("GNSS Status changed: %d", static_cast<int>(status));
        return Void();
    }

    // 4. 接收可见卫星状态信息（修复打印警告 %zu -> %u）
    Return<void> gnssSvStatusCb(const IGnssCallback::GnssSvStatus& svStatus) override {
        ALOGV("Visible Satellites count: %u", svStatus.numSvs);
        return Void();
    }

    // 5. 🌟【核心修复点】完美适配谷歌官方的错误拼写（少写了字母 i）以及专属的数据结构
    Return<void> gnssSetCapabilitesCb(hidl_bitfield<IGnssCallback::Capabilities> capabilities) override { 
        return Void(); 
    }

    // 6. 补全其余必须实现的硬件抽象虚函数
    Return<void> gnssAcquireWakelockCb() override { return Void(); }
    Return<void> gnssReleaseWakelockCb() override { return Void(); }
    Return<void> gnssRequestTimeCb() override { return Void(); }
    Return<void> gnssSetSystemInfoCb(const IGnssCallback::GnssSystemInfo& info) override { return Void(); }
};

int main() {
    printf("Starting GNSS TCP Proxy Daemon (Port 9001)...\n");
    android::hardware::ProcessState::self()->startThreadPool();

    printf("[*] Fetching GNSS HIDL service...\n");
    gnssHal = IGnss::getService("default");
    if (gnssHal == nullptr) {
        gnssHal = IGnss::getService("gnss");
    }

    if (gnssHal == nullptr) {
        printf("[-] 致命错误：无法在当前手机系统中找到任何 GNSS HIDL 定位服务！\n");
        return -1;
    }
    printf("[+] 成功连接至手机系统的 GNSS 定位硬件服务层！\n");

    android::sp<IGnssCallback> callbacks = new GnssCallbacks();
    Return<bool> result = gnssHal->setCallback(callbacks);
    if (!result.isOk() || !result) {
        printf("[-] 错误：无法向 GNSS HAL 注入数据拦截监听回调。\n");
        return -1;
    }
    printf("[+] 成功注册数据拦截回调。正在强行唤醒 GPS 搜星模块...\n");

    gnssHal->start();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(9001);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        printf("[-] 绑定端口 9001 失败！\n");
        return -1;
    }
    listen(server_fd, 1);
    printf("[+] 卫星数据转发服务器已就绪，正在监听端口 9001...\n");

    while (true) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) continue;

        printf("[+] 电脑端已通过 9001 端口接入！开始无损导流卫星数据...\n");
        std::lock_guard<std::mutex> lock(socket_mutex);
        if (client_socket >= 0) close(client_socket);
        client_socket = new_socket;
    }

    gnssHal->stop();
    close(server_fd);
    return 0;
}

