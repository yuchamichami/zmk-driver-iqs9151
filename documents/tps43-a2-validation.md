# A2ソフトウェア検証

実施日: 2026-09-24

| 検証 | 結果 |
|---|---|
| A2プロファイル / QEMU Cortex-M3 | 52/52 PASS |
| 従来プロファイル / QEMU Cortex-M3 | 52/52 PASS |
| XIAO BLE nRF52840 / reset-gpiosなし | ビルド・リンク成功 |
| XIAO BLE nRF52840 / reset-gpiosあり | ビルド・リンク成功 |
| Corcell右側構成のコピー + A2 + bringup設定 | ビルド・リンク成功 |

各プロファイルで既存ジェスチャー42件と設定・通信10件を実行。
追加検証は、電極・ALP・チャンネルマスク、材質調整値の反映、ATI要求のレジスタとバイト順、
RDYの論理、タイムアウト、GPIO/I2Cエラー伝播、リセット後の設定復元、ATIエラー検出、
RST未配線でホストだけ再起動する場合の強制ソフトリセット。

使用ソース:
- upstream driver: `08a6fd19c5aa5ae7f11daf371b5a391cd8596783`
- ZMK（ローカルCorcell開発環境）: `e5c9b6915b56801193e359dd9bad4a167ce0d1b8`
- Zephyr 4.1: `7c6b4cc486ecb41a68d9c2b1def2bb3178fbb826`
- Zephyr SDK: 0.16.9 / ARM GCC

再現用テストは `tests/iqs9151_work_cb` と `tests/build`。
手順は [導入手順](tps43-a2.md#検証の再現) を参照。

Corcellの確認は、既存ローカル構成のコピーへ検証用DTSを追加したコンパイル確認です。
Corcell本体リポジトリの編集・push・現物のピン対応確認・実機書込みは行っていません。
このコンパイル用UF2を完成版として配布しません。

未検証: 現物I2Cのタイミング、ATI/感度、1mmオーバーレイの材質差、FFC接点の向き、
センサーの実電源復帰、キーボードの省電力連携、消費電流、ノイズ耐性。
