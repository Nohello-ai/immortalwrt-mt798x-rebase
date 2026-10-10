#
# OpenFi 6C 风扇温控(LuCI 应用 + 守护进程)
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-openfi-fan
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=OpenFi 6C

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-openfi-fan
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=OpenFi 6C 风扇温控
  URL:=https://github.com/Nohello-ai/immortalwrt-mt798x-25.12
  DEPENDS:=+openfi-lib +luci-base
endef

define Package/luci-app-openfi-fan/description
  OpenFi 6C 风扇温控(重写版,C 实现):
    · 三路温度采集(CPU / WiFi / 5G 模组),按最高的一路做统一温度
    · 四点温度曲线 + 迟滞防抖 + 紧急温度满转
    · 物理开关联动"静默模式"(停转 + 关灯),带过热安全兜底
    · 双路 PWM(反极性,25 kHz)
    · 提供 --json / --status 给 LuCI 读状态
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

define Package/luci-app-openfi-fan/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/openfi-fan $(1)/usr/sbin/openfi-fan
	$(CP) ./root/. $(1)/
	$(CP) ./htdocs/. $(1)/www/
endef

$(eval $(call BuildPackage,luci-app-openfi-fan))
