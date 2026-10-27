# FPV C5 Lap Timer

🌐 **日本語** | [English](README.en.md)

[ESP-NOW Lap Timer](https://github.com/yanazoo/ESP-NOW-Lap-Timer) をベースにした、**FPV ドローン用ラップ計測装置**です。
機体側のビーコンは不要で、**各パイロットの 5.8GHz VTX 電波そのもの**の RSSI ピークで計測ラインの通過を検出し、ラップタイムを **ELRS バックパック経由でゴーグルの OSD に表示**します。

| 変更点 | ESP-NOW Lap Timer | FPV C5 Lap Timer |
|---|---|---|
| ゲート受信機 | ESP32-WROVER-E（ESP-NOW ビーコンの RSSI） | **XIAO ESP32-C5 + [FPVGate C5MK](https://github.com/yanazoo/FPVGateC5MK)**（5GHz VTX の RSSI、1パイロット約1kHz） |
| 機体側 | XIAO ESP32-C3/C6 ビーコンが必要 | **不要**（VTX の周波数でパイロットを識別） |
| 通過判定 | ゲートノード | **XIAO ESP32-S3**（C5 の生 RSSI からピーク検出） |
| ゴーグル表示 | なし | **[vrxc_elrs](https://github.com/yanazoo/vrxc_elrs) の OSD ロジックを S3 に移植**し、**[elrs-netpack](https://github.com/yanazoo/elrs-netpack)** 経由で送信 |
| Web UI / TTS / SD 記録 | あり | あり（VTX 周波数・C5 スケール・OSD 設定に対応） |

---

## システム構成

```
 パイロットの VTX (5.8GHz)
        │  電波
        ▼
┌──────────────────────┐ UART 921600bps ┌──────────────────────────┐
│ XIAO ESP32-C5        │ ─────────────▶ │ XIAO ESP32-S3（本FW）     │
│ FPVGate C5MK         │ ◀───────────── │ ・ゲート通過検出（ピーク）│
│ 8ch を ~1kHz で測定  │  周波数・ゲイン  │ ・ラップ計算/HS/順位      │
└──────────────────────┘                │ ・Web UI (WiFi AP)        │
                                        │ ・ELRS OSD（vrxc_elrs移植）│
                                        │ ・microSD（CSV記録）      │
                                        └─────┬────────────────────┘
                     WiFi(TCP 8080) または UART │ MSP
                                              ▼
                     ┌───────────────────────────────────────────┐
                     │ elrs-netpack                              │
                     │  XIAO ESP32-S3（TCP→UART）＋ ESP32 Wrover   │
                     │  （UART→ESP-NOW）                          │
                     └──────────────┬────────────────────────────┘
                                    │ ESP-NOW (2.4GHz ch1)
                                    ▼
                     各パイロットのゴーグル（ELRS バックパック）
                     → ARM NOW / GO! / LAP・順位 / ラップタイム / BEST / FINISHED!
```

スマホ・PC は S3 の WiFi AP（既定 `FPV-C5-LT`）に接続し、`http://20.0.0.1` を開きます。

### WiFi の動作モード（設定 → WiFi）

| モード | 動作 | 開く URL |
|---|---|---|
| **ホスト**（初期設定・未設定時） | タイマー自身がアクセスポイントになる | `http://20.0.0.1` |
| **クライアント** | 既存の WiFi ルーターに接続する | `http://fpv-c5-lt.local` またはルーターが割り当てた IP |

- クライアントで **60 秒接続できない**とき（起動時、または接続中に切れて 60 秒戻らないとき）は、自動でホスト（アクセスポイント）として起動します。次の再起動で再びクライアント接続を試みます。
- クライアント接続先のパスワードは本体にのみ保存され、Web UI には表示されません（変更しない場合は空欄のまま保存）。
- クライアントモードでは、elrs-netpack も同じルーターに接続させてください（netpack アドレス `auto` で `elrs-netpack.local` を自動検索します）。

---

## 必要なハードウェア

| 部品 | 用途 |
|---|---|
| Seeed Studio **XIAO ESP32-C5** ＋ 5.8GHz 対応アンテナ | ゲート受信機（FPVGate C5MK） |
| Seeed Studio **XIAO ESP32-S3**（SD を使う場合は **Sense**） | タイマー本体（本ファームウェア） |
| **elrs-netpack** 一式（XIAO ESP32-S3 ＋ ESP32 Wrover-E） | ゴーグルへの OSD 送信 |
| 5V 電源（モバイルバッテリー等） | C5 と S3 |
| ELRS バックパック対応ゴーグル（HDZero 等） | OSD 表示 |

### 配線：XIAO ESP32-S3（タイマー）⇔ XIAO ESP32-C5（受信機）

| XIAO ESP32-S3 | 方向 | XIAO ESP32-C5 |
|---|---|---|
| D3 / GPIO4（TX） | → | D4 / GPIO23（RX） |
| D4 / GPIO5（RX） | ← | D5 / GPIO24（TX） |
| 5V（VUSB） | — | 5V（VUSB） |
| GND | — | GND |

> [!IMPORTANT]
> **C5 は必ず 5V から給電してください。** S3 の 3V3 ピンから給電すると、C5 の無線起動時の電流に耐えられず起動しません（FPVGate C5MK の検証結果）。信号線は 3.3V ロジックです。

### microSD（任意）

XIAO ESP32-S3 **Sense** の拡張ボードの SD スロットをそのまま使います（CS=GPIO21, SCK=GPIO7, MISO=GPIO8, MOSI=GPIO9）。通常の XIAO ESP32-S3 の場合は同じピンに SD モジュールを配線してください。ピンは `src/config.h` で変更できます。

### elrs-netpack の接続方法（どちらか）

**A. WiFi 接続（既定・推奨）** — elrs-netpack をそのまま使います。netpack の XIAO を本タイマーの WiFi AP に接続させるだけで、追加配線はありません。

**B. UART 直結** — netpack の XIAO を使わず、ESP-NOW ブリッジ（Wrover）を S3 に直接つなぎます。

| XIAO ESP32-S3（タイマー） | 方向 | ESP32 Wrover-E（elrs-espnow-bridge） |
|---|---|---|
| D6 / GPIO43（TX） | → | GPIO26（RX） |
| D7 / GPIO44（RX） | ← | GPIO27（TX） |
| GND | — | GND |

---

## リポジトリ構成

このリポジトリが S3 のタイマーファームウェア本体です。構成要素の各リポジトリは **git サブモジュール**としてバージョンを固定しています。

```
platformio.ini                XIAO ESP32-S3 タイマーのビルド設定
check_submodules.py           ビルド前にサブモジュールを取得（ZIP ダウンロードにも対応）
src/                          Arduino 側（C5 リンク・WiFi・Web・SD・NVS・OSD 送信）
lib/timer_core/               プラットフォーム非依存のコア（PC でテスト可能）
  race_core.*                 ラップ計算（HS/計測モード・クールダウン・一時停止・順位・完走）
  gate_tracker.*              ゲート通過検出（FPVGate と同じアルゴリズム＋ピーク値）
  osd_engine.*                ELRS OSD（vrxc_elrs の移植）
  msp_v2.*, md5.*             MSP v2 フレーム・バインドフレーズ→UID
data/                         Web UI（LittleFS）
test/                         PC テスト（run.sh）
tools/mock_server.py          Web UI 開発用のモック API サーバー
third_party/                  サブモジュール（変更なしで使用）
  FPVGateC5MK/                C5 ファームウェア（multipilot）と UART ホストライブラリ（host/c5host）
  elrs-netpack/               netpack ファームウェア（XIAO / Wrover）
  vrxc_elrs/                  OSD ロジックの移植元（RotorHazard プラグイン）
.github/workflows/ci.yml      CI（テスト＋S3/C5 ファームウェアのビルド）
```

```bash
git clone --recurse-submodules https://github.com/yanazoo/FPV-C5-Lap-Timer.git
# 既に clone 済みなら
git submodule update --init --recursive
```

サブモジュールを新しいバージョンに上げるとき：

```bash
cd third_party/FPVGateC5MK && git fetch && git checkout <tag or commit> && cd ../..
bash test/run.sh                 # テストが通ることを確認
git add third_party/FPVGateC5MK && git commit -m "Bump FPVGate C5MK to <version>"
```

---

## セットアップ

PlatformIO（VS Code 拡張または CLI）を使います。Windows では PowerShell / cmd から実行してください（Git Bash 不可）。

### 1. XIAO ESP32-C5 に FPVGate C5MK を書き込む

```bash
cd third_party/FPVGateC5MK/multipilot
pio run -e xiaoc5 -t upload --upload-port <C5のポート>
```

[FPVGate のウェブフラッシャー](https://fpvgate.xyz/c5.html#flasher) で「Multi Pilot / XIAO ESP32-C5」を書き込んでも構いません。初回ビルドは ESP-IDF のダウンロードで数分かかります。

### 2. XIAO ESP32-S3 にタイマーを書き込む

```bash
pio run -e xiao_esp32s3 -t upload       # ファームウェア（リポジトリのルートで）
pio run -e xiao_esp32s3 -t uploadfs     # Web UI（data/）
```

CI（GitHub Actions）の成果物にも `firmware.bin` / `littlefs.bin` / C5 用バイナリが含まれます。

> [!NOTE]
> `c5host.h: No such file or directory` になる場合はサブモジュールが未取得です。リポジトリのルートで `git submodule update --init --recursive` を実行してください（ビルド時にも自動で取得します。ZIP でダウンロードした場合も、git がインストールされていれば固定バージョンを自動で取得します）。

### 3. elrs-netpack を準備する

`third_party/elrs-netpack` の README に従い、Wrover に `elrs-espnow-bridge`、netpack の XIAO に `elrs-xiao-webui` を書き込みます。

- **WiFi 接続の場合**：netpack のキャプティブポータル（`ELRS-Netpack-Setup` / `http://192.168.4.1`）で、接続先 WiFi を **本タイマーの SSID とパスワード**（既定 `FPV-C5-LT` / `fpv-c5-lt`）に設定します。タイマーは AP に接続してきた機器から netpack（TCP 8080）を自動で見つけます（設定の「netpack アドレス」= `auto`）。IP や `elrs-netpack.local` を直接指定することもできます。
- **UART 直結の場合**：上の配線表 B のとおり Wrover を S3 につなぎ、設定で接続方式を「UART」にします。

### 4. 初期設定（Web UI）

1. スマホ / PC で WiFi `FPV-C5-LT`（パスワード `fpv-c5-lt`）に接続 → `http://20.0.0.1`
2. 右上の「ログイン」（既定パスワード `admin`）
3. **設定 → WiFi アクセスポイント / 管理者パスワード を必ず変更**してください（公開リポジトリに載っている既定値のため）。変更値は本体の NVS にのみ保存され、リポジトリには含まれません。
4. **設定 → C5 受信機**：状態が `SCAN` になっていることを確認（`OFFLINE` なら配線・電源を確認）
5. **設定 → パイロット情報 → ＋追加**：名前・よみかた・**VTX チャンネル**（例 R1 5658）・**ELRS バインドフレーズ**（空欄ならパイロット名）・OSD 有無
6. 各パイロットを **Ch1〜Ch8** に割り当て（C5 の測定スロット。同じ周波数を2人に割り当てると警告が出ます）
7. **設定 → ELRS OSD**：「有効にする」をオンにして保存 → 「OSDテスト」でゴーグルに `OSD TEST` が出ることを確認

### 5. キャリブレーション

RSSI は C5 のスケール（0〜1023、**16 = 1dB**、ゲイン30でフロア約116）です。

1. **キャリブ**タブで、機体を離れた所に置いた時の値（フロア）と、ゲートを通過した時のピークを確認
2. **Enter** をフロアとピークの間、**Exit** を Enter より少し下に設定（既定 350 / 280）
3. 何周か飛んで、1回の通過が1回だけ数えられることを確認
4. 近距離でピークが頭打ちになる場合は **設定 → 受信ゲイン** を下げ（既定30）、閾値を再調整

---

## 使い方

- **レース**タブで「スタート」→ 3秒カウントダウン（このときゴーグルに **ARM NOW**）→ スタート（**GO!**）
- ゲート通過ごとにラップを記録し、Web UI の効果音・日本語 TTS と、ゴーグルの OSD に表示
- 「ストップ」で一時停止（**LAND NOW!**）、再度「スタート」で再開（停止時間はラップから除外）
- 「総周回数」を設定すると、達成したパイロットのゴーグルに **FINISHED!**、順位、トータルタイム
- 「クリア」で結果を SD に CSV 保存して次のレースへ

### ゴーグル OSD の表示（vrxc_elrs と同じ既定）

| 行 | 内容 | タイミング |
|---|---|---|
| 0 | イベント名（設定時） | ARM NOW 時 |
| 1 | `POSN: 1 \| LAP: 3`（順位とラップ数） | 通過ごと（他パイロットの通過で順位が変わった時も更新） |
| 2 | レースクロック `01:23` | 毎秒 |
| 5 | `ARM NOW` / `GO!` / `LAND NOW!` / `FINISHED!` | ステージ / スタート 0.5秒 / ストップ / 完走 |
| 8 | ラップタイム `0:11.3` | 通過から5秒（0.8秒間隔で2回再送し、取りこぼしに強く） |
| 10–11 | `PLACEMENT: 1` / `TOTAL TIME: 1:02.4` | 完走から10秒 |
| 12 | `BEST: 0:10.9` | 常時 |

ゴーグルは DISPLAY ごとに画面を描き直すため、本ファームウェアは vrxc_elrs と同様に**毎回その宛先の全行を送り直します**。行・列・メッセージ・表示時間は「設定 → ELRS OSD → 詳細」で変更できます。OSD は英数字のみ表示されます（日本語のパイロット名はバインドフレーズの既定値としてのみ使われます）。

---

## 動作の仕組み

- **C5 リンク**（`src/c5_link.cpp`）：FPVGate C5MK のホストライブラリ `c5host`（サブモジュールを無改変で使用）が UART を解析し、C5 のタイムスタンプを S3 の時計に変換します。S3 は専用タスクで UART を一括読み出しし、全サンプル（約1kHz/人）でゲート検出を行います。
- **ゲート検出**（`gate_tracker`）：`f = (3f + 値) / 4` で平滑化し、`f ≥ Enter` で通過開始、`f ≤ Exit` で終了、**通過時刻はピーク時刻**。FPVGate / c5host::GateDetector と同じアルゴリズムで、テストで一致を確認しています。
- **ラップ計算**（`race_core`）：ESP-NOW Lap Timer の HS モード / 計測モード、クールダウン、一時停止・再開、ベストラップ（HS は対象外）、順位、完走判定。
- **ELRS OSD**（`osd_engine` + `src/elrs.cpp`）：vrxc_elrs の各イベント処理を移植。パイロットごとの UID はバインドフレーズの MD5（ExpressLRS と同じ）。MSP v2 フレームを TCP（netpack XIAO）または UART（Wrover）へ送ります。

### PC テスト

```bash
bash test/run.sh
```

- `timer_core` の単体テスト（ラップ計算・OSD の表示と時間制御・MSP）
- C5MK の UART バイト列を模擬 → `c5host` → ゲート検出 → ラップ計算 の一連の流れ（ピーク時刻の精度、c5host::GateDetector との一致）
- **vrxc_elrs 本体の Python コードを読み込み**、バインドフレーズ UID と OSD 送信バイト列が**バイト単位で一致**することを確認

Web UI はモックサーバーで実機なしに確認できます：`python3 tools/mock_server.py` → `http://localhost:8000`

---

## 別構成：RotorHazard で使う場合

FPVGate C5MK は USB 接続で **RotorHazard のノード8個**としても動作します。RotorHazard を使う場合は S3 タイマーの代わりに

1. C5 を RotorHazard の PC / Raspberry Pi に USB 接続（C5MK の `docs/USB_ROTORHAZARD.md`）
2. RotorHazard に **vrxc_elrs** プラグインをインストール（`third_party/vrxc_elrs` の README）
3. vrxc_elrs の接続タイプを SOCKET にして **elrs-netpack** を指定

という構成でも同じ OSD が表示されます。本ファームウェアは、RotorHazard なしで S3 1台で完結させるための構成です。

---

## 実機確認チェックリスト

PC 上のテストと CI ビルドは通っていますが、初回は以下を実機で確認してください。

- [ ] C5 を 5V 給電で起動し、設定画面の C5 状態が `SCAN`、各 Ch のレートが約 1000/s
- [ ] VTX をオンにすると、そのチャンネルの RSSI だけが数百カウント上がる
- [ ] キャリブ後、1回の通過が1ラップとして記録される（クールダウン既定 3秒）
- [ ] netpack の XIAO がタイマーの AP に接続し、設定画面に「ELRS netpack 接続中」と表示される
- [ ] 「OSDテスト」でゴーグルに `OSD TEST` が表示される
- [ ] レースで ARM NOW → GO! → ラップタイム → (総周回数設定時) FINISHED! が表示される
- [ ] SD に `race_NNN.csv` が保存され、SD タブからダウンロードできる

## トラブルシューティング

| 症状 | 確認 |
|---|---|
| C5 状態が `OFFLINE` | TX/RX のクロス、GND 共通、**C5 を 5V 給電** |
| `ERR_FREQ` | パイロットに VTX 周波数が設定され、Ch に割り当てられているか（5180〜5917MHz） |
| 全 Ch がフロアのまま | アンテナ（C5 は外部アンテナ必須）、VTX のチャンネル |
| 通過が二重に数えられる | Exit を下げる / Enter を上げる / クールダウンを延ばす |
| ELRS netpack 未接続 | netpack XIAO の WiFi 設定がタイマーの SSID/パスワードか、netpack アドレスを IP で直接指定 |
| OSD が出ない | パイロットの OSD チェック、バインドフレーズがゴーグル側と一致しているか、Wrover の ESP-NOW チャンネル（既定 ch1） |

## ライセンスについて

- FPVGate C5MK（`third_party/FPVGateC5MK`、`c5host` を含む）：**CC BY-NC-SA 4.0（非商用）**。商用利用には別途ライセンスが必要です。
- vrxc_elrs・elrs-netpack：**GPL-3.0**。`osd_engine` はその移植です。
- 上記を組み合わせたファームウェアは**個人・非商用での利用**を前提としています。配布・販売する場合は各ライセンスを確認してください。
