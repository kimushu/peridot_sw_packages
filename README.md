# peridot\_sw\_packages

このリポジトリは、PERIDOT および PERIDOT NewGen で利用されるソフトウェアパッケージを集約したものです。

## 対象システム

PERIDOT向けのNiosIIベースソフトウェア環境

## 使い方

- Quartusプロジェクトフォルダ直下の"ip"フォルダ内に、このリポジトリをクローンします。
- NiosII EDS 上で BSP Editor を開き、Software Packages タブの一覧から必要なパッケージにチェックをつけます
- 追加したパッケージに対して適宜設定を編集したのち、Generate BSP を実行します。
- 後は通常通り、プロジェクトをビルドしてください。ビルドが失敗する場合、エラーメッセージやパッケージ間の依存関係が満たされているかを再確認してください。

## <a id="peridot_sw_hostbridge_gen2"></a>peridot\_sw\_hostbridge\_gen2

[Canarium](https://github.com/kimushu/canarium) 通信仕様のGen2に準拠したプロトコルをソフトウェアで実装したライブラリです。このライブラリに通信層(UARTなど)を連結させてNiosII上で起動することで、[CanariumGen2](http://kimushu.github.io/canarium/gen2/classes/canariumgen2.html) の通信相手として機能するようになります。

組み合わせできる通信層は、HAL上で8-bitキャラクタデバイスとしてドライバが構成されるIPです。(altera\_avalon\_uart や [buffered_uart](https://github.com/kimushu/buffered_uart) など)

## <a id="peridot_rpc_server"></a>peridot\_rpc\_server

PERIDOT内のNiosIIシステム上の関数を、USB接続したホストPCから呼び出すためのサーバーです。
[Canarium](https://github.com/kimushu/canarium)のバージョン 1.1.x 以降に搭載されている、
RPCクライアント機能の対抗側にあたります。

システム内に [peridot_sw_hostbridge_gen2](#peridot_sw_hostbridge_gen2) が必要です。

## peridot\_client\_fs

PERIDOT内のNiosIIシステム上でのファイルシステムを、USB接続したホストPCから操作するためのサーバーです。
[Canarium](https://github.com/kimushu/canarium)のバージョン 1.0.x 以降に搭載される、
リモートファイル操作機能のターゲットとなります。

ホスト側の通信にRPCを利用しているため、 [peridot\_rpc\_server](#peridot_rpc_server) が必要です。また、CRC32やMD5のハッシュ計算機能を有効にする場合は、[digests](#digests) パッケージで該当機能を有効にする必要があります。

## <a id="digests"></a>digests

CRC32(RFC2083と同等)やMD5のダイジェスト値計算機能を提供するパッケージです。

※このパッケージ単体は、PERIDOT固有のIPに依存しません。すべてのNiosII プロジェクトに適用可能です。

## named\_fifo

NiosII HALシステム上に、名前付きFIFOファイル作成機能を追加するパッケージです。
作成されたFIFOは、通常のファイルと同様に open/read/write/close 関数でアクセスできます。

標準入出力をこの名前付きFIFOで置き換えることもできます。
peridot\_client\_fs と組み合わせることで、UARTなど別の通信経路を使わずにホストPCと標準入出力をやりとりできます。

※このパッケージ単体は、PERIDOT固有のIPに依存しません。すべてのNiosII プロジェクトに適用可能です。

## rubic\_agent

VSCode拡張機能の [Rubic](https://marketplace.visualstudio.com/items?itemName=kimushu.rubic) のターゲットボードに必要な機能を提供するサーバーです。

ホスト側の通信にRPCを利用しているため、システム内に [peridot\_rpc\_server](#peridot_rpc_server) が必要です。

## ライセンス

The MIT License (MIT)

