# Linux / 幽兰本构建说明

本文是开发/构建细节，普通使用看 README 即可。

## 依赖

```sh
sudo apt install build-essential clang lld pkg-config meson ninja-build python3-venv \
    libsdl3-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libvulkan-dev libshaderc-dev \
    libdrm-dev libwayland-dev libwayland-egl-backend-dev libdisplay-info-dev \
    llvm-21-dev libllvmspirvlib-21-dev python3-mako
```

`scripts/setup.sh --system` 会安装这些包；随后 `scripts/setup.sh` 创建
`tools/venv` 并安装 `capstone`、`lz4`、`unicorn`、`Pillow`。

## 构建

```sh
./scripts/setup.sh
./scripts/build-mesa.sh          # Mesa 27 -> deps/mesa + deps/mesa-icd
make gen -j$(nproc)              # 生成 generated/main、generated/sdk
make -j$(nproc)                  # build/botwrecomp、build/botwgame.so
```

`Makefile` 在 Linux 上默认使用本机架构（aarch64），交叉编译时用
`ARCH=`、`SYSROOT=`、`GEN=`、`B=` 覆盖。

## Mesa 27

`scripts/build-mesa.sh` 固定 Mesa commit `6c2135ab76`，meson 选项：

```
--buildtype=release -Dplatforms=wayland
-Dvulkan-drivers=panfrost -Dgallium-drivers=panfrost -Dglx=disabled
```

产物：`deps/mesa/build/src/panfrost/vulkan/libvulkan_panfrost.so`。
ICD：`deps/mesa-icd/panfrost_devel.json`，`run-botw.sh` 通过
`VK_DRIVER_FILES` 指向它。

系统 Mesa 26.0.8 会在 BOTW 上触发 `vkEndCommandBuffer -2`，不要用它运行游戏。

## 零拷贝

BOTW 的 guest heap 约 3.1 GB，建议默认启用 zero-copy：

```sh
ls -l /dev/udmabuf
cat /sys/module/udmabuf/parameters/size_limit_mb   # >= 4096
```

6.12 vendor 内核默认关闭 `CONFIG_UDMABUF`。需要：

- `CONFIG_DMA_SHARED_BUFFER=y`、`CONFIG_MEMFD_CREATE=y`；
- `CONFIG_UDMABUF=y`（6.12 上游为 bool；改成 tristate 后可 `=m`）；
- 上限：内建用启动参数 `udmabuf.size_limit_mb=4096`，模块用
  `/etc/modprobe.d/udmabuf.conf` 的 `options udmabuf size_limit_mb=4096`；
- 权限：`/dev/udmabuf` 对当前用户可读写（kvm 组或 udev `MODE="0666"`）。

`run-botw.sh` 会自动检测并设置 `LARECOMP_VK_MEMFD=1`；没有设备时回退到
拷贝模式（`0`），仅用于临时排查。

## 游戏数据

启动器按版本使用 `v160/`、`v182/`、`v190/`，1.0.0 使用根目录的
`extracted/`。这些目录和 `generated/` 都是本地数据，不提交。

## 已知问题

固定场景 GPU `CS fault` / `device lost`，在 6.12 与 7.3.3、不同 panthor
和 Mesa 27 下均可复现；目前未解决。
