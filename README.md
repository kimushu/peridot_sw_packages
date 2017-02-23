# peridot\_sw\_packages 概要

このリポジトリは、PERIDOT および PERIDOT NewGen で利用されるソフトウェアパッケージを集約したものです。

## 対象システム

PERIDOT向けのNiosIIベースソフトウェア環境

## 使い方

- Quartusプロジェクトフォルダ直下の"ip"フォルダ内に、このリポジトリをクローンする。
- NiosII EDS 上で BSP Editor を開き、Software Packages タブの一覧から必要なパッケージにチェックをつける
- 追加したパッケージに対して適宜設定を編集したのち、Generate BSP を実行する。

## peridot\_rpc\_server

PERIDOT内のNiosIIシステム上の関数を、USB接続したホストPCから呼び出すためのサーバーです。
[Canarium](https://github.com/kimushu/canarium)のバージョン 1.x.x 以降に搭載される、
RPCクライアント機能の対抗側にあたります。

## ライセンス

The MIT License (MIT)

