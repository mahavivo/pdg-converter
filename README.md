# PDG Converter

🔴 本项目的代码和文本主要由 GPT-6.1-Sol 模型生成、输出。

⚠️ 此处只是一个参考实现，本人不打算继续改进和整合，主要是破解PDG解密机制供感兴趣的人参考。要真的把一个PDG转换工具做得完善，可以处理各种格式和情况，是相当复杂的，要长期迭代、维护，不断积累bug和案例，付出的精力、时间太多。因此想要稳定可靠地转换PDG，还是推荐用老马的 Pdg2Pic 程序。

---

将 PDG 文件批量转换或提取为 PNG、JPEG、TIFF 等图像文件，提供 Python 和 Windows 原生 C 两种实现。程序识别 PDG 内部的实际图像格式；黑白传真页默认输出 PNG，已经包含标准图像的文件默认保留原始格式和压缩数据。

项目根据 Pdg2Pic 3.06 的静态分析、核心函数隔离仿真与配对样本核对实现，无需安装或运行 Pdg2Pic、SsReader、Ghidra。算法和证据范围见 [Pdg2Pic_实现分析.md](Pdg2Pic_实现分析.md)。

## 两种实现

| 实现 | 环境 | 运行依赖 | 适合用途 |
|---|---|---|---|
| `pdg_converter.py` | Python 3.10+ | Pillow | 阅读算法、在 IDE 中运行、Python 集成 |
| `pdg_converter.c` | Windows 10 / 11，C11 | 编译后使用 Windows 自带的 BCrypt / WIC | 原生批量转换、由 Python 调用 EXE |

C 版保持单线程。运行已编译的 EXE 不需要 Python、GCC 或 Zig；编译源码时才需要编译器。Python 版不调用 Windows API，其他操作系统尚未专项验证。

## 项目结构

```text
pdg-converter/
├── README.md
├── Pdg2Pic_实现分析.md
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── requirements.txt
├── pdg_converter.py          # 主 Python 转换器，可在 IDE / CLI 中运行
├── pdg_converter.c                # Windows C 转换器
├── ccitt_tables.h                 # C 版 CCITT 解码树
├── build_gcc.cmd                  # MinGW-w64 GCC 构建
├── build_msvc.cmd                 # MSVC 构建
├── build_zig.cmd                  # 可选 Zig C 编译器构建
├── CMakeLists.txt
├── .vscode/                       # 使用 PATH 中的 GCC / GDB
├── input/                         # 默认输入目录，仅提交占位文件
├── examples/call_c_from_python.py # Python 按文件夹调用 C 版
├── reference/pdg2png_reference.py # 最初的窄范围 Python 参考实现
├── licenses/                      # 第三方许可声明
└── tests/                         # 自行生成测试数据，不含书籍样本
```

仓库以源码为主，不包含原 Pdg2Pic EXE、书籍 PDG / PNG、本机工具链、转换报告或构建产物。

## Python 快速开始

在项目目录中执行：

```powershell
python -m pip install -r requirements.txt
python pdg_converter.py "input" "converted_images"
```

也可以将 PDG 放入项目的 `input` 文件夹，在 IDE 中直接运行 `pdg_converter.py`，或不带参数运行：

```powershell
python pdg_converter.py
```

默认输入和输出目录位于脚本旁，不受 IDE 当前工作目录影响。命令行明确提供的相对路径则相对于终端当前工作目录。输入目录不存在时会报错；目录为空时会提示没有找到 PDG。

代码顶部保留 IDE 配置区：

```python
INPUT_DIR = Path(__file__).resolve().parent / "input"
OUTPUT_DIR = Path(__file__).resolve().parent / "converted_images"
RECURSIVE = True
OUTPUT_FORMAT = "auto"
OVERWRITE_EXISTING = False
SHOW_SUMMARY = False
WRITE_REPORTS = False
```

如需自定义路径，可写为 `Path(r"D:\Books\PDG")` 和 `Path(r"D:\Books\Images")`。命令行选项覆盖代码中的默认配置。

## 编译和运行 C 版

### MinGW-w64 GCC

安装 64 位 MinGW-w64 GCC，并确保终端能够执行 `gcc --version`。在项目目录运行：

```powershell
.\build_gcc.cmd
.\pdg_converter.exe "input" "converted_images"
```

脚本使用 `-O3` 优化，生成项目目录下的 `pdg_converter.exe`。直接编译的等价命令为：

```powershell
gcc -std=c11 -O3 -Wall -Wextra -D_WIN32_WINNT=0x0a00 -municode pdg_converter.c -lole32 -loleaut32 -lwindowscodecs -lbcrypt -luuid -o pdg_converter.exe
```

Windows 系统链接库和 `-municode` 必须保留。脚本从项目目录使用相对源文件及输出文件名，避免部分 MinGW 链接器处理中文绝对输出路径时出错。

### Visual Studio / MSVC

在 **x64 Native Tools Command Prompt for Visual Studio** 中进入项目目录：

```cmd
build_msvc.cmd
pdg_converter.exe input converted_images
```

需要支持 `/std:c11` 的 MSVC。脚本启用 `/O2` 优化和 UTF-8 源文件编译。

### CMake

在已配置编译器环境的终端中执行：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

单配置生成器通常输出 `build/pdg_converter.exe`，Visual Studio 等多配置生成器通常输出 `build/Release/pdg_converter.exe`。可以将 EXE 复制到项目根目录使用默认目录，也可以传入完整输入、输出路径。

### VS Code 与可选 Zig 构建

用 VS Code 打开整个项目文件夹，按 `Ctrl+Shift+B` 运行 **Build PDG converter (GCC)**，按 F5 使用 **PDG converter (GCC)** 调试配置。GCC / GDB 从 PATH 查找；如需使用特定工具链，可以自行调整 `.vscode` 配置。调试构建包含 `-g`，EXE 体积可能大于 CMD 脚本生成的版本。

已有 Zig 时，可用它的 C 编译器构建：

```powershell
.\build_zig.cmd "D:\Tools\zig\zig.exe"
```

项目不自动下载编译器。

## 输入、输出和参数

C 版源码顶部使用通用默认值：

```c
static const wchar_t *INPUT_DIR = L"input";
static const wchar_t *OUTPUT_DIR = L"converted_images";
static int RECURSIVE = 1;
static int OVERWRITE_EXISTING = 0;
static int SHOW_SUMMARY = 0;
static int WRITE_REPORTS = 0;
static const wchar_t *OUTPUT_FORMAT = L"auto";
```

无参数运行 C 版时，默认目录位于 **EXE 旁边**。C 版传入的相对路径也以 EXE 所在目录为基准。移动 EXE 后，路径基准随之改变；修改 C 源码配置后需要重新编译。

两种主转换器均支持以下选项：

| 选项 | 作用 |
|---|---|
| `input_dir output_dir` | 同时指定输入和输出目录 |
| `--format auto` | 默认：依据实际载荷选择输出格式 |
| `--format png/tiff/bmp/jpg` | 强制重新编码为指定格式；Python 另支持 `webp` |
| `--overwrite` | 覆盖已有输出；默认跳过 |
| `--no-recursive` | 仅处理输入目录直属的 PDG 文件 |
| `--summary` / `--no-summary` | 开启 / 关闭最后的数量和耗时汇总 |
| `--reports` / `--no-reports` | 开启 / 关闭 CSV、JSON 报告 |
| `--help` | 查看命令行帮助 |

例如：

```powershell
python pdg_converter.py "D:\Books\PDG" "D:\Books\Images" --format png --summary
.\pdg_converter.exe "D:\Books\PDG" "D:\Books\Images" --no-recursive --reports
```

逐文件进度始终输出，汇总和报告默认关闭。进度中的成功 / 跳过写入 stdout，失败写入 stderr。退出码：`0` 表示没有文件转换失败，`1` 表示启动或配置错误，`2` 表示至少一个文件转换失败；Python 用户中断返回 `130`。

默认递归扫描，并保留文件名和子目录结构。例如 `input/book/page.pdg` 输出为 `converted_images/book/page.png`。设置 `--no-recursive` 后，如果输入目录只包含两个子文件夹、没有直属 PDG，则处理数量为零。

输出目录不能与输入目录相同。输出位于输入内部时，转换器会排除其中的 PDG；C 版不跟随目录链接 / junction。文件先写到同目录临时文件，再提交目标；已有文件默认跳过，不检查已有图像是否正确。

报告位于输出目录，JSON 使用 UTF-8，CSV 使用带 BOM 的 UTF-8，记录路径、格式、尺寸、状态、耗时和错误。报告时间采用 UTC+08:00。

## 从 Python 调用 C 版

一次 C 调用处理一个 PDG 目录，可以在 Python 中负责查找多个目录：

```python
from pathlib import Path
import subprocess

exe = Path("pdg_converter.exe").resolve()
input_dir = Path("input/book").resolve()
output_dir = Path("converted_images/book").resolve()

subprocess.run(
    [str(exe), str(input_dir), str(output_dir), "--no-recursive"],
    check=True,
)
```

传入完整路径可避免 Python 与 C 的相对路径基准差异。未重定向 stdout / stderr 时，C 版进度直接显示在当前终端。

完整示例见 [examples/call_c_from_python.py](examples/call_c_from_python.py)：它查找含 PDG 的目录，为每个目录调用一次 C 版、逐行读取进度并保留相对目录结构。先编译 C 程序，再修改示例顶部配置，或使用示例的命令行参数。该示例只依赖 Python 标准库。

## 格式支持和边界

| PDG 中的实际内容 | `auto` 输出 | 说明 |
|---|---|---|
| 已支持的黑白传真页 | `.png` | 解密 / 扰码恢复后解码为 1 位黑白图像 |
| JPEG、PNG、TIFF、BMP、GIF | 原始扩展名 | 保留原始压缩数据；检查标准图像解码 |
| WebP、JPEG 2000、裸 J2K | `.webp` / `.jp2` / `.j2k` | C 版按容器签名提取；Python 版的验证依赖 Pillow 对相应格式的支持 |
| DjVu | `.djvu` | 检查 FORM 范围并提取，不提供光栅解码 |

已实现 HH v2 类型 `0/1/2/3/4/5/17` 和旧 HH v1 类型 `0/4/5` 的相应分支。**真实配对样本的全量验证范围是 HH v2 的 `0x02` 和 `0x11` 黑白页：222 页全部与原始 PNG 的 RGB 像素匹配。**其他分支包含静态分析与构造样本验证，尚不能宣称覆盖所有 PDG 版本。

未知编码、未实现的外层变换、附加图层、损坏的头部或不支持的传真模式会报错，批处理继续处理其他文件。多页 TIFF 和动画 GIF 在 `auto` 下保留原始内容；强制改变多帧载荷格式会拒绝转换，避免丢页。C 版拒绝所有多帧重新编码；Python 同格式输出可直接保留原始数据。

默认单文件上限 256 MB，图像上限 1 亿像素，可在源码配置区调整。传真页默认写入 300 DPI，这是已验证样本使用的输出设置，不是从所有 PDG 文件中读取出的真实扫描分辨率。

Python 和 WIC 的编码器可能生成不同的压缩数据和元数据；像素匹配并不要求 PNG 文件字节一致。测速应使用相同的输入、输出位置和实际转换数量，避免把跳过已有文件的耗时作为转换耗时，也不要跨磁盘直接比较运行时间。

## 验证与开发

安装 Python 依赖后，可运行由项目自行生成数据的测试：

```powershell
python -m unittest discover -s tests -v
```

Windows 上先编译项目根目录的 `pdg_converter.exe`，测试会同时验证 C 版。没有 EXE 时明确跳过 C 测试。测试覆盖简单传真页、标准图像、多帧保留、损坏文件、目录递归、覆盖规则、统计开关和退出码，不需要原软件或书籍样本。

C 代码使用 4 空格缩进和展开的条件 / 循环代码块；`.clang-format` 保存排版规则。`reference/pdg2png_reference.py` 是较窄的研究参考实现，面向日常使用的入口是两个主转换器。

## 许可证

项目源码和文档采用 [MIT License](LICENSE)。CCITT 码表整理自 pdfminer.six，保留其 MIT 版权声明，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 [licenses/pdfminer_CCITT_LICENSE.txt](licenses/pdfminer_CCITT_LICENSE.txt)。Pillow 是单独安装的依赖；Windows 编解码器由系统提供。
