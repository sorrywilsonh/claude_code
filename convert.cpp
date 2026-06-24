// convert.cpp
//
// 將「binary 固定長度 record」檔案轉成「全 ASCII 文字固定長度 record」檔案。
//
// 設計重點:
//   1. 兩邊都是固定長度 record，沒有分隔符，可批次處理。
//   2. 環境限制: 每次 read()/write() syscall 最多 57344 byte。
//      -> 緩衝區取「record 大小的整數倍」且 <= 57344，避免 record 被切斷，
//         同時讓每次 IO 盡量貼近上限以減少 syscall 次數。
//   3. char 欄位直接搬，int 欄位手寫轉成固定寬度 ASCII (比 snprintf 快)。
//   4. read()/write() 可能 short，用迴圈補滿。
//
// 編譯: g++ -O2 -o convert convert.cpp
// 執行: ./convert input.dat output.txt

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// ===========================================================================
// (1) 你的 record 定義 —— 改這裡
// ===========================================================================
// 用 #pragma pack(1) 確保沒有 compiler padding，sizeof 才會等於檔案上的長度。

#pragma pack(push, 1)
struct InputRecord {       // 來源檔 (binary)
    char     id[8];        // 文字欄位
    int32_t  amount;       // 整數欄位 (binary, host endian)
    char     name[20];     // 文字欄位
    int32_t  count;        // 整數欄位
};

struct OutputRecord {      // 目標檔 (全 ASCII)
    char id[8];            // 原樣搬
    char amount[11];       // int32 最多 -2147483648 = 11 個字元
    char name[20];         // 原樣搬
    char count[11];
};
#pragma pack(pop)

// ===========================================================================
// 整數 -> 固定寬度 ASCII (向右靠齊，左邊補空白)。比 snprintf 快。
// 例如 width=11, v=123  ->  "        123"
// ===========================================================================
static inline void int_to_ascii(int32_t v, char* dst, int width) {
    char tmp[12];
    int  i   = 0;
    bool neg = v < 0;
    // 用 64-bit 取絕對值，避免 INT32_MIN 溢位
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do { tmp[i++] = char('0' + (u % 10)); u /= 10; } while (u);
    if (neg) tmp[i++] = '-';

    int p = 0;
    for (int pad = width - i; p < pad; ++p) dst[p] = ' ';   // 左補空白
    for (int j = i - 1; j >= 0; --j)        dst[p++] = tmp[j];
}

// ===========================================================================
// (2) 單筆轉換邏輯 —— 改這裡
// ===========================================================================
static inline void convert_record(const InputRecord& in, OutputRecord& out) {
    memcpy(out.id,   in.id,   sizeof(out.id));      // char 欄位直接搬
    memcpy(out.name, in.name, sizeof(out.name));
    int_to_ascii(in.amount, out.amount, sizeof(out.amount));  // int -> ASCII
    int_to_ascii(in.count,  out.count,  sizeof(out.count));
}

// ===========================================================================
// IO 緩衝設定: 受 57344 byte/次 限制
// ===========================================================================
static const size_t IO_MAX = 57344;

static const size_t IN_REC  = sizeof(InputRecord);
static const size_t OUT_REC = sizeof(OutputRecord);

// 一批的 record 數: 讀、寫緩衝都不能超過 IO_MAX，取較小者
static const size_t BATCH =
    (IO_MAX / IN_REC) < (IO_MAX / OUT_REC) ? (IO_MAX / IN_REC)
                                           : (IO_MAX / OUT_REC);

static const size_t READ_BUF_SIZE  = BATCH * IN_REC;   // <= 57344
static const size_t WRITE_BUF_SIZE = BATCH * OUT_REC;  // <= 57344

// 讀滿 want byte (處理 short read / EINTR)。回傳實際讀到的 byte 數 (EOF 時 < want)。
static size_t read_block(int fd, char* buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read");
            exit(1);
        }
        if (r == 0) break;  // EOF
        got += (size_t)r;
    }
    return got;
}

// 寫滿 len byte (處理 short write / EINTR)。
static void write_block(int fd, const char* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            exit(1);
        }
        done += (size_t)w;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }

    int in_fd = open(argv[1], O_RDONLY);
    if (in_fd < 0) { perror("open input"); return 1; }

    int out_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { perror("open output"); close(in_fd); return 1; }

    char* read_buf  = (char*)malloc(READ_BUF_SIZE);
    char* write_buf = (char*)malloc(WRITE_BUF_SIZE);
    if (!read_buf || !write_buf) { fprintf(stderr, "malloc failed\n"); return 1; }

    unsigned long long total = 0;

    for (;;) {
        size_t got = read_block(in_fd, read_buf, READ_BUF_SIZE);
        if (got == 0) break;                 // 正常結束

        if (got % IN_REC != 0) {             // 檔尾有不完整 record
            fprintf(stderr, "Error: input size not a multiple of record size "
                            "(trailing %zu bytes)\n", got % IN_REC);
            free(read_buf); free(write_buf);
            close(in_fd); close(out_fd);
            return 1;
        }

        size_t n = got / IN_REC;             // 這批的 record 數
        for (size_t i = 0; i < n; ++i) {
            convert_record(
                *reinterpret_cast<const InputRecord*>(read_buf + i * IN_REC),
                *reinterpret_cast<OutputRecord*>(write_buf + i * OUT_REC));
        }
        write_block(out_fd, write_buf, n * OUT_REC);
        total += n;
    }

    free(read_buf);
    free(write_buf);
    if (close(in_fd) < 0)  perror("close input");
    if (close(out_fd) < 0) perror("close output");  // 確認資料落地

    fprintf(stderr, "Done. %llu records, BATCH=%zu, read_buf=%zu, write_buf=%zu\n",
            total, BATCH, READ_BUF_SIZE, WRITE_BUF_SIZE);
    return 0;
}
