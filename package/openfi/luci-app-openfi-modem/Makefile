#
# OpenFi 6C 5G 模组管理(LuCI 应用 + CLI/采样进程)
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-openfi-modem
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=OpenFi 6C

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-openfi-modem
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=OpenFi 6C 5G 模组管理
  URL:=https://github.com/Nohello-ai/immortalwrt-mt798x-25.12
  DEPENDS:=+openfi-lib +luci-base
endef

define Package/luci-app-openfi-modem/description
  OpenFi 6C 5G 模组(移远 RM500U-CN)管理:
    · 由一个常驻进程统一采样 AT 口(串口只能一个人用),
      其他查询读缓存 —— 界面轮询不会跟风扇抢串口
    · 型号/固件/IMEI/IMSI/ICCID/SIM 状态
    · 注网状态、运营商、制式、频段、RSRP/RSRQ/SINR、信号格数
    · 模组温度(顺便写给风扇用,风扇就不必自己开一路串口)
    · AT 命令终端
    · 自动探测 AT 口(不按 USB 描述符硬编码,更耐固件变化)
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/src \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -I$(STAGING_DIR)/usr/include/openfi" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/luci-app-openfi-modem/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/openfi-modem $(1)/usr/sbin/openfi-modem
	$(CP) ./root/. $(1)/
	$(CP) ./htdocs/. $(1)/www/
endef

$(eval $(call BuildPackage,luci-app-openfi-modem))
