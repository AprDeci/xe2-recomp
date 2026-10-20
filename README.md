# XC2 native recompilation research

《异度神剑 2》（Xenoblade Chronicles 2）的 Windows 静态重编译实验，基于 [WindowSmash](https://github.com/SNO2207/WindowSmash) 的重编译器与运行时。

当前是研究快照，目标输入为 **base 1.0.0 ExeFS + 同版 RomFS**，并非完整可玩的发行版。源码、构建工具和第三方通知均位于本仓库内，不再依赖旁边的 WindowSmash 目录。来源版本和已知问题见 [NOTICE.md](NOTICE.md) 与 [docs/STATUS.md](docs/STATUS.md)。

## 目录

```text
runtime/           手写 C 运行时：HLE、IPC、线程、D3D11、输入、音频
 tools/            AArch64 → C 重编译器、编译封装和包提取工具
 tests/            合成数据自检、图形回归检查、音频 C 测试入口
 third_party/      libvpx / Opus 许可与通知
 br.ps1            构建与运行入口
 exefs_base/       本机游戏模块，Git 忽略
 romfs/            本机解密 RomFS，Git 忽略
 dump/             模块镜像、字体与着色器缓存，Git 忽略
 build/            生成的游戏 C、目标文件和 ssbu.exe，Git 忽略
 save/             本机存档，Git 忽略
```

产物暂时保留上游的 `ssbu.exe` 文件名，运行时仍使用 `SSBU_*` 环境变量；文件名不代表加载的是 Smash。

## 环境

- Windows x64；CPU 支持 AVX2、FMA、F16C；支持 Direct3D 11 的显卡。
- Python **3.12 或更新版本**。当前构建已验证 Python 3.12；不需要 Smash 工具中的 Python 3.14 `compression.zstd`。
- Visual Studio C++ 桌面开发工具与 Windows SDK。
- `clang-cl.exe`：可使用 Visual Studio 的 C++ Clang tools、LLVM，或通过 `SSBU_CLANG` 指定现有安装。

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

`br.ps1` 会通过 `vswhere.exe` 自动发现 Visual Studio。也可以从 x64 Developer PowerShell 启动；自定义安装找不到时，设置 `VCVARS64` 为自己的 `vcvars64.bat` 路径。工具链路径属于本机配置，不要写回源码。

可选组件：`SSBU_SDL3_DIR` 指向 SDL3 Windows 开发包，可增加非 Xbox 手柄支持；`SSBU_OPUS_LIBS` 和 `SSBU_VPX_DIR` 用于可选编解码器。它们的头文件、库和 DLL 不随源码仓提供。当前 XC2 的 DSP-ADPCM / PCM16 音频路径不依赖这些可选库。

## 自备游戏数据

源码仓不包含游戏包、模块、重编译生成的游戏函数、RomFS、字体、密钥或存档。请自行准备匹配的 **XC2 base 1.0.0 NSP** 与密钥目录：

```powershell
.\.venv\Scripts\python.exe tools\extract.py 'E:\YourDump\XC2-base-1.0.0.nsp' --keys 'E:\YourKeys'
```

此命令从同一个包生成 `exefs_base/`、`romfs/romfs.bin` 和 `dump/modules/romfs.cfg`，不覆盖已有输出。提取中断会保留 `.partial` 文件；检查后移走未完成的输出，再重试。密钥仅从传入目录读取。

工具会拒绝明显的更新包，但不会验证包内的完整游戏版本。不要把更新版 ExeFS 和 base RomFS 混用；已知这会触发 BDAT 初始化断言。只有已提取数据时，按上述目录摆放；`dump/modules/romfs.cfg` 内容为：

```text
romfs/romfs.bin
0
```

相对路径按项目根目录解析。启动脚本会固定工作目录，所以项目整体移动后仍可运行。其他来源的字体按需要放在 `dump/fonts/`，不要提交到 Git。

## 构建与运行

```powershell
.\br.ps1                 # 增量构建，然后运行
.\br.ps1 -BuildOnly      # 只构建，不启动游戏或触碰存档
.\br.ps1 -NoBuild        # 直接运行已有产物
.\br.ps1 -BuildOnly -Jobs 4
```

首次构建会生成大量 C 文件。默认 12 个编译任务；内存不足时调低 `-Jobs`。构建失败会停止，避免继续运行旧产物。正常运行不设置超时，也不开 `--trace`。程序正常关闭后回到终端，脚本会恢复调用前的工作目录。

## 输入与性能诊断

键盘默认控制玩家 1，且仅在游戏窗口获得焦点时生效。目前没有自定义键盘键位配置；`controllers.ini` 中的按钮重映射针对手柄。

| Switch 操作 | 键盘 |
| --- | --- |
| 左摇杆 | 方向键 |
| A / B / X / Y | Z / X / C / V |
| L / R | A / S |
| ZL / ZR | Q / W |
| ＋ / － | Enter / Backspace |
| 右摇杆 | I 上、K 下、J 左、L 右 |

F1 切换帧率显示，F2 查看渲染与上传统计，F3 截图，F11 或 Alt+Enter 切换全屏。`controllers.ini`、`display.ini` 在项目根目录生成，均被 Git 忽略。

需要稀疏性能日志时：

```powershell
$env:SSBU_LOG_STATS = '1'
.\br.ps1 -NoBuild
Remove-Item Env:SSBU_LOG_STATS
```

`--trace` 会记录大量系统调用，曾把简单场景拖到约 4–5 FPS，不能用于正常性能比较。F4 会转储绘制命令与表面；F6 会做 10 秒线程采样并短暂暂停被采样线程，报告写在可执行文件旁。以上诊断仅按需使用。

## 检查

无需游戏内容的 Python 检查：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p 'test_*.py' -v
```

在已加载 VS x64 环境、且有 `clang-cl` 时，上述检查还会编译并运行合成音频 C 向量；否则该项明确跳过。它不启动游戏、不打开音频设备、不读写存档。可选的重编译器差分验证工具为 `tools/difftest.py`、`tools/functest.py`；另装 `requirements-dev.txt` 后根据其帮助使用。

保留的图形检查依赖你自行生成的本地诊断输入，不在普通单元测试中自动启动游戏：

```powershell
.\.venv\Scripts\python.exe tests\check_lut3d.py '<LUT dump.txt>'
.\.venv\Scripts\python.exe tests\check_model_consumed.py '<diagnostic log>' '<surface folder>'
.\.venv\Scripts\python.exe tests\check_title_quarter.py '<title frame.bmp>'
```

这些输入的生成开关、适用范围及已知失败见 [docs/STATUS.md](docs/STATUS.md)。检查通过不等于原机画面/音频一致或游戏全程可玩。

## Git 与公开边界

`.gitignore` 排除了无扩展名 ExeFS 模块所在目录、RomFS、生成 C/二进制、虚拟环境、缓存、日志、截图、密钥和存档。提交前仍应检查 `git status` 和 `git diff --cached --stat`，不要用 `git add -f` 把这些本机文件加入仓库。

上游未声明项目级许可证，本仓没有擅自为继承源码添加 MIT/GPL 等许可证。**准备公开前，请先确认你对继承源码的再分发授权。** 第三方通知必须保留；本地 Git 初始化不代表已经发布或完成许可审查。
