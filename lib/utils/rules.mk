include mk/subdir_pre.mk

LIB_UTILS_OBJS := $(addprefix $(d)/, \
  rng.o timeout.o reorder.o utils.o)
LIB_UTILS_SOBJS := $(LIB_UTILS_OBJS:.o=.shared.o)

$(LIB_UTILS_OBJS): CPPFLAGS += $(DPDK_CPPFLAGS)
$(LIB_UTILS_OBJS): CFLAGS += $(DPDK_CFLAGS)
$(LIB_UTILS_SOBJS): CPPFLAGS += $(DPDK_CPPFLAGS)
$(LIB_UTILS_SOBJS): CFLAGS += $(DPDK_CFLAGS)

DEPS += $(LIB_UTILS_OBJS:.o=.d) $(LIB_UTILS_SOBJS:.o=.d)
CLEAN += $(LIB_UTILS_OBJS) $(LIB_UTILS_SOBJS)

include mk/subdir_post.mk
