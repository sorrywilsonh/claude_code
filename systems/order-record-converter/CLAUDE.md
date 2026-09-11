# order-record-converter

## 這是什麼

把 NonStop Guardian 環境下的固定長度二進位委託(order)記錄,轉換成固定長度的全 ASCII 文字記錄。

- 輸入 record:`tlodru_rec_def`(280 bytes),schema 定義於外部的 `todsys.h`(本 repo 未包含這份標頭,需在編譯時自行提供)
- 輸出 record:`stlodr_rec_def`(233 bytes),同樣定義於 `todsys.h`
- 欄位語意對應證券委託單:股票代號、券商代號、委託人、買賣別、價格、數量等,詳細對應見 `src/convert.cpp` 的 `convert_record()`

## 執行環境

- 正式環境:NonStop Guardian,檔案 I/O 走 Guardian PUT library(`PUT_FILE_OPEN_` / `PUT_READX` / `PUT_WRITEX` / `PUT_FILE_CLOSE_`)
- 本機測試:Linux / POSIX I/O(`open` / `read` / `write`)

編譯(本機測試):
```
g++ -O2 -I<todsys.h 所在目錄> -o convert src/convert.cpp
```

執行:
```
RUN CONVERT <input> <output>   # Guardian
./convert <input> <output>     # 本機
```

## 修改規範

- Guardian 單次傳輸上限 57344 bytes 是硬性限制;調整批次大小(`BATCH`)時務必重新驗證 `READ_BUF_SIZE` / `WRITE_BUF_SIZE` 仍 `<= 57344`,且為 record 長度的整數倍
- 數字欄位一律靠右補零、固定寬度;超過欄寬視為資料異常,要 FATAL 並 `exit(2)`,不要靜默截斷或忽略
- 改動欄位對應(`convert_record`)前,先確認 `todsys.h` 的 schema 定義沒有跟著變,並重新核對編譯期的 size 斷言

## 待補

- `docs/` 尚未放入委託記錄的完整欄位說明或流程圖
- 若之後有其他轉換方向(例如反向:ASCII → binary),另開子資料夾,不要塞進同一支程式
