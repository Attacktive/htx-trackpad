NAME = htx-trackpad
LABEL = xyz.attacktive.$(NAME)
PREFIX ?= $(HOME)/.local
SPEED ?= 1.0
VERBOSE ?= 0
BINARY = build/$(NAME)
INSTALLED_BINARY = $(PREFIX)/bin/$(NAME)
PLIST = $(HOME)/Library/LaunchAgents/$(LABEL).plist
LOG = $(HOME)/Library/Logs/$(NAME).log
DOMAIN = gui/$(shell id -u)
# A real signing identity gives macOS a designated requirement that survives rebuilds; ad-hoc ("-") pins permissions to one exact build.
SIGN_IDENTITY ?= $(shell security find-identity -v -p codesigning | awk 'NR == 1 && $$2 ~ /^[0-9A-F]+$$/ { print $$2 }')
SIGNING = $(or $(SIGN_IDENTITY),-)
ifeq ($(VERBOSE),1)
VERBOSE_SUBSTITUTION = -e 's|@VERBOSE@|--verbose|'
else
VERBOSE_SUBSTITUTION = -e '/@VERBOSE@/d'
endif

.PHONY: build install uninstall restart log clean

build: $(BINARY)

$(BINARY): src/$(NAME).c Makefile
	mkdir -p build
	clang -O2 -Wall -Wextra -Wno-unused-parameter -o $@ $< -framework IOKit -framework CoreFoundation -framework ApplicationServices
	codesign --force --sign $(SIGNING) --identifier $(LABEL) $@

install: build
	install -d $(PREFIX)/bin
	install -m 755 $(BINARY) $(INSTALLED_BINARY)
	sed -e 's|@LABEL@|$(LABEL)|' -e 's|@BINARY@|$(INSTALLED_BINARY)|' -e 's|@SPEED@|$(SPEED)|' $(VERBOSE_SUBSTITUTION) -e 's|@LOG@|$(LOG)|g' launchd/$(LABEL).plist.in > $(PLIST)
	-launchctl bootout $(DOMAIN)/$(LABEL) 2>/dev/null
	launchctl bootstrap $(DOMAIN) $(PLIST)

uninstall:
	-launchctl bootout $(DOMAIN)/$(LABEL) 2>/dev/null
	rm -f $(PLIST) $(INSTALLED_BINARY)

restart:
	launchctl kickstart -k $(DOMAIN)/$(LABEL)

log:
	tail -n 30 $(LOG)

clean:
	rm -rf build
