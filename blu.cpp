#define LOG_TAG "bt_tcp_proxy"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <mutex>
#include <vector>

#include <android/hardware/bluetooth/1.0/IBluetoothHci.h>
#include <android/hardware/bluetooth/1.0/IBluetoothHciCallbacks.h>
#include <android/hardware/bluetooth/1.0/types.h>
#include <hwbinder/ProcessState.h>

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::bluetooth::V1_0::IBluetoothHci;
using ::android::hardware::bluetooth::V1_0::IBluetoothHciCallbacks;
using ::android::hardware::bluetooth::V1_0::Status;
using ::android::hardware::bluetooth::V1_0::HciPacket;

#define H4_TYPE_COMMAND 1
#define H4_TYPE_ACL     2
#define H4_TYPE_SCO     3
#define H4_TYPE_EVENT   4

android::sp<IBluetoothHci> btHci = nullptr;
int client_socket = -1;
std::mutex socket_mutex;

// 1. 向上发给 Ubuntu：采用标准 H4 格式 [1字节类型 + 载荷]
void send_to_ubuntu(uint8_t type, const hidl_vec<uint8_t>& data) {
	std::lock_guard<std::mutex> lock(socket_mutex);
	if (client_socket < 0) return;

	// 发送 1 字节 H4 类型
	write(client_socket, &type, 1);
	// 发送原始数据
	write(client_socket, data.data(), data.size());
}

class BluetoothHciCallbacks : public IBluetoothHciCallbacks {
	public:
		BluetoothHciCallbacks() {}
		Return<void> initializationComplete(Status status) override { return Void(); }

		Return<void> hciEventReceived(const hidl_vec<uint8_t>& event) override {
			send_to_ubuntu(H4_TYPE_EVENT, event);
			return Void();
		}
		Return<void> aclDataReceived(const hidl_vec<uint8_t>& data) override {
			send_to_ubuntu(H4_TYPE_ACL, data);
			return Void();
		}
		Return<void> scoDataReceived(const hidl_vec<uint8_t>& data) override {
			send_to_ubuntu(H4_TYPE_SCO, data);
			return Void();
		}
};

// 2. 从 Ubuntu 接收：解析标准的 H4 格式流
void tcp_rx_thread_func() {
	printf("TCP Receiver thread started.\n");
	while (true) {
		{
			std::lock_guard<std::mutex> lock(socket_mutex);
			if (client_socket < 0) {
				usleep(100000);
				continue;
			}
		}

		uint8_t type;
		// 先读取 1 字节的 H4 类型
		ssize_t ret = recv(client_socket, &type, 1, MSG_WAITALL);
		if (ret <= 0) {
			printf("Client disconnected.\n");
			std::lock_guard<std::mutex> lock(socket_mutex);
			close(client_socket);
			client_socket = -1;
			continue;
		}

		// 根据蓝牙 H4 标准协议，动态解析后续载荷长度
		uint32_t payload_len = 0;

		if (type == H4_TYPE_COMMAND) {
			// Command 结构: 2字节 Opcode + 1字节长度参数
			uint8_t cmd_hdr[3];
			recv(client_socket, cmd_hdr, 3, MSG_WAITALL);
			payload_len = cmd_hdr[2]; // 第 3 字节是命令参数长度

			std::vector<uint8_t> buffer(3 + payload_len);
			memcpy(buffer.data(), cmd_hdr, 3);
			if (payload_len > 0) {
				recv(client_socket, buffer.data() + 3, payload_len, MSG_WAITALL);
			}

			hidl_vec<uint8_t> packet;
			packet.setToExternal(buffer.data(), buffer.size());
			if (btHci != nullptr) btHci->sendHciCommand(packet);

		} else if (type == H4_TYPE_ACL) {
			// ACL 结构: 2字节 Handle + 2字节长度参数
			uint8_t acl_hdr[4];
			recv(client_socket, acl_hdr, 4, MSG_WAITALL);
			payload_len = acl_hdr[2] | (acl_hdr[3] << 8); // 16位小端长度

			std::vector<uint8_t> buffer(4 + payload_len);
			memcpy(buffer.data(), acl_hdr, 4);
			if (payload_len > 0) {
				recv(client_socket, buffer.data() + 4, payload_len, MSG_WAITALL);
			}

			hidl_vec<uint8_t> packet;
			packet.setToExternal(buffer.data(), buffer.size());
			if (btHci != nullptr) btHci->sendAclData(packet);

		} else if (type == H4_TYPE_SCO) {
			// SCO 结构: 2字节 Handle + 1字节长度参数
			uint8_t sco_hdr[3];
			recv(client_socket, sco_hdr, 3, MSG_WAITALL);
			payload_len = sco_hdr[2];

			std::vector<uint8_t> buffer(3 + payload_len);
			memcpy(buffer.data(), sco_hdr, 3);
			if (payload_len > 0) {
				recv(client_socket, buffer.data() + 3, payload_len, MSG_WAITALL);
			}

			hidl_vec<uint8_t> packet;
			packet.setToExternal(buffer.data(), buffer.size());
			if (btHci != nullptr) btHci->sendScoData(packet);
		}
	}
}

int main() {
	printf("Starting Bluetooth H4 TCP Proxy...\n");	
	android::hardware::ProcessState::self()->startThreadPool();

	btHci = IBluetoothHci::getService();
	if (btHci == nullptr) {
		// 修改为针对 Android 8 兼容的写法：
		btHci = IBluetoothHci::getService("bluetooth_hci");
		if (btHci == nullptr) {
			// 防御性安全检查：如果是标准的，再尝试一次 default
			btHci = IBluetoothHci::getService("default");
		}

		// 🌟 必须加上防御保护，防止空指针引发 Segmentation fault 崩溃
		if (btHci == nullptr) {
			printf("[-] 致命错误：无法在 Android 8 系统中找到任何蓝牙 HIDL 服务实例！\n");
			return -1;
		}
	}
	android::sp<IBluetoothHciCallbacks> callbacks = new BluetoothHciCallbacks();
	btHci->initialize(callbacks);

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(9002);

	bind(server_fd, (struct sockaddr*)&address, sizeof(address));
	listen(server_fd, 1);
	printf("ok\n");
	std::thread rx_thread(tcp_rx_thread_func);
	rx_thread.detach();

	while (true) {
		int new_socket = accept(server_fd, nullptr, nullptr);
		std::lock_guard<std::mutex> lock(socket_mutex);
		if (client_socket >= 0) close(client_socket);
		client_socket = new_socket;
		printf("Ubuntu client connected via socat!\n");
	}
	return 0;
}

