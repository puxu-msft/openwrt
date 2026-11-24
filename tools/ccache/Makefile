#
# Copyright (C) 2006-2015 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=ccache
PKG_VERSION:=4.14.3

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/ccache/ccache/releases/download/v$(PKG_VERSION)
PKG_HASH:=d50587abd5b787a7ba48cb1d76f7a4f09036e0181e742fc7383a8b154d73f0ba

include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_HOST_OPTIONS += \
	-DCCACHE_DEV_MODE=OFF \
	-DCMAKE_C_COMPILER_LAUNCHER="" \
	-DCMAKE_CXX_COMPILER_LAUNCHER="" \
	-DCMAKE_SKIP_RPATH=FALSE \
	-DCMAKE_INSTALL_RPATH=$(call host_rpath,$(STAGING_DIR_HOST)) \
	-DENABLE_DOCUMENTATION=OFF \
	-DREDIS_STORAGE_BACKEND=OFF \
	-DENABLE_TESTING=OFF

$(eval $(call HostBuild))
