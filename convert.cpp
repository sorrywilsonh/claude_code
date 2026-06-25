// convert.cpp  ── NonStop Guardian 版本
//
// 將 binary 固定長度 record (tlodru_rec_def, 280 bytes)
// 轉成全 ASCII 文字固定長度 record (stlodr_rec_def, 233 bytes)。
//
// 設計重點:
//   1. 直接 #include "todsys.h"，用你自己的 schema 型別 tlodru_rec_def /
//      stlodr_rec_def。layout 由 header 的 #pragma fieldalign shared2 保證，
//      不必在這支程式重猜 packing。編譯期用 typedef 斷言卡住 sizeof。
//   2. 檔案 I/O 用 Guardian PUT library: PUT_FILE_OPEN_ / PUT_READX /
//      PUT_WRITEX / PUT_FILE_CLOSE_ (輸出檔以 PUT_FILE_CREATE_ 重新建立，
//      PUT_PURGE 掉舊內容)。
//   3. Guardian 檔案系統單次傳輸上限 = 57344 byte。每批 record 數
//      = min(57344/280, 57344/233) = 204 -> 讀 204*280 = 57120、
//        寫 204*233 = 47532，皆 <= 57344，且為 record 整數倍不切斷。
//   4. 數字欄位 -> 靠右補前導 0 的固定寬度 ASCII；char 欄位直接搬。
//      整數以原生位元組順序讀取 (同機不轉換)。
//   5. 數字位數超過輸出欄寬 -> 視為資料異常，印 FATAL 並 exit(2)。
//
// 編譯 (NonStop, 目標為 Guardian 環境，編譯器會定義 _GUARDIAN_TARGET):
//   用原生 C++ 編譯器編譯，todsys.h 需在 include 路徑上。
// 本機測試 (Linux/OSS，POSIX I/O):
//   g++ -O2 -I<含 todsys.h 的目錄> -o convert convert.cpp
//
// 執行: RUN CONVERT <input> <output>   (Guardian)
//       ./convert <input> <output>     (本機)

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "todsys.h"     // 來源 schema: tlodru_rec_def / stlodr_rec_def

// ===========================================================================
// 平台 I/O 選擇: Guardian 原生 vs POSIX(本機測試用)
// ===========================================================================
#if defined(_GUARDIAN_TARGET) || defined(USE_GUARDIAN)
#  define IO_GUARDIAN 1
   // PUT library 的函式宣告標頭。請改成你站上 PUT 函式庫實際的標頭名稱；
   // 若 PUT_ 函式仍需底層系統程序，可能要一併保留對應的 cextdecs。
#  include <putlib.h>      /* TODO: 換成實際 PUT library 標頭 */
   typedef short io_handle;
#else
#  define IO_GUARDIAN 0
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
   typedef int io_handle;
#endif

typedef tlodru_rec_def InputRecord;    // 280 bytes
typedef stlodr_rec_def OutputRecord;   // 233 bytes

// 編譯期斷言 (相容舊版 C++ 編譯器，不依賴 static_assert):
// sizeof 與 header 宣告的長度不符就會編不過(陣列長度變 -1)。
typedef char assert_in_size_[sizeof(InputRecord)  == tlodru_rec_def_Size ? 1 : -1];
typedef char assert_out_size_[sizeof(OutputRecord) == stlodr_rec_def_Size ? 1 : -1];

// ===========================================================================
// 整數 -> 靠右、補前導 0 的固定寬度 ASCII。
// 值非負(計數/價格/時間)。位數超過欄位寬度視為資料異常 -> 報錯中止。
// ===========================================================================
static void fmt_u(unsigned long long v, char* dst, int width,
                  const char* field, unsigned long long rec) {
    unsigned long long orig = v;
    for (int i = width - 1; i >= 0; --i) { dst[i] = (char)('0' + (int)(v % 10)); v /= 10; }
    if (v != 0) {                       // 位數比欄位寬 -> 溢位，中止
        fprintf(stderr,
                "FATAL: record %llu field %s value %llu exceeds %d-digit width. Aborting.\n",
                rec, field, orig, width);
        exit(2);
    }
}

// ===========================================================================
// 單筆轉換 (欄位對應依 header 名稱)
//   H = 輸入 header 子結構, C = 輸入 content 子結構, O = 輸出 content 子結構
// ===========================================================================
static void convert_record(const InputRecord& in, OutputRecord& out,
                           unsigned long long rec) {
#define H in.odr_header
#define C in.odr_content
#define O out.sto_content

    fmt_u((unsigned long long)C.odr_odr_time,        O.sto_odr_time,        12, "sto_odr_time",        rec);
    fmt_u((unsigned long long)C.odr_stk_me_index,    O.sto_stk_me_index,     6, "sto_stk_me_index",    rec);
    fmt_u((unsigned long long)C.odr_stk_total_index, O.sto_stk_total_index,  8, "sto_stk_total_index", rec);

    memcpy(O.sto_stk_id,        C.odr_stk_id,        sizeof(O.sto_stk_id));
    memcpy(O.sto_brk_id,        C.odr_brk_id,        sizeof(O.sto_brk_id));
    memcpy(O.sto_odr_no,        C.odr_odr_no,        sizeof(O.sto_odr_no));
    memcpy(O.sto_investor_no,   C.odr_investor_no,   sizeof(O.sto_investor_no));
    memcpy(O.sto_investor_code, C.odr_investor_code, sizeof(O.sto_investor_code));
    memcpy(O.sto_function_code, C.odr_function_code, sizeof(O.sto_function_code));

    O.sto_buysell_code  = C.odr_buysell_code;
    O.sto_price_type    = C.odr_price_type;
    O.sto_time_in_force = C.odr_time_in_force;

    fmt_u((unsigned long long)C.odr_odr_price,  O.sto_odr_price,  9, "sto_odr_price",  rec);
    fmt_u((unsigned long long)C.odr_old_price,  O.sto_old_price,  9, "sto_old_price",  rec);
    fmt_u((unsigned long long)C.odr_odr_qty,    O.sto_odr_qty,    8, "sto_odr_qty",    rec);
    fmt_u((unsigned long long)C.odr_valid_qty,  O.sto_valid_qty,  8, "sto_valid_qty",  rec);
    fmt_u((unsigned long long)C.odr_bf_qty,     O.sto_bf_qty,     8, "sto_bf_qty",     rec);
    fmt_u((unsigned long long)C.odr_unit_share, O.sto_unit_share, 8, "sto_unit_share", rec);

    O.sto_trade_kind           = C.odr_trade_kind;
    O.sto_margin_code          = C.odr_margin_code;
    O.sto_abnormal_quota_check = C.odr_abnormal_quota_check;
    O.sto_foreign_quota_check  = C.odr_foreign_quota_check;
    O.sto_china_quota_check    = C.odr_china_quota_check;
    O.sto_slb_quota_check      = C.odr_slb_quota_check;

    fmt_u((unsigned long long)C.odr_priority_group.odr_priority_period,
          O.sto_priority_group.sto_priority_period,  2, "sto_priority_period", rec);
    fmt_u((unsigned long long)C.odr_priority_group.odr_priority_number,
          O.sto_priority_group.sto_priority_number, 12, "sto_priority_number", rec);

    memcpy(O.sto_link_brk_id, C.odr_link_brk_id, sizeof(O.sto_link_brk_id));
    memcpy(O.sto_link_pvc_id, C.odr_link_pvc_id, sizeof(O.sto_link_pvc_id));
    O.sto_link_type   = C.odr_link_type;
    O.sto_ivacno_flag = C.odr_ivacno_flag;
    memcpy(O.sto_mthprt, C.odr_mthprt, sizeof(O.sto_mthprt));

    fmt_u((unsigned long long)C.odr_fw_id,     O.sto_fw_id,      2, "sto_fw_id",     rec);
    fmt_u((unsigned long long)C.odr_fw_seq_no, O.sto_fw_seq_no, 18, "sto_fw_seq_no", rec);

    memcpy(O.sto_cl_ord_id, C.odr_cl_ord_id, sizeof(O.sto_cl_ord_id));

    fmt_u((unsigned long long)C.odr_session_id,       O.sto_session_id,       6, "sto_session_id",       rec);
    fmt_u((unsigned long long)C.odr_trans_price,      O.sto_trans_price,      9, "sto_trans_price",      rec);
    fmt_u((unsigned long long)C.odr_stable_ref_price, O.sto_stable_ref_price, 9, "sto_stable_ref_price", rec);

    O.sto_data_type = H.odr_data_type;     // 來自 odr_header

    fmt_u((unsigned long long)C.odr_me_write_time,  O.sto_me_write_time,  12, "sto_me_write_time",  rec);
    fmt_u((unsigned long long)C.odr_me_accept_time, O.sto_me_accept_time, 12, "sto_me_accept_time", rec);
    fmt_u((unsigned long long)C.odr_me_reply_time,  O.sto_me_reply_time,  12, "sto_me_reply_time",  rec);

#undef H
#undef C
#undef O
}

// ===========================================================================
// IO 緩衝 (受 57344 byte/次 限制)
// ===========================================================================
static const size_t IO_MAX  = 57344;
static const size_t IN_REC  = sizeof(InputRecord);   // 280
static const size_t OUT_REC = sizeof(OutputRecord);  // 233

static const size_t BATCH =
    (IO_MAX / IN_REC) < (IO_MAX / OUT_REC) ? (IO_MAX / IN_REC) : (IO_MAX / OUT_REC);
static const size_t READ_BUF_SIZE  = BATCH * IN_REC;   // 57120 <= 57344
static const size_t WRITE_BUF_SIZE = BATCH * OUT_REC;  // 47532 <= 57344

// ===========================================================================
// 平台 I/O 包裝: 上層邏輯不分平台共用
// ===========================================================================
#if IO_GUARDIAN

// ---- Guardian PUT library ----
// 註: 以下假設 PUT_ 函式與對應的 Guardian 系統程序「簽章相同、只是加 PUT_ 前綴」
//     (參數順序、回傳 error 慣例、EOF=1 等)。請對照 PUT library 手冊確認；
//     若 PUT 版本的參數或回傳慣例不同，需依手冊調整。
static io_handle io_open_read(const char* name) {
    short fnum;
    short err = PUT_FILE_OPEN_((char*)name, (short)strlen(name), &fnum,
                               /*access   */ 1,    // 1 = read-only
                               /*exclusion*/ 0);   // 0 = shared
    if (err) { fprintf(stderr, "PUT_FILE_OPEN_ input '%s' error %d\n", name, err); exit(1); }
    return fnum;
}

static io_handle io_open_write(const char* name) {
    short fnum;
    short len = (short)strlen(name);
    // 確保輸出為全新檔: 建立; 若已存在(10)就 PURGE 後重建，避免殘留舊資料。
    short cerr = PUT_FILE_CREATE_((char*)name, len);   // 省略選用參數 -> 預設 unstructured
    if (cerr == 10) {                                  // 10 = file already exists
        short perr = PUT_PURGE((char*)name, len);
        if (perr) { fprintf(stderr, "PUT_PURGE '%s' error %d\n", name, perr); exit(1); }
        cerr = PUT_FILE_CREATE_((char*)name, len);
    }
    if (cerr) { fprintf(stderr, "PUT_FILE_CREATE_ '%s' error %d\n", name, cerr); exit(1); }

    short err = PUT_FILE_OPEN_((char*)name, len, &fnum,
                               /*access   */ 2,    // 2 = write-only
                               /*exclusion*/ 1);   // 1 = exclusive
    if (err) { fprintf(stderr, "PUT_FILE_OPEN_ output '%s' error %d\n", name, err); exit(1); }
    return fnum;
}

// 讀滿 want byte。回傳實際讀到的 byte (EOF 時 < want)。want <= 57344。
static size_t io_read_block(io_handle fnum, char* buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        unsigned short xfer = 0;
        short err = PUT_READX(fnum, buf + got, (unsigned short)(want - got), &xfer);
        if (err == 1) break;            // 1 = EOF
        if (err) { fprintf(stderr, "PUT_READX error %d\n", err); exit(1); }
        if (xfer == 0) break;
        got += xfer;
    }
    return got;
}

// 寫滿 len byte。len <= 57344。
static void io_write_block(io_handle fnum, const char* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        unsigned short xfer = 0;
        short err = PUT_WRITEX(fnum, (char*)(buf + done), (unsigned short)(len - done), &xfer);
        if (err) { fprintf(stderr, "PUT_WRITEX error %d\n", err); exit(1); }
        if (xfer == 0) { fprintf(stderr, "PUT_WRITEX wrote 0 bytes\n"); exit(1); }
        done += xfer;
    }
}

static void io_close(io_handle fnum, const char* what) {
    short err = PUT_FILE_CLOSE_(fnum);
    if (err) fprintf(stderr, "PUT_FILE_CLOSE_ %s error %d\n", what, err);
}

#else  // ---- POSIX (本機測試) ----

static io_handle io_open_read(const char* name) {
    int fd = open(name, O_RDONLY);
    if (fd < 0) { perror("open input"); exit(1); }
    return fd;
}
static io_handle io_open_write(const char* name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open output"); exit(1); }
    return fd;
}
static size_t io_read_block(io_handle fd, char* buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r < 0) { if (errno == EINTR) continue; perror("read"); exit(1); }
        if (r == 0) break;              // EOF
        got += (size_t)r;
    }
    return got;
}
static void io_write_block(io_handle fd, const char* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); exit(1); }
        done += (size_t)w;
    }
}
static void io_close(io_handle fd, const char* what) {
    if (close(fd) < 0) perror(what);
}

#endif

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }

    io_handle in_h  = io_open_read(argv[1]);
    io_handle out_h = io_open_write(argv[2]);

    char* read_buf  = (char*)malloc(READ_BUF_SIZE);
    char* write_buf = (char*)malloc(WRITE_BUF_SIZE);
    if (!read_buf || !write_buf) { fprintf(stderr, "malloc failed\n"); return 1; }

    unsigned long long total = 0;

    for (;;) {
        size_t got = io_read_block(in_h, read_buf, READ_BUF_SIZE);
        if (got == 0) break;                       // 正常結束

        if (got % IN_REC != 0) {                   // 檔尾不完整 record
            fprintf(stderr, "Error: input size not a multiple of %lu (trailing %lu bytes)\n",
                    (unsigned long)IN_REC, (unsigned long)(got % IN_REC));
            return 1;
        }

        size_t n = got / IN_REC;
        for (size_t i = 0; i < n; ++i) {
            convert_record(*(const InputRecord*)(read_buf  + i * IN_REC),
                           *(OutputRecord*)     (write_buf + i * OUT_REC),
                           total + i + 1);
        }
        io_write_block(out_h, write_buf, n * OUT_REC);
        total += n;
    }

    free(read_buf);
    free(write_buf);
    io_close(in_h,  "input");
    io_close(out_h, "output");

    fprintf(stderr, "Done. %llu records. BATCH=%lu read_buf=%lu write_buf=%lu\n",
            total, (unsigned long)BATCH, (unsigned long)READ_BUF_SIZE,
            (unsigned long)WRITE_BUF_SIZE);
    return 0;
}
