> **IQS9151 TPS43-outline A2 用フォーク**
> A2基板を使う場合は、先に [A2導入・調整・テスト手順](documents/tps43-a2.md) を読んでください。
> `CONFIG_INPUT_IQS9151_TPS43_A2=y` で13Rx×12Txを選択します。
> 厚さ1mmのオーバーレイを初期条件としていますが、材質別の実機調整は未実施です。
> 以下はベース実装の説明です。純正TPS43ではなく、独自のIQS9151 A2基板が対象です。

﻿# zmk-driver-iqs9151

私が自作したIQS9151トラックパッドモジュールをZMKで使用するための専用ドライバです。  
トラックパッドによるカーソル移動/タップ/スクロール/ピンチインアウトや複数指ジェスチャなどの操作を扱えるようになります。  
また、ZMKからトラックパッドの動作設定を行いやすくする為の拡張機能がいくつか追加されます。

トラックパッドモジュール本体は[Booth（準備中）](https://shininet.booth.pm/)より入手可能です。  

<img width="600"  alt="image" src="https://github.com/user-attachments/assets/76c1e221-bab2-4d7d-9250-408a9b767e39" />

## ドライバの特徴

- IQS9151トラックパッドの入力をZMKへ統合
- 1本指カーソル移動、タップ&タップドラッグ
- 2本指スクロール（縦/横）、タップ&タップドラッグ、ピンチインアウト
- 3本指タップ&タップドラッグ、スワイプ系ジェスチャ（設定に応じて有効化）
- 滑らかな慣性カーソル/スクロール対応
- ZMKのキーマップ連携（レイヤーごとに動作の割り当て可能）
- カーソルやスクロールの速度をリアルタイムに調整可能（電源OFFで設定が消えない）


## クイックスタート

ここでは最小構成での導入手順を示します。  
前提となるZMKの基本構成・ビルド手順は本ドキュメントでは取り扱いません。

### 1. `west.yml` にモジュールを追加

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: ShiniNet                                #<---add this
      url-base: https://github.com/ShiniNet         #<---add this

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: v0.3.0
      import: app/west.yml

    - name: zmk-driver-iqs9151                      #<---add this
      remote: ShiniNet                              #<---add this
      revision: main                                #<---add this

  self:
    path: config
```

### 2. `.conf` に必要設定を追加

```conf
CONFIG_I2C=y
CONFIG_ZMK_POINTING=y
CONFIG_ZMK_POINTING_SMOOTH_SCROLLING=y
CONFIG_INPUT_IQS9151=y
```

必要に応じてトラックパッドの調整やジェスチャーON/OFFや閾値の設定を追加してください（[IQS9151 Driver Kconfig Reference](https://github.com/ShiniNet/zmk-driver-iqs9151/blob/main/documents/iqs9151_kconfig_reference.md)参照）。

### 3. DTS(overlay) にIQS9151ノードを追加（Xiao BLE且つセントラル側の例）

```dts
&xiao_i2c {
    status = "okay";

    iqs9151: iqs9151@56 {
        compatible = "azoteq,iqs9151";
        reg = <0x56>;
        status = "okay";
        irq-gpios = <&gpio1 11 (GPIO_ACTIVE_LOW)>;
    };
};

/ {
    trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&iqs9151>;
        status = "okay";
    };
};
```
必要に応じてトラックパッドの速度調整や仮想キー連携を行う場合は、Behavior / Input Processor を追加してください（[Behavior / Input Processor Reference](https://github.com/ShiniNet/zmk-driver-iqs9151/blob/main/documents/behavior_input_processor_reference.md)参照）。

### 4. 物理配線

- `SDA` -> MCUのI2C SDA（Xiao BLEの場合D4）
- `SCL` -> MCUのI2C SCL（Xiao BLEの場合D5）
- `DR` -> DTSの`irq-gpios`で指定したGPIO入力（今回の例ではGPIO1.11=D6）
- `RST` -> MCUのRESETピン（省略可能）
- `3.3V` -> 3.3V電源
- `GND` -> GND

※SDA/SCL/DRのプルアップ抵抗はトラックパッド側に4.7KΩ実装済みなので不要。  

<img width="1816" height="1334" alt="2026-03-27_16h37_57" src="https://github.com/user-attachments/assets/95b96d59-c3e7-432c-b41d-6f99093fd536" />

> [!WARNING]
> - 以下の画像の例はDTSの`irq-gpios`へGPIO0.02=D0に設定したときの配線例です。
> - （DRに接続した白ワイヤーをXiaoBLEのD0に接続しています。）

<img width="1083" height="600" alt="2026-03-27_15h43_31" src="https://github.com/user-attachments/assets/ba32499c-6387-4ba4-9f93-6c0c47308d7e" />

### 5. トラックパッドの接続と組立て

- FFCにトラックパッドを接続します。
- トラックパッドに厚さ1mm程度の任意のオーバーレイを貼り付けます。（アクリル板、プラ版、PLA、PETGなど。）

> [!WARNING]
> - FFCは青い面が手前側に見える向きに差し込んでください。
> - 付属のブレークアウトボードを使用する場合、FFCは6PINの接点が反転しているタイプを使用してください。
> - トラックパッドにオーバーレイを取り付けない裸の状態で使用すると、センサー飽和を起こして正しく動作しません。

<img width="1144" height="586" alt="2026-03-27_16h09_46" src="https://github.com/user-attachments/assets/63f06b36-9f48-481c-8e7e-c03caf2363a6" />

### 6. 動作確認（デフォルト機能）

- ビルド/書き込み後、1～2本指スワイプによるポインタ移動とスクロールを確認
- 1～3本指タップによる左右中クリック、タップドラッグを確認
- 3本指左右スワイプによるマウスボタン4-5(進む戻る)の出力を確認

※更なる動作をキーマップから設定できるようにするにはコンフィグ及びDTSの設定が必要です。  
  
  
## 応用編（ドキュメント整備中...）

- ZMKキーマップと連携しKeymap EditorやZMK Studioからトラックパッドの動作を変更する
- キー入力によってトラックパッドのカーソル(スクロール)速度を動的に変更する
- スプリットキーボード構成への組み込み。ダブルトラックパッド化

> [!TIP]
> - 具体的な実装例は[ LaLapad Gen2のファームウェアリポジトリ](https://github.com/ShiniNet/zmk-config-LalaPadGen2)を参照してください。
> - トラックパッドの機能や使い方については[LaLapag Gen2の使い方](https://github.com/ShiniNet/LaLaPadGen2/blob/main/guide/HowtoUse.md#%E3%83%88%E3%83%A9%E3%83%83%E3%82%AF%E3%83%91%E3%83%83%E3%83%89%E3%81%AE%E6%A9%9F%E8%83%BD)を参考にしてください。


## 対応環境

- ZMK Firmware `v0.3.0` 以上
- 確認済みMCU/ボード: `Seeed XIAO BLE` / `Seeed XIAO BLE Plus`
- 他のnRF52840系ボードでも動作する可能性はありますが、未検証です（利用は自己責任）。
- 最低でも `SDA/SCL/DR` 用GPIO + `3.3V/GND` が利用可能であること。

## 注意事項

- `TPS65` など同社の汎用トラックパッド等では動作しません。


## 免責事項・その他

- 本ドライバは継続開発中のため、意図しない動作の変更や不具合が発生する可能性があります。
- 意図しない更新を避けたい場合、ユーザー側でフォークしたバージョンを使用するなど対策してください。
- 本ソフトウェアは現状有姿（as is）で提供され、利用・導入・運用は利用者の自己責任です。
- ドライバ自体の不具合やその他の問題を見つけた場合、ISSUEを起こしてください。
