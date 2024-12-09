MAC_CC = clang
IOS_CC = xcrun --sdk iphoneos clang

IOS_CODESIGN = codesign
IOS_ENT = ent.plist

MAC_ARCH = -arch arm64 -arch x86_64
IOS_ARCH = -arch armv7 -arch arm64

CFLAGS = -Os
LDFLAGS = -framework IOKit -framework CoreFoundation

IOS_CFLAGS = -miphoneos-version-min=6.0
IOS_CFLAGS += -Iinclude
IOS_IOKIT_LINK = ln -fsh $(shell xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Versions/Current/Headers ./include/IOKit

MAC_CFLAGS = -mmacos-version-min=10.8

IRECOVERY_SRC = irecovery.c lilirecovery.c
IRECOVERY_IOS_BUILD = build/irecovery_ios
IRECOVERY_MAC_BUILD = build/irecovery

DIR_HELPER = mkdir -p $(@D)

.PHONY: all ios mac clean

all: ios mac
	@echo "done!"

ios: $(IRECOVERY_IOS_BUILD)

mac: $(IRECOVERY_MAC_BUILD)

$(IRECOVERY_IOS_BUILD): $(IRECOVERY_SRC)
	@echo "\tbuilding irecovery for iOS"
	@$(DIR_HELPER)
	@$(IOS_IOKIT_LINK)
	@$(IOS_CC) $(IOS_ARCH) $(IOS_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@
	@$(IOS_CODESIGN) -s - -f --entitlements $(IOS_ENT) $@

$(IRECOVERY_MAC_BUILD): $(IRECOVERY_SRC)
	@echo "\tbuilding irecovery for Mac"
	@$(DIR_HELPER)
	@$(MAC_CC) $(MAC_ARCH) $(MAC_CFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@

clean:
	@rm -rf build
	@echo "done cleaning!"
