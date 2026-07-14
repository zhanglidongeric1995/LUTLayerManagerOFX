# LUT 分层管理 OpenFX 插件

DaVinci Resolve / OpenFX CPU 插件，参考视频里的 `LUT Splitter v2.6` 功能做成 LIDONG 命名体系：

```text
OpenFX > LIDONG 色彩工具 > LUT 分层管理
```

## 快速下载

最新版安装包：

[下载 LUTLayerManager_1.2.0.pkg](https://github.com/zhanglidongeric1995/LUTLayerManagerOFX/raw/refs/heads/main/outputs/LUTLayerManager_1.2.0.pkg)

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
- 可分别设置 `节点输入色彩空间` 与 `LUT 色彩空间`。插件会先转换到 LUT 空间，应用 LUT 后再转换回节点空间。
- 内置 Rec.709 Gamma 2.4、ARRI LogC3/LogC4、Blackmagic Film Gen 5、DaVinci Intermediate、ACEScct、S-Log3、V-Log、Log3G10 和 Canon C-Log2 转换。
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
outputs/LUTLayerManager_1.2.0.pkg
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
5. `节点输入色彩空间` 选择进入插件节点时的空间。RCM 项目通常选择时间线色彩空间；普通 YRGB 项目根据插件位于 CST 之前还是之后选择相机空间或 CST 输出空间。
6. `LUT 色彩空间` 选择 `.cube` 预期的空间。选择 `与节点输入相同（不转换）` 可保持旧版行为。
7. 用 `亮度` 和 `色彩` 分别控制 LUT 的明暗影响和色彩风格。
8. 用 `暗部 / 中间调 / 高光` 控制 LUT 在不同亮度区域的强度。

`LUT 色彩空间` 适用于输入和输出处于同一空间的创意/风格 LUT。若 `.cube` 本身是从相机 Log 转换到 Rec.709 的技术 LUT，应在 Resolve 节点流程中明确处理其输出空间，避免重复转换。

## 验证

```bash
./scripts/test_macos.sh
```

测试会校验 1D、3D、组合 1D + 3D LUT 的插值顺序、浮点范围、10 种传递曲线的往返转换，以及与 OpenColorIO ACES Studio 参考结果的一致性。

## 说明

这个版本是纯 CPU OFX 插件。macOS 上通过原生文件选择器选取 `.cube`，并将实际路径保存在 OFX 参数中；重复使用时可从 LUT 历史菜单直接切换。

色彩转换常数与曲线参考 OpenColorIO 2.3.2 / ACES Studio Config 2.1.0，相关 BSD 许可随插件 bundle 一并提供。

本地 `build_macos.sh` 和 `package_macos_pkg.sh` 默认仍使用临时签名，便于开发与测试；对外发布请使用 `release_macos.sh` 生成经过 Developer ID 签名和 Apple 公证的安装包。
