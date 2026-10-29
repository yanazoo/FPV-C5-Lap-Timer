# ELRS OSD — UART 接続のセットアップ

FPV C5 Lap Timer（XIAO ESP32-S3）から、elrs-netpack の **ESP-NOW ブリッジ**を UART で直接つなぎ、ゴーグルの ELRS バックパックへラップタイムを表示する手順です。

WiFi 接続（netpack の XIAO を経由する方式）と違い、**netpack の XIAO も WiFi 設定も不要**です。ブリッジには次のどちらかを使います。

| ブリッジ用ボード | PlatformIO 環境 | 備考 |
|---|---|---|
| ESP32 Wrover-E（ESP32 開発ボード） | `esp32dev` | elrs-netpack 標準のボード |
| **Seeed Studio XIAO ESP32-C3** | `xiao_esp32c3` | Wrover-E の代わりに使える小型ボード |

```
XIAO ESP32-S3（タイマー） ──UART 115200bps── ESP-NOW ブリッジ（Wrover-E / XIAO C3）
                                                    │ ESP-NOW（2.4GHz ch 1）
                                                    ▼
                                         ゴーグルの ELRS バックパック
```

---

## 1. ブリッジにファームウェアを書き込む

ブリッジのファームウェアは、このリポジトリのサブモジュール `third_party/elrs-netpack/elrs-espnow-bridge` にあります。リポジトリのルート（`FPV-C5-Lap-Timer`）で実行してください。`COMx` はブリッジのポート番号に置き換えます。

**XIAO ESP32-C3 の場合**

```powershell
pio run -d third_party/elrs-netpack/elrs-espnow-bridge -e xiao_esp32c3 -t upload --upload-port COMx
```

**ESP32 Wrover-E の場合**

```powershell
pio run -d third_party/elrs-netpack/elrs-espnow-bridge -e esp32dev -t upload --upload-port COMx
```

> Windows で `pio` が見つからない場合は `C:\Users\<ユーザー名>\.platformio\penv\Scripts\platformio.exe` を使ってください。
> CI（GitHub Actions）の成果物 `elrs-espnow-bridge` にもビルド済みのバイナリがあります。

書き込み後、ブリッジの USB シリアル（115200bps）に次のように表示されれば正常です。

```
[boot] ESP-NOW bridge (UART RX=GPIO20 TX=GPIO21)     ← XIAO C3 の場合（Wrover-E は GPIO26 / GPIO27）
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

---

## 2. 配線

TX と RX は**交差**させます（タイマーの TX → ブリッジの RX、ブリッジの TX → タイマーの RX）。

**XIAO ESP32-C3 の場合**

| XIAO ESP32-S3（タイマー） | 向き | XIAO ESP32-C3（ブリッジ） |
|---|---|---|
| D6 / GPIO43（TX） | → | D7 / GPIO20（RX） |
| D7 / GPIO44（RX） | ← | D6 / GPIO21（TX） |
| GND | — | GND |

**ESP32 Wrover-E の場合**

| XIAO ESP32-S3（タイマー） | 向き | ESP32 Wrover-E（ブリッジ） |
|---|---|---|
| D6 / GPIO43（TX） | → | GPIO26（RX） |
| D7 / GPIO44（RX） | ← | GPIO27（TX） |
| GND | — | GND |

- **タイマーとブリッジの電源ピン（3V3 / 5V）同士は直接つながない**でください。信号線と一緒につなぐのは GND だけです。ブリッジは USB などで別に給電します。
- タイマーと同じ 5V 電源（モバイルバッテリー等）からまとめて給電する場合は、その電源から各ボードの 5V / GND へ配線します。このとき PC の USB をどちらかにつなぐと逆流するおそれがあるので、書き込み・ログ確認時は外部 5V を外してください。
- タイマー側の D6 / D7 は、C5 受信機（D3 / D4）や microSD（D8〜D10）とは別のピンなので同時に使えます。

---

## 3. タイマーの設定（Web UI → 設定タブ）

1. **パイロット情報**
   - OSD を出したいパイロットの「ELRS」欄に、そのパイロットの **ELRS バインドフレーズ**を入力します（送信機・ゴーグルのバックパックと同じフレーズ）。空欄の場合はパイロット名がバインドフレーズとして使われます。
   - 「OSD」にチェックを入れ、Ch1〜8 のどれかに割り当てます。
2. **ELRS OSD**
   - 「ELRS OSD を有効にする」をオン
   - 接続方式：**「UART（ESP-NOW ブリッジに直結：Wrover / XIAO C3）」**
   - 「保存」
3. **「🥽 OSDテスト」** を押し、ゴーグルに `OSD TEST` / `FPV C5 LAP TIMER` が 5 秒間表示されれば完了です。

レース中は、ARM NOW → GO!（同時にレースクロック）→ 順位・ラップ数／ラップタイム（通過から約 5 秒）／ベストラップ → （総周回数を設定した場合）FINISHED! と表示されます。

> タイマーは毎秒、各パイロットの画面の全行を送り直します。ESP-NOW でパケットが落ちても、その行は直前の表示のまま残ります（画面全体の消去は ARM NOW とクリアのときだけ）。

---

## 4. うまく表示されないとき

ブリッジの USB シリアル（115200bps）のログで、どこまで届いているか確認できます。

| 症状 | 確認すること |
|---|---|
| OSDテストを押してもブリッジのログに何も出ない | タイマー TX（GPIO43）→ ブリッジ RX の配線、GND が共通か、ELRS OSD が「有効」「UART」で保存されているか |
| `[uid] set to ...` は出るがゴーグルに表示されない | バインドフレーズがゴーグル側と完全に一致しているか（大文字・小文字・空白も区別）、ゴーグルのバックパックの電源が入っているか |
| `[espnow] no peer yet, drop MSP` が出る | パイロットの OSD チェック・Ch 割り当てを確認（宛先 UID が設定されていない） |
| `[espnow] send FAILED` が出る | ゴーグルまでの距離、ブリッジのアンテナ（XIAO C3 は付属アンテナを接続）、ESP-NOW チャンネル（ch 1 固定） |
| `[espnow] skipped N packets to unreachable goggles` が出る | そのゴーグルが応答していません（電源オフ・範囲外）。ブリッジはその宛先の残りを飛ばして他のパイロットへの送信を優先し、次の送信で再度試します。使っていないパイロットは OSD のチェックを外してください |
| 表示された行が一瞬消える・ちらつく | タイマー（S3）とブリッジの両方を最新のファームウェアに書き込み直してください（古い版はパケットが 1 つ落ちると行が消えました）。`git pull` の後に `git submodule update --init --recursive` を忘れずに |

> UART 方式は線がつながっていることを確認できないため、設定画面の「ELRS netpack」欄は配線前でも「接続中 UART」と表示されます。実際に届いているかはブリッジのログで確認してください。
