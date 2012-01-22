hardware_modules := gralloc hwcomposer nfc
include $(call all-named-subdir-makefiles,$(hardware_modules))
