# Workspace 總覽

這個 repo 是一個工作用 workspace,用來存放不同資訊系統的說明文件、程式碼、筆記與流程圖,並在同一個 workspace 裡產生文件/簡報、回答系統相關問題、撰寫與修改程式。

## 目前有哪些系統

- `systems/order-record-converter/` — 固定長度二進位委託(order)記錄轉 ASCII 文字記錄的轉換工具(NonStop Guardian 環境)

新增系統時,在這裡補一行,並在 `systems/` 底下建立同樣結構的子資料夾(`CLAUDE.md` / `src/` / `docs/` / `output/`)。

## 共用規範

- 程式撰寫規範:`standards/code-style-guide.md`
- 文件/簡報規範與公司模板:待補充到 `standards/`(目前尚未提供公司模板檔案)

## 系統間關聯

見 `systems/_relationships.md`。

## 一般規則

- 修改任何 `systems/*/src/` 底下的程式碼前,先說明改動內容並取得確認
- 產生的文件或簡報依 `standards/` 裡的規範與模板為準
- 找不到答案時如實說明「文件中未提及」,不要用常識臆測系統實際行為
