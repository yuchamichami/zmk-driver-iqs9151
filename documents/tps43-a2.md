# IQS9151 TPS43-outline A2

Corcellへ組み込むための基板固有ドライバー。ShiniNet/zmk-driver-iqs9151 の
`08a6fd19c5aa5ae7f11daf371b5a391cd8596783`（MIT）を基にしています。
元の著作権表記・ライセンスを維持しています。

## 対象と状態

- 独自A2基板: 43.30×40.30 mm、FR4 4層、厚さ1.0 mm、IQS9151-000QFR。
- オーバーレイ: **1.0 mm、材質未固定**。PLA/MJF造形品などを比較する初期段階。
- 電極: 13 Rx × 12 Tx、156交点、3 mmピッチ。
- 初期座標: X=3600 / Y=3300。DPIや実測感度ではありません。
- Azoteq純正TPS43向けドライバーではありません。
- ビルド・ソフトウェアテストと実機確認は別です。センサー実機での感度・電流・ノイズ検証は未実施。

## A2を有効にする

既存west manifestの同名プロジェクトをこのフォークに差し替えてください。
`examples/tps43-a2/west.yml` に例があります。導入後は確認したコミットSHAに固定することを推奨します。

キーボードのconfへ `examples/tps43-a2/overlay-1mm.conf` の設定を追加します。
初回はさらに `bringup.conf` を適用します。通常のジェスチャーや慣性は、通信・軸・座標が確認できた後で個別に有効化してください。
A2の選択はビルド全体で共通です。同一ファームに異なる形状のIQS9151を混在させる構成には対応していません。

## 配線

| FPCピン | 信号 | 備考 |
|---|---|---|
| 1 | RESET_EXT / MCLR | 任意、未接続可 |
| 2 | SCL | I2C |
| 3 | RDY | ACTIVE_LOW |
| 4 | SDA | I2C |
| 5 | 3V3 | 電源 |
| 6 | GND | 電源 |

SCL/SDA/RDYは基板側4.7kΩプルアップ。I2Cアドレスは7ビット表記 `0x56`。
RST未接続ならDTSの `reset-gpios` を**省略**します。基板のR5は実装したままで使用します。
GPIOへRSTを配線する場合だけ `reset-gpios = <&gpioX N GPIO_ACTIVE_LOW>;` を追加します。
旧PAWホストで1番がGND固定の場合は、GND固定を解除するかR5を除去してください。

`xiao-standalone.overlay` は単体XIAOの参考例です。CorcellのGPIO割り当てではありません。
Corcell側では既存SPIデバイス・同一周辺回路・キー走査との競合、FFC両端のピン対応を確認してください。
特に既存の別モジュール用overlayをピン番号だけ見て流用せず、信号名まで照合してください。
分割キーボードの接続側と入力転送はCorcell側で設定します。

## 電極設定と材質調整の分離

`drivers/input/profiles/tps43_a2.h` に基板固有の情報を保存しています。

- Rxチャンネル: `12,11,10,9,8,7,6,5,4,3,2,1,0`
- Txチャンネル: `45,43,42,41,40,39,38,37,36,35,34,33`
- これはチャンネル値で、ICの物理ピン番号ではありません。Tx44は使用しません。
- 全交点有効。ALPのRx0..12およびTx33..43/45も対応させています。
- 初期化ヘッダーの後に自動で適用されます。外部geometry.hの手動includeは不要です。

材質を比較するときは、電極設定を変えず、confで以下を記録・変更します。

| 項目 | 初期値 | Kconfig末尾（すべてINPUT_IQS9151_） |
|---|---:|---|
| ATI target | 400 | ATI_TARGETCOUNT |
| タッチ開始倍率 | 30 | TOUCH_SET_THRESHOLD |
| タッチ解除倍率 | 26 | TOUCH_CLEAR_THRESHOLD |
| ALP検出 | 8 | ALP_THRESHOLD |
| ALP auto-prox | 8 | ALP_AUTOPROX_THRESHOLD |
| 動的フィルター下限速度 | 30 | DYNAMIC_FILTER_BOTTOM_SPEED |
| 動的フィルター上限速度 | 511 | DYNAMIC_FILTER_TOP_SPEED |
| 動的フィルター下限beta | 20 | DYNAMIC_FILTER_BOTTOM_BETA |

これらはベース設定を出発点とした**未調整値**で、1mmなら必ず動くという保証ではありません。
clear > set またはフィルター下限 > 上限はビルドエラーにします。
厚さ、材質、接着層、空隙、取り付け方向、USB/電池駆動を記録し、一度に一項目ずつ変更してください。
充放電周波数や詳細なALP/ATI設定まで調整する場合はAzoteq GUIで確認してから
`iqs9151_init.h` の該当値を更新します。A2の電極定義とconfの上書きは引き続き優先されます。

## 初回テスト

1. 電源を切ってFFCの信号対応・3V3/GND・R5条件を確認。オーバーレイを実際の方法で取り付けます。
2. 指を離して起動。ログで `TPS43 A2 13Rx/12Tx`、製品番号 `0x09bc`、`ATI complete`、`Initialization complete` を確認します。
3. `RDY timeout` は配線・電源・GPIO指定、製品番号不一致はアドレス/接続先、`ATI calibration error` は電極/はんだ/オーバーレイと設定を確認します。ログを隠して先へ進めないでください。
4. 1本指で中央→四隅をなぞり、`finger` / `f1x` / `f1y` と相対移動を確認。回転・軸は取り付け状態を見て確定します。
5. 指離れ、無接触での誤検出、再起動、センサー再起動後の設定復元を確認します。
6. その後ジェスチャーを有効化し、USB/電池、キーボードのスリープ復帰も確認します。
7. 通常利用時はログレベルを下げ、実際の消費電流を測定します。

この変更はZMKのPM suspend/resumeコールバックを追加するものではありません。
チップ内部の既存省電力設定を継承しています。キーボードがセンサー電源やI2Cを切る構成では、
Corcell側の電源・復帰設計と追加対応が必要です。

## 通信と初期化の変更

- RDY待ちのタイムアウト/GPIOエラーを呼び出し元へ返し、準備ができない状態でI2Cを続行しません。
- 通常のI2CトランザクションをRDYと同期。設定はSTOPで通信ウィンドウを終了する方式です。
- 起動時の製品ID読み出しとソフトリセットだけは通常I2Cによる強制通信を使います。
  MCUだけ再起動した際、センサーがイベントモードのままでRDYを出さない状態に対応するためです。
  チップ既定および本プロファイルのForce Comms Method=0を前提とし、ホストI2Cのクロックストレッチ対応が必要です。
  外部ツールで別の強制通信方式へ変更した状態は、電源再投入またはRST配線でリセットしてください。
- ATI要求を16ビットレジスタのbit6/5へ書き込み、ATIエラーフラグを確認します。
- `reset-gpios` は任意。存在すれば起動時にパルスを出し、なければソフトリセットのみ。
- 実行中のSHOW_RESETではホストのジェスチャーを解除し、電極・調整値を再設定してATIを要求します。
  復元途中で失敗した場合は入力処理を保留し、次のRDY処理で再試行します。
  RDY自体が止まる電源断等は自動復帰を保証せず、ログ・配線確認と再起動が必要です。
- 割り込み有効化時にRDYが既に有効なら処理を投入します。

## 検証の再現

Zephyr 4.1とARM SDKを用意し、ZEPHYR_BASEを設定します。

```sh
west build -b qemu_cortex_m3 -s tests/iqs9151_work_cb -d build-tests-a2 -- -DIQS9151_TEST_A2=ON
python3 scripts/run-qemu-tests.py build-tests-a2/zephyr/zephyr.elf
west build -b qemu_cortex_m3 -s tests/iqs9151_work_cb -d build-tests-upstream
python3 scripts/run-qemu-tests.py build-tests-upstream/zephyr/zephyr.elf
west build -b xiao_ble/nrf52840 -s tests/build -d build-xiao
west build -b xiao_ble/nrf52840 -s tests/build -d build-xiao-reset -- -DEXTRA_DTC_OVERLAY_FILE=reset.overlay
```

QEMUテストは実際のドライバーの設定配列と通信関数を使用し、I2C/GPIOだけを模擬します。
`tests/build` はコンパイル検証専用で、Corcellに書き込む完成ファームではありません。

## 参照

- [ベース実装](https://github.com/ShiniNet/zmk-driver-iqs9151/tree/08a6fd19c5aa5ae7f11daf371b5a391cd8596783)
- [IQS9150/IQS9151 datasheet](https://www.azoteq.com/images/stories/pdf/IQS9150_IQS9151_datasheet.pdf): v1.1、通信§12、ATI§5.8、System Control付録A.12、ALP付録A.15–16

### Experimental ATI fine divider

`CONFIG_INPUT_IQS9151_TP_FINE_DIVIDER` controls TP register 0x117A bits
13:9 (1–21). The default remains 5 for compatibility. Datasheet v1.1 A.11
recommends values above 6; 5 is not an illegal value. A Corcell comparison
uses target 700 and fine divider 8, changing only the divider relative to
the preceding target-700 test. Other multiplier/coarse fields are preserved.
The driver verifies the readback and reapplies the setting after sensor reset.
This is experimental tuning, not hardware validation; ATI errors remain fatal.

### Base-count calibration survey (diagnostic only)

The Azoteq [User Guide v1.0, pp.20–23](https://www.azoteq.com/images/stories/pdf/IQS9150_IQS9151_User_Guide.pdf) specifies target=0 + TP Re-ATI to measure base counts before choosing ATI parameters. The guide's example target100/ATI250 is not a universal profile. Increasing fine divider raises base counts: Corcell's five floor-limited cells increased about 1.58x when fine changed5→8. Merely increasing fine into the recommended range was not sufficient tuning.

`CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY=y` is an opt-in, A2-only diagnostic. It runs after boot, holds manual Active and disables automatic re-ATI/event mode. It captures all156cells twice at coarse set0, fine20/12/8/6, target0. Target0 ATI errors are measurement results, never calibration success. Candidate ATI targets800/900/1000 atfine6 are tested only above the measured maximum base count plus50. Normal-target ATI errors remain failures; I2C/readback/reset failures abort the survey. No pointer output or persistent settings are produced. Keep the sensor untouched during capture.

Fine6 follows the User Guide's minimum; the earlier datasheet recommends above6. The finite candidate range is an experiment informed by this PCB's measurements, not a generic or shipping profile. Successful ATI alone does not validate sensitivity, overlay, drift, sleep behavior, or manufacturing repeatability.

### Frequency comparison survey

`CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY=y` extends the diagnostic survey (not normal firmware) with fine6 fixed at2.5MHz,1.5MHz,1MHz. Each frequency uses datasheet A.17's FRAC/PERIOD1/PERIOD2 bytes, verified by readback, and remeasures target0 base counts before trying eligible targets800/825/850/875. All candidate error/range checks and reseeding remain; no pointer output. At most15 phases run. User Guide §5.1 recommends slower conversions when incomplete charge transfer is suspected. The comparison does not establish that it is the cause on A2.

A2 first-board v9 has stable base83..705 atfine6, but targets800/900/1000 fail at1/2/4 cells. Every failed cell is at compensation511/767; the same cells pass at another target. Targets825/850/875 probe the gaps between previous100-count steps, while800 provides a controlled frequency comparison. D000 compensation values are read-only and never written.

### Fixed profile with input validation

`CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION=y` is an A2 opt-in input path, mutually exclusive with surveys. It calibrates the configured target directly in Manual Active, explicitly reseeds, and verifies two complete156-cell snapshots before restoring automatic sensing and queuing ALP ATI. The same validation is used after sensor reset. Runtime TP ATI errors release held inputs/inertia and latch input off until reset and successful revalidation. A sticky failed reset does not trigger endless restore attempts.

The first-board survey passed at2.5MHz, fine6,target850: count837–863, reference839–861, compensation48–799. This is one ATI followed by two reads, not two independent cold starts. Existing defaults are preserved; Corcell's experimental cursor artifact sets fine6/target850. Direct cold startup, pointing, sensitivity across the surface, battery power and sleep behavior still require hardware checks.
