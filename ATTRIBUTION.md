# 溯源声明 / Attribution

本仓库不是从零写的。它是在别人的树上**只加设备支持**得到的,所以在这里把
「从哪来、改了哪些、许可怎么算」写清楚。

---

## 一、这份代码从哪来

| 角色 | 仓库 | 分支 / 提交 |
|---|---|---|
| **本仓库** | `Nohello-ai/immortalwrt-mt798x-25.12` | `main` |
| **直接上游(我们复制的就是它)** | [`chasey-dev/immortalwrt-mt798x-rebase`](https://github.com/chasey-dev/immortalwrt-mt798x-rebase) | 分支 `25.12` |
| **上游的基准提交** | 同上 | `a99b0888b88f`<br>*mtk: eqos: preserve the HNAT exception bit*<br>2026-10-10 |

> 说明:上游的 `25.12` 分支会持续更新,而我们是**某个时间点的快照**。
> 上面那个提交号是逐字节核对出来的 —— 拿 `package/mtk/applications/`
> `luci-app-eqos-mtk/root/usr/sbin/eqos` 与上游同路径文件比对,
> sha256 完全一致(`57248dd1827041c1…`),所以可以确定基准就是它。

## 二、上游自己又基于什么

`chasey-dev/immortalwrt-mt798x-rebase` 的 README 里钉了三个上游提交,
我们完整继承下来(**没有改动**):

| 组件 | 仓库 | 提交 |
|---|---|---|
| ImmortalWrt | [`immortalwrt/immortalwrt`](https://github.com/immortalwrt/immortalwrt) | [`ce35a9e`](https://github.com/immortalwrt/immortalwrt/commit/ce35a9e4ef1b87fbbc2146036ce6693e4242861a) |
| MTK OpenWrt Feeds | [`mediatek/mtk-openwrt-feeds`](https://github.com/mediatek/mtk-openwrt-feeds) | [`08196fa`](https://github.com/mediatek/mtk-openwrt-feeds/commit/08196fa6e5d338ce88aa0002f781e2d24289b1f1) |
| l1parser | [`chasey-dev/l1parser`](https://github.com/chasey-dev/l1parser) | [`081bb31`](https://github.com/chasey-dev/l1parser/commit/081bb31211efc74594d25bfd1bb5811f3408a205) |

而这棵树的**再上游**是:

```
OpenWrt (openwrt/openwrt)
   └── ImmortalWrt (immortalwrt/immortalwrt) @ openwrt-25.12
          └── chasey-dev/immortalwrt-mt798x-rebase @ 25.12
                 └── 本仓库
```

`feeds.conf.default` 里的五个 feed 是 ImmortalWrt / OpenWrt 官方的
`openwrt-25.12` 分支,**未做任何修改**:

```
src-git packages  https://github.com/immortalwrt/packages.git;openwrt-25.12
src-git luci      https://github.com/immortalwrt/luci.git;openwrt-25.12
src-git routing   https://github.com/openwrt/routing.git;openwrt-25.12
src-git telephony https://github.com/openwrt/telephony.git;openwrt-25.12
src-git video     https://github.com/openwrt/video.git;openwrt-25.12
```

另外,设备 HNAT 相关实现沿用自
[`padavanonly/immortalwrt-mt798x-6.6`](https://github.com/padavanonly/immortalwrt-mt798x-6.6)
(这是上游 README 的 Acknowledgements 里就写明的,我们照旧保留)。

## 三、我们改了什么

**原则:底座一行不动,只加"让这块板子能用"的部分。**

### 新增

| 路径 | 内容 |
|---|---|
| `target/linux/mediatek/dts/mt7981b-openfi-6c.dts` | 设备树(LED / 按键 / GPIO 引出) |
| `target/linux/mediatek/image/filogic.mk` | 设备定义(在既有文件里**追加**一段) |
| `target/linux/mediatek/filogic/base-files/etc/board.d/01_leds` | 灯名映射(在既有文件里**追加**一段) |
| `defconfig/openfi6c.config` | 本设备的构建配置 |
| `package/openfi/` | 三个自研包(C 写的散热 / 5G 模组 / 短信) |
| `scripts/openfi/` | 板级自检与测试工具 |
| `.github/workflows/build.yml` | 出固件的 CI |

### 修改

| 路径 | 改了什么 | 为什么 |
|---|---|---|
| `feeds.conf.default` | 去掉了我们**自己旧包**的源 | 旧包与新包同名,会互相覆盖。**上游的五个 feed 一个没动。** |
| `target/linux/mediatek/filogic/base-files/etc/board.d/01_leds` | 修掉上游给 `openfi,6c` 抄错的那段 | 上游引用了本板不存在的 `green:lan` / `eth0`,灯不会亮 |

**除此之外,`target/`、`package/`(除 `package/openfi/`)、`include/`、`toolchain/`、
`feeds.conf.default` 的官方部分,全部保持上游原样。**

## 四、许可

- 本仓库的 `COPYING` / `LICENSES/` 原样继承上游(**GPL-2.0**)。
- 底座(ImmortalWrt / OpenWrt / MTK OpenWrt Feeds / 上游 rebase 树)的版权
  归各自作者,许可随各自声明。
- **本仓库新增的部分**(设备树、`package/openfi/` 下的代码、`scripts/openfi/`、
  CI 配置)由本仓库作者编写,同样以 **GPL-2.0** 发布 —— 与底座一致,
  不引入额外限制。
- 闭源组件(MTK 的 `mt_wifi` 驱动等)不在这里重新分发,而是由
  `feeds` 在构建时从各自的来源拉取。

## 五、如果要向上游反馈

我们只加了设备支持,所以:

- **设备相关的问题**(这块板子、我们的三个包)→ 提到本仓库。
- **底座/通用问题**(内核、无线驱动、其他机型)→ 提到上游
  [`chasey-dev/immortalwrt-mt798x-rebase`](https://github.com/chasey-dev/immortalwrt-mt798x-rebase)。

请不要把我们加的设备支持当成上游的行为。

---

## 六、怎么在这个仓库上干活

本仓库是 [`chasey-dev/immortalwrt-mt798x-rebase`](https://github.com/chasey-dev/immortalwrt-mt798x-rebase) 的 fork。
两个分支分工明确:

| 分支 | 用途 | 规则 |
|---|---|---|
| `25.12` | 纯上游镜像 | **只同步,不往上提交任何东西** |
| `openfi-6c` | 我们的设备支持 | 日常开发都在这里;推上去会自动触发构建 |

**为什么这样分**:底座和"我们加的东西"混在一起的话,以后想同步上游
就只能整棵树重抄一遍,也看不出自己改了什么。分开之后,同步是一次
`git pull`,我们的改动永远是一个干净的 diff。

### 同步上游

```sh
git fetch origin 25.12
git checkout 25.12
git merge --ff-only origin/25.12     # 纯上游,直接快进,不会有冲突
git checkout openfi-6c
git rebase 25.12                     # 我们的提交重放到新底座上
git push --force-with-lease fork openfi-6c
```

`openfi-6c` 上的提交顺序是:先设备支持,再包和脚本,最后 CI 与说明 ——
所以 rebase 时冲突通常只落在设备树和 `filogic.mk` 上,范围可控。

### 提交给上游

**不要拿 `openfi-6c` 直接开 PR** —— 那上面有我们固件特有的东西
(自研的三个包、CI、说明文档),上游不需要。正确做法是从 `25.12`
拉一个只含目标修复的分支:

```sh
git checkout -b fix-xxx origin/25.12
# …只做那一处修复…
git push fork fix-xxx
# 然后在 GitHub 上向 chasey-dev:25.12 开 PR
```

已经这样提过一个:PR #111(设备树与板级配置修正)。

### 构建

推 `openfi-6c` 会自动跑 `.github/workflows/build.yml`,产物在
仓库的 Releases 里(固件 + 它的 MD5)。

CI 有五层缓存(dl / 工具链 / 宿主机工具 / 内核目录 / ccache),
所以**第一次**约 60~90 分钟,之后 15~25 分钟。
换分支名的话记得把 workflow 里 `push.branches` 一起改掉 ——
名字不改就不会自动触发。
