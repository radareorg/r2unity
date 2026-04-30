R2UNITY_CORE_WD=$(LIBR)/xps/p/r2unity
CFLAGS+=-I$(R2UNITY_CORE_WD)/src/lib
R2UNITY_CORE_OBJ= \
	$(R2UNITY_CORE_WD)/src/lib/elf.o \
	$(R2UNITY_CORE_WD)/src/lib/lib.o \
	$(R2UNITY_CORE_WD)/src/lib/macho.o \
	$(R2UNITY_CORE_WD)/src/lib/paths.o \
	$(R2UNITY_CORE_WD)/src/lib/pe.o \
	$(R2UNITY_CORE_WD)/src/r2/core_r2unity.o
EXTERNAL_STATIC_OBJS+=$(R2UNITY_CORE_OBJ)
