import socket
import struct
import logging
import sys
import time
import select

# --- log 物件（實際的輸出目的地在下方設定區決定後才掛上）---
log = logging.getLogger('mcast_rcv')
log.setLevel(logging.INFO)

# ==========================================================================
# 使用者設定區
# ==========================================================================

# --- 多播群組清單：想收幾組就加幾組 ---
# 每一組都是一個 dict：
#   name    : 自訂名稱，只用來在 log / 檔案裡辨識，方便閱讀
#   grp     : multicast group 位址
#   port    : multicast port
#   enable  : 這次啟動要不要收這一組（True=收、False=跳過）
#   outfile : 這一組的行情要寫到哪個檔案（可省略，省略時用 DEFAULT_OUTPUT_FILE）
#             不同組可以寫不同檔；若幾組填一樣的檔名，就會一起寫進同一個檔。
MCAST_GROUPS = [
    {'name': 'feed_A', 'grp': '233.6.100.100', 'port': 10006, 'enable': True,  'outfile': 'feed_A.bin'},
    {'name': 'feed_B', 'grp': '233.6.100.101', 'port': 10007, 'enable': True,  'outfile': 'feed_B.bin'},
    {'name': 'feed_C', 'grp': '233.6.100.102', 'port': 10008, 'enable': False, 'outfile': 'feed_C.bin'},
    # 想再加就往下貼：
    # {'name': 'feed_D', 'grp': '233.6.100.103', 'port': 10009, 'enable': True, 'outfile': 'feed_D.bin'},
]

IFACE_IP = '0.0.0.0'          # 多網卡時改成要收封包那張網卡的 IP

# --- 接收時間長短（秒）---
# 程式啟動後，從第一步開始計時，收滿這麼多秒就自動停止並關閉。
# 設成 0 或負數代表「不限時，一直收到使用者 Ctrl+C 為止」。
RECV_DURATION_SEC = 60

# --- 預設行情資訊輸出檔 ---
# 當某一組沒有指定自己的 outfile 時，就寫進這個預設檔。
# 注意：這些「行情資料檔」只放接收到的原始封包位元組（binary），
#       一個封包接著一個封包原樣寫入，不混入時間/群組/來源等任何其他資訊，
#       方便直接餵給 convert 之類的工具做後續解析。
DEFAULT_OUTPUT_FILE = 'market_data.bin'

# --- 執行紀錄（log）輸出方式 ---
# 這裡的 log 指「每筆封包一行的訊息」以及程式運作訊息（含時間、群組、來源、長度、hex），
# 和上面各組的「純行情資料檔」是分開的兩回事，彼此不會混入。
#   LOG_TO_SCREEN : 是否把即時訊息刷在畫面（螢幕 / stdout）上。
#   RCV_LOG_FILE  : 執行紀錄要寫到哪個檔；設成 None 或 '' 代表不寫檔。
# 兩者可各自開關。預設「只寫進 rcv_log 檔、不刷畫面」。
LOG_TO_SCREEN = False
RCV_LOG_FILE  = 'rcv_log.txt'

# ==========================================================================


def _configure_logging():
    """依設定把 log 掛到畫面 / 檔案。至少會保留一個輸出，避免訊息全丟失。"""
    fmt = logging.Formatter('%(asctime)s [%(levelname)s] %(message)s',
                            datefmt='%Y-%m-%d %H:%M:%S')
    if RCV_LOG_FILE:
        try:
            fh = logging.FileHandler(RCV_LOG_FILE, encoding='utf-8')
            fh.setFormatter(fmt)
            log.addHandler(fh)
        except OSError as e:
            sys.stderr.write('無法開啟 log 檔 %s：%s，改輸出到畫面\n' % (RCV_LOG_FILE, e))
    # 有要求刷畫面，或者上面沒能掛上任何 handler（例如沒設 log 檔）時，補一個螢幕輸出
    if LOG_TO_SCREEN or not log.handlers:
        sh = logging.StreamHandler()
        sh.setFormatter(fmt)
        log.addHandler(sh)


_configure_logging()


def mcast_in_igmp(group):
    """直接讀 /proc/net/igmp 確認核心是否真的登記了這個群組。"""
    # /proc/net/igmp 內的群組是用 16 進位、小端序表示
    try:
        octets = [int(x) for x in group.split('.')]
        hex_le = ''.join(f'{o:02X}' for o in reversed(octets))  # 小端
        with open('/proc/net/igmp') as f:
            content = f.read().upper()
        return hex_le in content
    except Exception as e:
        log.warning('無法讀取 /proc/net/igmp 驗證：%s', e)
        return None


def setup_socket(cfg):
    """依照單一組設定建立 socket、bind、join 群組並驗證。

    成功回傳已加入群組的 socket，失敗回傳 None（該組略過，不影響其他組）。
    """
    name, grp, port = cfg['name'], cfg['grp'], cfg['port']

    # --- 1. 建立 socket ---
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # 允許多個 socket 綁在同一個 port（不同群組共用 port 時需要）
        if hasattr(socket, 'SO_REUSEPORT'):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        log.info('[%s] Socket 建立成功 (UDP)，已設定 SO_REUSEADDR', name)
    except OSError as e:
        log.error('[%s] Socket 建立失敗：%s', name, e)
        return None

    # --- 2. Bind ---
    try:
        sock.bind(('', port))
        log.info('[%s] Bind 成功：監聽 port %d', name, port)
    except OSError as e:
        log.error('[%s] Bind 失敗（port %d 可能被占用？）：%s', name, port, e)
        sock.close()
        return None

    # --- 3. 加入多播群組（關鍵步驟）---
    try:
        group = socket.inet_aton(grp)
        iface = socket.inet_aton(IFACE_IP)
        mreq  = struct.pack('4s4s', group, iface)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        log.info('[%s] IP_ADD_MEMBERSHIP 呼叫成功 → 已送出 IGMP join：群組 %s，介面 %s',
                 name, grp, IFACE_IP)
    except OSError as e:
        # 常見原因：群組位址非法、介面 IP 不存在、權限不足
        log.error('[%s] 加入群組失敗（join 沒成功！）：%s', name, e)
        sock.close()
        return None

    # --- 4. 主動驗證核心是否真的登記了群組 ---
    verified = mcast_in_igmp(grp)
    if verified is True:
        log.info('[%s] ✅ 驗證成功：/proc/net/igmp 中已出現 %s，join 確實生效', name, grp)
    elif verified is False:
        log.warning('[%s] ⚠️ join 呼叫成功，但 /proc/net/igmp 找不到 %s，'
                    '請確認是否綁到正確網卡（IFACE_IP）', name, grp)
    # verified is None 代表無法讀取驗證，略過

    return sock


def main():
    # --- 依設定挑出這次要收的群組 ---
    enabled = [c for c in MCAST_GROUPS if c.get('enable')]
    if not enabled:
        log.error('沒有任何 enable=True 的群組，沒東西可收，結束。')
        sys.exit(1)

    log.info('本次啟用 %d 組群組：%s', len(enabled),
             ', '.join(f"{c['name']}({c['grp']}:{c['port']})" for c in enabled))

    # --- 依各組的 outfile 開檔；同一個檔名只開一次，讓填相同檔名的群組共用 handle ---
    files_by_path = {}   # 檔名 -> file handle

    def open_outfile(path):
        if path not in files_by_path:
            # 'ab' = 二進位附加、buffering=0 = 不緩衝，收到就直接落地；只寫原始封包位元組
            files_by_path[path] = open(path, 'ab', buffering=0)
        return files_by_path[path]

    # --- 為每一組建立 socket 並開好輸出檔；
    #     記下 fileno -> (socket, 設定, file) 的對照，收到封包時好辨識來源與寫入目標 ---
    sock_by_fd = {}
    for cfg in enabled:
        path = cfg.get('outfile') or DEFAULT_OUTPUT_FILE
        try:
            fh = open_outfile(path)
        except OSError as e:
            log.error('[%s] 無法開啟輸出檔 %s：%s（此組略過）', cfg['name'], path, e)
            continue

        s = setup_socket(cfg)
        if s is not None:
            sock_by_fd[s.fileno()] = (s, cfg, fh)
            log.info('[%s] 行情資訊將寫入檔案：%s', cfg['name'], path)

    if not sock_by_fd:
        log.error('所有群組都建立失敗，結束。')
        for fh in files_by_path.values():
            fh.close()
        sys.exit(1)

    socks = [s for (s, _, _) in sock_by_fd.values()]

    # --- 接收迴圈（限時 or 不限時）---
    unlimited = RECV_DURATION_SEC <= 0
    start = time.monotonic()
    deadline = None if unlimited else start + RECV_DURATION_SEC

    if unlimited:
        log.info('開始接收，未設定時間上限，收到 Ctrl+C 為止 ...')
    else:
        log.info('開始接收，預計收 %d 秒後自動停止 ...', RECV_DURATION_SEC)

    recv_count = 0
    try:
        while True:
            # 計算這一輪 select 還能等多久：不能超過總截止時間
            if unlimited:
                wait = 5.0
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    log.info('已達設定的接收時間 %d 秒，準備停止。', RECV_DURATION_SEC)
                    break
                wait = min(5.0, remaining)

            readable, _, _ = select.select(socks, [], [], wait)

            if not readable:
                # 這段時間沒有任何 socket 有資料
                log.info('... %.1f 秒內沒收到封包（join 已生效，但群組目前無流量？）累計 %d 筆',
                         wait, recv_count)
                continue

            for s in readable:
                _, cfg, fh = sock_by_fd[s.fileno()]
                try:
                    data, address = s.recvfrom(65535)
                except OSError as e:
                    log.warning('[%s] recvfrom 發生錯誤：%s', cfg['name'], e)
                    continue

                recv_count += 1
                # log（rcv_log 檔或畫面）：每筆封包一行的訊息，含群組、來源、長度、hex
                log.info('[%s] 收到第 %d 筆\t%s:%d\tfrom=%s\tlen=%d\t%s',
                         cfg['name'], recv_count, cfg['grp'], cfg['port'],
                         address, len(data), data.hex())
                # 行情資料檔：只寫原始封包位元組，不混入任何其他資訊
                fh.write(data)

    except KeyboardInterrupt:
        log.info('使用者中斷，準備收尾。')

    # --- 收尾 ---
    for fh in files_by_path.values():
        fh.flush()
        fh.close()
    for s in socks:
        s.close()

    elapsed = time.monotonic() - start
    log.info('全部 socket 已關閉，共收到 %d 筆封包（實際接收 %.1f 秒），輸出檔：%s',
             recv_count, elapsed, ', '.join(sorted(files_by_path)))


if __name__ == '__main__':
    main()
