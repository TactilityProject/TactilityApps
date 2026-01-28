#include "FtpServerCore.h"

#include <atomic>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dirent.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

namespace FtpServer {

// Internal constants
static constexpr uint32_t FTP_LOG_THROTTLE_MS = 200;
static constexpr uint32_t FTP_LOG_THROTTLE_MAX = 5;
static constexpr uint32_t FTP_SEND_TIMEOUT_MS = 200;
static constexpr uint32_t FTP_PROGRESS_INTERVAL = 100 * 1024; // 100KB
static constexpr uint32_t FTP_DIR_ENTRY_MIN_SPACE = 64;
static constexpr uint32_t FTP_TASK_STACK_SIZE = 1024 * 8; // 8KB - increased for safety margin with path operations

// Static member initialization
const Server::ftp_cmd_t Server::ftp_cmd_table[] = {
    {"FEAT"},
    {"SYST"},
    {"CDUP"},
    {"CWD"},
    {"PWD"},
    {"XPWD"},
    {"SIZE"},
    {"MDTM"},
    {"TYPE"},
    {"USER"},
    {"PASS"},
    {"PASV"},
    {"LIST"},
    {"RETR"},
    {"STOR"},
    {"DELE"},
    {"RMD"},
    {"MKD"},
    {"RNFR"},
    {"RNTO"},
    {"NOOP"},
    {"QUIT"},
    {"APPE"},
    {"NLST"},
    {"AUTH"}
};

// Constructor
Server::Server()
    : xEventTask(nullptr),
      ftp_task_handle(nullptr),
      ftp_mutex(nullptr),
      ftp_buff_size(FTPSERVER_BUFFER_SIZE),
      ftp_timeout(FTP_CMD_TIMEOUT_MS),
      TAG("[Server]"),
      MOUNT_POINT(""),
      ftp_path(nullptr),
      ftp_scratch_buffer(nullptr),
      ftp_cmd_buffer(nullptr),
      ftp_stop(0),
      ftp_nlist(0),
      ftp_cmd_port(FTP_CMD_PORT) {
    ftp_mutex = xSemaphoreCreateMutex();
    if (!ftp_mutex) {
        ESP_LOGE(TAG, "Failed to create FTP mutex!");
    }
    memset(&ftp_data, 0, sizeof(ftp_data_t));
    memset(ftp_user, 0, sizeof(ftp_user));
    memset(ftp_pass, 0, sizeof(ftp_pass));
}

// Destructor
Server::~Server() {
    stop();
    deinit();
    if (ftp_mutex) {
        vSemaphoreDelete(ftp_mutex);
        ftp_mutex = nullptr;
    }
}

static uint32_t last_screen_log_ms = 0;
static uint32_t screen_log_count = 0;

static std::atomic<void (*)(const char*)> screen_log_callback {nullptr};

void Server::register_screen_log_callback(void (*callback)(const char*)) {
    screen_log_callback.store(callback, std::memory_order_release);
}

void Server::log_to_screen(const char* format, ...) {
    auto cb = screen_log_callback.load(std::memory_order_acquire);
    if (!cb) return;

    // Throttle progress messages (only every Nth call)
    if (strstr(format, "total:") != nullptr) {
        static uint32_t progress_counter = 0;
        if (++progress_counter % 5 != 0) return; // Show every 5th progress
    }

    uint32_t now = mp_hal_ticks_ms();

    if (now - last_screen_log_ms < FTP_LOG_THROTTLE_MS) {
        screen_log_count++;
        if (screen_log_count > FTP_LOG_THROTTLE_MAX) {
            return;
        }
    } else {
        screen_log_count = 0;
        last_screen_log_ms = now;
    }

    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    cb(buffer);
}

// Helper functions

// Sanitize path to prevent path traversal attacks and check length constraints
// Returns false if path is invalid/malicious or exceeds max_len
// If max_len is 0, only traversal checks are performed (no length check)
bool Server::sanitize_path(const char* path, size_t max_len) {
    if (!path) return true;

    size_t path_len = strlen(path);
    if (path_len == 0) return true;

    // Check path length if max_len is specified
    if (max_len > 0 && path_len >= max_len) {
        ESP_LOGW(TAG, "Path too long (%zu >= %zu): rejected", path_len, max_len);
        return false;
    }

    // Check for dangerous sequences that could escape the allowed directories
    const char* p = path;
    while (*p) {
        // Look for /../ or /.. at end - these could traverse up
        if (p[0] == '/' && p[1] == '.' && p[2] == '.') {
            if (p[3] == '/' || p[3] == '\0') {
                // Found /../ or /.. - this is handled by close_child() but
                // we need to ensure it doesn't escape root storage directories
                // For now, reject paths with .. in middle of path after prefix
                // The FTP path navigation handles .. correctly via CDUP/CWD
                // but direct paths like /sdcard/foo/../../../etc should be blocked

                // Count depth after first component
                int depth = 0;
                const char* scan = path;
                bool after_prefix = false;
                while (*scan) {
                    if (*scan == '/') {
                        scan++;
                        if (!after_prefix && (*scan == 'd' || *scan == 's')) {
                            // Skip first path component (data or sdcard)
                            while (*scan && *scan != '/') scan++;
                            after_prefix = true;
                            continue;
                        }
                        if (scan[0] == '.' && scan[1] == '.' && (scan[2] == '/' || scan[2] == '\0')) {
                            depth--;
                            scan += 2;
                        } else if (scan[0] != '\0' && scan[0] != '/') {
                            depth++;
                            while (*scan && *scan != '/') scan++;
                        }
                    } else {
                        scan++;
                    }
                }
                if (depth < 0) {
                    ESP_LOGW(TAG, "Path traversal attempt blocked: %s", path);
                    return false;
                }
            }
        }
        p++;
    }
    return true;
}

void Server::translate_path(char* actual, size_t actual_size, const char* display) {
    if (actual_size == 0) return;

    // Check for /data prefix
    const char* internal_prefix = "/" FTP_STORAGE_NAME_INTERNAL;
    size_t internal_len = strlen(internal_prefix);

    if (strncmp(display, internal_prefix, internal_len) == 0 &&
        (display[internal_len] == '/' || display[internal_len] == '\0')) {
        const char* suffix = display + internal_len;
        snprintf(actual, actual_size, "%s%s", VFS_NATIVE_INTERNAL_MP, suffix);
        return;
    }

    // Check for /sdcard prefix
    const char* sd_prefix = "/" FTP_STORAGE_NAME_SDCARD;
    size_t sd_len = strlen(sd_prefix);

    if (strncmp(display, sd_prefix, sd_len) == 0 &&
        (display[sd_len] == '/' || display[sd_len] == '\0')) {
        const char* suffix = display + sd_len;
        snprintf(actual, actual_size, "%s%s", VFS_NATIVE_EXTERNAL_MP, suffix);
        return;
    }

    // No match - use display path as-is
    snprintf(actual, actual_size, "%s", display);
}

void Server::get_full_path(char* fullname, size_t size, const char* display_path) {
    char actual[FTP_MAX_PATH_SIZE];
    translate_path(actual, sizeof(actual), display_path);
    snprintf(fullname, size, "%s%s", MOUNT_POINT, actual);
}

bool Server::secure_compare(const char* a, const char* b, size_t len) {
    volatile uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    return result == 0;
}

uint64_t Server::mp_hal_ticks_ms() {
    uint64_t time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return time_ms;
}

bool Server::add_virtual_dir_if_mounted(const char* mount_point, const char* name, char* list, uint32_t maxlistsize, uint32_t* next) {
    DIR* test_dir = opendir(mount_point);
    if (test_dir == nullptr) return false;
    closedir(test_dir);

    if (*next >= maxlistsize) return false;

    struct dirent de = {};
    de.d_type = DT_DIR;
    strncpy(de.d_name, name, sizeof(de.d_name) - 1);

    char* list_ptr = list + *next;
    uint32_t remaining = maxlistsize - *next;
    if (remaining < 128) return false; // Ensure adequate space for entry
    *next += get_eplf_item(list_ptr, remaining, &de);
    return true;
}

void Server::stoupper(char* str) {
    while (str && *str != '\0') {
        *str = (char)toupper((int)(*str));
        str++;
    }
}

// File operations
bool Server::open_file(const char* path, const char* mode) {
    ESP_LOGD(TAG, "open_file: path=[%s]", path);

    // Validate path to prevent traversal attacks and check length
    if (!sanitize_path(path, FTP_MAX_PATH_SIZE)) {
        ESP_LOGW(TAG, "open_file: invalid path rejected");
        return false;
    }

    char fullname[FTP_MAX_PATH_SIZE];
    get_full_path(fullname, sizeof(fullname), path);

    // Check if path is on SD Card
    if (strncmp(fullname, VFS_NATIVE_EXTERNAL_MP, strlen(VFS_NATIVE_EXTERNAL_MP)) == 0) {
        // Verify SD Card is still accessible
        struct stat st;
        // Delay before SD card operation to prevent SPI bus conflicts
        // on devices like T-Deck Plus
        vTaskDelay(pdMS_TO_TICKS(5));
        if (stat(VFS_NATIVE_EXTERNAL_MP, &st) != 0) {
            ESP_LOGE(TAG, "SD Card not accessible!");
            log_to_screen("[!!] SD Card unavailable");
            return false;
        }
    }

    ESP_LOGD(TAG, "open_file: fullname=[%s]", fullname);
    // Small delay before file open to allow SPI bus to settle
    vTaskDelay(pdMS_TO_TICKS(2));
    ftp_data.fp = fopen(fullname, mode);
    if (ftp_data.fp == nullptr) {
        ESP_LOGE(TAG, "open_file: open fail [%s]", fullname);
        return false;
    }
    ftp_data.e_open = E_FTP_FILE_OPEN;
    return true;
}

void Server::close_files_dir() {
    if (ftp_data.e_open == E_FTP_FILE_OPEN) {
        fclose(ftp_data.fp);
        ftp_data.fp = nullptr;
    } else if (ftp_data.e_open == E_FTP_DIR_OPEN) {
        if (!ftp_data.listroot) {
            closedir(ftp_data.dp);
        }
        ftp_data.dp = nullptr;
    }
    ftp_data.e_open = E_FTP_NOTHING_OPEN;
}

void Server::close_filesystem_on_error() {
    close_files_dir();
    if (ftp_data.fp) {
        fclose(ftp_data.fp);
        ftp_data.fp = nullptr;
    }
    if (ftp_data.dp && !ftp_data.listroot) {
        closedir(ftp_data.dp);
        ftp_data.dp = nullptr;
    }
}

Server::ftp_result_t Server::read_file(char* filebuf, uint32_t desiredsize, uint32_t* actualsize) {
    ftp_result_t result = E_FTP_RESULT_CONTINUE;
    *actualsize = fread(filebuf, 1, desiredsize, ftp_data.fp);
    if (*actualsize == 0) {
        if (feof(ftp_data.fp))
            result = E_FTP_RESULT_OK;
        else
            result = E_FTP_RESULT_FAILED;
        close_files_dir();
    } else if (*actualsize < desiredsize) {
        close_files_dir();
        result = E_FTP_RESULT_OK;
    }
    return result;
}

Server::ftp_result_t Server::write_file(char* filebuf, uint32_t size) {
    ftp_result_t result = E_FTP_RESULT_FAILED;
    uint32_t actualsize = fwrite(filebuf, 1, size, ftp_data.fp);
    if (actualsize == size) {
        result = E_FTP_RESULT_OK;
    } else {
        close_files_dir();
    }
    return result;
}

Server::ftp_result_t Server::open_dir_for_listing(const char* path) {
    if (ftp_data.dp) {
        closedir(ftp_data.dp);
        ftp_data.dp = nullptr;
    }

    // Validate path to prevent traversal attacks and check length
    if (!sanitize_path(path, FTP_MAX_PATH_SIZE)) {
        ESP_LOGW(TAG, "open_dir_for_listing: invalid path rejected");
        return E_FTP_RESULT_FAILED;
    }

    if (strcmp(path, "/") == 0) {
        ftp_data.listroot = true;
        ftp_data.e_open = E_FTP_DIR_OPEN;
        return E_FTP_RESULT_CONTINUE;
    } else {
        ftp_data.listroot = false;
        char actual_path[FTP_MAX_PATH_SIZE];
        translate_path(actual_path, sizeof(actual_path), path);
        char fullname[FTP_MAX_PATH_SIZE];
        int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path);
        if (written < 0 || (size_t)written >= sizeof(fullname)) {
            ESP_LOGW(TAG, "open_dir_for_listing: path too long, truncation rejected");
            return E_FTP_RESULT_FAILED;
        }
        // Delay before SD card operation to prevent SPI bus conflicts
        vTaskDelay(pdMS_TO_TICKS(2));
        ftp_data.dp = opendir(fullname);
        if (ftp_data.dp == nullptr) {
            return E_FTP_RESULT_FAILED;
        }
        ftp_data.e_open = E_FTP_DIR_OPEN;
        return E_FTP_RESULT_CONTINUE;
    }
}

int Server::get_eplf_item(char* dest, uint32_t destsize, struct dirent* de) {
    const char* type = (de->d_type & DT_DIR) ? "d" : "-";

    char fullname[FTP_MAX_PATH_SIZE];
    int written = snprintf(fullname, sizeof(fullname), "%s%s%s%s", MOUNT_POINT, ftp_path, (ftp_path[strlen(ftp_path) - 1] != '/') ? "/" : "", de->d_name);
    if (written >= (int)sizeof(fullname)) {
        ESP_LOGW(TAG, "Path too long in get_eplf_item, truncated");
    }

    struct stat buf;
    int res = stat(fullname, &buf);
    if (res < 0) {
        buf.st_size = 0;
        buf.st_mtime = 946684800;
    }

    char str_time[64];
    struct tm* tm_info;
    time_t now;
    if (time(&now) < 0) now = 946684800;
    tm_info = localtime(&buf.st_mtime);

    if (tm_info != nullptr) {
        if ((buf.st_mtime + FTP_UNIX_SECONDS_180_DAYS) < now)
            strftime(str_time, sizeof(str_time), "%b %d %Y", tm_info);
        else
            strftime(str_time, sizeof(str_time), "%b %d %H:%M", tm_info);
    } else {
        snprintf(str_time, sizeof(str_time), "Jan  1  1970");
    }

    int addsize;
    if (ftp_nlist)
        addsize = snprintf(dest, destsize, "%s\r\n", de->d_name);
    else
        addsize = snprintf(dest, destsize, "%srw-rw-rw-   1 root  root %9" PRIu32 " %s %s\r\n", type, (uint32_t)buf.st_size, str_time, de->d_name);

    // If entry doesn't fit in remaining buffer, skip it
    if (addsize < 0 || (uint32_t)addsize >= destsize) {
        ESP_LOGW(TAG, "Entry '%s' too long for buffer (%d >= %" PRIu32 "), skipping", de->d_name, addsize, destsize);
        return 0;
    }

    return addsize;
}

Server::ftp_result_t Server::list_dir(char* list, uint32_t maxlistsize, uint32_t* listsize) {
    uint32_t next = 0;
    uint32_t listcount = 0;
    ftp_result_t result = E_FTP_RESULT_CONTINUE;
    if (ftp_data.listroot) {
        // Add virtual directories for mounted storage devices
        add_virtual_dir_if_mounted(VFS_NATIVE_INTERNAL_MP, FTP_STORAGE_NAME_INTERNAL, list, maxlistsize, &next);
        add_virtual_dir_if_mounted(VFS_NATIVE_EXTERNAL_MP, FTP_STORAGE_NAME_SDCARD, list, maxlistsize, &next);
        result = E_FTP_RESULT_OK;
    } else {
        struct dirent* de;
        while (((maxlistsize - next) > 64) && (listcount < 8)) {
            de = readdir(ftp_data.dp);
            if (de == nullptr) {
                result = E_FTP_RESULT_OK;
                break;
            }
            if (de->d_name[0] == '.' && de->d_name[1] == 0) continue;
            if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == 0) continue;
            char* list_ptr = list + next;
            uint32_t remaining = maxlistsize - next;
            next += get_eplf_item(list_ptr, remaining, de);
            listcount++;
        }
    }
    if (result == E_FTP_RESULT_OK) {
        close_files_dir();
    }
    *listsize = next;
    return result;
}

// Socket operations
void Server::close_cmd_data() {
    closesocket(ftp_data.c_sd);
    closesocket(ftp_data.d_sd);
    ftp_data.c_sd = -1;
    ftp_data.d_sd = -1;
    close_filesystem_on_error();
}

void Server::reset() {
    ESP_LOGW(TAG, "FTP RESET");
    closesocket(ftp_data.lc_sd);
    closesocket(ftp_data.ld_sd);
    ftp_data.lc_sd = -1;
    ftp_data.ld_sd = -1;
    close_cmd_data();
    ftp_data.e_open = E_FTP_NOTHING_OPEN;
    ftp_data.state = E_FTP_STE_START;
    ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
}

bool Server::create_listening_socket(int32_t* sd, uint32_t port, uint8_t backlog) {
    struct sockaddr_in sServerAddress;
    int32_t _sd;
    int32_t result;

    *sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    _sd = *sd;

    if (_sd > 0) {
        uint32_t option = fcntl(_sd, F_GETFL, 0);
        option |= O_NONBLOCK;
        fcntl(_sd, F_SETFL, option);

        option = 1;
        result =
            setsockopt(_sd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

        sServerAddress.sin_family = AF_INET;
        sServerAddress.sin_addr.s_addr = INADDR_ANY;
        sServerAddress.sin_len = sizeof(sServerAddress);
        sServerAddress.sin_port = htons(port);

        result |= bind(_sd, (const struct sockaddr*)&sServerAddress, sizeof(sServerAddress));
        result |= listen(_sd, backlog);

        if (!result) {
            return true;
        }
        closesocket(*sd);
    }
    return false;
}

Server::ftp_result_t Server::wait_for_connection(int32_t l_sd, int32_t* n_sd, uint32_t* ip_addr) {
    struct sockaddr_in sClientAddress;
    socklen_t in_addrSize = sizeof(sClientAddress);

    *n_sd = accept(l_sd, (struct sockaddr*)&sClientAddress, (socklen_t*)&in_addrSize);
    int32_t _sd = *n_sd;
    if (_sd < 0) {
        if (errno == EAGAIN) {
            return E_FTP_RESULT_CONTINUE;
        }
        reset();
        return E_FTP_RESULT_FAILED;
    }

    if (ip_addr) {
        struct sockaddr_in clientAddr = {};
        struct sockaddr_in serverAddr = {};
        in_addrSize = sizeof(clientAddr);
        if (getpeername(_sd, (struct sockaddr*)&clientAddr, (socklen_t*)&in_addrSize) == 0) {
            ESP_LOGI(TAG, "Client IP: 0x%08" PRIx32, clientAddr.sin_addr.s_addr);
        } else {
            ESP_LOGW(TAG, "getpeername failed (errno=%d)", errno);
        }
        in_addrSize = sizeof(serverAddr);
        if (getsockname(_sd, (struct sockaddr*)&serverAddr, (socklen_t*)&in_addrSize) == 0) {
            ESP_LOGI(TAG, "Server IP: 0x%08" PRIx32, serverAddr.sin_addr.s_addr);
            *ip_addr = serverAddr.sin_addr.s_addr;
        } else {
            ESP_LOGW(TAG, "getsockname failed (errno=%d)", errno);
        }
    }

    uint32_t option = fcntl(_sd, F_GETFL, 0);
    if (l_sd != ftp_data.ld_sd) option |= O_NONBLOCK;
    fcntl(_sd, F_SETFL, option);

    return E_FTP_RESULT_OK;
}

// Communication
void Server::send_reply(uint32_t status, const char* message) {
    const char* msg = message ? message : "";
    // Use snprintf to safely format the entire reply, avoiding strcpy/strcat overflow
    int written = snprintf((char*)ftp_cmd_buffer, FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX - 1,
                           "%" PRIu32 " %s\r\n", status, msg);
    if (written < 0 || written >= FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX - 1) {
        ESP_LOGW(TAG, "Reply truncated (status=%lu)", (unsigned long)status);
        // Ensure null termination
        ftp_cmd_buffer[FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX - 1] = '\0';
    }

    int32_t timeout = 200;
    ssize_t send_result;
    size_t size = strlen((char*)ftp_cmd_buffer);

    vTaskDelay(1);

    while (timeout > 0) {
        send_result = send(ftp_data.c_sd, ftp_cmd_buffer, size, 0);
        if (send_result == (ssize_t)size) {
            if (status == 221) {
                if (ftp_data.d_sd >= 0) {
                    closesocket(ftp_data.d_sd);
                    ftp_data.d_sd = -1;
                }
                if (ftp_data.ld_sd >= 0) {
                    closesocket(ftp_data.ld_sd);
                    ftp_data.ld_sd = -1;
                }
                if (ftp_data.c_sd >= 0) {
                    closesocket(ftp_data.c_sd);
                    ftp_data.c_sd = -1;
                }
                ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
                close_filesystem_on_error();
            } else if (status == 426 || status == 451 || status == 550) {
                if (ftp_data.d_sd >= 0) {
                    closesocket(ftp_data.d_sd);
                    ftp_data.d_sd = -1;
                }
                close_filesystem_on_error();
            }
            vTaskDelay(1);
            break;
        } else {
            if (errno != EAGAIN) {
                reset();
                ESP_LOGW(TAG, "Error sending command reply.");
                break;
            }
            vTaskDelay(1);
            timeout -= portTICK_PERIOD_MS;
        }
    }
    if (timeout <= 0) {
        ESP_LOGW(TAG, "Timeout sending command reply.");
        reset();
    }
}

void Server::send_list(uint32_t datasize) {
    int32_t timeout = 200;
    ssize_t send_result;

    vTaskDelay(1);

    while (timeout > 0) {
        send_result = send(ftp_data.d_sd, ftp_data.dBuffer, datasize, 0);
        if (send_result == (ssize_t)datasize) {
            vTaskDelay(1);
            ESP_LOGI(TAG, "Send OK");
            break;
        } else {
            if (errno != EAGAIN) {
                reset();
                ESP_LOGW(TAG, "Error sending list data.");
                break;
            }
            vTaskDelay(1);
            timeout -= portTICK_PERIOD_MS;
        }
    }
    if (timeout <= 0) {
        ESP_LOGW(TAG, "Timeout sending list data.");
        reset();
    }
}

void Server::send_file_data(uint32_t datasize) {
    ssize_t send_result;
    uint32_t timeout = 200;

    vTaskDelay(1);

    while (timeout > 0) {
        send_result = send(ftp_data.d_sd, ftp_data.dBuffer, datasize, 0);
        if (send_result == (ssize_t)datasize) {
            vTaskDelay(1);
            ESP_LOGI(TAG, "Send OK");
            break;
        } else {
            if (errno != EAGAIN) {
                reset();
                ESP_LOGW(TAG, "Error sending file data.");
                break;
            }
            vTaskDelay(1);
            timeout -= portTICK_PERIOD_MS;
        }
    }
    if (timeout <= 0) {
        ESP_LOGW(TAG, "Timeout sending file data.");
        reset();
    }
}

Server::ftp_result_t Server::recv_non_blocking(int32_t sd, void* buff, int32_t Maxlen, int32_t* rxLen) {
    if (sd < 0) return E_FTP_RESULT_FAILED;

    *rxLen = recv(sd, buff, Maxlen, 0);
    if (*rxLen > 0)
        return E_FTP_RESULT_OK;
    else if (errno != EAGAIN)
        return E_FTP_RESULT_FAILED;

    return E_FTP_RESULT_CONTINUE;
}

// Path operations
void Server::open_child(char* pwd, char* dir) {
    ESP_LOGD(TAG, "open_child: [%s] + [%s]", pwd, dir);
    if (strlen(dir) > 0) {
        if (dir[0] == '/') {
            // ** absolute path
            strncpy(pwd, dir, FTP_MAX_PARAM_SIZE - 1);
            pwd[FTP_MAX_PARAM_SIZE - 1] = '\0';
        } else {
            // ** relative path
            size_t pwd_len = strlen(pwd);
            // add trailing '/' if needed
            if ((pwd_len > 1) && (pwd[pwd_len - 1] != '/') && (dir[0] != '/')) {
                if (pwd_len < FTP_MAX_PARAM_SIZE - 1) {
                    pwd[pwd_len++] = '/';
                    pwd[pwd_len] = '\0';
                }
            }
            // append directory/file name
            strncat(pwd, dir, FTP_MAX_PARAM_SIZE - pwd_len - 1);
        }
    }
    ESP_LOGD(TAG, "open_child, New pwd: %s", pwd);
}

void Server::close_child(char* pwd) {
    ESP_LOGD(TAG, "close_child: [%s] (len=%d)", pwd, strlen(pwd));

    // Remove last path component
    uint len = strlen(pwd);

    // Handle trailing slash first
    if (len > 1 && pwd[len - 1] == '/') {
        pwd[len - 1] = '\0';
        len--;
    }

    // Now find and remove the last path component
    // Walk backwards to find the previous '/'
    while (len > 1) {
        len--;
        if (pwd[len] == '/') {
            pwd[len] = '\0';
            break;
        }
    }

    // If we're at a top-level directory like "/sdcard" or "/data", go to root
    if (len <= 1 || pwd[0] != '/' || strchr(pwd + 1, '/') == nullptr) {
        pwd[0] = '/';
        pwd[1] = '\0';
    }

    ESP_LOGD(TAG, "close_child, New pwd: %s", pwd);
}

void Server::remove_fname_from_path(char* pwd, char* fname) {
    ESP_LOGD(TAG, "remove_fname_from_path: %s - %s", pwd, fname);
    if (strlen(fname) == 0) return;
    char* xpwd = strstr(pwd, fname);
    if (xpwd == nullptr) return;
    xpwd[0] = '\0';
    ESP_LOGD(TAG, "remove_fname_from_path: New pwd: %s", pwd);
}

// Command parsing
void Server::pop_param(char** str, char* param, size_t maxlen, bool stop_on_space, bool stop_on_newline) {
    char lastc = '\0';
    size_t copied = 0;
    bool in_quotes = false;

    // Skip leading spaces
    while (**str == ' ') (*str)++;

    // Check if parameter is quoted
    if (**str == '"') {
        in_quotes = true;
        (*str)++; // Skip opening quote
    }

    while (**str != '\0') {
        // Handle closing quote
        if (in_quotes && **str == '"') {
            (*str)++; // Skip closing quote
            break;
        }

        if (!in_quotes && stop_on_space && (**str == ' ')) break;
        if ((**str == '\r') || (**str == '\n')) {
            if (!stop_on_newline) {
                (*str)++;
                continue;
            } else
                break;
        }
        if ((**str == '/') && (lastc == '/')) {
            (*str)++;
            continue;
        }
        lastc = **str;
        if (copied + 1 < maxlen) {
            *param++ = **str;
            copied++;
        }
        (*str)++;
    }
    *param = '\0';

    // Trim trailing whitespace
    while (copied > 0 && (param[-1] == ' ' || param[-1] == '\r' || param[-1] == '\n')) {
        param--;
        *param = '\0';
        copied--;
    }
}

Server::ftp_cmd_index_t Server::pop_command(char** str) {
    char _cmd[FTP_CMD_SIZE_MAX];
    pop_param(str, _cmd, FTP_CMD_SIZE_MAX, true, true);
    stoupper(_cmd);
    for (ftp_cmd_index_t i = (ftp_cmd_index_t)0; i < E_FTP_NUM_FTP_CMDS;
         i = (ftp_cmd_index_t)(i + 1)) {
        if (!strcmp(_cmd, ftp_cmd_table[i].cmd)) {
            (*str)++;
            return i;
        }
    }
    return E_FTP_CMD_NOT_SUPPORTED;
}

void Server::get_param_and_open_child(char** bufptr) {
    pop_param(bufptr, ftp_scratch_buffer, FTP_MAX_PARAM_SIZE, false, false);
    open_child(ftp_path, ftp_scratch_buffer);
    ftp_data.closechild = true;
}

// Main command processing
void Server::process_cmd() {
    int32_t len;
    char* bufptr = (char*)ftp_cmd_buffer;
    ftp_result_t result;
    struct stat buf;
    int res;

    memset(bufptr, 0, FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX);
    ftp_data.closechild = false;

    const int32_t cmd_buf_cap = FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX;
    result = recv_non_blocking(ftp_data.c_sd, ftp_cmd_buffer, cmd_buf_cap - 1, &len);
    if (result == E_FTP_RESULT_FAILED) {
        ESP_LOGI(TAG, "Client disconnected");
        close_cmd_data();
        ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
        return;
    }
    if (result == E_FTP_RESULT_OK) {
        if (len >= cmd_buf_cap) {
            len = cmd_buf_cap - 1;
        }
        ftp_cmd_buffer[len] = '\0';
        ftp_cmd_index_t cmd = pop_command(&bufptr);
        if (!ftp_data.loggin.passvalid &&
            ((cmd != E_FTP_CMD_USER) && (cmd != E_FTP_CMD_PASS) &&
             (cmd != E_FTP_CMD_QUIT) && (cmd != E_FTP_CMD_FEAT) &&
             (cmd != E_FTP_CMD_AUTH))) {
            send_reply(332, nullptr);
            return;
        }
        if ((cmd >= 0) && (cmd < E_FTP_NUM_FTP_CMDS)) {
            ESP_LOGI(TAG, "CMD: %s", ftp_cmd_table[cmd].cmd);
        } else {
            ESP_LOGI(TAG, "CMD: %d", cmd);
        }
        // Use safe path buffers with proper size and safe string operations
        char fullname[FTP_MAX_PATH_SIZE];
        char fullname2[FTP_MAX_PATH_SIZE];
        snprintf(fullname, sizeof(fullname), "%s", MOUNT_POINT);
        snprintf(fullname2, sizeof(fullname2), "%s", MOUNT_POINT);

        switch (cmd) {
            case E_FTP_CMD_FEAT:
                send_reply(502, (char*)"no-features");
                break;
            case E_FTP_CMD_AUTH:
                send_reply(504, (char*)"not-supported");
                break;
            case E_FTP_CMD_SYST:
                send_reply(215, (char*)"UNIX Type: L8");
                break;
            case E_FTP_CMD_CDUP:
                ESP_LOGI(TAG, "CDUP from %s", ftp_path);
                close_child(ftp_path);
                ESP_LOGI(TAG, "CDUP to %s", ftp_path);
                send_reply(250, nullptr);
                break;
            case E_FTP_CMD_CWD:
                pop_param(&bufptr, ftp_scratch_buffer, FTP_MAX_PARAM_SIZE, false, true); // Don't stop on space, DO stop on newline
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_scratch_buffer, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "CWD: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                if (strlen(ftp_scratch_buffer) > 0) {
                    if ((ftp_scratch_buffer[0] == '.') &&
                        (ftp_scratch_buffer[1] == '\0')) {
                        ftp_data.dp = nullptr;
                        send_reply(250, nullptr);
                        break;
                    }
                    if ((ftp_scratch_buffer[0] == '.') &&
                        (ftp_scratch_buffer[1] == '.') &&
                        (ftp_scratch_buffer[2] == '\0')) {
                        close_child(ftp_path);
                        send_reply(250, nullptr);
                        break;
                    } else {
                        open_child(ftp_path, ftp_scratch_buffer);
                    }
                }
                if ((ftp_path[0] == '/') && (ftp_path[1] == '\0')) {
                    ftp_data.dp = nullptr;
                    send_reply(250, nullptr);
                } else {
                    char actual_path[FTP_MAX_PATH_SIZE];
                    translate_path(actual_path, sizeof(actual_path), ftp_path);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "CWD: path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_CWD fullname=[%s]", fullname);
                    // Delay before SD card operation to prevent SPI bus conflicts
                    vTaskDelay(pdMS_TO_TICKS(2));
                    ftp_data.dp = opendir(fullname);
                    if (ftp_data.dp != nullptr) {
                        closedir(ftp_data.dp);
                        ftp_data.dp = nullptr;
                        ESP_LOGI(TAG, "Changed directory to: %s", ftp_path);
                        send_reply(250, nullptr);
                    } else {
                        close_child(ftp_path);
                        send_reply(550, nullptr);
                    }
                }
                break;
            case E_FTP_CMD_PWD:
            case E_FTP_CMD_XPWD: {
                // Buffer needs to hold ftp_path (up to FTP_MAX_PARAM_SIZE) plus quotes
                char lpath[FTP_MAX_PARAM_SIZE + 4];
                // RFC 959 requires quoted path: 257 "pathname" is current directory
                snprintf(lpath, sizeof(lpath), "\"%s\"", ftp_path);
                send_reply(257, lpath);
            } break;
            case E_FTP_CMD_SIZE: {
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "SIZE: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                char actual_path_size[FTP_MAX_PATH_SIZE];
                translate_path(actual_path_size, sizeof(actual_path_size), ftp_path);
                int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_size);
                if (written < 0 || (size_t)written >= sizeof(fullname)) {
                    ESP_LOGW(TAG, "SIZE: path too long, truncation rejected");
                    send_reply(550, nullptr);
                    break;
                }
                ESP_LOGI(TAG, "E_FTP_CMD_SIZE fullname=[%s]", fullname);
                int res = stat(fullname, &buf);
                if (res == 0) {
                    snprintf((char*)ftp_data.dBuffer, ftp_buff_size, "%" PRIu32, (uint32_t)buf.st_size);
                    send_reply(213, (char*)ftp_data.dBuffer);
                } else {
                    send_reply(550, nullptr);
                }
            } break;
            case E_FTP_CMD_MDTM: {
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "MDTM: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                char actual_path_mdtm[FTP_MAX_PATH_SIZE];
                translate_path(actual_path_mdtm, sizeof(actual_path_mdtm), ftp_path);
                int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_mdtm);
                if (written < 0 || (size_t)written >= sizeof(fullname)) {
                    ESP_LOGW(TAG, "MDTM: path too long, truncation rejected");
                    send_reply(550, nullptr);
                    break;
                }
                ESP_LOGI(TAG, "E_FTP_CMD_MDTM fullname=[%s]", fullname);
                res = stat(fullname, &buf);
                if (res == 0) {
                    time_t time = buf.st_mtime;
                    struct tm* ptm = localtime(&time);
                    strftime((char*)ftp_data.dBuffer, ftp_buff_size, "%Y%m%d%H%M%S", ptm);
                    ESP_LOGI(TAG, "E_FTP_CMD_MDTM ftp_data.dBuffer=[%s]", ftp_data.dBuffer);
                    send_reply(213, (char*)ftp_data.dBuffer);
                } else {
                    send_reply(550, nullptr);
                }
            } break;
            case E_FTP_CMD_TYPE:
                send_reply(200, nullptr);
                break;
            case E_FTP_CMD_USER:
                pop_param(&bufptr, ftp_scratch_buffer, FTP_MAX_PARAM_SIZE, true, true);
                {
                    size_t user_len = strlen(ftp_user);
                    size_t input_len = strlen(ftp_scratch_buffer);
                    if (user_len == input_len && user_len > 0 &&
                        secure_compare(ftp_scratch_buffer, ftp_user, user_len)) {
                        ftp_data.loggin.uservalid = true;
                    }
                    // Clear credentials from memory after validation
                    memset(ftp_scratch_buffer, 0, FTP_MAX_PARAM_SIZE);
                }
                send_reply(331, nullptr);
                break;
            case E_FTP_CMD_PASS:
                pop_param(&bufptr, ftp_scratch_buffer, FTP_MAX_PARAM_SIZE, true, true);
                {
                    // Rate limiting: max 3 login attempts, then delay increases
                    static const uint8_t MAX_LOGIN_RETRIES = 3;
                    if (ftp_data.logginRetries >= MAX_LOGIN_RETRIES) {
                        // Add exponential backoff delay for repeated failures
                        uint32_t delay_ms = 1000 * (ftp_data.logginRetries - MAX_LOGIN_RETRIES + 1);
                        if (delay_ms > 5000) delay_ms = 5000;  // Cap at 5 seconds
                        ESP_LOGW(TAG, "Login rate limited, delaying %lu ms", (unsigned long)delay_ms);
                        vTaskDelay(pdMS_TO_TICKS(delay_ms));
                    }

                    size_t pass_len = strlen(ftp_pass);
                    size_t input_len = strlen(ftp_scratch_buffer);
                    bool valid = ftp_data.loggin.uservalid && pass_len == input_len &&
                        secure_compare(ftp_scratch_buffer, ftp_pass, pass_len);

                    // Clear password from memory immediately after validation
                    memset(ftp_scratch_buffer, 0, FTP_MAX_PARAM_SIZE);

                    if (valid) {
                        ftp_data.loggin.passvalid = true;
                        ftp_data.logginRetries = 0;  // Reset on success
                        send_reply(230, nullptr);
                        ESP_LOGW(TAG, "Connected.");
                        break;
                    }
                    ftp_data.logginRetries++;
                }
                send_reply(530, nullptr);
                break;
            case E_FTP_CMD_PASV: {
                closesocket(ftp_data.d_sd);
                ftp_data.d_sd = -1;
                ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
                bool socketcreated = true;
                if (ftp_data.ld_sd < 0) {
                    socketcreated = create_listening_socket(
                        &ftp_data.ld_sd, FTP_PASSIVE_DATA_PORT,
                        FTP_DATA_CLIENTS_MAX - 1
                    );
                }
                if (socketcreated) {
                    uint8_t* pip = (uint8_t*)&ftp_data.ip_addr;
                    ftp_data.dtimeout = 0;
                    snprintf((char*)ftp_data.dBuffer, ftp_buff_size, "(%u,%u,%u,%u,%u,%u)", pip[0], pip[1], pip[2], pip[3], (FTP_PASSIVE_DATA_PORT >> 8), (FTP_PASSIVE_DATA_PORT & 0xFF));
                    ftp_data.substate = E_FTP_STE_SUB_LISTEN_FOR_DATA;
                    ESP_LOGI(TAG, "Data socket created");
                    send_reply(227, (char*)ftp_data.dBuffer);
                } else {
                    ESP_LOGW(TAG, "Error creating data socket");
                    send_reply(425, nullptr);
                }
            } break;
            case E_FTP_CMD_LIST:
            case E_FTP_CMD_NLST:
                get_param_and_open_child(&bufptr);
                if (cmd == E_FTP_CMD_LIST)
                    ftp_nlist = 0;
                else
                    ftp_nlist = 1;
                if (open_dir_for_listing(ftp_path) == E_FTP_RESULT_CONTINUE) {
                    ftp_data.state = E_FTP_STE_CONTINUE_LISTING;
                    send_reply(150, nullptr);
                } else {
                    send_reply(550, nullptr);
                }
                break;
            case E_FTP_CMD_RETR:
                ftp_data.total = 0;
                ftp_data.time = 0;
                get_param_and_open_child(&bufptr);
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    if (open_file(ftp_path, "rb")) {
                        log_to_screen("[<<] Download: %s", ftp_path);
                        ftp_data.state = E_FTP_STE_CONTINUE_FILE_TX;
                        vTaskDelay(pdMS_TO_TICKS(20));
                        send_reply(150, nullptr);
                    } else {
                        ftp_data.state = E_FTP_STE_END_TRANSFER;
                        send_reply(550, nullptr);
                    }
                } else {
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    send_reply(550, nullptr);
                }
                break;
            case E_FTP_CMD_APPE:
                ftp_data.total = 0;
                ftp_data.time = 0;
                get_param_and_open_child(&bufptr);
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    if (open_file(ftp_path, "ab")) {
                        log_to_screen("[OK] Append: %s", ftp_path);
                        ftp_data.state = E_FTP_STE_CONTINUE_FILE_RX;
                        vTaskDelay(pdMS_TO_TICKS(20));
                        send_reply(150, nullptr);
                    } else {
                        ftp_data.state = E_FTP_STE_END_TRANSFER;
                        send_reply(550, nullptr);
                    }
                } else {
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    send_reply(550, nullptr);
                }
                break;
            case E_FTP_CMD_STOR:
                ftp_data.total = 0;
                ftp_data.time = 0;
                get_param_and_open_child(&bufptr);
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    ESP_LOGI(TAG, "E_FTP_CMD_STOR ftp_path=[%s]", ftp_path);
                    if (open_file(ftp_path, "wb")) {
                        log_to_screen("[>>] Upload: %s", ftp_path);
                        ftp_data.state = E_FTP_STE_CONTINUE_FILE_RX;
                        vTaskDelay(pdMS_TO_TICKS(20));
                        send_reply(150, nullptr);
                    } else {
                        ftp_data.state = E_FTP_STE_END_TRANSFER;
                        send_reply(550, nullptr);
                    }
                } else {
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    send_reply(550, nullptr);
                }
                break;
            case E_FTP_CMD_DELE:
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "DELE: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    ESP_LOGI(TAG, "E_FTP_CMD_DELE ftp_path=[%s]", ftp_path);
                    char actual_path_dele[FTP_MAX_PATH_SIZE];
                    translate_path(actual_path_dele, sizeof(actual_path_dele), ftp_path);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_dele);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "DELE: path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_DELE fullname=[%s]", fullname);
                    // Delay before SD card operation to prevent SPI bus conflicts
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (unlink(fullname) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        ESP_LOGI(TAG, "File deleted: %s", ftp_path);
                        send_reply(250, nullptr);
                        log_to_screen("[OK] Deleted: %s", ftp_path);
                    } else
                        send_reply(550, nullptr);
                } else
                    send_reply(250, nullptr);
                break;
            case E_FTP_CMD_RMD:
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "RMD: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    ESP_LOGI(TAG, "E_FTP_CMD_RMD ftp_path=[%s]", ftp_path);
                    char actual_path_rmd[FTP_MAX_PATH_SIZE];
                    translate_path(actual_path_rmd, sizeof(actual_path_rmd), ftp_path);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_rmd);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "RMD: path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_RMD fullname=[%s]", fullname);
                    // Delay before SD card operation to prevent SPI bus conflicts
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (rmdir(fullname) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        ESP_LOGI(TAG, "Directory removed: %s", ftp_path);
                        send_reply(250, nullptr);
                        log_to_screen("[OK] Removed dir: %s", ftp_path);
                    } else
                        send_reply(550, nullptr);
                } else
                    send_reply(250, nullptr);
                break;
            case E_FTP_CMD_MKD:
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "MKD: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                if ((strlen(ftp_path) > 0) &&
                    (ftp_path[strlen(ftp_path) - 1] != '/')) {
                    ESP_LOGI(TAG, "E_FTP_CMD_MKD ftp_path=[%s]", ftp_path);
                    char actual_path_mkd[FTP_MAX_PATH_SIZE];
                    translate_path(actual_path_mkd, sizeof(actual_path_mkd), ftp_path);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_mkd);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "MKD: path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_MKD fullname=[%s]", fullname);
                    // Add delay before SD card operation to allow SPI bus to settle
                    // This helps prevent SPI conflicts on devices like T-Deck Plus
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (mkdir(fullname, 0755) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        ESP_LOGI(TAG, "Directory created: %s", ftp_path);
                        send_reply(250, nullptr);
                        log_to_screen("[OK] Created dir: %s", ftp_path);
                    } else
                        send_reply(550, nullptr);
                } else
                    send_reply(250, nullptr);
                break;
            case E_FTP_CMD_RNFR:
                get_param_and_open_child(&bufptr);
                // Validate path to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "RNFR: invalid path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                ESP_LOGI(TAG, "E_FTP_CMD_RNFR ftp_path=[%s]", ftp_path);
                {
                    char actual_path_rnfr[FTP_MAX_PATH_SIZE];
                    translate_path(actual_path_rnfr, sizeof(actual_path_rnfr), ftp_path);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_path_rnfr);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "RNFR: path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_RNFR fullname=[%s]", fullname);
                    res = stat(fullname, &buf);
                    if (res == 0) {
                        send_reply(350, nullptr);
                        strncpy((char*)ftp_data.dBuffer, ftp_path, ftp_buff_size - 1);
                        ((char*)ftp_data.dBuffer)[ftp_buff_size - 1] = '\0';
                    } else {
                        send_reply(550, nullptr);
                    }
                    log_to_screen("[**] Renaming: %s", ftp_path);
                }
                break;
            case E_FTP_CMD_RNTO:
                get_param_and_open_child(&bufptr);
                // Validate both paths to prevent traversal attacks and check length
                if (!sanitize_path(ftp_path, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "RNTO: invalid destination path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                if (!sanitize_path((char*)ftp_data.dBuffer, FTP_MAX_PATH_SIZE)) {
                    ESP_LOGW(TAG, "RNTO: invalid source path rejected");
                    send_reply(550, nullptr);
                    break;
                }
                ESP_LOGI(TAG, "E_FTP_CMD_RNTO ftp_path=[%s], ftp_data.dBuffer=[%s]", ftp_path, (char*)ftp_data.dBuffer);
                {
                    char actual_old[FTP_MAX_PATH_SIZE];
                    translate_path(actual_old, sizeof(actual_old), (char*)ftp_data.dBuffer);
                    int written = snprintf(fullname, sizeof(fullname), "%s%s", MOUNT_POINT, actual_old);
                    if (written < 0 || (size_t)written >= sizeof(fullname)) {
                        ESP_LOGW(TAG, "RNTO: source path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_RNTO fullname=[%s]", fullname);
                    char actual_new[FTP_MAX_PATH_SIZE];
                    translate_path(actual_new, sizeof(actual_new), ftp_path);
                    written = snprintf(fullname2, sizeof(fullname2), "%s%s", MOUNT_POINT, actual_new);
                    if (written < 0 || (size_t)written >= sizeof(fullname2)) {
                        ESP_LOGW(TAG, "RNTO: destination path too long, truncation rejected");
                        send_reply(550, nullptr);
                        break;
                    }
                    ESP_LOGI(TAG, "E_FTP_CMD_RNTO fullname2=[%s]", fullname2);
                    // Delay before SD card operation to prevent SPI bus conflicts
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (rename(fullname, fullname2) == 0) {
                        ESP_LOGI(TAG, "File renamed from %s to %s", (char*)ftp_data.dBuffer, ftp_path);
                        send_reply(250, nullptr);
                    } else {
                        send_reply(550, nullptr);
                    }
                }
                log_to_screen("[OK] Renamed to: %s", ftp_path);
                break;
            case E_FTP_CMD_NOOP:
                send_reply(200, nullptr);
                break;
            case E_FTP_CMD_QUIT:
                ESP_LOGI(TAG, "Client disconnected (QUIT)");
                send_reply(221, nullptr);
                close_cmd_data();
                ftp_data.state = E_FTP_STE_START;
                break;
            default:
                send_reply(502, nullptr);
                break;
        }

        if (ftp_data.closechild) {
            remove_fname_from_path(ftp_path, ftp_scratch_buffer);
        }
    } else if (result == E_FTP_RESULT_CONTINUE) {
        if (ftp_data.ctimeout > ftp_timeout) {
            send_reply(221, nullptr);
            ESP_LOGW(TAG, "Connection timeout");
        }
    } else {
        close_cmd_data();
    }
}

void Server::wait_for_enabled() {
    if (ftp_data.enabled) {
        ftp_data.state = E_FTP_STE_START;
    }
}

void Server::deinit() {
    if (ftp_path) free(ftp_path);
    if (ftp_cmd_buffer) free(ftp_cmd_buffer);
    if (ftp_data.dBuffer) free(ftp_data.dBuffer);
    if (ftp_scratch_buffer) free(ftp_scratch_buffer);
    ftp_path = nullptr;
    ftp_cmd_buffer = nullptr;
    ftp_data.dBuffer = nullptr;
    ftp_scratch_buffer = nullptr;
}

bool Server::init() {
    ftp_stop = 0;
    deinit();
    // Reset buffer size to default at init to prevent memory accumulation from previous sessions
    ftp_buff_size = FTPSERVER_BUFFER_SIZE;
    memset(&ftp_data, 0, sizeof(ftp_data_t));
    ftp_data.dBuffer = (uint8_t*)malloc(ftp_buff_size + 1);
    if (ftp_data.dBuffer == nullptr) {
        goto error_dbuffer;
    }
    ftp_path = (char*)malloc(FTP_MAX_PARAM_SIZE);
    if (ftp_path == nullptr) {
        goto error_path;
    }
    strcpy(ftp_path, "/");
    ftp_scratch_buffer = (char*)malloc(FTP_MAX_PARAM_SIZE);
    if (ftp_scratch_buffer == nullptr) {
        goto error_scratch;
    }
    ftp_cmd_buffer = (char*)malloc(FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX);
    if (ftp_cmd_buffer == nullptr) {
        goto error_cmd;
    }

    ftp_data.c_sd = -1;
    ftp_data.d_sd = -1;
    ftp_data.lc_sd = -1;
    ftp_data.ld_sd = -1;
    ftp_data.e_open = E_FTP_NOTHING_OPEN;
    ftp_data.state = E_FTP_STE_DISABLED;
    ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;

    return true;

error_cmd:
    free(ftp_scratch_buffer);
error_scratch:
    free(ftp_path);
error_path:
    free(ftp_data.dBuffer);
error_dbuffer:
    ftp_data.dBuffer = nullptr;
    ftp_path = nullptr;
    ftp_scratch_buffer = nullptr;
    ftp_cmd_buffer = nullptr;
    return false;
}

int Server::run(uint32_t elapsed) {
    xSemaphoreTake(ftp_mutex, portMAX_DELAY);

    if (ftp_stop) {
        ESP_LOGI(TAG, "Stop flag detected in run()");
        xSemaphoreGive(ftp_mutex);
        return -2;
    }

    ftp_data.dtimeout += elapsed;
    ftp_data.ctimeout += elapsed;
    ftp_data.time += elapsed;

    switch (ftp_data.state) {
        case E_FTP_STE_DISABLED:
            wait_for_enabled();
            break;
        case E_FTP_STE_START:
            if (create_listening_socket(&ftp_data.lc_sd, ftp_cmd_port, FTP_CMD_CLIENTS_MAX - 1)) {
                ftp_data.state = E_FTP_STE_READY;
            }
            break;
        case E_FTP_STE_READY:
            if (ftp_data.c_sd < 0 &&
                ftp_data.substate == E_FTP_STE_SUB_DISCONNECTED) {
                if (E_FTP_RESULT_OK == wait_for_connection(ftp_data.lc_sd, &ftp_data.c_sd, &ftp_data.ip_addr)) {
                    ftp_data.txRetries = 0;
                    ftp_data.logginRetries = 0;
                    ftp_data.ctimeout = 0;
                    ftp_data.loggin.uservalid = false;
                    ftp_data.loggin.passvalid = false;
                    strcpy(ftp_path, "/");
                    ESP_LOGI(TAG, "Connected.");
                    send_reply(220, (char*)FTP_SERVER_NAME);
                    break;
                }
            }
            if (ftp_data.c_sd > 0 &&
                ftp_data.substate != E_FTP_STE_SUB_LISTEN_FOR_DATA) {
                process_cmd();
                if (ftp_data.state != E_FTP_STE_READY) {
                    break;
                }
            }
            break;
        case E_FTP_STE_END_TRANSFER:
            if (ftp_data.d_sd >= 0) {
                closesocket(ftp_data.d_sd);
                ftp_data.d_sd = -1;
            }
            break;
        case E_FTP_STE_CONTINUE_LISTING: {
            uint32_t listsize = 0;
            ftp_result_t list_res =
                list_dir((char*)ftp_data.dBuffer, ftp_buff_size, &listsize);
            if (listsize > 0) send_list(listsize);
            if (list_res == E_FTP_RESULT_OK) {
                send_reply(226, nullptr);
                ftp_data.state = E_FTP_STE_END_TRANSFER;
            }
            ftp_data.ctimeout = 0;
        } break;
        case E_FTP_STE_CONTINUE_FILE_TX: {
            uint32_t readsize;
            ftp_result_t result;
            ftp_data.ctimeout = 0;
            result =
                read_file((char*)ftp_data.dBuffer, ftp_buff_size, &readsize);
            if (result == E_FTP_RESULT_FAILED) {
                send_reply(451, nullptr);
                ftp_data.state = E_FTP_STE_END_TRANSFER;
            } else {
                if (readsize > 0) {
                    send_file_data(readsize);
                    ftp_data.total += readsize;
                    ESP_LOGI(TAG, "Sent %" PRIu32 ", total: %" PRIu32, readsize, ftp_data.total);
                    if (ftp_data.total % 102400 == 0 && ftp_data.total > 0) {
                        log_to_screen("[^^] Progress: %" PRIu32 " KB", ftp_data.total / 1024);
                    }
                }
                if (result == E_FTP_RESULT_OK) {
                    send_reply(226, nullptr);
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    ESP_LOGI(TAG, "File sent (%" PRIu32 " bytes in %" PRIu32 " msec).", ftp_data.total, ftp_data.time);
                }
            }
        } break;
        case E_FTP_STE_CONTINUE_FILE_RX: {
            int32_t len;
            ftp_result_t result = E_FTP_RESULT_OK;
            ESP_LOGI(TAG, "ftp_buff_size=%d", ftp_buff_size);
            result = recv_non_blocking(ftp_data.d_sd, ftp_data.dBuffer, ftp_buff_size, &len);
            if (result == E_FTP_RESULT_OK) {
                ftp_data.dtimeout = 0;
                ftp_data.ctimeout = 0;
                if (E_FTP_RESULT_OK !=
                    write_file((char*)ftp_data.dBuffer, len)) {
                    send_reply(451, nullptr);
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    ESP_LOGW(TAG, "Error writing to file");
                } else {
                    ftp_data.total += len;
                    ESP_LOGI(TAG, "Received %" PRIu32 ", total: %" PRIu32, len, ftp_data.total);
                    if (ftp_data.total % 102400 == 0 && ftp_data.total > 0) {
                        log_to_screen("[^^] Progress: %" PRIu32 " KB", ftp_data.total / 1024);
                    }
                }
            } else if (result == E_FTP_RESULT_CONTINUE) {
                if (ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS) {
                    close_files_dir();
                    send_reply(426, nullptr);
                    ftp_data.state = E_FTP_STE_END_TRANSFER;
                    ESP_LOGW(TAG, "Receiving to file timeout");
                }
            } else {
                close_files_dir();
                send_reply(226, nullptr);
                ftp_data.state = E_FTP_STE_END_TRANSFER;
                ESP_LOGI(TAG, "File received (%" PRIu32 " bytes in %" PRIu32 " msec).", ftp_data.total, ftp_data.time);
                break;
            }
        } break;
        default:
            break;
    }

    switch (ftp_data.substate) {
        case E_FTP_STE_SUB_DISCONNECTED:
            break;
        case E_FTP_STE_SUB_LISTEN_FOR_DATA:
            if (E_FTP_RESULT_OK ==
                wait_for_connection(ftp_data.ld_sd, &ftp_data.d_sd, nullptr)) {
                ftp_data.dtimeout = 0;
                ftp_data.substate = E_FTP_STE_SUB_DATA_CONNECTED;
            } else if (ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Waiting for data connection timeout (%" PRIi32 ")", ftp_data.dtimeout);
                ftp_data.dtimeout = 0;
                closesocket(ftp_data.ld_sd);
                ftp_data.ld_sd = -1;
                ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
            }
            break;
        case E_FTP_STE_SUB_DATA_CONNECTED:
            if (ftp_data.state == E_FTP_STE_READY &&
                (ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS)) {
                closesocket(ftp_data.ld_sd);
                closesocket(ftp_data.d_sd);
                ftp_data.ld_sd = -1;
                ftp_data.d_sd = -1;
                close_filesystem_on_error();
                ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
                ESP_LOGW(TAG, "Data connection timeout");
            }
            break;
        default:
            break;
    }

    if (ftp_data.d_sd < 0 && (ftp_data.state > E_FTP_STE_READY)) {
        ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
        ftp_data.state = E_FTP_STE_READY;
    }

    xSemaphoreGive(ftp_mutex);
    return 0;
}

bool Server::enable() {
    bool res = false;
    if (ftp_data.state == E_FTP_STE_DISABLED) {
        ftp_data.enabled = true;
        res = true;
    }
    return res;
}

bool Server::disable() {
    bool res = false;
    if (ftp_data.state == E_FTP_STE_READY) {
        reset();
        ftp_data.enabled = false;
        ftp_data.state = E_FTP_STE_DISABLED;
        res = true;
    }
    return res;
}

bool Server::terminate() {
    bool res = false;
    if (ftp_data.state == E_FTP_STE_READY) {
        ftp_stop = 1;
        reset();
        res = true;
    }
    return res;
}

bool Server::stop_requested() { return (ftp_stop == 1); }

// Task loop - the main FTP server loop running in FreeRTOS task
void Server::task_loop() {
    ESP_LOGI(TAG, "ftp_task start");
    // Use default credentials if not set
    if (ftp_user[0] == '\0') {
        strncpy(ftp_user, "ftp", FTP_USER_PASS_LEN_MAX);
        ftp_user[FTP_USER_PASS_LEN_MAX] = '\0';
    }
    if (ftp_pass[0] == '\0') {
        strncpy(ftp_pass, "ftp123", FTP_USER_PASS_LEN_MAX);
        ftp_pass[FTP_USER_PASS_LEN_MAX] = '\0';
    }

    uint64_t elapsed, time_ms = mp_hal_ticks_ms();

    if (!init()) {
        ESP_LOGE(TAG, "Init Error");
        xEventGroupSetBits(xEventTask, FTP_TASK_FINISH_BIT);
        vTaskDelete(nullptr);
        return;
    }

    enable();

    time_ms = mp_hal_ticks_ms();

    while (1) {
        // CHECK STOP FLAG FIRST
        if (ftp_stop || stop_requested()) {
            ESP_LOGI(TAG, "Stop requested, exiting task loop");
            break;
        }

        elapsed = mp_hal_ticks_ms() - time_ms;
        time_ms = mp_hal_ticks_ms();

        int res = run(elapsed);
        if (res < 0) {
            if (res == -1) {
                ESP_LOGE(TAG, "Run Error");
            }
            if (res == -2) {
                ESP_LOGI(TAG, "Stop requested via run()");
            }
            break;
        }

        vTaskDelay(1);
    }

    ESP_LOGW(TAG, "Task terminating, cleaning up...");
    // Cleanup before exit
    reset(); // Close all sockets
    deinit(); // Free memory

    ESP_LOGW(TAG, "Task terminated!");
    xEventGroupSetBits(xEventTask, FTP_TASK_FINISH_BIT);
    vTaskDelete(nullptr);
}

// Static task wrapper for FreeRTOS
void Server::task_wrapper(void* pvParameters) {
    Server* server = static_cast<Server*>(pvParameters);
    server->task_loop();
}

// Public interface implementation
void Server::start() {
    if (ftp_mutex) {
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    if (ftp_task_handle) {
        if (ftp_mutex) {
            xSemaphoreGive(ftp_mutex);
        }
        ESP_LOGW("FTP", "FTP server already running");
        return;
    }

    if (xEventTask) {
        ESP_LOGW("FTP", "Event group already exists, cleaning up");
        vEventGroupDelete(xEventTask);
    }

    xEventTask = xEventGroupCreate();
    if (!xEventTask) {
        if (ftp_mutex) {
            xSemaphoreGive(ftp_mutex);
        }
        ESP_LOGE("FTP", "Failed to create event group");
        return;
    }
    BaseType_t result =
        xTaskCreate(task_wrapper, "FTP", FTP_TASK_STACK_SIZE, this, 1, &ftp_task_handle);
    if (result != pdPASS) {
        ESP_LOGE("FTP", "Failed to create FTP task");
        ftp_task_handle = nullptr;
        vEventGroupDelete(xEventTask);
        xEventTask = nullptr;
    } else {
        ESP_LOGI("FTP", "FTP server started");
    }

    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }
}

void Server::stop() {
    ESP_LOGI(TAG, "stop() called");

    // Thread-safe check
    if (ftp_mutex) {
        ESP_LOGI(TAG, "Acquiring mutex for stop");
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    if (!ftp_task_handle) {
        ESP_LOGI(TAG, "No task running, nothing to stop");
        if (ftp_mutex) {
            xSemaphoreGive(ftp_mutex);
        }
        return;
    }

    ESP_LOGI(TAG, "Setting stop bit");

    // Set the stop flag so task_loop and run() see it
    ftp_stop = 1;

    // Release mutex before waiting (task needs it to check stop flag)
    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }

    // Set stop bit
    xEventGroupSetBits(xEventTask, FTP_STOP_BIT);

    ESP_LOGI(TAG, "Waiting for task to finish...");

    // Wait for task to finish with timeout
    EventBits_t bits = xEventGroupWaitBits(
        xEventTask,
        FTP_TASK_FINISH_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(5000) // 5 second timeout
    );

    if (bits & FTP_TASK_FINISH_BIT) {
        ESP_LOGI(TAG, "Task finished cleanly");
    } else {
        ESP_LOGE(TAG, "Task did not finish in time! Forcing cleanup");
        // Force kill task (not ideal but prevents lockup)
        if (ftp_task_handle) {
            vTaskDelete(ftp_task_handle);
            // Allow FreeRTOS to fully remove the task before accessing shared state
            // This reduces (but doesn't eliminate) the risk of racing with task cleanup
            vTaskDelay(pdMS_TO_TICKS(100));
            ftp_task_handle = nullptr;
        }
        // Task cleanup didn't run, do it here
        // Note: These may access partially corrupted state if task was mid-operation
        // but it's better than leaking resources
        reset();
        deinit();
    }

    // Re-acquire mutex for cleanup
    if (ftp_mutex) {
        ESP_LOGI(TAG, "Re-acquiring mutex for cleanup");
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    // Cleanup
    if (xEventTask) {
        vEventGroupDelete(xEventTask);
        xEventTask = nullptr;
    }
    ftp_task_handle = nullptr;
    ftp_data.enabled = false; // Clear enabled flag so isEnabled() returns false
    ftp_stop = 0; // Reset stop flag for next start

    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }

    ESP_LOGI(TAG, "stop() completed");
}

bool Server::isEnabled() const {
    bool enabled = false;
    if (ftp_mutex) {
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }
    enabled = ftp_data.enabled;
    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }
    return enabled;
}

int Server::getState() const {
    int fstate = 0;
    if (ftp_mutex) {
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    fstate = ftp_data.state | (ftp_data.substate << 8);
    if ((ftp_data.state == E_FTP_STE_READY) && (ftp_data.c_sd > 0))
        fstate = E_FTP_STE_CONNECTED;

    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }
    return fstate;
}

void Server::setCredentials(const char* username, const char* password) {
    if (ftp_mutex) {
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    if (username) {
        strncpy(ftp_user, username, FTP_USER_PASS_LEN_MAX);
        ftp_user[FTP_USER_PASS_LEN_MAX] = '\0';
    }

    if (password) {
        strncpy(ftp_pass, password, FTP_USER_PASS_LEN_MAX);
        ftp_pass[FTP_USER_PASS_LEN_MAX] = '\0';
    }

    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }
}

void Server::setPort(uint16_t port) {
    if (ftp_mutex) {
        xSemaphoreTake(ftp_mutex, portMAX_DELAY);
    }

    ftp_cmd_port = port;

    if (ftp_mutex) {
        xSemaphoreGive(ftp_mutex);
    }

    ESP_LOGI(TAG, "Port updated to %u", port);
}

} // namespace FtpServer