#Enables the listed display HAL modules
<<<<<<< HEAD
display-hals := libgralloc libgenlock libcopybit
=======
display-hals := libhwcomposer liboverlay libgralloc libcopybit
>>>>>>> parent of 710fb87... Add a generic buffer locking API.
include $(call all-named-subdir-makefiles,$(display-hals))

