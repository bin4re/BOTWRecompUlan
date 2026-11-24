# BOTWRecompUlan

《塞尔达传说：旷野之息》的 **Linux / 幽兰本** 移植，基于
[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) 的 fork。

这不是 Switch 模拟器：游戏代码被静态重编译成 C，再编译为原生 ARM64
`botwgame.so`；游戏的 NVN 图形 API 由运行时翻译为 Vulkan。

目标机器是 **幽兰本（RK3588 / Mali-G610 MC4 / panthor / 6.12 vendor 内核）**。
本仓库只包含移植代码和构建脚本，**不包含任何游戏文件**。

## 当前状态

- 在 6.12 vendor 内核上可以启动、进入游戏，并正常游玩一段时间；
- **已知问题**：进入特定场景后 GPU 报 `CS fault`，运行时报 `vkWaitSemaphores: -4 (device lost)`
  并退出。该问题在 6.12/7.3.3、不同 panthor/Mesa 组合下都能复现，尚未解决；
- 系统自带的 **Mesa 26.0.8 运行本游戏不稳定**（`vkEndCommandBuffer -2`），默认 Mesa 27 运行。

因此：能跑，但**不承诺稳定**；遇到固定场景崩溃属于已知问题。

## 环境要求

- 幽兰本（RK3588，Mali-G610），16 GB 内存；
- 6.12 vendor 内核，`/dev/dri` 下可用的 panthor 驱动，Wayland 桌面会话；
- 约 30 GB 空闲磁盘（游戏数据 + 构建产物）；
- `/dev/udmabuf`（默认启用零拷贝运行游戏，否则回退到拷贝模式运行会不稳定）。

## 方式一：使用预编译版（推荐给普通兰友）

1. 从 [Releases](https://github.com/bin4re/BOTWRecompUlan/releases) 下载 Linux aarch64 压缩包并解压；
2. 进入目录，首次运行 `./scripts/setup.sh`（创建 `tools/venv`，检查编译器）；
3. 准备自己的游戏文件（见下一节）；
4. 运行 `./run-botw.sh`，在启动器菜单里选择游戏目录，并点击构建游戏代码；
5. 构建完成后即可进入游戏。

Release 包只包含运行时、`tools/`、`gamelib-src/` 和脚本，**不包含**
`v190/`、`botwgame.so`、`generated/` 或任何游戏资源。

## 方式二：从源码构建

```sh
git clone https://github.com/bin4re/BOTWRecompUlan
cd BOTWRecompUlan
./scripts/setup.sh --system        # 可选：安装构建依赖（需要 sudo）
./scripts/setup.sh                 # 创建 tools/venv

./scripts/build-mesa.sh            # 构建 Mesa 27 到 deps/（或解压预编译 Mesa 到 deps/）

# 游戏文件（以自己的 1.9.0 提取物为例）
mkdir -p v190
cp -r /path/to/extracted v190/
echo 190 > game_version.txt

make gen -j$(nproc)                # 生成 C 代码（耗时较长）
make -j$(nproc)                    # 构建 build/botwrecomp 和 botwgame.so
./run-botw.sh
```

如果只想先构建运行时、不生成游戏代码，也可以直接 `make -j$(nproc)`，
之后在启动器菜单里构建游戏代码。

## 游戏文件

本仓库不提供、也不下载游戏文件。需要自备对应版本的游戏提取物：

| 游戏版本 | 数据目录 |
|---|---|
| 1.0.0 | `extracted/`（仓库根目录） |
| 1.6.0 | `v160/extracted/` |
| 1.8.2 | `v182/extracted/` |
| 1.9.0 | `v190/extracted/` |

`game_version.txt` 记录当前版本（例如 `190`）。也可以在启动器菜单里选择/导入游戏目录，
程序会自动创建对应的版本目录、存档和缓存。

`v190/`、`generated/`、`game_version.txt` 都属于本地数据，已在 `.gitignore` 中忽略，
**不要提交到仓库**。

## Mesa 27

系统 Mesa 26.0.8 的 panvk 在 BOTW 上会触发 `vkEndCommandBuffer -2`，因此需要 Mesa 27：

- 预编译包：Release 自带 `deps/mesa/libvulkan_panfrost.so`，
  `run-botw.sh` 首次运行会自动生成 `deps/mesa-icd/panfrost_devel.json`；
- 或运行 `./scripts/build-mesa.sh`，脚本会拉取并构建固定版本的 Mesa 27，
  输出到 `deps/mesa/`，并生成 ICD 描述文件。

`run-botw.sh` 会自动使用 `deps/mesa-icd/panfrost_devel.json`；没有找到时退回系统驱动，
但系统 Mesa 26 无法运行游戏。

Mesa 构建依赖（脚本 `--system` 会安装）：

```
clang lld meson ninja-build pkg-config python3-mako
llvm-21-dev libllvmspirvlib-21-dev
libdrm-dev libwayland-dev libwayland-egl-backend-dev libdisplay-info-dev
libshaderc-dev
```

## 零拷贝（建议默认开启）

BOTW 的 guest heap 约 **3.1 GB**。不开零拷贝时走 CPU 拷贝路径，帧率明显下降，
场景切换时也更容易卡顿/崩溃；建议把 memfd + udmabuf 零拷贝作为默认配置。

### 检查

```sh
ls -l /dev/udmabuf
cat /sys/module/udmabuf/parameters/size_limit_mb   # 需要 >= 4096
```

### 开启

6.12 vendor 内核默认是 `# CONFIG_UDMABUF is not set`，需要重新配置内核：

- `CONFIG_DMA_SHARED_BUFFER=y`
- `CONFIG_MEMFD_CREATE=y`
- `CONFIG_UDMABUF=y`（6.12 上游是 `bool`，只能内建；如果内核树已改成
  `tristate`，也可以 `=m` 后 `modprobe udmabuf`）

改完重新编译/安装内核（或模块），重启。

BOTW 需要约 3.1 GB 的 udmabuf 上限：

- 内建：启动参数加 `udmabuf.size_limit_mb=4096`；
- 模块：`/etc/modprobe.d/udmabuf.conf` 写
  `options udmabuf size_limit_mb=4096`；
- 权限：`/dev/udmabuf` 默认 `root:kvm 660`，可加 udev 规则
  `KERNEL=="udmabuf", MODE="0666"`，或把当前用户加入 `kvm` 组。

### 验证

启动日志里应看到：

```
[vk] udmabuf heap: type 0
[vk] guest heap 3120 MB is GPU memory (memfd+udmabuf ...)
```

`run-botw.sh` 检测到 `/dev/udmabuf` 会自动设置 `LARECOMP_VK_MEMFD=1`。
没有时回退到 `LARECOMP_VK_MEMFD=0` 拷贝模式，只建议临时排查，不建议日常使用。

## 已知问题

- 特定场景的 GPU `CS fault`（见上），目前无解；
- 首次启动/首次进入新区域需要编译 shader，会明显卡顿；
- 首次 `make gen` / 菜单构建游戏代码耗时较长，需要足够内存和磁盘；
- SSH 会话里直接启动会报 `SDL_Vulkan_CreateSurface: Vulkan can't find any displays`，
  请在幽兰本桌面会话中运行。

## 目录说明

```
runtime/       移植后的运行时源码
tools/         游戏代码生成器（Python）
scripts/       setup.sh / build-mesa.sh
deps/          Mesa 27 和 ICD（本地生成，已忽略）
build/         运行时和游戏库构建产物（已忽略）
v190/          玩家数据目录（已忽略）
generated/     生成的重编译 C 代码（已忽略）
```

更详细的构建说明见 [docs/linux.md](docs/linux.md)。

## 致谢与许可

- 上游：[BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)
- 引擎：[LAS Recomp](https://github.com/peiikai/LASRecomp)
- 本仓库遵循上游的 MIT 许可证，见 [LICENSE](LICENSE)。
