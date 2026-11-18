# 第三方组件

本目录只放**上游源码**，不做任何修改。各组件保留自己的许可证；分发时请一并附带。

| 目录 | 组件 | 版本 | 许可证 | 是否参与编译 |
| --- | --- | --- | --- | --- |
| `curl/` | curl | 7.43.0（2015） | curl License（MIT 风格） | **否**。`DzjsTrainerUpdater.vcxproj` 的 `IncludePath` 已指向本目录，但 `ClCompile` 未包含任何 curl 源文件 |
| `MemoryModule/` | MemoryModule（Joachim Bauch） | — | MPL 1.1 / GPL 2.0 / LGPL 2.1 三选一 | **否**。反射式加载 DLL 的旧路线，已被"释放到磁盘 + `LoadLibraryW` 远程线程"取代 |
| `XZip/` | XZip / XUnzip（CodeProject 1.3） | 1.3 | CodeProject Open License 1.02 | **否**。驱动与 AV 驱动以未压缩 BIN 资源直接释放 |

另有两个第三方组件不在本目录：

| 组件 | 位置 | 许可证 | 是否参与编译 |
| --- | --- | --- | --- |
| MinHook（Tsuda Kageyu） | `DzjsTrainerHooks/minhook/` | BSD-2-Clause | **是**，内嵌编译进 Hook DLL |
| md5（L. Peter Deutsch） | `DzjsTrainer/md5.cpp`、`md5.h` | 见文件头声明 | 是（工程内），但实际 MD5 校验走 BCrypt |

## 维护约定

- 升级上游时**整目录替换**，不要逐文件改；curl 内部有大量相对 `#include`，结构必须保持一致。
- 不要在第三方目录里写项目专用补丁；确有必要时在项目侧另建文件并在 PR 里说明。
- 新增第三方组件请同时更新本表与根 `README.md` 的致谢。
- 未参与编译的组件仍保留在树内，方便将来启用；删除前请确认没有工程或脚本引用它们（`python tools/verify_repo.py --strict` 会检查 `.vcxproj` 中的引用）。
