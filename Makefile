#
# simple makefile!
#

TARGET := objshape
CSRC := $(shell find . -iname '*.c')
LIBS := 
CFLAGS := -O2 -pipe -Wall -Wextra -Werror -std=gnu99
LDFLAGS := 

include common.mk
