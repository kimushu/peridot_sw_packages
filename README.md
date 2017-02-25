# peridot\_sw\_packages

このリポジトリは、PERIDOT および PERIDOT NewGen で利用されるソフトウェアパッケージを集約したものです。

## 対象システム

PERIDOT向けのNiosIIベースソフトウェア環境

## 使い方

- Quartusプロジェクトフォルダ直下の"ip"フォルダ内に、このリポジトリをクローンする。
- NiosII EDS 上で BSP Editor を開き、Software Packages タブの一覧から必要なパッケージにチェックをつける
- 追加したパッケージに対して適宜設定を編集したのち、Generate BSP を実行する。

## peridot\_rpc\_server

PERIDOT内のNiosIIシステム上の関数を、USB接続したホストPCから呼び出すためのサーバーです。
[Canarium](https://github.com/kimushu/canarium)のバージョン 1.0.x 以降に搭載される、
RPCクライアント機能の対抗側にあたります。

システム内に [peridot\_swi](https://github.com/kimushu/peridot_gen1_peripherals) または [peridot\_hostbridge](https://github.com/kimushu/peridot_peripherals) が必要です。

## peridot\_client\_fs

PERIDOT内のNiosIIシステム上でのファイルシステムを、USB接続したホストPCから操作するためのサーバーです。
[Canarium](https://github.com/kimushu/canarium)のバージョン 1.0.x 以降に搭載される、
リモートファイル操作機能のターゲットとなります。

ホスト側の通信にRPCを利用しているため、peridot\_rpc\_server が必要です。

## named\_fifo

NiosII HALシステム上に、名前付きFIFOファイル作成機能を追加するパッケージです。
作成されたFIFOは、通常のファイルと同様に open/read/write/close 関数でアクセスできます。

標準入出力をこの名前付きFIFOで置き換えることもできます。
peridot\_client\_fs と組み合わせることで、UARTなど別の通信経路を使わずにホストPCと標準入出力をやりとりできます。

※このパッケージ単体は、PERIDOT固有のIPに依存しません。すべてのNiosII プロジェクトに適用可能です。

## ライセンス

The MIT License (MIT)

