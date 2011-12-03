#Enables the listed display HAL modules
display-hals := libgralloc libcopybit
include $(call all-named-subdir-makefiles,$(display-hals))

