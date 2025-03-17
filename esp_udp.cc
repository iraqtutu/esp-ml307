#include "esp_udp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

static const char *TAG = "EspUdp";

EspUdp::EspUdp() : udp_fd_(-1) {
}

EspUdp::~EspUdp() {
    Disconnect();
}

bool EspUdp::Connect(const std::string& host, int port) {
    // 使用getaddrinfo替代gethostbyname，以支持IPv4和IPv6
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;    // 允许IPv4或IPv6
    hints.ai_socktype = SOCK_DGRAM; // UDP套接字
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    int ret = getaddrinfo(host.c_str(), port_str, &hints, &result);
    if (ret != 0) {
        ESP_LOGE(TAG, "getaddrinfo失败: 错误码 %d", ret);
        return false;
    }
    
    // 尝试每个地址直到成功连接
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        udp_fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (udp_fd_ < 0) {
            continue; // 尝试下一个地址
        }
        
        // 针对IPv6套接字，设置IPV6_V6ONLY选项为0，允许IPv4映射地址
        if (rp->ai_family == AF_INET6) {
            int ipv6only = 0;
            if (setsockopt(udp_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only)) < 0) {
                ESP_LOGW(TAG, "无法设置IPV6_V6ONLY选项");
            }
        }
        
        if (connect(udp_fd_, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; // 成功
        }
        
        close(udp_fd_);
        udp_fd_ = -1;
    }
    
    freeaddrinfo(result);
    
    if (udp_fd_ < 0) {
        ESP_LOGE(TAG, "无法连接到 %s:%d", host.c_str(), port);
        return false;
    }
    
    // 打印连接信息
    char addr_str[128];
    if (rp->ai_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
        inet_ntop(AF_INET, &ipv4->sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "已连接到IPv4地址: %s:%d", addr_str, port);
    } else if (rp->ai_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
        inet_ntop(AF_INET6, &ipv6->sin6_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "已连接到IPv6地址: [%s]:%d", addr_str, port);
    }
    
    connected_ = true;
    receive_thread_ = std::thread(&EspUdp::ReceiveTask, this);
    return true;
}

void EspUdp::Disconnect() {
    if (udp_fd_ != -1) {
        close(udp_fd_);
        udp_fd_ = -1;
    }
    connected_ = false;
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

int EspUdp::Send(const std::string& data) {
    int ret = send(udp_fd_, data.data(), data.size(), 0);
    if (ret <= 0) {
        connected_ = false;
        ESP_LOGE(TAG, "Send failed: ret=%d", ret);
    }
    return ret;
}

void EspUdp::ReceiveTask() {
    while (true) {
        std::string data;
        data.resize(1500);
        int ret = recv(udp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            break;
        }
        data.resize(ret);
        if (message_callback_) {
            message_callback_(data);
        }
    }
}
