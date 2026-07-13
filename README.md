# LUT 分层管理 OpenFX 插件

DaVinci Resolve / OpenFX CPU 插件，参考视频里的 `LUT Splitter v2.6` 功能做成 LIDONG 命名体系：

```text
OpenFX > LIDONG 色彩工具 > LUT 分层管理
```

## 快速下载

最新版安装包：

[下载 LUTLayerManager_1.0.1.pkg](https://github.com/zhanglidongeric1995/LUTLayerManagerOFX/raw/refs/heads/main/outputs/LUTLayerManager_1.0.1.pkg)

备用手动安装包：

[下载 LUTLayerManager.ofx.bundle.zip](https://github.com/zhanglidongeric1995/LUTLayerManagerOFX/raw/refs/heads/main/outputs/LUTLayerManager.ofx.bundle.zip)

安装后请完全退出并重新打开 DaVinci Resolve。

## 功能

- 读取 `.cube` 1D / 3D LUT 文件，也支持包含 1D shaper + 3D cube 的组合 LUT。
- 支持 Byte、Short、Float 像素深度。
- 支持 RGB / RGBA 输入输出。
- 将 LUT 效果拆成：
  - `亮度`
  - `色彩`
  - `暗部`
  - `中间调`
  - `高光`
- 高级控制：
  - `中性色偏`
  - `黑位修正`
  - `白点滚降`
  - `色彩纯度`
  - `密度偏移`
  - `色相偏移`
- 浮点图像会保留负值和高光余量；未选择 LUT 时，画面保持直通。
- `.cube 路径` 是一个下拉菜单：第一项是 `选择 .cube 文件...`，后面是历史 LUT。
- 选择过的 LUT 会保存到：

```text
~/Library/Application Support/LIDONGFILMS/LUTLayerManager/lut_history.txt
```

## 构建

```bash
./scripts/build_macos.sh
```

输出：

```text
build/LUTLayerManager.ofx.bundle
outputs/LUTLayerManager.ofx.bundle.zip
```

## 安装

```bash
./scripts/install_macos_user.sh
```

也可以双击项目里的 `安装.command`。

安装到：

```text
/Library/OFX/Plugins/LUTLayerManager.ofx.bundle
```

安装后请完全退出并重新打开 DaVinci Resolve。如果插件没有出现，删除 Resolve 的 OFX 缓存后重启：

```bash
rm "$HOME/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml"
```

## 打包 pkg

```bash
./scripts/package_macos_pkg.sh
```

输出：

```text
outputs/LUTLayerManager_1.0.1.pkg
```

## 使用

1. 在 Resolve 的 OpenFX 面板找到 `LIDONG 色彩工具 > LUT 分层管理`。
2. 拖到节点或素材上。
3. 在 `LUT 文件` 里点击 `.cube 路径` 下拉菜单。
4. 选择 `选择 .cube 文件...` 打开文件选择器，或直接选择历史 LUT。
5. 用 `亮度` 和 `色彩` 分别控制 LUT 的明暗影响和色彩风格。
6. 用 `暗部 / 中间调 / 高光` 控制 LUT 在不同亮度区域的强度。
7. `亮度计算色域` 只影响暗部 / 中间调 / 高光的亮度权重，不会转换输入素材或 LUT 的色彩空间；请先在 Resolve 色彩管理中完成色彩空间转换。

## 验证

```bash
./scripts/test_macos.sh
```

测试会校验 1D、3D、组合 1D + 3D LUT 的插值顺序，以及浮点像素是否保留超出 0–1 的范围。

## 说明

这个版本是纯 CPU OFX 插件。macOS 上通过原生文件选择器选取 `.cube`，并将实际路径保存在 OFX 参数中；重复使用时可从 LUT 历史菜单直接切换。

当前安装包使用本机临时签名完成构建验证；若要对外公开分发，还需要 Developer ID Installer 证书签名并完成公证。
