#
# OpenFi 6C 短信收发(LuCI 应用 + CLI/守护进程)
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-openfi-sms
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=OpenFi 6C

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-openfi-sms
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=OpenFi 6C 短信
  URL:=https://github.com/Nohello-ai/immortalwrt-mt798x-25.12
  DEPENDS:=+openfi-lib +luci-base
endef

define Package/luci-app-openfi-sms/description
  OpenFi 6C 短信收发(重写版,C 实现):
    · 自己实现 GSM 03.40/03.38 的 PDU 编解码(收 + 发都走 PDU)
      中文自动走 UCS2,纯英文走 7 位,长短信自动加 UDH 分片
    · 短信用"一条一个文件"存 /etc/openfi-sms,原子写入,掉电不坏
    · 后台定时从模组取未读短信,解完入库后把模组里那条删掉,不会塞满模组存储
    · CLI 与 LuCI 页面共用同一套接口(list/read/send/delete/unread)
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

define Package/luci-app-openfi-sms/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/openfi-sms $(1)/usr/bin/openfi-sms
	$(CP) ./root/. $(1)/
	$(CP) ./htdocs/. $(1)/www/
endef

$(eval $(call BuildPackage,luci-app-openfi-sms))
