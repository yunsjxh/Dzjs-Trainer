#pragma once
#include "stdafx.h"

// Driver guard (anti-malicious-driver fallback):
// 后台线程枚举可信驱动文件，复用 DriverSignaturePolicy 的发布者白名单验证
// 签名，把 (文件名, SHA-256) 白名单下发给内核驱动并启用。启用后内核在
// 镜像加载回调里拦截不在白名单的内核驱动，把其 DriverEntry 改写为
// 返回 STATUS_ACCESS_DENIED，使加载失败。
BOOL XStartDriverGuard();
VOID XStopDriverGuard();
