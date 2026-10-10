#
# OpenFi 6C 后端公共库
#
# 三个包(风扇 / 模组 / 短信)共用这一套。做成静态库(.a):
#   · 体积很小,三个包各链一份也就几十 KB
#   · 不用管 .so 版本、rpath、ldconfig —— 少一类上机才发现的问题
#
include $(TOPDIR)/rules.mk

PKG_NAME:=openfi-lib
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=OpenFi 6C

include $(INCLUDE_DIR)/package.mk

define Package/openfi-lib
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=OpenFi 6C 后端公共库(静态)
  URL:=https://github.com/Nohello-ai/immortalwrt-mt798x-25.12
  DEPENDS:=
endef

#
# ★ 必须有 install 段。
#   OpenWrt 的打包规则是:没有 install 段就只打印
#   "skipping <name> -- package has no install section",不产出包文件。
#   而三个应用包都写着 DEPENDS:=+openfi-lib —— 依赖解析时找不到提供者,
#   整个 image 构建就会失败。
#   这个库是纯静态链接用的(staging 里有头文件和 .a 就够),
#   运行期不需要装任何东西 —— 但包本身必须存在。
#
# ★ 注意:这里的注释只能用 #,不能用 C 风格的 /* */。
#   我第一版就是手滑写成 /* */ —— Makefile 解析直接失败,
#   而 OpenWrt 的包扫描器(scan.mk)对解析失败的 Makefile 是【静默跳过】的,
#   于是这个包在索引里根本不存在,构建到一半才报"依赖不存在",
#   排查起来很绕(错误信息指向依赖,不指向那个坏掉的 Makefile)。
#
define Package/openfi-lib/install
	$(INSTALL_DIR) $(1)/usr/lib
endef

define Package/openfi-lib/description
  OpenFi 6C 后端各包共用的基础设施:
    uci 配置读取(自己解析 UCI 文件,不依赖 libuci)
    sysfs 读写、LED 控制、温度采集
    通用分段阈值模板(曲线/信号格数等)
    风扇控制律与状态机
    AT 指令响应解析 + 串口通道(termios)
  全部逻辑都有本地单元测试覆盖。
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS)"
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/openfi
	$(CP) $(PKG_BUILD_DIR)/*.h $(1)/usr/include/openfi/
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/libopenfi.a $(1)/usr/lib/
endef

$(eval $(call BuildPackage,openfi-lib))
