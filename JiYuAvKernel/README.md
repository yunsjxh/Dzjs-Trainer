# JiYu AV Kernel

这是一个独立的 x64 WDM 防病毒内核原型，不依赖 `JiYuTrainerDriver`。

## 已实现

- SHA-256 精确样本特征，最多 256 条。
- 4 到 64 字节的掩码模式特征，支持 `??` 单字节通配以及 `A?`、`?F` 半字节通配。
- 特征按 ID 更新、删除和枚举；低于 4 个固定字节等价信息量的模式会被拒绝。
- 内核流式读取文件，64 KiB 分块扫描并处理跨块特征。
- 使用 Windows CNG 在内核计算 SHA-256。
- `ObRegisterCallbacks` 保护一个用户态主进程及其线程句柄。
- `CmRegisterCallbackEx` 保护 `JiYuAvKernel` 服务配置。
- 控制设备只允许 `SYSTEM` 和本机管理员访问。
- 驱动默认没有卸载入口；控制端注册后执行 `unload` 才开放卸载并删除服务。
- 扫描次数、命中数和扫描字节数统计。

## 构建

```powershell
tools\Build-JiYuAv.ps1
```

脚本会构建驱动、控制端并运行共享匹配核心的测试，输出位于 `JiYuAvRelease`。驱动交付前仍需使用目标机器认可的内核代码签名证书签名，
并为目录重新生成、签名 catalog。

## 使用

以下命令需要管理员终端：

```powershell
JiYuAvRelease\JiYuAvCtl.exe install JiYuAvRelease\JiYuAvKernel.sys
JiYuAvRelease\JiYuAvCtl.exe query
JiYuAvRelease\JiYuAvCtl.exe add-hash 1001 "sample.exact" SHA256_HEX
JiYuAvRelease\JiYuAvCtl.exe add-pattern 2001 "family.loader" "48895C24??574883EC20"
JiYuAvRelease\JiYuAvCtl.exe list
JiYuAvRelease\JiYuAvCtl.exe remove 2001
JiYuAvRelease\JiYuAvCtl.exe scan C:\Samples\suspect.exe
JiYuAvRelease\JiYuAvCtl.exe protect 1234
JiYuAvRelease\JiYuAvCtl.exe stats
JiYuAvRelease\JiYuAvCtl.exe unload
```

扫描返回码：`0` 表示未命中，`10` 表示命中特征，其他值表示命令错误。

`install ... auto` 可将服务设为开机自动加载。自动加载后，用户态服务应在启动时重新加载
特征库并重新指定受保护 PID；当前特征库存储在非分页内存中，不写注册表。相同 ID 的
`add-hash`/`add-pattern` 会原位更新规则，`list` 可核对当前实际加载内容。

## 获取特征码

精确查杀优先生成 SHA-256：

```powershell
tools\New-JiYuAvSignature.ps1 C:\Samples\suspect.exe -Id 1001 -Name sample.exact
```

脚本会输出哈希和可直接执行的 `add-hash` 命令。SHA-256 对文件任何一个字节的变化都敏感，
适合确认过的单一样本，不适合覆盖同家族变种。

变种检测使用稳定字节窗口。先在 Ghidra、IDA 或十六进制工具中定位样本 `.text` 段内稳定的
函数指令，选择 24 到 48 字节；将地址、相对跳转位移、时间戳等会变化的字节设为通配。
例如从文件偏移 `0x1200` 提取 32 字节，并通配窗口内第 3、4、9、10 字节：

```powershell
tools\New-JiYuAvSignature.ps1 C:\Samples\suspect.exe `
  -Id 2001 -Name family.loader -Offset 0x1200 -Length 32 -Wildcard 3,4,9,10
```

获取模式后应执行两类验证：

1. 用同家族的多个样本扫描，确认模式能覆盖预期变种。
2. 用 Windows、常用软件和你的安装目录作为干净样本集扫描，出现任何误报就延长或更换模式。

不要把 `MZ/PE` 头、编译器启动代码、通用库函数、明文产品字符串单独作为特征；它们的误报率很高。
不要只取 4 到 8 个固定字节用于发布规则。生产规则通常还需要签名数据库版本、厂商签名、回滚和
云端误报撤销机制。

## 架构边界

当前版本提供扫描内核和控制协议，按命令扫描指定文件。实时文件落地拦截应在下一层实现为文件系统
Minifilter，并把 `IRP_MJ_CREATE/WRITE` 事件送到用户态服务判定；不要在进程或镜像通知回调中直接
读取磁盘和做大文件扫描。
