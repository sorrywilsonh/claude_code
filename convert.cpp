// convert.cpp
//
// 將 NonStop 產生的 binary 固定長度 record (tlodru_rec_def, 280 bytes)
// 轉成全 ASCII 文字固定長度 record (stlodr_rec_def, 233 bytes)。
//
// 設計重點:
//   1. 兩邊都是固定長度 record，沒有分隔符，批次處理。
//   2. 環境限制: 每次 read()/write() syscall 最多 57344 byte。
//      -> 每批 record 數 = min(57344/280, 57344/233) = 204，
//         讀緩衝 204*280 = 57120、寫緩衝 204*233 = 47532，皆 <= 57344，
//         且為 record 整數倍，不會把 record 切斷。
//   3. 來源 header 用 #pragma fieldalign shared2 (2-byte 對齊)，
//      這裡用 #pragma pack(2) 重現，static_assert 卡 sizeof 防 packing 出錯。
//   4. 數字欄位 -> 靠右、補前導 0 的固定寬度 ASCII；char 欄位直接搬。
//   5. 來源整數為 NonStop 大端序。本機若也是大端則不翻轉(identity)；
//      若在小端機器編譯，from_src* 會自動 byte swap。
//
// 編譯: g++ -O2 -o convert convert.cpp        (或 NonStop c++ 編譯器)
// 執行: ./convert input.dat output.txt

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// ===========================================================================
// 位元組順序
// ===========================================================================
#define SOURCE_BIG_ENDIAN 1   // 來源檔(NonStop)整數為大端序

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define HOST_BIG_ENDIAN 1
#else
#  define HOST_BIG_ENDIAN 0
#endif

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0xFF000000u) >> 24) | ((x & 0x00FF0000u) >> 8) |
           ((x & 0x0000FF00u) << 8)  | ((x & 0x000000FFu) << 24);
}
static inline uint64_t bswap64(uint64_t x) {
    return ((x & 0xFF00000000000000ull) >> 56) | ((x & 0x00FF000000000000ull) >> 40) |
           ((x & 0x0000FF0000000000ull) >> 24) | ((x & 0x000000FF00000000ull) >> 8)  |
           ((x & 0x00000000FF000000ull) << 8)  | ((x & 0x0000000000FF0000ull) << 24) |
           ((x & 0x000000000000FF00ull) << 40) | ((x & 0x00000000000000FFull) << 56);
}

#if HOST_BIG_ENDIAN == SOURCE_BIG_ENDIAN
static inline uint16_t from_src16(uint16_t x) { return x; }
static inline uint32_t from_src32(uint32_t x) { return x; }
static inline uint64_t from_src64(uint64_t x) { return x; }
#else
static inline uint16_t from_src16(uint16_t x) { return bswap16(x); }
static inline uint32_t from_src32(uint32_t x) { return bswap32(x); }
static inline uint64_t from_src64(uint64_t x) { return bswap64(x); }
#endif

// ===========================================================================
// Record 定義 (對應 todsys.h，fieldalign shared2 -> pack(2))
//   unsigned short -> uint16_t, __uint32_t -> uint32_t, long long -> int64_t
// ===========================================================================
#pragma pack(push, 2)

// 輸入: tlodru_rec_def (280 bytes)
struct InputRecord {
    struct {
        uint16_t odr_me_pid;
        int64_t  odr_seqno;
        int64_t  odr_lrpt_write_time;
        char     odr_data_location;
        char     odr_data_type;
        char     filler_0[2];
    } odr_header;
    struct {
        int64_t  odr_odr_time;
        uint32_t odr_stk_me_index;
        uint32_t odr_stk_total_index;
        char     odr_stk_id[6];
        char     odr_brk_id[4];
        char     odr_odr_no[5];
        char     odr_investor_no[7];
        char     odr_investor_code[5];
        char     odr_function_code[2];
        char     odr_buysell_code;
        char     odr_price_type;
        char     odr_time_in_force;
        uint32_t odr_odr_price;
        uint32_t odr_old_price;
        int64_t  odr_odr_qty;
        int64_t  odr_valid_qty;
        int64_t  odr_bf_qty;
        uint32_t odr_unit_share;
        char     odr_trade_kind;
        char     odr_margin_code;
        char     odr_abnormal_quota_check;
        char     odr_foreign_quota_check;
        char     odr_china_quota_check;
        char     odr_slb_quota_check;
        struct {
            uint16_t odr_priority_period;
            int64_t  odr_priority_number;
        } odr_priority_group;
        char     odr_link_brk_id[4];
        char     odr_link_pvc_id[2];
        char     odr_link_type;
        char     odr_ivacno_flag;
        char     odr_mthprt[4];
        uint16_t odr_fw_id;
        int64_t  odr_fw_seq_no;
        char     odr_cl_ord_id[12];
        uint32_t odr_session_id;
        uint32_t odr_trans_price;
        uint32_t odr_stable_ref_price;
        int64_t  odr_me_write_time;
        int64_t  odr_me_accept_time;
        int64_t  odr_me_reply_time;
        char     filler_1[88];
    } odr_content;
};

// 輸出: stlodr_rec_def (233 bytes，全 char)
struct OutputRecord {
    char sto_odr_time[12];
    char sto_stk_me_index[6];
    char sto_stk_total_index[8];
    char sto_stk_id[6];
    char sto_brk_id[4];
    char sto_odr_no[5];
    char sto_investor_no[7];
    char sto_investor_code[5];
    char sto_function_code[2];
    char sto_buysell_code;
    char sto_price_type;
    char sto_time_in_force;
    char sto_odr_price[9];
    char sto_old_price[9];
    char sto_odr_qty[8];
    char sto_valid_qty[8];
    char sto_bf_qty[8];
    char sto_unit_share[8];
    char sto_trade_kind;
    char sto_margin_code;
    char sto_abnormal_quota_check;
    char sto_foreign_quota_check;
    char sto_china_quota_check;
    char sto_slb_quota_check;
    struct {
        char sto_priority_period[2];
        char sto_priority_number[12];
    } sto_priority_group;
    char sto_link_brk_id[4];
    char sto_link_pvc_id[2];
    char sto_link_type;
    char sto_ivacno_flag;
    char sto_mthprt[4];
    char sto_fw_id[2];
    char sto_fw_seq_no[18];
    char sto_cl_ord_id[12];
    char sto_session_id[6];
    char sto_trans_price[9];
    char sto_stable_ref_price[9];
    char sto_data_type;
    char sto_me_write_time[12];
    char sto_me_accept_time[12];
    char sto_me_reply_time[12];
};

#pragma pack(pop)

// packing 一旦不對(layout 跟 header 不符)就編不過
static_assert(sizeof(InputRecord)  == 280, "InputRecord must be 280 bytes (check pack(2))");
static_assert(sizeof(OutputRecord) == 233, "OutputRecord must be 233 bytes");

// ===========================================================================
// 數字 -> 靠右、補前導 0 的固定寬度 ASCII。
// 值非負(計數/價格/時間)。位數超過欄位寬度視為資料異常 -> 報錯中止。
// ===========================================================================
static inline void fmt_u(uint64_t v, char* dst, int width,
                         const char* field, unsigned long long rec) {
    uint64_t orig = v;
    for (int i = width - 1; i >= 0; --i) { dst[i] = char('0' + (int)(v % 10)); v /= 10; }
    if (v != 0) {                       // 位數比欄位寬 -> 溢位，中止
        fprintf(stderr,
                "FATAL: record %llu field %s value %llu exceeds %d-digit width. Aborting.\n",
                rec, field, (unsigned long long)orig, width);
        exit(2);
    }
}

// 取出已校正端序的整數值
static inline uint64_t v16(uint16_t f) { return (uint64_t)from_src16(f); }
static inline uint64_t v32(uint32_t f) { return (uint64_t)from_src32(f); }
static inline uint64_t v64(int64_t  f) { return from_src64((uint64_t)f); }

// ===========================================================================
// 單筆轉換 (欄位對應依 header 名稱)
// ===========================================================================
static inline void convert_record(const InputRecord& in, OutputRecord& out,
                                  unsigned long long rec) {
    const auto& h = in.odr_header;
    const auto& c = in.odr_content;

    fmt_u(v64(c.odr_odr_time),        out.sto_odr_time,        12, "sto_odr_time",        rec);
    fmt_u(v32(c.odr_stk_me_index),    out.sto_stk_me_index,     6, "sto_stk_me_index",    rec);
    fmt_u(v32(c.odr_stk_total_index), out.sto_stk_total_index,  8, "sto_stk_total_index", rec);

    memcpy(out.sto_stk_id,        c.odr_stk_id,        sizeof(out.sto_stk_id));
    memcpy(out.sto_brk_id,        c.odr_brk_id,        sizeof(out.sto_brk_id));
    memcpy(out.sto_odr_no,        c.odr_odr_no,        sizeof(out.sto_odr_no));
    memcpy(out.sto_investor_no,   c.odr_investor_no,   sizeof(out.sto_investor_no));
    memcpy(out.sto_investor_code, c.odr_investor_code, sizeof(out.sto_investor_code));
    memcpy(out.sto_function_code, c.odr_function_code, sizeof(out.sto_function_code));

    out.sto_buysell_code   = c.odr_buysell_code;
    out.sto_price_type     = c.odr_price_type;
    out.sto_time_in_force  = c.odr_time_in_force;

    fmt_u(v32(c.odr_odr_price),  out.sto_odr_price,  9, "sto_odr_price",  rec);
    fmt_u(v32(c.odr_old_price),  out.sto_old_price,  9, "sto_old_price",  rec);
    fmt_u(v64(c.odr_odr_qty),    out.sto_odr_qty,    8, "sto_odr_qty",    rec);
    fmt_u(v64(c.odr_valid_qty),  out.sto_valid_qty,  8, "sto_valid_qty",  rec);
    fmt_u(v64(c.odr_bf_qty),     out.sto_bf_qty,     8, "sto_bf_qty",     rec);
    fmt_u(v32(c.odr_unit_share), out.sto_unit_share, 8, "sto_unit_share", rec);

    out.sto_trade_kind           = c.odr_trade_kind;
    out.sto_margin_code          = c.odr_margin_code;
    out.sto_abnormal_quota_check = c.odr_abnormal_quota_check;
    out.sto_foreign_quota_check  = c.odr_foreign_quota_check;
    out.sto_china_quota_check    = c.odr_china_quota_check;
    out.sto_slb_quota_check      = c.odr_slb_quota_check;

    fmt_u(v16(c.odr_priority_group.odr_priority_period),
          out.sto_priority_group.sto_priority_period,  2, "sto_priority_period", rec);
    fmt_u(v64(c.odr_priority_group.odr_priority_number),
          out.sto_priority_group.sto_priority_number, 12, "sto_priority_number", rec);

    memcpy(out.sto_link_brk_id, c.odr_link_brk_id, sizeof(out.sto_link_brk_id));
    memcpy(out.sto_link_pvc_id, c.odr_link_pvc_id, sizeof(out.sto_link_pvc_id));
    out.sto_link_type   = c.odr_link_type;
    out.sto_ivacno_flag = c.odr_ivacno_flag;
    memcpy(out.sto_mthprt, c.odr_mthprt, sizeof(out.sto_mthprt));

    fmt_u(v16(c.odr_fw_id),     out.sto_fw_id,      2, "sto_fw_id",     rec);
    fmt_u(v64(c.odr_fw_seq_no), out.sto_fw_seq_no, 18, "sto_fw_seq_no", rec);

    memcpy(out.sto_cl_ord_id, c.odr_cl_ord_id, sizeof(out.sto_cl_ord_id));

    fmt_u(v32(c.odr_session_id),       out.sto_session_id,       6, "sto_session_id",       rec);
    fmt_u(v32(c.odr_trans_price),      out.sto_trans_price,      9, "sto_trans_price",      rec);
    fmt_u(v32(c.odr_stable_ref_price), out.sto_stable_ref_price, 9, "sto_stable_ref_price", rec);

    out.sto_data_type = h.odr_data_type;   // 來自 odr_header

    fmt_u(v64(c.odr_me_write_time),  out.sto_me_write_time,  12, "sto_me_write_time",  rec);
    fmt_u(v64(c.odr_me_accept_time), out.sto_me_accept_time, 12, "sto_me_accept_time", rec);
    fmt_u(v64(c.odr_me_reply_time),  out.sto_me_reply_time,  12, "sto_me_reply_time",  rec);
}

// ===========================================================================
// IO 緩衝 (受 57344 byte/次 限制)
// ===========================================================================
static const size_t IO_MAX  = 57344;
static const size_t IN_REC  = sizeof(InputRecord);   // 280
static const size_t OUT_REC = sizeof(OutputRecord);  // 233

static const size_t BATCH =
    (IO_MAX / IN_REC) < (IO_MAX / OUT_REC) ? (IO_MAX / IN_REC) : (IO_MAX / OUT_REC);
static const size_t READ_BUF_SIZE  = BATCH * IN_REC;   // <= 57344
static const size_t WRITE_BUF_SIZE = BATCH * OUT_REC;  // <= 57344

// 讀滿 want byte (處理 short read / EINTR)。回傳實際讀到的 byte (EOF 時 < want)。
static size_t read_block(int fd, char* buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0) { if (errno == EINTR) continue; perror("read"); exit(1); }
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
        if (w < 0) { if (errno == EINTR) continue; perror("write"); exit(1); }
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
        if (got == 0) break;                       // 正常結束

        if (got % IN_REC != 0) {                   // 檔尾不完整 record
            fprintf(stderr, "Error: input size not a multiple of %zu (trailing %zu bytes)\n",
                    IN_REC, got % IN_REC);
            free(read_buf); free(write_buf); close(in_fd); close(out_fd);
            return 1;
        }

        size_t n = got / IN_REC;
        for (size_t i = 0; i < n; ++i) {
            convert_record(
                *reinterpret_cast<const InputRecord*>(read_buf + i * IN_REC),
                *reinterpret_cast<OutputRecord*>(write_buf + i * OUT_REC),
                total + i + 1);
        }
        write_block(out_fd, write_buf, n * OUT_REC);
        total += n;
    }

    free(read_buf);
    free(write_buf);
    if (close(in_fd) < 0)  perror("close input");
    if (close(out_fd) < 0) perror("close output");   // 確認資料落地

    fprintf(stderr, "Done. %llu records. BATCH=%zu read_buf=%zu write_buf=%zu\n",
            total, BATCH, READ_BUF_SIZE, WRITE_BUF_SIZE);
    return 0;
}
