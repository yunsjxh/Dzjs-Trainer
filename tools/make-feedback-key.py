# -*- coding: utf-8 -*-
"""把反馈上行签名用的 RSA 私钥打包成 DzjsTrainerUpdater/FeedbackSignKey.h。

和 make-ticket-key.py 的区别
---------------------------
那个脚本处理的是公钥，泄漏无所谓，所以直接摊成 DER 字节数组。

这个处理的是 **私钥**。它必须编进客户端，否则客户端无法向服务端自证身份；
而编进去的东西迟早会被逆向出来 —— 这一点没有任何密码学手段能改变，因为
验证方（服务端）不在攻击者打不到的地方，密钥必须完整地存在于客户端进程里。

所以这里的目标不是"不可提取"，而是把提取成本从 grep 抬到一个真正逆向的
工程量：

  · 私钥不以 PEM / DER 的原样形态出现，静态扫描找不到 -----BEGIN RSA
    PRIVATE KEY-----，也找不到 PKCS#1 的 0x30 0x82 开头；
  · 用 AES-256-CBC 加密，密钥和 IV 与密文一起放在生成的头文件里 ——
    对称密钥必然和密文同处，看到这里就该知道这是混淆而非保密；
  · 真正的防线在服务端：验签、时间窗、nonce 去重、按 IP 限流。私钥
    泄漏只会让人能伪造"客户端身份"，他伪造不出别人的反馈记录，也不会
    拿到任何下行能力（那是另一把钥匙，永远不出服务器）。

这一点必须写清楚，不然维护者会误以为加密提供了它并不提供的保证。

输入输出
--------
    输入  update-server/feedback-client.der    PKCS#1 RSAPrivateKey DER
    输出  DzjsTrainerUpdater/FeedbackSignKey.h

用法
----
    python tools/make-feedback-key.py
    python tools/make-feedback-key.py --der a.der --out b.h
    python tools/make-feedback-key.py --placeholder      # 生成 .h.example

开源仓库里只带 FeedbackSignKey.h.example —— 它对应一把公开的示例私钥，
签出来的东西只对同样用示例钥配的服务端有效。
"""

import argparse
import os
import subprocess
import sys

# AES-256-CBC 的密钥与 IV。与密文放在同一个头文件里，作用仅是让私钥不
# 以明文形态躺在 exe 里；换值等于换一次生成物，不影响协议。
AES_KEY = bytes([
    0x9e, 0x3b, 0x71, 0x5c, 0xc4, 0x18, 0x2a, 0x8f,
    0x6d, 0xb2, 0x40, 0xd7, 0x15, 0x7a, 0xe9, 0x03,
    0x58, 0x1c, 0xa6, 0x2f, 0xbb, 0x74, 0x0d, 0x92,
    0xc3, 0x4e, 0x87, 0x21, 0x6b, 0xf5, 0x30, 0x9a,
])
AES_IV = bytes([
    0x24, 0xd8, 0x6e, 0x05, 0xb1, 0x3c, 0x9f, 0x47,
    0x70, 0xa2, 0x5d, 0xe8, 0x13, 0x8b, 0xc6, 0x39,
])

# RSA-2048 的 PKCS#1 DER 大约 1190 字节（长度随整数前导零浮动）。给一个
# 宽松区间：太小说明不是 2048 位，太大说明拿成了 PKCS#8 或带了证书链。
MIN_DER = 1100
MAX_DER = 1250

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DER = os.path.join(ROOT, 'update-server', 'feedback-client.der')
DEFAULT_OUT = os.path.join(ROOT, 'DzjsTrainerUpdater', 'FeedbackSignKey.h')

TEMPLATE = '''// 自动生成，请勿手工编辑。
//
// 由 tools/make-feedback-key.py 从 {source} 生成。
//
// 本文件不进版本库（见 .gitignore）。开源仓库里只有 FeedbackSignKey.h.example，
// 它装着一把公开的示例私钥，任何人拿到都能签；正式服务端只信正式公钥，
// 所以示例钥签出来的请求会被拒。它的用途仅限让开源代码能编译、能自测。
//
// 重新生成：
//     python tools/make-feedback-key.py
//
// {note}
#pragma once

// 0 = 正式私钥，1 = 开源示例私钥。运行期据它给出明确原因，而不是笼统的
// "签名无效" —— 拿示例钥签的请求服务端必然拒绝，本机就该先说清楚。
static const int kFeedbackKeyIsPlaceholder = {placeholder};

// AES-256-CBC 的密钥与 IV。与下面的密文同处一个文件是这种方案的固有形态：
// 对称密钥没法藏在对端。它的作用是把"grep 一下就能拿到私钥"变成"要真正
// 逆向"。别把它当成保密措施。
static const unsigned char kFeedbackKeyAesKey[32] = {{
{key}
}};

static const unsigned char kFeedbackKeyIv[16] = {{
{iv}
}};

// PKCS#1 RSAPrivateKey DER（{plain} 字节）的 AES-256-CBC 密文，PKCS#7 填充。
static const unsigned char kFeedbackKeyCipher[{cipher_len}] = {{
{cipher}
}};
'''

REAL_NOTE = ('这把钥匙对应 update-server/feedback-client.pem，私钥只存在于构建机与\n'
             '// 离线备份，永不进版本库。换钥必须同时更新服务端的 feedback-pub.php，\n'
             '// 否则所有反馈都会因验签失败被拒。')
DEMO_NOTE = ('这是示例私钥，对应的公钥在 update-server/feedback-pub.php.example，\n'
             '// 两者都是公开的。它唯一的作用是让克隆下来的源码能编译、能对着自建的\n'
             '// 服务端跑通协议；正式服务端不认这把钥匙，换正式钥重新编译才会被接受。')


def format_bytes(data, indent='\t\t', per_line=12):
    lines = []
    for start in range(0, len(data), per_line):
        chunk = data[start:start + per_line]
        lines.append(indent + ', '.join('0x%02x' % byte for byte in chunk) + ',')
    return '\n'.join(lines)


def encrypt(der):
    """调 openssl 做 AES-256-CBC。不引第三方库，openssl 本来就在签名链上。"""
    key_hex = AES_KEY.hex()
    iv_hex = AES_IV.hex()
    result = subprocess.run(
        ['openssl', 'enc', '-aes-256-cbc', '-K', key_hex, '-iv', iv_hex],
        input=der, capture_output=True)
    if result.returncode != 0:
        raise SystemExit('openssl enc 失败：' + (result.stderr or b'').decode('utf-8', 'replace'))
    return result.stdout


def read_der(path):
    if not os.path.isfile(path):
        raise SystemExit(
            '找不到私钥 DER：%s\n'
            '\n'
            '这个文件不进版本库。生成一对新的：\n'
            '    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out update-server/feedback-client.pem\n'
            '    openssl rsa -in update-server/feedback-client.pem -traditional -outform DER \\\n'
            '        -out update-server/feedback-client.der\n'
            '    openssl pkey -in update-server/feedback-client.pem -pubout \\\n'
            '        -out update-server/feedback-client.pub.pem\n'
            '\n'
            '换钥之后服务端的 feedback-pub.php 也要换成对应的公钥，否则全部拒收。\n'
            '只想让源码编过的话不用跑本脚本 —— 仓库里的 FeedbackSignKey.h.example\n'
            '会被自动采用。' % path)
    with open(path, 'rb') as stream:
        der = stream.read()
    if not (MIN_DER <= len(der) <= MAX_DER):
        raise SystemExit(
            '私钥 DER 是 %d 字节，不在 %d~%d 区间。\n'
            '是不是 2048 位密钥，或者拿成了 PKCS#8（-----BEGIN PRIVATE KEY-----）？\n'
            '本脚本要的是 PKCS#1：openssl rsa -traditional -outform DER。'
            % (len(der), MIN_DER, MAX_DER))
    # PKCS#1 RSAPrivateKey 就是裸 SEQUENCE，开头必须是 0x30 且第二位是长整型标记。
    if der[0] != 0x30 or der[1] not in (0x82, 0x83):
        raise SystemExit('首字节是 0x%02x 0x%02x，不像 PKCS#1 DER SEQUENCE。' % (der[0], der[1]))
    return der


def display_path(path):
    # 仓库内的路径写相对形式；仓库外只留文件名 —— 注释里出现构建机的绝对路径
    # 没有意义，也不该被提交。
    try:
        label = os.path.relpath(path, ROOT).replace(os.sep, '/')
    except ValueError:
        # Windows 上跨盘符算不出相对路径，直接抛 ValueError。
        return os.path.basename(path)
    if not label or label.startswith('..'):
        return os.path.basename(path)
    return label


def write_text(path, text):
    # UTF-8 BOM 给 MSVC 当编码提示，与仓库其它含中文的源文件一致。
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8-sig', newline='\r\n') as stream:
        stream.write(text)


def main():
    parser = argparse.ArgumentParser(description='从 PKCS#1 私钥 DER 生成 FeedbackSignKey.h')
    parser.add_argument('--der', help='私钥 DER 路径，默认 update-server/feedback-client.der')
    parser.add_argument('--out', help='输出路径，默认 DzjsTrainerUpdater/FeedbackSignKey.h')
    parser.add_argument('--placeholder', action='store_true',
                        help='标记为示例私钥（生成 .h.example 时用）')
    args = parser.parse_args()

    der = read_der(args.der or DEFAULT_DER)
    cipher = encrypt(der)
    # 示例头文件不带来源路径：那条注释会写进仓库，让人以为是某个私有文件
    # 派生的，实际它就是一把随仓库发布的钥匙。
    source = '示例私钥（仓库内公开）' if args.placeholder else display_path(args.der or DEFAULT_DER)
    text = TEMPLATE.format(
        source=source,
        note=DEMO_NOTE if args.placeholder else REAL_NOTE,
        placeholder=1 if args.placeholder else 0,
        key=format_bytes(AES_KEY),
        iv=format_bytes(AES_IV),
        cipher_len=len(cipher),
        cipher=format_bytes(cipher),
        plain=len(der))
    target = args.out or (DEFAULT_OUT + '.example' if args.placeholder else DEFAULT_OUT)
    write_text(target, text)
    print('已生成 %s（私钥 %d 字节，密文 %d 字节，%s）' % (
        display_path(target), len(der), len(cipher),
        '示例占位' if args.placeholder else '正式私钥'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
