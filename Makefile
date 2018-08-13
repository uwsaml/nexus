CONFIG_FILE := Makefile.config
ifeq ($(wildcard $(CONFIG_FILE)),)
	$(error $(CONFIG_FILE) not found. See $(CONFIG_FILE).example.)
endif
include $(CONFIG_FILE)

ROOTDIR = $(CURDIR)

# protobuf srcs and objs
PROTO_SRC_DIR = src/nexus/proto
PROTO_SRCS := $(PROTO_SRC_DIR)/nnquery.proto
PROTO_GEN_HEADERS := ${PROTO_SRCS:src/%.proto=build/gen/%.pb.h}
PROTO_GEN_CC := ${PROTO_SRCS:src/%.proto=build/gen/%.pb.cc}
PROTO_OBJS := ${PROTO_SRCS:src/nexus/%.proto=build/obj/%.pb.o}
# gen python code
PROTO_GEN_PY_DIR = python/nexus/proto
PROTO_GEN_PY := $(patsubst $(PROTO_SRC_DIR)/%.proto, $(PROTO_GEN_PY_DIR)/%_pb2.py, $(PROTO_SRCS))
# grpc protobuf
GRPC_PROTO_SRCS := $(PROTO_SRC_DIR)/control.proto
PROTO_GEN_HEADERS += ${GRPC_PROTO_SRCS:src/%.proto=build/gen/%.pb.h} \
	${GRPC_PROTO_SRCS:src/%.proto=build/gen/%.grpc.pb.h}
PROTO_GEN_CC += ${GRPC_PROTO_SRCS:src/%.proto=build/gen/%.pb.cc} \
	${GRPC_PROTO_SRCS:src/%.proto=build/gen/%.grpc.pb.cc}
PROTO_OBJS += ${GRPC_PROTO_SRCS:src/nexus/%.proto=build/obj/%.pb.o} \
	${GRPC_PROTO_SRCS:src/nexus/%.proto=build/obj/%.grpc.pb.o}

# protoc config
PROTOC = `which protoc`
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

# c++ srcs and objs
CXX_COMMON_SRCS := $(wildcard src/nexus/common/*.cpp)
CXX_APP_SRCS := $(wildcard src/nexus/app/*.cpp)
CXX_BACKEND_SRCS := $(wildcard src/nexus/backend/*.cpp)
CXX_SCHEDULER_SRCS := $(wildcard src/nexus/scheduler/*.cpp)
CXX_LIB_SRCS := $(shell find src ! -name "*_main.cpp" -name "*.cpp")
CXX_TEST_SRCS := $(wildcard tests/cpp/*.cpp) $(wildcard tests/cpp/*/*.cpp)
CXX_TOOL_SRCS := $(wildcard tools/*/*.cpp)

CXX_COMMON_OBJS := $(patsubst src/nexus/%.cpp, build/obj/%.o, $(CXX_COMMON_SRCS)) $(PROTO_OBJS)
CXX_APP_OBJS := $(patsubst src/nexus/%.cpp, build/obj/%.o, $(CXX_APP_SRCS))
CXX_BACKEND_OBJS := $(patsubst src/nexus/%.cpp, build/obj/%.o, $(CXX_BACKEND_SRCS))
CXX_SCHEDULER_OBJS := $(patsubst src/nexus/%.cpp, build/obj/%.o, $(CXX_SCHEDULER_SRCS))
CXX_LIB_OBJS := $(patsubst src/nexus/%.cpp, build/obj/%.o, $(CXX_LIB_SRCS)) $(PROTO_OBJS)
CXX_TEST_OBJS := $(patsubst tests/cpp/%.cpp, build/obj/tests/%.o, $(CXX_TEST_SRCS))
CXX_TOOL_OBJS := $(patsubst %.cpp, build/obj/%.o, $(CXX_TOOL_SRCS))
OBJS := $(CXX_COMMON_OBJS) $(CXX_APP_OBJS) $(CXX_BACKEND_OBJS) $(CXX_SCHEDULER_OBJS) \
	$(CXX_TEST_OBJS) $(CXX_TOOL_OBJS)
DEPS := ${OBJS:.o=.d}

TEST_BIN := build/bin/test

# c++ configs
CXX = g++
WARNING = -Wall -Wfatal-errors -Wno-unused -Wno-unused-result
CXXFLAGS = -std=c++11 -O3 -fPIC $(WARNING) -Isrc -Ibuild/gen `pkg-config --cflags protobuf`
# Automatic dependency generation
CXXFLAGS += -MMD -MP
LD_FLAGS = -lm -pthread -lglog -lgflags -lgtest -lgtest_main \
	-lboost_system -lboost_thread -lboost_filesystem -lyaml-cpp \
	`pkg-config --libs protobuf` `pkg-config --libs grpc++ grpc` \
	`pkg-config --libs opencv`
DLL_LINK_FLAGS = -shared
ifeq ($(USE_GPU), 1)
	CXXFLAGS += -I$(CUDA_PATH)/include
	LD_FLAGS += -L$(CUDA_PATH)/lib64 -lcuda -lcudart -lcurand
endif

# library dependency
DARKNET_ROOT_DIR = $(ROOTDIR)/frameworks/darknet
DARKNET_BUILD_DIR = $(DARKNET_ROOT_DIR)
CAFFE_ROOT_DIR = $(ROOTDIR)/frameworks/caffe
CAFFE_BUILD_DIR = $(CAFFE_ROOT_DIR)/build
CAFFE2_ROOT_DIR = $(ROOTDIR)/frameworks/caffe2
CAFFE2_BUILD_DIR = $(CAFFE2_ROOT_DIR)/build
CAFFE2_INSTALL_DIR = $(CAFFE2_ROOT_DIR)/install
TENSORFLOW_ROOT_DIR = $(ROOTDIR)/frameworks/tensorflow
TENSORFLOW_BUILD_DIR = $(TENSORFLOW_ROOT_DIR)/build

BACKEND_DEPS =
BACKEND_CXXFLAGS = 
BACKEND_LD_FLAGS = 

ifeq ($(USE_CAFFE2), 1)
	BACKEND_DEPS += caffe2
	BACKEND_CXXFLAGS += -DUSE_CAFFE2 -I$(CAFFE2_INSTALL_DIR)/include
	BACKEND_LD_FLAGS += -L$(CAFFE2_INSTALL_DIR)/lib -lcaffe2 -lcaffe2_gpu -Wl,-rpath,$(CAFFE2_INSTALL_DIR)/lib
	USE_CAFFE = 0
endif
ifeq ($(USE_CAFFE), 1)
	BACKEND_DEPS += caffe
	BACKEND_CXXFLAGS += -DUSE_CAFFE -I$(CAFFE_ROOT_DIR)/include -I$(CAFFE_BUILD_DIR)/src
	BACKEND_LD_FLAGS += -L$(CAFFE_BUILD_DIR)/lib -lcaffe -Wl,-rpath,$(CAFFE_BUILD_DIR)/lib
endif
ifeq ($(USE_DARKNET), 1)
	BACKEND_DEPS += darknet
	BACKEND_CXXFLAGS += -DUSE_DARKNET -I$(DARKNET_ROOT_DIR)/src -I$(DARKNET_ROOT_DIR)/include
	BACKEND_LD_FLAGS += -L$(DARKNET_BUILD_DIR) -ldarknet -Wl,-rpath,$(DARKNET_BUILD_DIR)
endif
ifeq ($(USE_TENSORFLOW), 1)
	export PKG_CONFIG_PATH:=$(TENSORFLOW_BUILD_DIR)/lib/pkgconfig:${PKG_CONFIG_PATH}
	BACKEND_DEPS += tensorflow
	BACKEND_CXXFLAGS += -DUSE_TENSORFLOW `pkg-config --cflags tensorflow`
	BACKEND_LD_FLAGS += `pkg-config --libs tensorflow`
endif

all: proto python lib backend scheduler tools

caffe: $(CAFFE_BUILD_DIR)/lib/libcaffe.so
caffe2: $(CAFFE2_INSTALL_DIR)/lib/libcaffe2_gpu.so
darknet: $(DARKNET_BUILD_DIR)/libdarknet.so
tensorflow: $(TENSORFLOW_BUILD_DIR)/lib/tensorflow/libtensorflow_cc.so

$(CAFFE_BUILD_DIR)/lib/libcaffe.so:
	cd $(CAFFE_ROOT_DIR) && $(MAKE) proto && $(MAKE) all && $(MAKE) pycaffe && cd -

$(CAFFE2_INSTALL_DIR)/lib/libcaffe2_gpu.so:
	@mkdir -p $(CAFFE2_BUILD_DIR)
	@cd $(CAFFE2_BUILD_DIR) && \
	cmake .. -DUSE_NNPACK=OFF -DUSE_NCCL=OFF -DCMAKE_INSTALL_PREFIX=../install \
	&& $(MAKE) && $(MAKE) install

$(DARKNET_BUILD_DIR)/libdarknet.so:
	cd $(DARKNET_ROOT_DIR) && $(MAKE) all && cd -

$(TENSORFLOW_BUILD_DIR)/lib/tensorflow/libtensorflow_cc.so:
	@mkdir -p $(TENSORFLOW_BUILD_DIR)
	cd $(TENSORFLOW_BUILD_DIR) && cmake -DCMAKE_INSTALL_PREFIX=. .. && make
	@if [ -e $(TENSORFLOW_ROOT_DIR)/bazel-bin/tensorflow/libtensorflow_cc.so ]; then \
	 	cd $(TENSORFLOW_BUILD_DIR) && make install; \
	else \
		echo "Build Tensorflow failed"; exit 0; \
	fi

proto: $(PROTO_GEN_CC)

python: $(PROTO_GEN_PY)

lib: build/lib/libnexus.so

backend: build/bin/backend

scheduler: build/bin/scheduler

tools: build/bin/profiler

test: build/bin/runtest

runtest: test
	@build/bin/runtest -model_root $(ROOTDIR)/tests/data/model_db

build/lib/libnexus.so: $(CXX_COMMON_OBJS) $(CXX_APP_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(DLL_LINK_FLAGS) -o $@ $^ $(LD_FLAGS)

build/bin/backend: $(CXX_COMMON_OBJS) $(CXX_BACKEND_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LD_FLAGS) $(BACKEND_LD_FLAGS)

build/bin/scheduler: $(CXX_COMMON_OBJS) $(CXX_SCHEDULER_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LD_FLAGS)

build/bin/profiler: $(CXX_LIB_OBJS) build/obj/tools/profiler/profiler.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LD_FLAGS) $(BACKEND_LD_FLAGS)

build/bin/runtest: $(CXX_TEST_OBJS) $(CXX_LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LD_FLAGS) $(BACKEND_LD_FLAGS)

build/gen/%.pb.cc build/gen/%.pb.h: src/%.proto
	@mkdir -p $(@D)
	$(PROTOC) -I$(PROTO_SRC_DIR) --cpp_out=$(@D) $<

build/gen/%.grpc.pb.cc build/gen/%.grpc.pb.h: src/%.proto | build/gen/%.pb.h
	@mkdir -p $(@D)
	$(PROTOC) -I$(PROTO_SRC_DIR) --grpc_out=$(@D) --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<

$(PROTO_GEN_PY_DIR)/%_pb2.py: $(PROTO_SRC_DIR)/%.proto
	@mkdir -p $(@D)
	touch $(@D)/__init__.py
	$(PROTOC) --proto_path=$(PROTO_SRC_DIR) --python_out=$(@D) $<

build/obj/%.pb.o: build/gen/nexus/%.pb.cc build/gen/nexus/%.pb.h
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/backend/%.o: src/nexus/backend/%.cpp | $(PROTO_GEN_HEADERS) $(BACKEND_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(BACKEND_CXXFLAGS) -c $< -o $@

build/obj/%.o: src/nexus/%.cpp | $(PROTO_GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/tools/%.o: tools/%.cpp | $(PROTO_GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(BACKEND_CXXFLAGS) -c $< -o $@

build/obj/tests/%.o: tests/cpp/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(BACKEND_CXXFLAGS) -c $< -o $@

.PRECIOUS: $(PROTO_GEN_HEADERS) $(PROTO_GEN_CC)

.PHONY: proto python lib backend scheduler tools test runtest \
	darknet caffe tensorflow \
	clean clean-darknet clean-caffe clean-caffe2 clean-tensorflow cleanall

clean:
	rm -rf build $(PROTO_GEN_PY_DIR) $(PROTO_GEN_CC) $(PROTO_GEN_HEADERS)

clean-darknet:
	cd $(DARKNET_ROOT_DIR) && $(MAKE) clean && cd -

clean-caffe:
	cd $(CAFFE_ROOT_DIR) && $(MAKE) clean && cd -

clean-caffe2:
	rm -rf $(CAFFE2_BUILD_DIR) $(CAFFE2_INSTALL_DIR)

clean-tensorflow:
	rm -rf $(TENSORFLOW_BUILD_DIR) $(TENSORFLOW_ROOT_DIR)/.tf_configure.bazelrc $(TENSORFLOW_ROOT_DIR)/bazel-*

cleanall: clean clean-darknet clean-caffe clean-caffe2 clean-tensorflow

-include $(DEPS)
