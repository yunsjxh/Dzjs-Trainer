# -*- coding: utf-8 -*-
"""从 DER 公钥生成 DzjsTrainerUpdater/UpdaterTicketKey.h。

为什么要有这一步
----------------
开源仓库里不能放"能让人冒充正式服务器"的东西。RSA 方案下客户端只持有公钥，
公钥本身泄漏无害（只能验签、不能签发），但把它做成构建期注入仍然有好处：

  · 开源出去的源码里那把公钥不指向任何真实服务器，看代码的人不会误以为
    自己编出来的程序能通过官方渠道更新；
  · 谁想自己搭一套更新通道，换掉 DER 重跑本脚本即可，不用改 C++；
  · 仓库里那把 key（.h）被 .gitignore 挡住，不存在"手滑提交"这条路径。

生成物与被生成物
----------------
    输入  update-server/ticket-sign.pub.der   294 字节 SPKI DER（正式公钥）
    输出  DzjsTrainerUpdater/UpdaterTicketKey.h

开源仓库携带的是 UpdaterTicketKey.h.example（示例公钥，对应私钥在
update-server/ticket-sign.pem.example，同样公开）。UpdaterTicket.h 用
__has_include 做回退：有 .h 用 .h，没有就用 .example，所以克隆下来直接能编。

用法
----
    python tools/make-ticket-key.py                  # 用默认路径生成正式 key
    python tools/make-ticket-key.py --check          # 只检查，不写文件（CI 用）
    python tools/make-ticket-key.py --der a.der --out b.h
    python tools/make-ticket-key.py --pem pub.pem    # 从 PEM 转，需 openssl 在 PATH

输出格式：UTF-8 带 BOM + CRLF。这跟仓库里其它含中文的源文件一致 ——
MSVC 在没有 BOM 时会按本地代码页（GBK）解释源文件，中文注释会把后面的
字节吃错位。
"""

import argparse
import os
import subprocess
import sys
import tempfile

# RSA-2048 的 SubjectPublicKeyInfo 定长 294 字节：8 字节 SPKI 头 +
# 19 字节 AlgorithmIdentifier + 4 字节 BIT STRING 头 + 1 字节前导 0 +
# 256 字节模数 + 5 字节指数。长度不对说明不是 2048 位，直接拒绝，
# 免得生成一个能编过、运行期才炸的头文件。
EXPECTED_DER_LENGTH = 294

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DER = os.path.join(ROOT, 'update-server', 'ticket-sign.pub.der')
DEFAULT_OUT = os.path.join(ROOT, 'DzjsTrainerUpdater', 'UpdaterTicketKey.h')

TEMPLATE = '''// 自动生成，请勿手工编辑。
//
// 由 tools/make-ticket-key.py 从 {source} 生成。
//
// 本文件不进版本库（见 .gitignore）。开源仓库里只有 UpdaterTicketKey.h.example，
// 那是示例公钥，能编译、能让协议跑通，但验不过正式服务器的签名 —— 它的私钥
// 是公开的，任何人都能签，所以签出来的票据只对同样用示例钥编译的客户端有效。
//
// 重新生成：
//     python tools/make-ticket-key.py
//
// {note}
#pragma once

// 0 = 正式公钥，1 = 开源示例公钥。
// 校验入口据它给出"这是占位公钥"这种能看懂的失败原因，而不是笼统的签名不符。
static const int kJupTicketKeyIsPlaceholder = {placeholder};

static const unsigned char kPublicKeyDer[{length}] = {{
{body}
}};
'''

REAL_NOTE = ('这把钥匙对应服务端 update-server/ticket-sign.pem，私钥只在服务器上，\n'
             '// 永不进版本库。换钥必须同时发新版客户端：旧客户端只认编译进去的那把。')
DEMO_NOTE = ('这是示例公钥。对应私钥 update-server/ticket-sign.pem.example 是公开的，\n'
             '// 因此它不具备任何"证明这是官方服务器"的能力，只用于让开源代码能编译和\n'
             '// 自测。正式发行版必须用 tools/make-ticket-key.py 换成真公钥重新编译。')


def display_path(path):
    """仓库内的路径用相对形式，仓库外或跨盘符就原样返回。

    Windows 上 os.path.relpath 遇到跨盘符会直接抛 ValueError，
    所以不能无脑调用。
    """
    try:
        label = os.path.relpath(path, ROOT).replace(os.sep, '/')
    except ValueError:
        return path
    if not label or label.startswith('..'):
        return path
    return label


def read_der(args):
    """按参数取 DER 字节。返回 (bytes, 来源说明)。"""
    if args.pem:
        # 不引第三方库：让 openssl 做 PEM -> DER 的转换，它本来就在签名链上。
        handle, temp_path = tempfile.mkstemp(suffix='.der')
        os.close(handle)
        try:
            result = subprocess.run(
                ['openssl', 'pkey', '-pubin', '-in', args.pem, '-outform', 'DER', '-out', temp_path],
                capture_output=True)
            if result.returncode != 0:
                text = (result.stderr or b'').decode('utf-8', 'replace')
                raise SystemExit('openssl 转换失败：' + text.strip())
            with open(temp_path, 'rb') as stream:
                return stream.read(), os.path.basename(args.pem)
        finally:
            if os.path.exists(temp_path):
                os.remove(temp_path)

    source = args.der or DEFAULT_DER
    # DER 不进版本库（见 update-server/.gitignore），所以开源克隆上来一定没有
    # 这个文件。报错要直接给出"怎么造一个"，否则只看到一个 FileNotFoundError。
    if not os.path.isfile(source):
        raise SystemExit(
            '找不到公钥 DER：%s\n'
            '\n'
            '这个文件不进版本库。从自己的私钥导出：\n'
            '    openssl pkey -in update-server/ticket-sign.pem -pubout -outform DER -out update-server/ticket-sign.pub.der\n'
            '\n'
            '已经有 PEM 形式的公钥时，也可以直接 --pem 指定，跳过这一步。\n'
            '如果只是想让源码编译通过，不需要跑本脚本 —— 仓库里带的\n'
            'UpdaterTicketKey.h.example 会被自动采用。' % source)
    with open(source, 'rb') as stream:
        try:
            label = os.path.relpath(source, ROOT).replace('\\', '/')
        except ValueError:
            # Windows 上跨盘符算不出相对路径，直接抛异常。
            label = ''
        if not label or label.startswith('..'):
            # 仓库外的路径写进注释没有意义，只留文件名。
            label = os.path.basename(source)
        return stream.read(), label


def format_header(der, source, placeholder):
    if len(der) != EXPECTED_DER_LENGTH:
        raise SystemExit(
            '公钥长度是 %d 字节，期望 %d（RSA-2048 SPKI）。\n'
            '不是 2048 位密钥，或者拿成了 PEM / PKCS#1（-----BEGIN RSA PUBLIC KEY-----）格式。'
            % (len(der), EXPECTED_DER_LENGTH))
    if der[0] != 0x30:
        raise SystemExit('首个字节是 0x%02x，不是 0x30（DER SEQUENCE），这不是 SPKI DER。' % der[0])

    lines = []
    for start in range(0, len(der), 12):
        chunk = der[start:start + 12]
        lines.append('		' + ', '.join('0x%02x' % byte for byte in chunk) + ',')
    return TEMPLATE.format(
        source=source,
        note=DEMO_NOTE if placeholder else REAL_NOTE,
        placeholder=1 if placeholder else 0,
        length=len(der),
        body='\n'.join(lines))


def write_text(path, text):
    # UTF-8 BOM 是给 MSVC 的编码提示，不是可选项：没有它，中文注释会按 GBK
    # 解释，末尾字节可能吞掉后面的引号或换行，报出一堆莫名其妙的语法错。
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8-sig', newline='\r\n') as stream:
        stream.write(text)


def main():
    parser = argparse.ArgumentParser(description='从 DER 公钥生成 UpdaterTicketKey.h')
    parser.add_argument('--der', help='SPKI DER 路径，默认 update-server/ticket-sign.pub.der')
    parser.add_argument('--pem', help='改为从 PEM 公钥文件读取（需要 openssl 在 PATH）')
    parser.add_argument('--out', help='输出路径，默认 DzjsTrainerUpdater/UpdaterTicketKey.h')
    parser.add_argument('--placeholder', action='store_true',
                        help='把公钥标记为示例占位符（生成 .h.example 时用）')
    parser.add_argument('--check', action='store_true',
                        help='只比对现有文件是否与 DER 同步，不写入')
    args = parser.parse_args()

    if args.der and args.pem:
        raise SystemExit('--der 和 --pem 只能给一个')

    der, source = read_der(args)
    text = format_header(der, source, args.placeholder)
    target = args.out or (DEFAULT_OUT + '.example' if args.placeholder else DEFAULT_OUT)

    if args.check:
        if not os.path.isfile(target):
            print('缺少 ' + target)
            return 1
        with open(target, 'r', encoding='utf-8-sig') as stream:
            current = stream.read().replace('\r\n', '\n')
        if current.strip() != text.strip():
            print('%s 与 %s 不一致，需要重新生成' % (display_path(target), source))
            return 1
        print('%s 与 %s 一致' % (display_path(target), source))
        return 0

    write_text(target, text)
    print('已生成 %s（%d 字节公钥，%s）' % (
        display_path(target), len(der),
        '示例占位符' if args.placeholder else '正式公钥'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
