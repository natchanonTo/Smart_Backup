/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║        SMART BACKUP & CLEANUP TOOL v1.1                     ║
 * ║        ระบบสำรองข้อมูลอัจฉริยะ                              ║
 * ║        Author: Smart Backup System                          ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * คอมไพล์ (Windows - MinGW):
 *   gcc -o smart_backup.exe smart_backup.c
 *
 * คอมไพล์ (Linux / macOS):
 *   gcc -o smart_backup smart_backup.c
 *
 * ฟีเจอร์:
 *  1. สำรองข้อมูลไฟล์/โฟลเดอร์ (พร้อม timestamp)
 *  2. ลบไฟล์ชั่วคราวและไฟล์ขยะอัตโนมัติ
 *  3. ค้นหาไฟล์ซ้ำ (Duplicate Finder)
 *  4. แสดงรายงานการใช้พื้นที่
 *  5. กำหนดตารางสำรองข้อมูลอัตโนมัติ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════
   PLATFORM DETECTION & COMPATIBILITY LAYER
   ══════════════════════════════════════════════════════════ */
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <sys/stat.h>

    #define PATH_SEP      "\\"
    #define MKDIR(p)      _mkdir(p)
    #define COPY_CMD      "xcopy /E /I /Y \"%s\" \"%s\""
    #define RM_CMD        "del /F /Q \"%s\""
    #define CLEAR_CMD     "cls"
    #define strcasecmp    _stricmp

    /* ── Windows wrapper: DIR / dirent (แทน <dirent.h>) ── */
    typedef struct {
        HANDLE          handle;
        WIN32_FIND_DATA data;
        int             first;
    } DIR;

    struct dirent {
        char d_name[MAX_PATH];
    };

    static DIR *opendir(const char *path) {
        char search[MAX_PATH];
        snprintf(search, sizeof(search), "%s\\*", path);
        DIR *d = (DIR *)malloc(sizeof(DIR));
        if (!d) return NULL;
        d->handle = FindFirstFileA(search, &d->data);
        d->first  = 1;
        if (d->handle == INVALID_HANDLE_VALUE) { free(d); return NULL; }
        return d;
    }

    static struct dirent *readdir(DIR *d) {
        static struct dirent e;
        if (d->first) {
            d->first = 0;
        } else {
            if (!FindNextFileA(d->handle, &d->data)) return NULL;
        }
        strncpy(e.d_name, d->data.cFileName, MAX_PATH - 1);
        e.d_name[MAX_PATH - 1] = '\0';
        return &e;
    }

    static void closedir(DIR *d) {
        if (d) { FindClose(d->handle); free(d); }
    }

    /* ── Windows: stat wrapper ── */
    #define stat  _stat
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)

    /* ── เปิด ANSI color + UTF-8 ใน Windows Console ── */
    static void enable_win_console(void) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD  mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(65001);
    }

#else
    /* Linux / macOS — ใช้ header มาตรฐาน POSIX */
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>

    #define PATH_SEP  "/"
    #define MKDIR(p)  mkdir(p, 0755)
    #define COPY_CMD  "cp -r \"%s\" \"%s\""
    #define RM_CMD    "rm -f \"%s\""
    #define CLEAR_CMD "clear"

    static void enable_win_console(void) {} /* ไม่ต้องทำอะไรบน Linux/macOS */
#endif

/* ─── Constants ─── */
#define MAX_PATH        512
#define MAX_FILES       1000
#define MAX_LOG_ENTRIES 500
#define VERSION         "1.0.0"
#define LOG_FILE        "backup_log.txt"
#define CONFIG_FILE     "backup_config.txt"

/* ─── Colors (ANSI) ─── */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_WHITE   "\033[1;37m"
#define COLOR_BOLD    "\033[1m"
#define BG_BLUE       "\033[44m"
#define BG_GREEN      "\033[42m"

/* ─── Structs ─── */
typedef struct {
    char   path[MAX_PATH];
    long   size;
    time_t modified;
    char   hash[33]; /* MD5-like simple hash */
} FileInfo;

typedef struct {
    char   timestamp[32];
    char   action[64];
    char   source[MAX_PATH];
    char   destination[MAX_PATH];
    int    success;
    long   bytes_copied;
} LogEntry;

typedef struct {
    char source_dir[MAX_PATH];
    char backup_dir[MAX_PATH];
    int  auto_cleanup;
    int  keep_days;
    int  compress;
    int  schedule_hours; /* ทุกกี่ชั่วโมง */
} BackupConfig;

/* ─── Global Variables ─── */
LogEntry  g_log[MAX_LOG_ENTRIES];
int       g_log_count = 0;
BackupConfig g_config = {
    "./source",
    "./backups",
    1,
    30,
    0,
    24
};

/* ══════════════════════════════════════════════════════════
   UTILITY FUNCTIONS
   ══════════════════════════════════════════════════════════ */

void clear_screen(void) {
    system(CLEAR_CMD);
}

void get_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%Y%m%d_%H%M%S", t);
}

void get_display_time(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%d/%m/%Y %H:%M:%S", t);
}

long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return -1;
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return 0;
}

/* แปลง bytes เป็น KB/MB/GB */
void format_size(long bytes, char *buf, size_t size) {
    if (bytes < 1024)
        snprintf(buf, size, "%ld B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, size, "%.2f KB", bytes / 1024.0);
    else if (bytes < 1024L * 1024 * 1024)
        snprintf(buf, size, "%.2f MB", bytes / (1024.0 * 1024));
    else
        snprintf(buf, size, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
}

/* Simple checksum (ไม่ใช่ MD5 จริง แต่ใช้สำหรับตรวจหาซ้ำ) */
unsigned long simple_hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    unsigned long hash = 5381;
    int c;
    while ((c = fgetc(f)) != EOF) {
        hash = ((hash << 5) + hash) + c;
    }
    fclose(f);
    return hash;
}

/* ══════════════════════════════════════════════════════════
   UI / BANNER
   ══════════════════════════════════════════════════════════ */

void print_banner(void) {
    clear_screen();
    printf(COLOR_CYAN);
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                                                              ║\n");
    printf("║   " COLOR_WHITE "███████╗███╗   ███╗ █████╗ ██████╗ ████████╗" COLOR_CYAN "             ║\n");
    printf("║   " COLOR_WHITE "██╔════╝████╗ ████║██╔══██╗██╔══██╗╚══██╔══╝" COLOR_CYAN "             ║\n");
    printf("║   " COLOR_WHITE "███████╗██╔████╔██║███████║██████╔╝   ██║   " COLOR_CYAN "              ║\n");
    printf("║   " COLOR_WHITE "╚════██║██║╚██╔╝██║██╔══██║██╔══██╗   ██║   " COLOR_CYAN "              ║\n");
    printf("║   " COLOR_WHITE "███████║██║ ╚═╝ ██║██║  ██║██║  ██║   ██║   " COLOR_CYAN "              ║\n");
    printf("║   " COLOR_WHITE "╚══════╝╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝   " COLOR_CYAN "             ║\n");
    printf("║                                                              ║\n");
    printf("║   " COLOR_GREEN "BACKUP & CLEANUP TOOL" COLOR_CYAN " v%-6s  " COLOR_YELLOW "ระบบสำรองข้อมูลอัจฉริยะ" COLOR_CYAN "   ║\n", VERSION);
    printf("║                                                              ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET "\n");
}

void print_separator(void) {
    printf(COLOR_CYAN "──────────────────────────────────────────────────────────────\n" COLOR_RESET);
}

void print_section(const char *title) {
    printf(COLOR_BOLD COLOR_BLUE "\n  ▶ %s\n" COLOR_RESET, title);
    printf(COLOR_BLUE "  ");
    for (int i = 0; i < (int)strlen(title) + 4; i++) printf("─");
    printf(COLOR_RESET "\n");
}

void print_success(const char *msg) {
    printf(COLOR_GREEN "  ✓ %s\n" COLOR_RESET, msg);
}

void print_error(const char *msg) {
    printf(COLOR_RED "  ✗ %s\n" COLOR_RESET, msg);
}

void print_warning(const char *msg) {
    printf(COLOR_YELLOW "  ⚠ %s\n" COLOR_RESET, msg);
}

void print_info(const char *msg) {
    printf(COLOR_CYAN "  ℹ %s\n" COLOR_RESET, msg);
}

/* Progress Bar */
void print_progress(int current, int total, const char *label) {
    int bar_width = 40;
    float progress = (total > 0) ? (float)current / total : 0;
    int filled = (int)(progress * bar_width);

    printf("\r  " COLOR_CYAN "[");
    for (int i = 0; i < bar_width; i++) {
        if (i < filled)      printf(COLOR_GREEN "█");
        else if (i == filled) printf(COLOR_YELLOW "▌");
        else                  printf(COLOR_BLUE "░");
    }
    printf(COLOR_CYAN "] " COLOR_WHITE "%3d%% " COLOR_YELLOW "%s" COLOR_RESET,
           (int)(progress * 100), label);
    fflush(stdout);
    if (current == total) printf("\n");
}

/* ══════════════════════════════════════════════════════════
   LOGGING
   ══════════════════════════════════════════════════════════ */

void add_log(const char *action, const char *src, const char *dst, int ok, long bytes) {
    if (g_log_count >= MAX_LOG_ENTRIES) return;

    LogEntry *e = &g_log[g_log_count++];
    get_display_time(e->timestamp, sizeof(e->timestamp));
    strncpy(e->action,      action, sizeof(e->action) - 1);
    strncpy(e->source,      src,    sizeof(e->source) - 1);
    strncpy(e->destination, dst,    sizeof(e->destination) - 1);
    e->success      = ok;
    e->bytes_copied = bytes;

    /* เขียนลงไฟล์ log ด้วย */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] %s | %s -> %s | %s | %ld bytes\n",
                e->timestamp, e->action, e->source, e->destination,
                ok ? "SUCCESS" : "FAILED", bytes);
        fclose(f);
    }
}

void show_log(void) {
    print_banner();
    print_section("บันทึกการทำงาน (Activity Log)");

    if (g_log_count == 0) {
        print_info("ยังไม่มีประวัติการทำงาน");
        return;
    }

    printf("  %-22s %-15s %-10s %s\n",
           "เวลา", "การกระทำ", "สถานะ", "ขนาด");
    print_separator();

    for (int i = g_log_count - 1; i >= 0 && i >= g_log_count - 20; i--) {
        LogEntry *e = &g_log[i];
        char size_str[32];
        format_size(e->bytes_copied, size_str, sizeof(size_str));

        printf("  %s%s%-22s%s %-15s %s%-10s%s %s\n",
               COLOR_WHITE, "", e->timestamp, COLOR_RESET,
               e->action,
               e->success ? COLOR_GREEN : COLOR_RED,
               e->success ? "✓ สำเร็จ" : "✗ ล้มเหลว",
               COLOR_RESET,
               size_str);
    }
    printf("\n  %s(แสดง %d รายการล่าสุด จากทั้งหมด %d)%s\n",
           COLOR_YELLOW, (g_log_count < 20 ? g_log_count : 20),
           g_log_count, COLOR_RESET);
}

/* ══════════════════════════════════════════════════════════
   CONFIG
   ══════════════════════════════════════════════════════════ */

void save_config(void) {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) { print_error("ไม่สามารถบันทึก config ได้"); return; }

    fprintf(f, "source_dir=%s\n",    g_config.source_dir);
    fprintf(f, "backup_dir=%s\n",    g_config.backup_dir);
    fprintf(f, "auto_cleanup=%d\n",  g_config.auto_cleanup);
    fprintf(f, "keep_days=%d\n",     g_config.keep_days);
    fprintf(f, "schedule_hours=%d\n",g_config.schedule_hours);
    fclose(f);
    print_success("บันทึกการตั้งค่าเรียบร้อย");
}

void load_config(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return; /* ใช้ค่า default */

    char line[256], key[64], val[MAX_PATH];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63[^=]=%511s", key, val) == 2) {
            if (!strcmp(key, "source_dir"))    strncpy(g_config.source_dir, val, MAX_PATH - 1);
            if (!strcmp(key, "backup_dir"))    strncpy(g_config.backup_dir, val, MAX_PATH - 1);
            if (!strcmp(key, "auto_cleanup"))  g_config.auto_cleanup  = atoi(val);
            if (!strcmp(key, "keep_days"))     g_config.keep_days     = atoi(val);
            if (!strcmp(key, "schedule_hours"))g_config.schedule_hours = atoi(val);
        }
    }
    fclose(f);
}

/* ══════════════════════════════════════════════════════════
   FEATURE 1: BACKUP
   ══════════════════════════════════════════════════════════ */

/* นับขนาดโฟลเดอร์แบบ recursive */
long calc_dir_size(const char *path) {
    DIR *d = opendir(path);
    if (!d) return get_file_size(path);

    long total = 0;
    struct dirent *entry;
    char sub[MAX_PATH];

    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(sub, sizeof(sub), "%s%s%s", path, PATH_SEP, entry->d_name);
        total += is_directory(sub) ? calc_dir_size(sub) : get_file_size(sub);
    }
    closedir(d);
    return total;
}

int do_backup(const char *src, const char *dst_base) {
    if (!path_exists(src)) {
        print_error("ไม่พบต้นทางที่ระบุ");
        return 0;
    }

    /* สร้างชื่อโฟลเดอร์ backup พร้อม timestamp */
    char ts[32], dst[MAX_PATH];
    get_timestamp(ts, sizeof(ts));
    snprintf(dst, sizeof(dst), "%s%sbackup_%s", dst_base, PATH_SEP, ts);

    /* สร้างโฟลเดอร์ปลายทาง */
    if (!path_exists(dst_base)) MKDIR(dst_base);

    printf(COLOR_CYAN "\n  กำลังสำรองข้อมูล...\n" COLOR_RESET);
    printf("  " COLOR_WHITE "ต้นทาง : " COLOR_YELLOW "%s\n" COLOR_RESET, src);
    printf("  " COLOR_WHITE "ปลายทาง: " COLOR_YELLOW "%s\n" COLOR_RESET, dst);

    long src_size = calc_dir_size(src);
    char size_str[32];
    format_size(src_size, size_str, sizeof(size_str));
    printf("  " COLOR_WHITE "ขนาดข้อมูล: " COLOR_MAGENTA "%s\n" COLOR_RESET, size_str);

    /* แสดง progress */
    for (int i = 0; i <= 10; i++) {
        print_progress(i, 10, "กำลังคัดลอก...");
        struct timespec ts_sleep = {0, 50000000L};
#ifndef _WIN32
        nanosleep(&ts_sleep, NULL);
#endif
    }

    /* สั่ง copy จริง */
    char cmd[MAX_PATH * 2 + 64];
    snprintf(cmd, sizeof(cmd), COPY_CMD, src, dst);
    int ret = system(cmd);
    int success = (ret == 0);

    add_log("BACKUP", src, dst, success, src_size);

    if (success) {
        print_success("สำรองข้อมูลสำเร็จ!");
        printf("  " COLOR_GREEN "บันทึกที่: %s\n" COLOR_RESET, dst);
    } else {
        print_error("เกิดข้อผิดพลาดขณะสำรองข้อมูล");
    }
    return success;
}

void menu_backup(void) {
    print_banner();
    print_section("สำรองข้อมูล (Backup)");

    char src[MAX_PATH], dst[MAX_PATH];

    printf("  " COLOR_WHITE "โฟลเดอร์ต้นทาง [%s]: " COLOR_RESET, g_config.source_dir);
    fgets(src, sizeof(src), stdin);
    src[strcspn(src, "\n")] = 0;
    if (strlen(src) == 0) strncpy(src, g_config.source_dir, MAX_PATH - 1);

    printf("  " COLOR_WHITE "โฟลเดอร์ปลายทาง [%s]: " COLOR_RESET, g_config.backup_dir);
    fgets(dst, sizeof(dst), stdin);
    dst[strcspn(dst, "\n")] = 0;
    if (strlen(dst) == 0) strncpy(dst, g_config.backup_dir, MAX_PATH - 1);

    do_backup(src, dst);
}

/* ══════════════════════════════════════════════════════════
   FEATURE 2: CLEANUP (ลบไฟล์ขยะ)
   ══════════════════════════════════════════════════════════ */

/* นามสกุลที่ถือว่าเป็นไฟล์ชั่วคราว */
static const char *JUNK_EXT[] = {
    ".tmp", ".temp", ".bak", ".old", ".log",
    ".swp", ".DS_Store", "Thumbs.db", NULL
};

int is_junk_file(const char *name) {
    for (int i = 0; JUNK_EXT[i]; i++) {
        size_t elen = strlen(JUNK_EXT[i]);
        size_t nlen = strlen(name);
        if (nlen >= elen && strcasecmp(name + nlen - elen, JUNK_EXT[i]) == 0)
            return 1;
        /* ตรวจ full name ด้วย (เช่น Thumbs.db) */
        if (strcasecmp(name, JUNK_EXT[i]) == 0) return 1;
    }
    return 0;
}

typedef struct {
    long total_size;
    int  total_files;
    int  deleted;
} CleanResult;

void scan_cleanup(const char *dir, int dry_run, CleanResult *res) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    char path[MAX_PATH];

    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, PATH_SEP, entry->d_name);

        if (is_directory(path)) {
            scan_cleanup(path, dry_run, res);
        } else if (is_junk_file(entry->d_name)) {
            long sz = get_file_size(path);
            res->total_files++;
            res->total_size += sz;

            char sz_str[32];
            format_size(sz, sz_str, sizeof(sz_str));

            if (dry_run) {
                printf("  " COLOR_YELLOW "  [พบ] %s " COLOR_WHITE "(%s)\n" COLOR_RESET, path, sz_str);
            } else {
                char cmd[MAX_PATH + 32];
                snprintf(cmd, sizeof(cmd), RM_CMD, path);
                if (system(cmd) == 0) {
                    printf("  " COLOR_RED "  [ลบ] %s " COLOR_WHITE "(%s)\n" COLOR_RESET, path, sz_str);
                    res->deleted++;
                    add_log("DELETE", path, "", 1, sz);
                } else {
                    print_error(path);
                }
            }
        }
    }
    closedir(d);
}

void menu_cleanup(void) {
    print_banner();
    print_section("ล้างไฟล์ขยะ (Cleanup Junk Files)");

    char dir[MAX_PATH];
    printf("  " COLOR_WHITE "โฟลเดอร์ที่ต้องการสแกน [%s]: " COLOR_RESET, g_config.source_dir);
    fgets(dir, sizeof(dir), stdin);
    dir[strcspn(dir, "\n")] = 0;
    if (strlen(dir) == 0) strncpy(dir, g_config.source_dir, MAX_PATH - 1);

    if (!path_exists(dir)) { print_error("ไม่พบโฟลเดอร์ที่ระบุ"); return; }

    /* Dry run ก่อน */
    CleanResult res = {0};
    printf("\n  " COLOR_CYAN "กำลังสแกนหาไฟล์ขยะ...\n\n" COLOR_RESET);
    scan_cleanup(dir, 1, &res);

    if (res.total_files == 0) {
        print_success("ไม่พบไฟล์ขยะ โฟลเดอร์สะอาดแล้ว!");
        return;
    }

    char sz_str[32];
    format_size(res.total_size, sz_str, sizeof(sz_str));
    printf("\n");
    print_separator();
    printf("  " COLOR_YELLOW "พบไฟล์ขยะ: " COLOR_WHITE "%d ไฟล์ " COLOR_YELLOW "รวม: " COLOR_WHITE "%s\n" COLOR_RESET,
           res.total_files, sz_str);

    printf("\n  " COLOR_WHITE "ต้องการลบทั้งหมด? (y/n): " COLOR_RESET);
    char ans[8];
    fgets(ans, sizeof(ans), stdin);
    ans[strcspn(ans, "\n")] = 0;

    if (tolower(ans[0]) == 'y') {
        CleanResult del_res = {0};
        scan_cleanup(dir, 0, &del_res);
        printf("\n");
        print_success("ลบไฟล์ขยะเสร็จสิ้น!");
        format_size(del_res.total_size, sz_str, sizeof(sz_str));
        printf("  " COLOR_GREEN "ลบทั้งหมด: " COLOR_WHITE "%d ไฟล์ (%s)\n" COLOR_RESET,
               del_res.deleted, sz_str);
    } else {
        print_info("ยกเลิกการลบ");
    }
}

/* ══════════════════════════════════════════════════════════
   FEATURE 3: DUPLICATE FINDER
   ══════════════════════════════════════════════════════════ */

FileInfo g_files[MAX_FILES];
int g_file_count = 0;

void scan_files(const char *dir) {
    if (g_file_count >= MAX_FILES) return;

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    char path[MAX_PATH];

    while ((entry = readdir(d)) && g_file_count < MAX_FILES) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, PATH_SEP, entry->d_name);

        if (is_directory(path)) {
            scan_files(path);
        } else {
            FileInfo *fi = &g_files[g_file_count++];
            strncpy(fi->path, path, MAX_PATH - 1);
            fi->size = get_file_size(path);

            struct stat st;
            if (stat(path, &st) == 0) fi->modified = st.st_mtime;

            unsigned long h = simple_hash_file(path);
            snprintf(fi->hash, sizeof(fi->hash), "%lu_%ld", h, fi->size);
        }
    }
    closedir(d);
}

void menu_duplicates(void) {
    print_banner();
    print_section("ค้นหาไฟล์ซ้ำ (Duplicate Finder)");

    char dir[MAX_PATH];
    printf("  " COLOR_WHITE "โฟลเดอร์ที่ต้องการสแกน [%s]: " COLOR_RESET, g_config.source_dir);
    fgets(dir, sizeof(dir), stdin);
    dir[strcspn(dir, "\n")] = 0;
    if (strlen(dir) == 0) strncpy(dir, g_config.source_dir, MAX_PATH - 1);

    if (!path_exists(dir)) { print_error("ไม่พบโฟลเดอร์ที่ระบุ"); return; }

    g_file_count = 0;
    printf("\n  " COLOR_CYAN "กำลังสแกน...\n" COLOR_RESET);
    scan_files(dir);

    printf("  สแกนพบไฟล์ทั้งหมด: " COLOR_WHITE "%d ไฟล์\n\n" COLOR_RESET, g_file_count);

    int dup_count = 0;
    long wasted  = 0;

    /* เปรียบ hash คู่กัน O(n²) — เหมาะสำหรับชุดไฟล์ขนาดกลาง */
    for (int i = 0; i < g_file_count; i++) {
        int printed_header = 0;
        for (int j = i + 1; j < g_file_count; j++) {
            if (strcmp(g_files[i].hash, g_files[j].hash) == 0) {
                if (!printed_header) {
                    char sz_str[32];
                    format_size(g_files[i].size, sz_str, sizeof(sz_str));
                    printf("  " COLOR_YELLOW "━━ กลุ่มซ้ำ [%s] ━━\n" COLOR_RESET, sz_str);
                    printf("    " COLOR_WHITE "%s\n" COLOR_RESET, g_files[i].path);
                    printed_header = 1;
                    dup_count++;
                }
                printf("    " COLOR_RED "↳ %s\n" COLOR_RESET, g_files[j].path);
                wasted += g_files[j].size;

                /* ถามว่าจะลบไฟล์ซ้ำหรือไม่ */
                printf("    " COLOR_WHITE "  ลบไฟล์ซ้ำนี้? (y/n): " COLOR_RESET);
                char ans[8];
                fgets(ans, sizeof(ans), stdin);
                ans[strcspn(ans, "\n")] = 0;
                if (tolower(ans[0]) == 'y') {
                    char cmd[MAX_PATH + 32];
                    snprintf(cmd, sizeof(cmd), RM_CMD, g_files[j].path);
                    system(cmd);
                    add_log("DEL_DUP", g_files[j].path, g_files[i].path, 1, g_files[j].size);
                    print_success("ลบเรียบร้อย");
                }
            }
        }
    }

    printf("\n");
    print_separator();
    if (dup_count == 0) {
        print_success("ไม่พบไฟล์ซ้ำ");
    } else {
        char sz_str[32];
        format_size(wasted, sz_str, sizeof(sz_str));
        printf("  " COLOR_YELLOW "พบกลุ่มซ้ำ: " COLOR_WHITE "%d กลุ่ม "
               COLOR_YELLOW "พื้นที่ที่ประหยัดได้: " COLOR_WHITE "%s\n" COLOR_RESET,
               dup_count, sz_str);
    }
}

/* ══════════════════════════════════════════════════════════
   FEATURE 4: DISK USAGE REPORT
   ══════════════════════════════════════════════════════════ */

typedef struct { char name[MAX_PATH]; long size; } DirEntry;

int cmp_dir_size(const void *a, const void *b) {
    return (int)(((DirEntry*)b)->size - ((DirEntry*)a)->size);
}

void menu_report(void) {
    print_banner();
    print_section("รายงานการใช้พื้นที่ (Disk Usage Report)");

    char dir[MAX_PATH];
    printf("  " COLOR_WHITE "โฟลเดอร์ที่ต้องการวิเคราะห์ [%s]: " COLOR_RESET, g_config.source_dir);
    fgets(dir, sizeof(dir), stdin);
    dir[strcspn(dir, "\n")] = 0;
    if (strlen(dir) == 0) strncpy(dir, g_config.source_dir, MAX_PATH - 1);

    if (!path_exists(dir)) { print_error("ไม่พบโฟลเดอร์ที่ระบุ"); return; }

    DIR *d = opendir(dir);
    if (!d) { print_error("ไม่สามารถเปิดโฟลเดอร์ได้"); return; }

    DirEntry entries[256];
    int entry_count = 0;
    long grand_total = 0;

    struct dirent *ent;
    char path[MAX_PATH];

    while ((ent = readdir(d)) && entry_count < 256) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, PATH_SEP, ent->d_name);

        long sz = is_directory(path) ? calc_dir_size(path) : get_file_size(path);
        strncpy(entries[entry_count].name, ent->d_name, MAX_PATH - 1);
        entries[entry_count].size = sz;
        grand_total += sz;
        entry_count++;
    }
    closedir(d);

    /* เรียงลำดับจากใหญ่ไปเล็ก */
    qsort(entries, entry_count, sizeof(DirEntry), cmp_dir_size);

    printf("\n  %-40s %12s %8s\n", "ชื่อ", "ขนาด", "สัดส่วน");
    print_separator();

    for (int i = 0; i < entry_count; i++) {
        char sz_str[32];
        format_size(entries[i].size, sz_str, sizeof(sz_str));
        float pct = (grand_total > 0) ? (float)entries[i].size / grand_total * 100 : 0;

        /* แถบ bar graph */
        int bar_len = (int)(pct / 2.5);
        printf("  " COLOR_WHITE "%-38.38s" COLOR_RESET " %s%12s%s %5.1f%% ",
               entries[i].name,
               COLOR_CYAN, sz_str, COLOR_RESET,
               pct);
        printf(COLOR_GREEN);
        for (int b = 0; b < bar_len; b++) printf("█");
        printf(COLOR_RESET "\n");
    }

    printf("\n");
    print_separator();
    char total_str[32];
    format_size(grand_total, total_str, sizeof(total_str));
    printf("  " COLOR_BOLD "รวมทั้งหมด: " COLOR_MAGENTA "%s" COLOR_RESET
           "  (%d รายการ)\n\n", total_str, entry_count);
}

/* ══════════════════════════════════════════════════════════
   FEATURE 5: SETTINGS
   ══════════════════════════════════════════════════════════ */

void menu_settings(void) {
    print_banner();
    print_section("การตั้งค่า (Settings)");

    char buf[MAX_PATH];

    printf("  " COLOR_WHITE "โฟลเดอร์ต้นทาง [%s]: " COLOR_RESET, g_config.source_dir);
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf)) strncpy(g_config.source_dir, buf, MAX_PATH - 1);

    printf("  " COLOR_WHITE "โฟลเดอร์ backup [%s]: " COLOR_RESET, g_config.backup_dir);
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf)) strncpy(g_config.backup_dir, buf, MAX_PATH - 1);

    printf("  " COLOR_WHITE "ล้างข้อมูลอัตโนมัติ (1=เปิด/0=ปิด) [%d]: " COLOR_RESET, g_config.auto_cleanup);
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf)) g_config.auto_cleanup = atoi(buf);

    printf("  " COLOR_WHITE "เก็บ backup กี่วัน [%d]: " COLOR_RESET, g_config.keep_days);
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf)) g_config.keep_days = atoi(buf);

    printf("  " COLOR_WHITE "กำหนด backup ทุกกี่ชั่วโมง [%d]: " COLOR_RESET, g_config.schedule_hours);
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf)) g_config.schedule_hours = atoi(buf);

    save_config();

    printf("\n  " COLOR_CYAN "─── การตั้งค่าปัจจุบัน ───\n" COLOR_RESET);
    printf("  ต้นทาง        : " COLOR_YELLOW "%s\n" COLOR_RESET, g_config.source_dir);
    printf("  ปลายทาง       : " COLOR_YELLOW "%s\n" COLOR_RESET, g_config.backup_dir);
    printf("  Auto Cleanup  : " COLOR_YELLOW "%s\n" COLOR_RESET, g_config.auto_cleanup ? "เปิด" : "ปิด");
    printf("  เก็บ backup   : " COLOR_YELLOW "%d วัน\n" COLOR_RESET, g_config.keep_days);
    printf("  กำหนดเวลา    : " COLOR_YELLOW "ทุก %d ชั่วโมง\n" COLOR_RESET, g_config.schedule_hours);
}

/* ══════════════════════════════════════════════════════════
   MAIN MENU
   ══════════════════════════════════════════════════════════ */

void press_enter(void) {
    printf("\n  " COLOR_CYAN "กด [Enter] เพื่อกลับเมนูหลัก..." COLOR_RESET);
    getchar();
}

void show_main_menu(void) {
    print_banner();

    char now[32];
    get_display_time(now, sizeof(now));
    printf("  " COLOR_WHITE "เวลา: " COLOR_YELLOW "%s\n", now);
    printf("  " COLOR_WHITE "ต้นทาง: " COLOR_CYAN "%s\n" COLOR_RESET, g_config.source_dir);
    printf("  " COLOR_WHITE "ปลายทาง: " COLOR_CYAN "%s\n" COLOR_RESET, g_config.backup_dir);
    printf("\n");

    print_separator();
    printf(COLOR_BOLD);
    printf("   %s[1]%s    สำรองข้อมูล             Backup\n", COLOR_GREEN,  COLOR_WHITE);
    printf("   %s[2]%s     ล้างไฟล์ขยะ             Cleanup Junk\n", COLOR_RED,   COLOR_WHITE);
    printf("   %s[3]%s    ค้นหาไฟล์ซ้ำ             Duplicate Finder\n", COLOR_BLUE, COLOR_WHITE);
    printf("   %s[4]%s    รายงานการใช้พื้นที่        Disk Report\n", COLOR_MAGENTA, COLOR_WHITE);
    printf("   %s[5]%s    ดูประวัติการทำงาน          Activity Log\n", COLOR_CYAN, COLOR_WHITE);
    printf("   %s[6]%s     การตั้งค่า                Settings\n", COLOR_YELLOW, COLOR_WHITE);
    printf("   %s[0]%s    ออกจากโปรแกรม             Exit\n", COLOR_RED, COLOR_WHITE);
    printf(COLOR_RESET);
    print_separator();
    printf("\n  " COLOR_WHITE "เลือกเมนู: " COLOR_RESET);
}

int main(void) {
    enable_win_console(); /* เปิด ANSI + UTF-8 บน Windows */
    load_config();

    char choice[8];
    while (1) {
        show_main_menu();
        fgets(choice, sizeof(choice), stdin);
        choice[strcspn(choice, "\n")] = 0;

        if (!strcmp(choice, "0")) {
            print_banner();
            printf("  " COLOR_CYAN "ขอบคุณที่ใช้งาน Smart Backup Tool\n");
            printf("  Goodbye! 👋\n\n" COLOR_RESET);
            break;
        } else if (!strcmp(choice, "1")) {
            menu_backup();
        } else if (!strcmp(choice, "2")) {
            menu_cleanup();
        } else if (!strcmp(choice, "3")) {
            menu_duplicates();
        } else if (!strcmp(choice, "4")) {
            menu_report();
        } else if (!strcmp(choice, "5")) {
            show_log();
        } else if (!strcmp(choice, "6")) {
            menu_settings();
        } else {
            print_error("กรุณาเลือก 0-6");
        }
        press_enter();
    }
    return 0;
}
