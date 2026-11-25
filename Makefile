# Makefile for Dudes Control Center Applications
# Builds: dcc-fan-cli, dcc-fan-daemon, dcc-cpu-daemon, dcc-kbd-daemon, dcc-gui, hard-limit-enforcer

# Rule 8: Use all CPU cores for compilation
MAKEFLAGS += -j$(shell nproc)

CC = gcc
# EXTRA_CFLAGS=-Werror is used by CI so new warnings fail the build
CFLAGS = -Wall -Wextra -O2 $(EXTRA_CFLAGS)
LDFLAGS_COMMON = -lpthread

# GTK3 flags (will be obtained via pkg-config)
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || echo "")
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null || echo "-lgtk-3 -lgdk-3")

# cJSON flags (for JSON profile storage)
CJSON_CFLAGS = $(shell pkg-config --cflags libcjson 2>/dev/null || echo "")
CJSON_LIBS = $(shell pkg-config --libs libcjson 2>/dev/null || echo "-lcjson")

# Source files
COMMON_SRC = src/dcc-common.c
COMMON_HDR = src/dcc-common.h
LOG_SRC = src/dcc-log.c
LOG_HDR = src/dcc-log.h
# Socket authorization + safe user-file I/O for the root daemons
SECURE_SRC = src/dcc-secure.c
SECURE_HDR = src/dcc-secure.h

# Group whose members may control the daemons (see src/dcc-secure.h)
DCC_GROUP = dudes-control-center

# Targets
CLI_TARGET = dcc-fan-cli
CPU_CLI_TARGET = dcc-cpu-cli
KBD_CLI_TARGET = dcc-kbd-cli
DAEMON_TARGET = dcc-fan-daemon
CPU_DAEMON_TARGET = dcc-cpu-daemon
KBD_DAEMON_TARGET = dcc-kbd-daemon
GUI_TARGET = dcc-gui

# Version from VERSION file
VERSION := $(shell cat VERSION 2>/dev/null || echo "0.0.0")
PACKAGE = dudes-control-center

# Deb build directory
DEB_BUILD_DIR = build-deb

# Man pages (1.33.0): man/<name>.<section>, @VERSION@ filled in
MAN_PAGES = dudes-control-center.7 dcc-gui.1 dcc-fan-cli.1 dcc-cpu-cli.1 dcc-kbd-cli.1 \
            dcc-fan-daemon.8 dcc-cpu-daemon.8 dcc-kbd-daemon.8 dcc-component.8 dcc_fan.4

# DKMS module name and version
DKMS_MODULE_NAME = ite_829x
DKMS_MODULE_VERSION = $(VERSION)

.PHONY: all clean cli daemon cpu-daemon kbd-daemon gui install uninstall deb help test man-check

all: $(CLI_TARGET) $(CPU_CLI_TARGET) $(KBD_CLI_TARGET) $(DAEMON_TARGET) $(CPU_DAEMON_TARGET) $(KBD_DAEMON_TARGET) $(GUI_TARGET)

$(CLI_TARGET): $(COMMON_SRC) src/dcc-fan-cli.c $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ $(COMMON_SRC) src/dcc-fan-cli.c $(LDFLAGS_COMMON)

$(DAEMON_TARGET): $(COMMON_SRC) $(LOG_SRC) $(SECURE_SRC) src/dcc-fan-daemon.c $(COMMON_HDR) $(LOG_HDR) $(SECURE_HDR) VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ $(COMMON_SRC) $(LOG_SRC) $(SECURE_SRC) src/dcc-fan-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

$(CPU_DAEMON_TARGET): src/dcc-cpu-common.c $(LOG_SRC) $(SECURE_SRC) src/dcc-cpu-daemon.c src/dcc-cpu-common.h $(LOG_HDR) $(SECURE_HDR) VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ src/dcc-cpu-common.c $(LOG_SRC) $(SECURE_SRC) src/dcc-cpu-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

$(GUI_TARGET): $(COMMON_SRC) $(LOG_SRC) src/dcc-gui.c $(COMMON_HDR) $(LOG_HDR) src/dcc-kbd-common.h src/dcc-cpu-common.h VERSION
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(CJSON_CFLAGS) -DAPP_VERSION="\"$(VERSION)\"" -o $@ $(COMMON_SRC) src/dcc-log.c src/dcc-gui.c $(LDFLAGS_COMMON) $(GTK_LIBS) $(CJSON_LIBS)

cli: $(CLI_TARGET)

cpu-cli: $(CPU_CLI_TARGET)

$(CPU_CLI_TARGET): src/dcc-cpu-cli.c src/dcc-cpu-common.h
	$(CC) $(CFLAGS) -o $@ src/dcc-cpu-cli.c $(LDFLAGS_COMMON)

kbd-cli: $(KBD_CLI_TARGET)

$(KBD_CLI_TARGET): src/dcc-kbd-cli.c
	$(CC) $(CFLAGS) -o $@ src/dcc-kbd-cli.c $(LDFLAGS_COMMON)

daemon: $(DAEMON_TARGET)

cpu-daemon: $(CPU_DAEMON_TARGET)

$(KBD_DAEMON_TARGET): src/dcc-kbd-common.c $(LOG_SRC) $(SECURE_SRC) src/dcc-kbd-daemon.c src/dcc-kbd-common.h $(COMMON_HDR) $(LOG_HDR) $(SECURE_HDR) VERSION
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ src/dcc-kbd-common.c $(LOG_SRC) $(SECURE_SRC) src/dcc-kbd-daemon.c $(LDFLAGS_COMMON) $(CJSON_LIBS)

kbd-daemon: $(KBD_DAEMON_TARGET)

gui: $(GUI_TARGET)

clean:
	rm -f $(CLI_TARGET) $(CPU_CLI_TARGET) $(KBD_CLI_TARGET) $(DAEMON_TARGET) $(CPU_DAEMON_TARGET) $(KBD_DAEMON_TARGET) $(GUI_TARGET)
	rm -rf $(TEST_BUILD)
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
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/doc/dudes-control-center/images
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN
	@cp $(CLI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(CPU_CLI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(KBD_CLI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(CPU_DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(KBD_DAEMON_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp $(GUI_TARGET) $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/
	@cp resources/keyboard_dark.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/doc/dudes-control-center/images/keyboard_dark.png 2>/dev/null || true
	@# Keyboard layouts: key positions (keys.csv) and picture (SVG) per layout (1.28.0)
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/dudes-control-center
	@cp -r resources/layouts $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/dudes-control-center/
	@cp resources/logo_dark.png resources/logo_light.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/dudes-control-center/
	@# UI translations (1.31.0)
	@for l in $(UI_LANGS); do msgfmt -c -o /dev/null po/$$l.po && \
	  mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/locale/$$l/LC_MESSAGES && \
	  msgfmt -o $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/locale/$$l/LC_MESSAGES/dudes-control-center.mo po/$$l.po; done
	@cp dudes-control-center.desktop $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/applications/
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/pixmaps/ 2>/dev/null || true
	@cp DudesControlCenter.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/256x256/apps/dudes-control-center.png 2>/dev/null || true
	@# Smaller icon sizes for menus and panels (1.34.4)
	@for n in 128 64 48; do mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/$${n}x$${n}/apps && \
	  cp resources/icons/dudes-control-center_$$n.png $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/icons/hicolor/$${n}x$${n}/apps/dudes-control-center.png; done
	@cp dcc-fan-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dcc-cpu-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dcc-kbd-daemon.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dcc-hard-limit-enforcer.service $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp dcc-hard-limit-enforcer.timer $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/etc/systemd/system/
	@cp scripts/dcc-hard-limit-enforce.sh $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/dcc-hard-limit-enforce
	@# Man pages (1.33.0), compressed, next to the programs in /usr/local
	@for p in $(MAN_PAGES); do n=$${p##*.}; mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/share/man/man$$n && \
	  sed 's/@VERSION@/$(VERSION)/g' man/$$p | gzip -9n > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/share/man/man$$n/$$p.gz; done
	@# Settings tab: switch components on/off through pkexec (1.27.0)
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/sbin $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/polkit-1/actions
	@cp scripts/dcc-component.sh $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/sbin/dcc-component
	@chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/sbin/dcc-component
	@cp packaging/org.dudes-control-center.component.policy $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/polkit-1/actions/
	@# DKMS kernel module source
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)
	@cp drivers/ite_829x.c drivers/dcc_fan.c $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@cp drivers/Kbuild $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@printf 'ccflags-y += -DITE829X_VERSION=\\"%s\\" -DDCC_FAN_VERSION=\\"%s\\"\n' '$(DKMS_MODULE_VERSION)' '$(DKMS_MODULE_VERSION)' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/Kbuild
	@# dcc_fan has no hardware alias: load it at boot (1.36.0)
	@mkdir -p $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/lib/modules-load.d
	@echo dcc_fan > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/lib/modules-load.d/dudes-control-center.conf
	@cp drivers/Makefile $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/
	@cp drivers/LICENSE $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/LICENSE
	@cp packaging/copyright $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/doc/dudes-control-center/copyright
	@cp LICENSE $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/doc/dudes-control-center/LICENSE
	@# Help tab (1.34.0): the GUI shows these uncompressed
	@cp README.md CHANGELOG.md $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/share/doc/dudes-control-center/
	@sed 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="$(DKMS_MODULE_VERSION)"/' drivers/dkms.conf > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/src/$(DKMS_MODULE_NAME)-$(DKMS_MODULE_VERSION)/dkms.conf
	@echo 'Package: $(PACKAGE)' > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Version: $(VERSION)' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Architecture: amd64' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Maintainer: Dudes Control Center' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Description: Dudes Control Center - Monitor and control notebook fan speed, CPU frequency, and keyboard backlight' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Section: admin' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Priority: optional' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@echo 'Depends: libgtk-3-0, librsvg2-common, libcjson1, dkms, pkexec | policykit-1' >> $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/control
	@# Maintainer scripts from packaging/ (@VERSION@ and @GROUP@ filled in)
	@for f in preinst postinst prerm postrm; do \
		sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@GROUP@/$(DCC_GROUP)/g' packaging/$$f > $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/$$f; \
		chmod 755 $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/DEBIAN/$$f; \
	done
	@# Old command names (before 1.19.0) keep working for now
	@for n in gui fan-cli cpu-cli; do ln -s dcc-$$n $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)/usr/local/bin/dudes-control-center-$$n; done
	@# Files belong to root and only root may change them: systemd runs the
	@# daemons as root. Without this the archive kept the build user as owner
	@# (and the umask's group write), so that user could replace a root daemon.
	@chmod -R u=rwX,go=rX $(DEB_BUILD_DIR)/$(PACKAGE)-$(VERSION)
	@cd $(DEB_BUILD_DIR) && dpkg-deb --root-owner-group --build $(PACKAGE)-$(VERSION) $(PACKAGE)_$(VERSION)_amd64.deb
	@echo "Built: $(DEB_BUILD_DIR)/$(PACKAGE)_$(VERSION)_amd64.deb"

install: all
	@echo "Installing applications..."
	install -m 755 $(CLI_TARGET) /usr/local/bin/
	install -m 755 $(CPU_CLI_TARGET) /usr/local/bin/
	install -m 755 $(KBD_CLI_TARGET) /usr/local/bin/
	install -m 755 $(DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(CPU_DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(KBD_DAEMON_TARGET) /usr/local/bin/
	install -m 755 $(GUI_TARGET) /usr/local/bin/
	@echo "Setting up access group $(DCC_GROUP)..."
	groupadd -f --system $(DCC_GROUP)
	@if [ -n "$${SUDO_USER:-}" ] && [ "$$SUDO_USER" != "root" ]; then \
		usermod -aG $(DCC_GROUP) "$$SUDO_USER" && echo "Added $$SUDO_USER to group $(DCC_GROUP)"; \
	else \
		echo "NOTE: add your desktop user: sudo usermod -aG $(DCC_GROUP) <user>"; \
	fi
	@echo "Creating systemd service for daemon..."
	install -d /etc/systemd/system
	cp dcc-fan-daemon.service /etc/systemd/system/
	cp dcc-cpu-daemon.service /etc/systemd/system/
	cp dcc-kbd-daemon.service /etc/systemd/system/
	cp dcc-hard-limit-enforcer.service /etc/systemd/system/
	cp dcc-hard-limit-enforcer.timer /etc/systemd/system/
	@echo "Installing hard limit enforcer script..."
	install -m 755 scripts/dcc-hard-limit-enforce.sh /usr/local/bin/dcc-hard-limit-enforce
	install -D -m 755 scripts/dcc-component.sh /usr/local/sbin/dcc-component
	install -D -m 644 packaging/org.dudes-control-center.component.policy /usr/share/polkit-1/actions/org.dudes-control-center.component.policy
	mkdir -p /usr/share/dudes-control-center && cp -r resources/layouts /usr/share/dudes-control-center/
	install -m 644 resources/logo_dark.png resources/logo_light.png /usr/share/dudes-control-center/
	install -D -m 644 -t /usr/share/doc/dudes-control-center README.md CHANGELOG.md LICENSE
	for p in $(MAN_PAGES); do n=$${p##*.}; mkdir -p /usr/local/share/man/man$$n && \
	  sed 's/@VERSION@/$(VERSION)/g' man/$$p | gzip -9n > /usr/local/share/man/man$$n/$$p.gz; done
	mandb -q 2>/dev/null || true
	for l in $(UI_LANGS); do mkdir -p /usr/share/locale/$$l/LC_MESSAGES && \
	  msgfmt -o /usr/share/locale/$$l/LC_MESSAGES/dudes-control-center.mo po/$$l.po; done
	systemctl daemon-reload
	systemctl enable dcc-fan-daemon
	systemctl enable dcc-cpu-daemon
	@echo "Enabling keyboard backlight daemon..."
	systemctl enable dcc-kbd-daemon
	@echo "Enabling hard limit enforcer timer..."
	systemctl enable dcc-hard-limit-enforcer.timer
	systemctl start dcc-hard-limit-enforcer.timer || true
	@echo "Restarting daemons..."
	systemctl restart dcc-fan-daemon || systemctl start dcc-fan-daemon
	systemctl restart dcc-cpu-daemon || systemctl start dcc-cpu-daemon
	@echo "Starting keyboard backlight daemon..."
	systemctl restart dcc-kbd-daemon || systemctl start dcc-kbd-daemon
	@echo "Installing desktop launcher..."
	install -m 644 dudes-control-center.desktop /usr/share/applications/
	install -D -m 644 DudesControlCenter.png /usr/share/icons/hicolor/256x256/apps/dudes-control-center.png
	for n in 128 64 48; do install -D -m 644 resources/icons/dudes-control-center_$$n.png /usr/share/icons/hicolor/$${n}x$${n}/apps/dudes-control-center.png; done
	gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
	update-desktop-database >/dev/null 2>&1 || true
	@echo "Installation complete!"

uninstall:
	@echo "Uninstalling applications..."
	systemctl stop dcc-fan-daemon 2>/dev/null || true
	systemctl disable dcc-fan-daemon 2>/dev/null || true
	systemctl stop dcc-cpu-daemon 2>/dev/null || true
	systemctl disable dcc-cpu-daemon 2>/dev/null || true
	systemctl stop dcc-kbd-daemon 2>/dev/null || true
	systemctl disable dcc-kbd-daemon 2>/dev/null || true
	systemctl stop dcc-hard-limit-enforcer.timer 2>/dev/null || true
	systemctl disable dcc-hard-limit-enforcer.timer 2>/dev/null || true
	rm -f /usr/local/bin/$(CLI_TARGET)
	rm -f /usr/local/bin/$(CPU_CLI_TARGET)
	rm -f /usr/local/bin/$(KBD_CLI_TARGET)
	rm -f /usr/local/bin/$(DAEMON_TARGET)
	rm -f /usr/local/bin/$(CPU_DAEMON_TARGET)
	rm -f /usr/local/bin/$(KBD_DAEMON_TARGET)
	rm -f /usr/local/bin/$(GUI_TARGET)
	rm -f /etc/systemd/system/dcc-fan-daemon.service
	rm -f /etc/systemd/system/dcc-cpu-daemon.service
	rm -f /etc/systemd/system/dcc-kbd-daemon.service
	rm -f /etc/systemd/system/dcc-hard-limit-enforcer.service
	rm -f /etc/systemd/system/dcc-hard-limit-enforcer.timer
	rm -f /usr/local/bin/dcc-hard-limit-enforce
	for p in $(MAN_PAGES); do rm -f /usr/local/share/man/man$${p##*.}/$$p.gz; done
	rm -f /usr/share/applications/dudes-control-center.desktop
	systemctl daemon-reload
	update-desktop-database >/dev/null 2>&1 || true
	@echo "Uninstallation complete!"

# ---------------------------------------------------------------------------
# Test suite: "make test"
# Builds test binaries with AddressSanitizer + UBSan into $(TEST_BUILD)/ and
# runs tests/run_tests.sh. Needs no root and no hardware: daemons are built
# with relative socket/sysfs/config paths and run in temporary directories.
# ---------------------------------------------------------------------------
# UI translations (1.31.0): po/<lang>.po -> <lang>.mo
UI_LANGS = de es ja ko en_GB
MO_BUILD = build-po
MO_FILES = $(UI_LANGS:%=$(MO_BUILD)/%.mo)

$(MO_BUILD)/%.mo: po/%.po | $(MO_BUILD)
	msgfmt -c -o $@ $<

$(MO_BUILD):
	mkdir -p $@

mo: $(MO_FILES)

# regenerate the .pot and the .po files after GUI texts changed
po-update:
	xgettext --language=C --from-code=UTF-8 --keyword=_ --keyword=N_ --add-comments=TRANSLATORS \
	  --package-name=dudes-control-center -o po/dudes-control-center.pot src/dcc-gui.c
	python3 po/tools/make_po.py

TEST_BUILD = build-test
TEST_CFLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -Isrc
# The current user's group plays the role of "dudes-control-center" in tests
TEST_GROUP := $(shell id -gn)
FAN_DAEMON_DEPS = src/dcc-fan-daemon.c $(COMMON_SRC) $(COMMON_HDR) $(LOG_SRC) $(LOG_HDR) $(SECURE_SRC) $(SECURE_HDR)
KBD_DAEMON_DEPS = src/dcc-kbd-daemon.c src/dcc-kbd-common.c src/dcc-kbd-common.h $(COMMON_HDR) $(LOG_SRC) $(LOG_HDR) $(SECURE_SRC) $(SECURE_HDR)
CPU_DAEMON_DEPS = src/dcc-cpu-daemon.c src/dcc-cpu-common.c src/dcc-cpu-common.h $(LOG_SRC) $(LOG_HDR) $(SECURE_SRC) $(SECURE_HDR)

TEST_BINS = $(TEST_BUILD)/test_fan_responses $(TEST_BUILD)/test_fan_safety \
            $(TEST_BUILD)/test_secure_allow $(TEST_BUILD)/test_secure_deny \
            $(TEST_BUILD)/cpud_test $(TEST_BUILD)/test_gui_save $(TEST_BUILD)/kbdd_test $(TEST_BUILD)/kbdcli_test \
            $(TEST_BUILD)/test_paths $(TEST_BUILD)/test_fan_backend

test: $(TEST_BINS) test-locale
	bash tests/run_tests.sh $(TEST_BUILD)

# catalogs for the GUI test (DCC_LOCALE_DIR=build-test/locale)
test-locale: $(MO_FILES)
	@for l in $(UI_LANGS); do mkdir -p $(TEST_BUILD)/locale/$$l/LC_MESSAGES; \
	  cp $(MO_BUILD)/$$l.mo $(TEST_BUILD)/locale/$$l/LC_MESSAGES/dudes-control-center.mo; done

$(TEST_BUILD):
	mkdir -p $@

$(TEST_BUILD)/test_fan_responses: tests/test_fan_responses.c $(FAN_DAEMON_DEPS) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) $(CJSON_CFLAGS) -DDCC_PCI_DIR='"pci"' -DDCC_NVIDIA_SMI='"./nvidia-smi"' -o $@ tests/test_fan_responses.c $(COMMON_SRC) $(LOG_SRC) $(SECURE_SRC) $(CJSON_LIBS)

$(TEST_BUILD)/test_fan_safety: tests/test_fan_safety.c $(FAN_DAEMON_DEPS) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) $(CJSON_CFLAGS) -DDCC_PCI_DIR='"pci"' -DDCC_NVIDIA_SMI='"./nvidia-smi"' -o $@ tests/test_fan_safety.c $(COMMON_SRC) $(LOG_SRC) $(SECURE_SRC) $(CJSON_LIBS)

$(TEST_BUILD)/test_secure_allow: tests/test_secure.c $(SECURE_SRC) $(SECURE_HDR) $(LOG_SRC) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -DDCC_AUTH_GROUP='"$(TEST_GROUP)"' -o $@ tests/test_secure.c $(SECURE_SRC) $(LOG_SRC)

$(TEST_BUILD)/test_secure_deny: tests/test_secure.c $(SECURE_SRC) $(SECURE_HDR) $(LOG_SRC) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -DDCC_AUTH_GROUP='"dcc-test-no-such-group"' -o $@ tests/test_secure.c $(SECURE_SRC) $(LOG_SRC)

$(TEST_BUILD)/test_fan_backend: tests/test_fan_backend.c $(COMMON_SRC) $(COMMON_HDR) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -DDCC_HWMON_DIR='"hwmon"' -DDCC_PCI_DIR='"pci"' -DDCC_NVIDIA_SMI='"./nvidia-smi"' -o $@ tests/test_fan_backend.c $(COMMON_SRC)

$(TEST_BUILD)/test_paths: tests/test_paths.c $(SECURE_SRC) $(SECURE_HDR) $(LOG_SRC) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -DDCC_USER_FILE='"user"' -o $@ tests/test_paths.c $(SECURE_SRC) $(LOG_SRC)

# The real CPU daemon with relative paths inside its working directory
$(TEST_BUILD)/cpud_test: $(CPU_DAEMON_DEPS) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) $(CJSON_CFLAGS) -DSOCKET_PATH='"cpu.sock"' -DCPU_LOG_DIR='"logs"' \
	  -DCPUSYSFS='"sys"' -DCONTROL_CENTER_CONFIG_DIR='"cfg"' -DDCC_AUTH_GROUP='"$(TEST_GROUP)"' -DDCC_POWERCAP_DIR='"powercap"' \
	  -o $@ src/dcc-cpu-daemon.c src/dcc-cpu-common.c $(LOG_SRC) $(SECURE_SRC) $(CJSON_LIBS)

# The real keyboard daemon against a fake /sys/class/leds ("leds")
# The keyboard CLI against the test daemon's socket "kbd.sock"
$(TEST_BUILD)/kbdcli_test: src/dcc-kbd-cli.c | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -DKBD_SOCKET_PATH='"kbd.sock"' -o $@ src/dcc-kbd-cli.c

$(TEST_BUILD)/kbdd_test: $(KBD_DAEMON_DEPS) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) $(CJSON_CFLAGS) -DKBD_SOCKET_PATH='"kbd.sock"' -DKBD_LOG_DIR='"logs"' \
	  -DKBD_LEDS_DIR='"leds"' -DKBD_STATE_DIR='"state"' -DMAIN_PROFILES_PATH='"cfg/profiles.json"' -DDCC_AUTH_GROUP='"$(TEST_GROUP)"' \
	  -o $@ src/dcc-kbd-daemon.c src/dcc-kbd-common.c $(LOG_SRC) $(SECURE_SRC) $(CJSON_LIBS)

$(TEST_BUILD)/test_gui_save: tests/test_gui_save.c src/dcc-gui.c $(COMMON_SRC) $(LOG_SRC) $(LOG_HDR) | $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -w $(GTK_CFLAGS) $(CJSON_CFLAGS) -DAPP_VERSION='"$(VERSION)"' \
	  -DSOCKET_PATH='"fan.sock"' -DCPU_SOCKET_PATH='"cpu.sock"' -DKBD_SOCKET_PATH='"kbd.sock"' \
	  -DDCC_COMPONENT_DIR='"components"' -DDCC_LAYOUT_DIR='"$(CURDIR)/resources/layouts"' -DDCC_DOC_DIR='"$(CURDIR)"' -DDCC_LOGO_DIR='"$(CURDIR)/resources"' \
	  -DDCC_LOCALE_DIR='"$(CURDIR)/$(TEST_BUILD)/locale"' \
	  -o $@ tests/test_gui_save.c $(COMMON_SRC) $(LOG_SRC) $(GTK_LIBS) $(CJSON_LIBS) -lm

# Man pages render without groff warnings (1.33.0)
man-check:
	@for p in $(MAN_PAGES); do out=$$(groff -man -ww -z man/$$p 2>&1); \
	  if [ -n "$$out" ]; then echo "FAIL  man/$$p: $$out"; exit 1; fi; echo "PASS  man/$$p renders without warnings"; done

# Check the built package: names, units, maintainer scripts (CI runs it after "make deb")
deb-check:
	bash tests/check_deb.sh $(DEB_BUILD_DIR)/$(PACKAGE)_$(VERSION)_amd64.deb

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
	@echo "  test     - Build and run the test suite (sanitizers, no root needed)"
	@echo "  deb      - Build .deb package"
	@echo "  man-check - Check the man pages for groff warnings"
	@echo "  install  - Install all applications and systemd service"
	@echo "  uninstall - Remove installed files"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make          # Build everything"
	@echo "  make deb      # Build .deb package"
	@echo "  sudo make install   # Install to system"
	@echo "  sudo dpkg -i build-deb/$(PACKAGE)_$(VERSION)_amd64.deb  # Install .deb"
