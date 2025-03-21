#include "esp_http.h"
#include <esp_tls.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "EspHttp";

EspHttp::EspHttp() : client_(nullptr), status_code_(0) {}

EspHttp::~EspHttp() {
    Close();
}

void EspHttp::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

bool EspHttp::Open(const std::string& method, const std::string& url, const std::string& content) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;  // 设置连接超时时间为15秒
    config.buffer_size = 4096;  // 缓冲区大小
    config.skip_cert_common_name_check = true;  // 跳过证书通用名称检查
    config.keep_alive_enable = true;  // 禁用keep-alive
    config.disable_auto_redirect = false;  // 允许自动重定向
    config.event_handler = HttpEventHandler;  // 事件处理器
    config.user_data = this;

    // 允许自动选择连接类型 (IPv4或IPv6)
    config.transport_type = HTTP_TRANSPORT_OVER_TCP; // 让ESP-IDF自动选择连接类型
    config.is_async = false; // 使用同步模式，便于控制
    
    ESP_LOGI(TAG, "允许自动选择连接类型 (IPv4或IPv6)");

    // 内部连接重试
    const int max_connect_retries = 3;
    int retry_count = 0;
    esp_err_t last_err = ESP_OK;

    ESP_LOGI(TAG, "Opening HTTP connection to %s (timeout: %d ms)", url.c_str(), config.timeout_ms);

    ESP_LOGI(TAG, "HTTP 方法: %s, 内容长度: %zu", method.c_str(), content.length());

    // 尝试DNS预解析（仅打印，不影响后续操作）
    bool has_ipv6 = this->resolve_url(url.c_str());
    if (has_ipv6) {
        ESP_LOGI(TAG, "目标支持IPv6访问");
    } else {
        ESP_LOGI(TAG, "目标可能不支持IPv6，将尝试IPv4");
    }
    // 多次尝试连接
    while (retry_count < max_connect_retries) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "HTTP连接重试 %d/%d...", retry_count + 1, max_connect_retries);
            vTaskDelay(pdMS_TO_TICKS(1000)); // 重试前等待1秒
        }

        // 初始化客户端
        if (client_) {
            esp_http_client_cleanup(client_);
            client_ = nullptr;
        }

        client_ = esp_http_client_init(&config);
        if (!client_) {
            ESP_LOGE(TAG, "初始化HTTP客户端失败");
            return false;
        }

        // 设置方法和头部
        esp_http_client_set_method(client_, 
            method == "GET" ? HTTP_METHOD_GET : 
            method == "POST" ? HTTP_METHOD_POST : 
            method == "PUT" ? HTTP_METHOD_PUT : 
            method == "DELETE" ? HTTP_METHOD_DELETE : HTTP_METHOD_GET);

        for (const auto& header : headers_) {
            esp_http_client_set_header(client_, header.first.c_str(), header.second.c_str());
        }

        // 尝试打开连接
        ESP_LOGI(TAG, "尝试打开连接，Method = %s, content.length() = %d", method.c_str(), content.length());
        last_err = esp_http_client_open(client_, content.length());
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP连接成功建立");
            break; // 连接成功
        }

        // 连接失败，打印详细错误
        ESP_LOGE(TAG, "尝试 %d/%d: HTTP连接失败: %s (0x%x)", 
                retry_count + 1, max_connect_retries, esp_err_to_name(last_err), last_err);
        
        retry_count++;
    }

    // 如果所有尝试都失败
    if (last_err != ESP_OK) {
        ESP_LOGE(TAG, "多次尝试后HTTP连接仍然失败: %s (0x%x)", esp_err_to_name(last_err), last_err);
        
        // 提供更多诊断信息
        if (last_err == ESP_ERR_HTTP_CONNECT) {
            ESP_LOGE(TAG, "HTTP 连接失败，请检查网络连接和服务器地址");
            ESP_LOGE(TAG, "请确认：1.服务器地址正确 2.网络稳定 3.服务器在线 4.防火墙未阻止连接");
            ESP_LOGE(TAG, "如果使用的是 IPv6 地址，请确保网络和服务器支持 IPv6");
        } else if (last_err == ESP_ERR_HTTP_EAGAIN) {
            ESP_LOGE(TAG, "HTTP 连接超时，请检查网络连接质量或增加超时时间");
        } else {
            ESP_LOGE(TAG, "DNS 解析失败或其他网络问题，请检查域名是否正确");
        }
        
        Close();
        return false;
    }

    auto written = esp_http_client_write(client_, content.data(), content.length());
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write request body: %s", esp_err_to_name(last_err));
        Close();
        return false;
    }
    content_length_ = esp_http_client_fetch_headers(client_);
    if (content_length_ <= 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        Close();
        return false;
    }
    return true;
}

void EspHttp::Close() {
    if (client_) {
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }
}

int EspHttp::GetStatusCode() const {
    return status_code_;
}

std::string EspHttp::GetResponseHeader(const std::string& key) const {
    if (!client_) return "";
    char* value = nullptr;
    esp_http_client_get_header(client_, key.c_str(), &value);
    if (!value) return "";
    std::string result(value);
    free(value);
    return result;
}

size_t EspHttp::GetBodyLength() const {
    return content_length_;
}

const std::string& EspHttp::GetBody() {
    response_body_.resize(content_length_);
    assert(Read(const_cast<char*>(response_body_.data()), content_length_) == content_length_);
    return response_body_;
}

int EspHttp::Read(char* buffer, size_t buffer_size) {
    if (!client_) return -1;
    return esp_http_client_read(client_, buffer, buffer_size);
}

esp_err_t EspHttp::HttpEventHandler(esp_http_client_event_t *evt) {
    EspHttp* http = static_cast<EspHttp*>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                http->response_body_.append((char*)evt->data, evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool EspHttp::resolve_url(const char *url) {
    // 1. 解析URL以提取主机名
    const char *protocol_end = strstr(url, "://");
    if (!protocol_end) {
        ESP_LOGE(TAG, "URL格式无效: %s", url);
        return false;
    }
    
    protocol_end += 3; // 跳过"://"
    
    // 检查是否有IPv6地址格式 [xxxx:xxxx::xxxx]
    bool is_ipv6_format = false;
    const char *host_start = protocol_end;
    if (*host_start == '[') {
        is_ipv6_format = true;
        host_start++; // 跳过'['
    }
    
    const char *host_end;
    if (is_ipv6_format) {
        host_end = strchr(host_start, ']');
        if (!host_end) {
            ESP_LOGE(TAG, "IPv6地址格式错误: %s", url);
            return false;
        }
    } else {
        // 寻找主机名结束位置（端口号、路径或结束符）
        host_end = strchr(host_start, ':'); // 检查端口
        const char *path = strchr(host_start, '/');
        
        if (host_end == NULL || (path != NULL && path < host_end)) {
            host_end = path;
        }
        
        if (host_end == NULL) {
            host_end = host_start + strlen(host_start);
        }
    }
    
    // 提取主机名
    size_t hostname_len = host_end - host_start;
    char hostname[256] = {0};
    if (hostname_len >= sizeof(hostname)) {
        ESP_LOGE(TAG, "主机名太长");
        return false;
    }
    strncpy(hostname, host_start, hostname_len);
    hostname[hostname_len] = '\0';
    
    ESP_LOGI(TAG, "从URL提取的主机名: %s", hostname);
    
    // 2. 解析提取的主机名
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_UNSPEC;  // 同时支持IPv4和IPv6
    hints.ai_socktype = SOCK_STREAM;
    
    int err = getaddrinfo(hostname, "80", &hints, &res);
    if (err != 0) {
        ESP_LOGE(TAG, "DNS解析失败: %d", err);
        return false;
    }
    
    // 打印解析结果
    bool found_ipv6 = false;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        char ipstr[INET6_ADDRSTRLEN];
        void *addr;
        
        if (p->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            inet_ntop(AF_INET, addr, ipstr, sizeof(ipstr));
            ESP_LOGI(TAG, "解析到IPv4地址: %s", ipstr);
        } else if (p->ai_family == AF_INET6) { // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            inet_ntop(AF_INET6, addr, ipstr, sizeof(ipstr));
            ESP_LOGI(TAG, "解析到IPv6地址: %s", ipstr);
            found_ipv6 = true;
        }
    }
    
    freeaddrinfo(res);
    return found_ipv6; // 如果找到IPv6地址则返回true
}
