# LUT 分层管理 OpenFX 插件

DaVinci Resolve / OpenFX 插件，参考视频里的 `LUT Splitter v2.6` 功能做成 LIDONG 命名体系：

```text
OpenFX > LIDONG 色彩工具 > LUT 分层管理
```

## 快速下载

最新版安装包：

[下载 LUTLayerManager_1.4.0.pkg](https://github.com/zhanglidongeric1995/LUTLayerManagerOFX/raw/refs/heads/main/outputs/LUTLayerManager_1.4.0.pkg)

备用手动安装包（熟悉 OFX 手动安装的用户）：

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
- 可分别设置 `节点输入色彩空间`、`LUT 输入色彩空间` 和 `LUT 输出色彩空间`。
- `插件输出方式` 可选择返回节点输入空间，或保留技术 LUT 的目标输出空间。
- 内置 Rec.709 Gamma 2.4、ARRI LogC3/LogC4、Blackmagic Film Gen 5、DaVinci Intermediate、ACEScct、S-Log3、V-Log、Log3G10、Canon C-Log2，以及 DJI D-Log / D-Gamut 转换。
- macOS / DaVinci Resolve 默认使用 Metal GPU 渲染；实测 59.94 fps 时间线可接近实时播放，CPU 占用从旧版约 500% 降到约 60%–75%。
- Metal 不可用时自动使用 CPU 回退：多线程分行渲染、高精度传递函数查表，并为同空间分层处理建立 65 点实时缓存；复杂跨色彩空间及超出 0–1 的浮点信号保留高精度处理路径。
- 常见默认参数会直接应用原始 LUT，不再重复计算无效的分层和色彩转换；拖动参数时也不会反复读取 `.cube` 文件。
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
outputs/LUTLayerManager_1.4.0.pkg
```

## 对外发布：Developer ID 签名与 Apple 公证

对外销售时，请只交付完成签名和公证的 `.pkg`；手动安装 ZIP 仅作为备用选项。不要用本地构建时的临时签名产物。

### 1. 准备账户与证书

加入 Apple Developer Program 后，在开发者后台创建并安装两张证书：

- `Developer ID Application`：签名 OFX bundle。
- `Developer ID Installer`：签名最终 `.pkg`。

在“钥匙串访问”中确认两张证书均显示有效；不要把证书私钥、Apple ID 密码或 App 专用密码提交到仓库。

### 2. 保存本机公证凭据（只需一次）

在终端运行以下命令，按提示输入 Apple ID、团队 ID 和 App 专用密码；凭据会保存在本机钥匙串中：

```bash
xcrun notarytool store-credentials "LIDONG_NOTARY"
```

### 3. 构建、签名和公证

将下列两张证书的完整名称替换为你钥匙串中的实际名称：

```bash
export DEVELOPER_ID_APPLICATION="Developer ID Application: Your Legal Name (TEAMID)"
export DEVELOPER_ID_INSTALLER="Developer ID Installer: Your Legal Name (TEAMID)"
export NOTARY_KEYCHAIN_PROFILE="LIDONG_NOTARY"

./scripts/release_macos.sh
```

这个命令会依次运行测试、构建通用二进制、Developer ID 签名、生成签名 `.pkg`、将 `.pkg` 与备用 ZIP 提交 Apple 公证、为 `.pkg` 写入公证票据并做 Gatekeeper 验证。默认交付文件为：

```text
outputs/LUTLayerManager_版本号.pkg
```

## 使用

1. 在 Resolve 的 OpenFX 面板找到 `LIDONG 色彩工具 > LUT 分层管理`。
2. 拖到节点或素材上。
3. 在 `LUT 文件` 里点击 `.cube 路径` 下拉菜单。
4. 选择 `选择 .cube 文件...` 打开文件选择器，或直接选择历史 LUT。
5. `节点输入色彩空间` 选择实际进入插件节点时的空间。RCM 项目通常选择时间线色彩空间；普通 YRGB 项目根据插件位于 CST 之前还是之后，选择相机空间或 CST 输出空间。
6. `LUT 输入色彩空间` 选择 `.cube` 在应用之前预期接收的空间。
7. `LUT 输出色彩空间` 选择 `.cube` 应用之后产生的空间。创意 LUT 通常选择 `与 LUT 输入相同`；相机 Log 转 Rec.709 的技术 LUT 选择 `Rec.709 Gamma 2.4`。
8. `插件输出方式`：创意 LUT 及 RCM 流程选择 `返回节点输入空间`；在普通 YRGB 流程中需要技术 LUT 直接完成 Log 到 Rec.709 转换时，选择 `保留 LUT 输出空间`。
9. 用 `亮度` 和 `色彩` 分别控制 LUT 的明暗影响和色彩风格。
10. 用 `暗部 / 中间调 / 高光` 控制 LUT 在不同亮度区域的强度。

### 色彩空间怎么选

插件无法可靠地从 OpenFX 接口读取 Resolve 为素材设置的相机输入色彩空间，因此不会根据文件名或素材元数据自动猜测。`节点输入色彩空间` 描述的是到达当前节点的像素，不一定是相机拍摄时的原始空间。

| 使用场景 | 节点输入 | LUT 输入 | LUT 输出 | 插件输出方式 |
| --- | --- | --- | --- | --- |
| RCM / DWG 时间线中的 Rec.709 创意 LUT | DaVinci Wide Gamut / Intermediate | Rec.709 Gamma 2.4 | 与 LUT 输入相同 | 返回节点输入空间 |
| 普通 YRGB 中的 DJI D-Log 转 Rec.709 技术 LUT | DJI D-Log / D-Gamut | DJI D-Log / D-Gamut | Rec.709 Gamma 2.4 | 保留 LUT 输出空间 |
| 普通 YRGB 中，插件位于 CST 之后 | CST 的输出空间 | LUT 实际要求的输入 | LUT 实际产生的输出 | 按后续节点要求选择 |

如果上游 CST 或 Resolve 色彩管理已经把 DJI D-Log 转成了 DWG/Intermediate，当前节点输入就应选择 DWG/Intermediate，而不是因为素材由 DJI 拍摄就继续选择 D-Log。技术型 D-Log 转 Rec.709 LUT 最适合放在尚未执行同类输入转换的节点位置，避免重复转换。

## 验证

```bash
./scripts/test_macos.sh
```

测试会校验 1D、3D、组合 1D + 3D LUT 的插值顺序、浮点范围、11 种传递曲线的往返转换、技术 LUT 的独立输入/输出空间、DJI 官方 D-Log / D-Gamut 参考值、Metal 与 CPU 同帧结果、快速缓存与精确处理结果的一致性，以及与 OpenColorIO ACES Studio 参考结果的一致性。

## 说明

这个版本在 macOS 上优先使用 Resolve 提供的 Metal 缓冲区和命令队列直接处理画面，不把每帧像素搬回 CPU。CPU 回退路径会保留约四分之一的处理器资源给 Resolve 解码、界面和其他应用。macOS 上通过原生文件选择器选取 `.cube`，并将实际路径保存在 OFX 参数中；重复使用时可从 LUT 历史菜单直接切换。

常规色彩转换常数与曲线参考 OpenColorIO 2.3.2 / ACES Studio Config 2.1.0，相关 BSD 许可随插件 bundle 一并提供。DJI D-Log 曲线与 D-Gamut 色域参考 [DJI X9 D-Log D-Gamut Whitepaper](https://dl.djicdn.com/downloads/DJI_Ronin_4D/X9_D_Log_D_Gamut_Whitepaper_I.pdf)。

本地 `build_macos.sh` 和 `package_macos_pkg.sh` 默认仍使用临时签名，便于开发与测试；对外发布请使用 `release_macos.sh` 生成经过 Developer ID 签名和 Apple 公证的安装包。
