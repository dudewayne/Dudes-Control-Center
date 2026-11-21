# Makefile for Dudes Control Center Applications
# Builds: dudes-control-center-fan-cli, dudes-control-center-fan-daemon, dudes-control-center-cpu-daemon, dudes-control-center-kbd-backlight-daemon, dudes-control-center-gui, hard-limit-enforcer

# Rule 8: Use all CPU cores for compilation
MAKEFLAGS += -j$(shell nproc)

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS_COMMON = -lpthread

# GTK3 flags (will be obtained via pkg-config)
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || echo "")
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null || echo "-lgtk-3 -lgdk-3")

# cJSON flags (for JSON profile storage)
CJSON_CFLAGS = $(shell pkg-config --cflags libcjson 2>/dev/null || echo "")
CJSON_LIBS = $(shell pkg-config --libs libcjson 2>/dev/null || echo "-lcjson")

# Source files
COMMON_SRC = src/dudes-control-center-common.c
COMMON_HDR = src/dudes-control-center-common.h

# Targets
CLI_TARGET = dudes-control-center-fan-cli
CPU_CLI_TARGET = dudes-control-center-cpu-cli
DAEMON_TARGET = dudes-control-center-fan-daemon
CPU_DAEMON_TARGET = dudes-control-center-cpu-daemon
KBD_DAEMON_TARGET = dudes-control-center-kbd-backlight-daemon
GUI_TARGET = dudes-control-center-gui

# Version from VERSION file
VERSION := $(shell cat VERSION 2>/dev/null || echo "0.0.0")
PACKAGE = dudes-control-center

# Deb build directory
DEB_BUILD_DIR = build-deb

# DKMS module name and version
DKMS_MODULE_NAME = ite_829x
DKMS_MODULE_VERSION = $(VERSION)

.PHONY: all clean cli daemon cpu-daemon kbd-daemon gui install uninstall deb help

all: $(CLI_TARGET) $(CPU_CLI_TARGET) $(DAEMON_TARGET) $(CPU_DAEMON_TARGET) $(KBD_DAEMON_TARGET) $(GUI_TARGET)

$(CLI_TARGET): $(COMMON_SRC) src/dudes-control-center-fan-cli.c $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ $(COMMON_SRC) src/dudes-control-center-fan-cli.c $(LDFLAGS_COMMON)

$(DAEMON_TARGET): $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-fan-daemon.c $(COMMON_HDR) VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-fan-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

$(CPU_DAEMON_TARGET): src/dudes-control-center-cpu-common.c src/dudes-control-center-log.c src/dudes-control-center-cpu-daemon.c src/dudes-control-center-cpu-common.h VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ src/dudes-control-center-cpu-common.c src/dudes-control-center-log.c src/dudes-control-center-cpu-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

$(GUI_TARGET): $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-gui.c $(COMMON_HDR) VERSION
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(CJSON_CFLAGS) -DAPP_VERSION="\"$(VERSION)\"" -o $@ $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-gui.c $(LDFLAGS_COMMON) $(GTK_LIBS) $(CJSON_LIBS)

cli: $(CLI_TARGET)

cpu-cli: $(CPU_CLI_TARGET)

$(CPU_CLI_TARGET): src/dudes-control-center-cpu-cli.c src/dudes-control-center-cpu-common.h
	$(CC) $(CFLAGS) -o $@ src/dudes-control-center-cpu-cli.c $(LDFLAGS_COMMON)

daemon: $(DAEMON_TARGET)

cpu-daemon: $(CPU_DAEMON_TARGET)

$(KBD_DAEMON_TARGET): src/dudes-control-center-kbd-common.c src/dudes-control-center-log.c src/dudes-control-center-kbd-backlight-daemon.c src/dudes-control-center-kbd-common.h VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ src/dudes-control-center-kbd-common.c src/dudes-control-center-log.c src/dudes-control-center-kbd-backlight-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

kbd-daemon: $(KBD_DAEMON_TARGET)

gui: $(GUI_TARGET)

clean:
	rm -f $(CLI_TARGET) $(CPU_CLI_TARGET) $(DAEMON_TARGET) $(CPU_DAEMON_TARGET) $(KBD_DAEMON_TARGET) $(GUI_TARGET)
	rm -f $(DEB_BUILD_DIR)/$(PACKAGE)_*.deb 2>/dev/null || true

# Build .deb package
deb: all
	@echo "Building .deb package $(PACKAGE) version $(VERSION)..."
	@rm -rf $(DEB_BUILD_DIR)
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/applications
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/pixmaps
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/256x256/apps
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN
	@cp $(CLI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(CPU_CLI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(CPU_DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(KBD_DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(GUI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp dudes-control-center.desktop $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/applications/
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/pixmaps/ 2>/dev/null || true
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/256x256/apps/dudes-control-center.png 2>/dev/null || true
	@cp dudes-control-center-fan-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dudes-control-center-cpu-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dudes-control-center-kbd-backlight-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dudes-control-center-hard-limit-enforcer.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dudes-control-center-hard-limit-enforcer.timer $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp scripts/dudes-control-center-hard-limit-enforce.sh $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/dudes-control-center-hard-limit-enforce
	@# DKMS kernel module source
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)
	@cp drivers/ite_829x.c $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@cp drivers/Kbuild $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@cp drivers/Makefile $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@cp drivers/dkms.conf $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@echo 'Package: $(PACKAGE)' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Version: $(VERSION)' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Architecture: amd64' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Maintainer: Dudes Control Center' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Description: Dudes Control Center - Monitor and control notebook fan speed, CPU frequency, and keyboard backlight' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Section: admin' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Priority: optional' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Depends: libgtk-3-0, libcjson1, dkms' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@printf '#!/bin/sh\nset -e\nif [ "$$1" = "configure" ]; then\n    systemctl daemon-reload\n    # DKMS: register and build ite_829x kernel module\n    DKMS_MOD_NAME=ite_829x\n    DKMS_MOD_VER=$(VERSION)\n    if [ -d /usr/src/$$DKMS_MOD_NAME-$$DKMS_MOD_VER ]; then\n        dkms add -m $$DKMS_MOD_NAME -v $$DKMS_MOD_VER || true\n        dkms build -m $$DKMS_MOD_NAME -v $$DKMS_MOD_VER || true\n        dkms install -m $$DKMS_MOD_NAME -v $$DKMS_MOD_VER || true\n        modprobe $$DKMS_MOD_NAME || true\n    fi\n    systemctl enable dudes-control-center-fan-daemon\n    systemctl restart dudes-control-center-fan-daemon || systemctl start dudes-control-center-fan-daemon\n    systemctl enable dudes-control-center-cpu-daemon\n    systemctl restart dudes-control-center-cpu-daemon || systemctl start dudes-control-center-cpu-daemon\n    systemctl enable dudes-control-center-kbd-backlight-daemon\n    systemctl restart dudes-control-center-kbd-backlight-daemon || systemctl start dudes-control-center-kbd-backlight-daemon\n    systemctl enable dudes-control-center-hard-limit-enforcer.timer\n    systemctl start dudes-control-center-hard-limit-enforcer.timer || true\n    update-desktop-database >/dev/null 2>&1 || true\nfi\nexit 0\n' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postinst
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postinst
	@printf '#!/bin/sh\nset -e\nif [ "$$1" = "remove" ]; then\n    systemctl stop dudes-control-center-kbd-backlight-daemon 2>/dev/null || true\n    systemctl disable dudes-control-center-kbd-backlight-daemon 2>/dev/null || true\n    # DKMS: unload ite_829x kernel module\n    rmmod ite_829x 2>/dev/null || true\n    systemctl stop dudes-control-center-fan-daemon 2>/dev/null || true\n    systemctl disable dudes-control-center-fan-daemon 2>/dev/null || true\n    systemctl stop dudes-control-center-cpu-daemon 2>/dev/null || true\n    systemctl disable dudes-control-center-cpu-daemon 2>/dev/null || true\n    systemctl stop dudes-control-center-hard-limit-enforcer.timer 2>/dev/null || true\n    systemctl disable dudes-control-center-hard-limit-enforcer.timer 2>/dev/null || true\n    systemctl daemon-reload\nfi\nexit 0\n' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/prerm
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/prerm
	@printf '#!/bin/sh\nset -e\nif [ "$$1" = "purge" ]; then\n    rm -rf /home/*/.config/dudes-control-center/ 2>/dev/null || true\n    rm -rf /var/log/dudes-control-center/ 2>/dev/null || true\n    rm -f /etc/systemd/system/dudes-control-center-fan-daemon.service 2>/dev/null || true\n    rm -f /etc/systemd/system/dudes-control-center-cpu-daemon.service 2>/dev/null || true\n    rm -f /etc/systemd/system/dudes-control-center-kbd-backlight-daemon.service 2>/dev/null || true\n    rm -f /etc/systemd/system/dudes-control-center-hard-limit-enforcer.service 2>/dev/null || true\n    rm -f /etc/systemd/system/dudes-control-center-hard-limit-enforcer.timer 2>/dev/null || true\n    # DKMS: unregister and remove ite_829x\n    dkms remove -m ite_829x --all 2>/dev/null || true\n    rm -rf /usr/src/ite_829x-* 2>/dev/null || true\n    systemctl daemon-reload 2>/dev/null || true\nfi\nexit 0\n' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postrm
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postrm
	@cd $(DEB_BUILD_DIR) && dpkg-deb --build $(PACKAGE)-$(VERSION) $(PACKAGE)_$(VERSION)_amd64.deb
	@echo "Built: $(DEB_BUILD_DIR)/$(PACKAGE)_$(VERSION)_amd64.deb"

install: all
	@echo "Installing applications..."
	install -m 755 $(CLI_TARGET) /usr/local/bin/
	install -m 755 $(CPU_CLI_TARGET) /usr/local/bin/
	install -m 755 $(DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(CPU_DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(KBD_DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(GUI_TARGET) /usr/local/bin/
	@echo "Creating systemd service for daemon..."
	install -d /etc/systemd/system
	cp dudes-control-center-fan-daemon.service /etc/systemd/system/
	cp dudes-control-center-cpu-daemon.service /etc/systemd/system/
	cp dudes-control-center-kbd-backlight-daemon.service /etc/systemd/system/
	cp dudes-control-center-hard-limit-enforcer.service /etc/systemd/system/
	cp dudes-control-center-hard-limit-enforcer.timer /etc/systemd/system/
	@echo "Installing hard limit enforcer script..."
	install -m 755 scripts/dudes-control-center-hard-limit-enforce.sh /usr/local/bin/dudes-control-center-hard-limit-enforce
	systemctl daemon-reload
	systemctl enable dudes-control-center-fan-daemon
	systemctl enable dudes-control-center-cpu-daemon
	@echo "Enabling keyboard backlight daemon..."
	systemctl enable dudes-control-center-kbd-backlight-daemon
	@echo "Enabling hard limit enforcer timer..."
	systemctl enable dudes-control-center-hard-limit-enforcer.timer
	systemctl start dudes-control-center-hard-limit-enforcer.timer || true
	@echo "Restarting daemons..."
	systemctl restart dudes-control-center-fan-daemon || systemctl start dudes-control-center-fan-daemon
	systemctl restart dudes-control-center-cpu-daemon || systemctl start dudes-control-center-cpu-daemon
	@echo "Starting keyboard backlight daemon..."
	systemctl restart dudes-control-center-kbd-backlight-daemon || systemctl start dudes-control-center-kbd-backlight-daemon
	@echo "Installing desktop launcher..."
	install -m 644 dudes-control-center.desktop /usr/share/applications/
	update-desktop-database >/dev/null 2>&1 || true
	@echo "Installation complete!"

uninstall:
	@echo "Uninstalling applications..."
	systemctl stop dudes-control-center-fan-daemon 2>/dev/null || true
	systemctl disable dudes-control-center-fan-daemon 2>/dev/null || true
	systemctl stop dudes-control-center-cpu-daemon 2>/dev/null || true
	systemctl disable dudes-control-center-cpu-daemon 2>/dev/null || true
	systemctl stop dudes-control-center-kbd-backlight-daemon 2>/dev/null || true
	systemctl disable dudes-control-center-kbd-backlight-daemon 2>/dev/null || true
	systemctl stop dudes-control-center-hard-limit-enforcer.timer 2>/dev/null || true
	systemctl disable dudes-control-center-hard-limit-enforcer.timer 2>/dev/null || true
	rm -f /usr/local/bin/$(CLI_TARGET)
	rm -f /usr/local/bin/$(CPU_CLI_TARGET)
	rm -f /usr/local/bin/$(DAEMON_TARGET)
	rm -f /usr/local/bin/$(CPU_DAEMON_TARGET)
	rm -f /usr/local/bin/$(KBD_DAEMON_TARGET)
	rm -f /usr/local/bin/$(GUI_TARGET)
	rm -f /etc/systemd/system/dudes-control-center-fan-daemon.service
	rm -f /etc/systemd/system/dudes-control-center-cpu-daemon.service
	rm -f /etc/systemd/system/dudes-control-center-kbd-backlight-daemon.service
	rm -f /etc/systemd/system/dudes-control-center-hard-limit-enforcer.service
	rm -f /etc/systemd/system/dudes-control-center-hard-limit-enforcer.timer
	rm -f /usr/local/bin/dudes-control-center-hard-limit-enforce
	rm -f /usr/share/applications/dudes-control-center.desktop
	systemctl daemon-reload
	update-desktop-database >/dev/null 2>&1 || true
	@echo "Uninstallation complete!"

# Help target
help:
	@echo "Dudes Control Center Build System (v$(VERSION))"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build all applications (default)"
	@echo "  cli      - Build CLI application only"
	@echo "  daemon   - Build daemon application only"
	@echo "  gui      - Build GUI application only"
	@echo "  clean    - Remove built binaries"
	@echo "  deb      - Build .deb package"
	@echo "  install  - Install all applications and systemd service"
	@echo "  uninstall - Remove installed files"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make          # Build everything"
	@echo "  make deb      # Build .deb package"
	@echo "  sudo make install   # Install to system"
	@echo "  sudo dpkg -i build-deb/$(PACKAGE)_$(VERSION)_amd64.deb  # Install .deb"
