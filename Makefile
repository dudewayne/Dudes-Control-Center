# Makefile for Dudes Control Center Applications
# Builds: dudes-control-center-fan-cli, dudes-control-center-fan-daemon, dudes-control-center-gui

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
DAEMON_TARGET = dudes-control-center-fan-daemon
GUI_TARGET = dudes-control-center-gui

# Version from VERSION file
VERSION := $(shell cat VERSION 2>/dev/null || echo "0.0.0")
PACKAGE = dudes-control-center

# Deb build directory
DEB_BUILD_DIR = build-deb

.PHONY: all clean cli daemon gui install uninstall deb help

all: $(CLI_TARGET) $(DAEMON_TARGET) $(GUI_TARGET)

$(CLI_TARGET): $(COMMON_SRC) src/dudes-control-center-fan-cli.c $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ $(COMMON_SRC) src/dudes-control-center-fan-cli.c $(LDFLAGS_COMMON)

$(DAEMON_TARGET): $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-fan-daemon.c $(COMMON_HDR) VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-fan-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

$(GUI_TARGET): $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-gui.c $(COMMON_HDR) VERSION
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(CJSON_CFLAGS) -DAPP_VERSION="\"$(VERSION)\"" -o $@ $(COMMON_SRC) src/dudes-control-center-log.c src/dudes-control-center-gui.c $(LDFLAGS_COMMON) $(GTK_LIBS) $(CJSON_LIBS)

cli: $(CLI_TARGET)

daemon: $(DAEMON_TARGET)

gui: $(GUI_TARGET)

clean:
	rm -f $(CLI_TARGET) $(DAEMON_TARGET) $(GUI_TARGET)
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
	@cp $(DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(GUI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp dudes-control-center.desktop $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/applications/
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/pixmaps/ 2>/dev/null || true
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/256x256/apps/dudes-control-center.png 2>/dev/null || true
	@cp dudes-control-center-fan-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@echo 'Package: $(PACKAGE)' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Version: $(VERSION)' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Architecture: amd64' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Maintainer: Dudes Control Center' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Description: Dudes Control Center - Monitor and control notebook fan speed' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Section: admin' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Priority: optional' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Depends: libgtk-3-0, libcjson1' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@printf '#!/bin/sh\nset -e\nif [ "$$1" = "configure" ]; then\n    systemctl daemon-reload\n    systemctl enable dudes-control-center-fan-daemon\n    systemctl restart dudes-control-center-fan-daemon || systemctl start dudes-control-center-fan-daemon\n    update-desktop-database >/dev/null 2>&1 || true\nfi\nexit 0\n' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postinst
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/postinst
	@printf '#!/bin/sh\nset -e\nif [ "$$1" = "remove" ]; then\n    systemctl stop dudes-control-center-fan-daemon 2>/dev/null || true\n    systemctl disable dudes-control-center-fan-daemon 2>/dev/null || true\n    systemctl daemon-reload\nfi\nexit 0\n' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/prerm
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/prerm
	@cd $(DEB_BUILD_DIR) && dpkg-deb --build $(PACKAGE)-$(VERSION) $(PACKAGE)_$(VERSION)_amd64.deb
	@echo "Built: $(DEB_BUILD_DIR)/$(PACKAGE)_$(VERSION)_amd64.deb"

install: all
	@echo "Installing applications..."
	install -m 755 $(CLI_TARGET) /usr/local/bin/
	install -m 755 $(DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(GUI_TARGET) /usr/local/bin/
	@echo "Creating systemd service for daemon..."
	install -d /etc/systemd/system
	cp dudes-control-center-fan-daemon.service /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable dudes-control-center-fan-daemon
	@echo "Restarting daemon..."
	systemctl restart dudes-control-center-fan-daemon || systemctl start dudes-control-center-fan-daemon
	@echo "Installing desktop launcher..."
	install -m 644 dudes-control-center.desktop /usr/share/applications/
	update-desktop-database >/dev/null 2>&1 || true
	@echo "Installation complete!"

uninstall:
	@echo "Uninstalling applications..."
	systemctl stop dudes-control-center-fan-daemon 2>/dev/null || true
	systemctl disable dudes-control-center-fan-daemon 2>/dev/null || true
	rm -f /usr/local/bin/$(CLI_TARGET)
	rm -f /usr/local/bin/$(DAEMON_TARGET)
	rm -f /usr/local/bin/$(GUI_TARGET)
	rm -f /etc/systemd/system/dudes-control-center-fan-daemon.service
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
