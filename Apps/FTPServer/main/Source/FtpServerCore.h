#ifndef FTP_SERVER_CORE_H
#define FTP_SERVER_CORE_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

// String constants used for compile-time concatenation (must be macros)
#define FTP_STORAGE_NAME_INTERNAL "data"
#define FTP_STORAGE_NAME_SDCARD "sdcard"

namespace FtpServer {

// Namespace-scoped constants (preferred over macros for type safety)
inline constexpr uint16_t FTP_CMD_PORT = 21;
inline constexpr uint16_t FTP_PASSIVE_DATA_PORT = 2024;
inline constexpr size_t FTP_CMD_SIZE_MAX = 6;
inline constexpr uint8_t FTP_CMD_CLIENTS_MAX = 1;
inline constexpr uint8_t FTP_DATA_CLIENTS_MAX = 1;
inline constexpr size_t FTP_MAX_PARAM_SIZE = 512 + 1;
// 180 days = 15552000 seconds
// FTP LIST shows "MMM DD YYYY" for old files, "MMM DD HH:MM" for recent
inline constexpr time_t FTP_UNIX_SECONDS_180_DAYS = 180 * 24 * 60 * 60;
inline constexpr uint32_t FTP_DATA_TIMEOUT_MS = 10000;
inline constexpr uint8_t FTP_SOCKETFIFO_ELEMENTS_MAX = 4;
inline constexpr size_t FTP_USER_PASS_LEN_MAX = 32;
inline constexpr uint32_t FTP_CMD_TIMEOUT_MS = 300 * 1000;
inline constexpr size_t FTPSERVER_BUFFER_SIZE = 1024;
inline constexpr size_t FTPSERVER_MAX_BUFFER_SIZE = 16 * 1024;  // Maximum buffer growth cap (16KB)
inline constexpr size_t FTP_MAX_PATH_SIZE = 256;  // Safe maximum path size for all operations

inline constexpr const char* VFS_NATIVE_INTERNAL_MP = "/data";
inline constexpr const char* VFS_NATIVE_EXTERNAL_MP = "/sdcard";
inline constexpr const char* FTP_SERVER_NAME = "Tactility FTP Server";

// Use std::min/std::max from <algorithm> instead of custom MIN/MAX macros

class Server {
public:

    // Public enums
    typedef enum {
        E_FTP_STE_DISABLED = 0,
        E_FTP_STE_START,
        E_FTP_STE_READY,
        E_FTP_STE_END_TRANSFER,
        E_FTP_STE_CONTINUE_LISTING,
        E_FTP_STE_CONTINUE_FILE_TX,
        E_FTP_STE_CONTINUE_FILE_RX,
        E_FTP_STE_CONNECTED
    } ftp_state_t;

    typedef enum {
        E_FTP_STE_SUB_DISCONNECTED = 0,
        E_FTP_STE_SUB_LISTEN_FOR_DATA,
        E_FTP_STE_SUB_DATA_CONNECTED
    } ftp_substate_t;

    typedef enum {
        E_FTP_RESULT_OK = 0,
        E_FTP_RESULT_CONTINUE,
        E_FTP_RESULT_FAILED
    } ftp_result_t;

    typedef enum {
        E_FTP_NOTHING_OPEN = 0,
        E_FTP_FILE_OPEN,
        E_FTP_DIR_OPEN
    } ftp_e_open_t;

    // Constructor/Destructor
    Server();
    ~Server();

    // Public interface
    void start();
    void stop();
    bool isEnabled() const;
    int getState() const;
    void register_screen_log_callback(void (*callback)(const char*));
    void setCredentials(const char* username, const char* password);
    void setPort(uint16_t port);

private:

    // Private structs
    typedef struct {
        const char* cmd;
    } ftp_cmd_t;

    typedef struct {
        bool uservalid : 1;
        bool passvalid : 1;
    } ftp_loggin_t;

    typedef struct {
        uint8_t* dBuffer;
        uint32_t ctimeout;
        union {
            DIR* dp;
            FILE* fp;
        };
        int32_t lc_sd;
        int32_t ld_sd;
        int32_t c_sd;
        int32_t d_sd;
        int32_t dtimeout;
        uint32_t ip_addr;
        uint8_t state;
        uint8_t substate;
        uint8_t txRetries;
        uint8_t logginRetries;
        ftp_loggin_t loggin;
        uint8_t e_open;
        bool closechild;
        bool enabled;
        bool listroot;
        uint32_t total;
        uint32_t time;
    } ftp_data_t;

    typedef enum {
        E_FTP_CMD_NOT_SUPPORTED = -1,
        E_FTP_CMD_FEAT = 0,
        E_FTP_CMD_SYST,
        E_FTP_CMD_CDUP,
        E_FTP_CMD_CWD,
        E_FTP_CMD_PWD,
        E_FTP_CMD_XPWD,
        E_FTP_CMD_SIZE,
        E_FTP_CMD_MDTM,
        E_FTP_CMD_TYPE,
        E_FTP_CMD_USER,
        E_FTP_CMD_PASS,
        E_FTP_CMD_PASV,
        E_FTP_CMD_LIST,
        E_FTP_CMD_RETR,
        E_FTP_CMD_STOR,
        E_FTP_CMD_DELE,
        E_FTP_CMD_RMD,
        E_FTP_CMD_MKD,
        E_FTP_CMD_RNFR,
        E_FTP_CMD_RNTO,
        E_FTP_CMD_NOOP,
        E_FTP_CMD_QUIT,
        E_FTP_CMD_APPE,
        E_FTP_CMD_NLST,
        E_FTP_CMD_AUTH,
        E_FTP_NUM_FTP_CMDS
    } ftp_cmd_index_t;

    // Member variables (formerly global)
    static constexpr int FTP_STOP_BIT = (1 << 0);
    static constexpr int FTP_TASK_FINISH_BIT = (1 << 2);

    EventGroupHandle_t xEventTask;
    TaskHandle_t ftp_task_handle;
    SemaphoreHandle_t ftp_mutex;

    int ftp_buff_size;
    int ftp_timeout;
    const char* TAG;
    const char* MOUNT_POINT;

    ftp_data_t ftp_data;
    char* ftp_path;
    char* ftp_scratch_buffer;
    char* ftp_cmd_buffer;
    std::atomic<uint8_t> ftp_stop;
    char ftp_user[FTP_USER_PASS_LEN_MAX + 1];
    char ftp_pass[FTP_USER_PASS_LEN_MAX + 1];
    uint8_t ftp_nlist;
    uint16_t ftp_cmd_port;

    static const ftp_cmd_t ftp_cmd_table[];

    // Private helper methods
    bool sanitize_path(const char* path, size_t max_len = 0);
    void translate_path(char* actual, size_t actual_size, const char* display);
    void get_full_path(char* fullname, size_t size, const char* display_path);
    bool secure_compare(const char* a, const char* b, size_t len);
    bool add_virtual_dir_if_mounted(const char* mount_point, const char* name, char* list, uint32_t maxlistsize, uint32_t* next);
    uint64_t mp_hal_ticks_ms();
    void stoupper(char* str);
    void log_to_screen(const char* format, ...);

    // File operations
    bool open_file(const char* path, const char* mode);
    void close_files_dir();
    void close_filesystem_on_error();
    ftp_result_t read_file(char* filebuf, uint32_t desiredsize, uint32_t* actualsize);
    ftp_result_t write_file(char* filebuf, uint32_t size);
    ftp_result_t open_dir_for_listing(const char* path);
    int get_eplf_item(char* dest, uint32_t destsize, struct dirent* de);
    ftp_result_t list_dir(char* list, uint32_t maxlistsize, uint32_t* listsize);

    // Socket operations
    void close_cmd_data();
    void reset();
    bool create_listening_socket(int32_t* sd, uint32_t port, uint8_t backlog);
    ftp_result_t wait_for_connection(int32_t l_sd, int32_t* n_sd, uint32_t* ip_addr);

    // Communication
    void send_reply(uint32_t status, const char* message);
    void send_list(uint32_t datasize);
    void send_file_data(uint32_t datasize);
    ftp_result_t recv_non_blocking(int32_t sd, void* buff, int32_t Maxlen, int32_t* rxLen);

    // Path operations
    void open_child(char* pwd, char* dir);
    void close_child(char* pwd);
    void remove_fname_from_path(char* pwd, char* fname);

    // Command parsing
    void pop_param(char** str, char* param, size_t maxlen, bool stop_on_space, bool stop_on_newline);
    ftp_cmd_index_t pop_command(char** str);
    void get_param_and_open_child(char** bufptr);

    // Main processing
    void process_cmd();
    void wait_for_enabled();

    // Initialization
    bool init();
    void deinit();
    int run(uint32_t elapsed);
    bool enable();
    bool disable();
    bool terminate();
    bool stop_requested();

    // Task wrapper (must be static for FreeRTOS)
    static void task_wrapper(void* pvParameters);
    void task_loop();
};

} // namespace FtpServer

#endif /* FTP_SERVER_CORE_H */
