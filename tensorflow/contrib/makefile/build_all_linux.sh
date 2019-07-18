#!/usr/bin/env bash
# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
# Downloads and builds all of TensorFlow's dependencies for Linux, and compiles
# the TensorFlow library itself. Supported on Ubuntu 14.04 and 16.04.

set -e

# Make sure we're in the correct directory, at the root of the source tree.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd ${SCRIPT_DIR}/../../../

source "${SCRIPT_DIR}/build_helper.subr"
JOB_COUNT="${JOB_COUNT:-$(get_job_count)}"

if [ "$SHOULD_CLEAN" == "ON" ]; then
    echo "Cleaning old files."
    # Remove any old files first.
    make -f tensorflow/contrib/makefile/Makefile clean
    rm -rf tensorflow/contrib/makefile/downloads
fi

if [ "$DOWNLOAD_DEPENDENCIES" == "ON" ]; then
    # Pull down the required versions of the frameworks we need.
    USE_SYSTEM_PROTOBUF="$USE_SYSTEM_PROTOBUF" tensorflow/contrib/makefile/download_dependencies.sh
fi

# Compile nsync.
# Don't use  export var=`something` syntax; it swallows the exit status.
if [ "$BUILD_SHARED" == "ON" ]; then
    export NSYNC_CPPFLAGS=-fPIC
else
    BUILD_SHARED=OFF
fi

HOST_NSYNC_LIB=`tensorflow/contrib/makefile/compile_nsync.sh`
TARGET_NSYNC_LIB="$HOST_NSYNC_LIB"
export HOST_NSYNC_LIB TARGET_NSYNC_LIB

if [ "$USE_SYSTEM_PROTOBUF" != "ON" ]; then
    echo "Using downloaded protobuf"
    # Compile protobuf.
    tensorflow/contrib/makefile/compile_linux_protobuf.sh
else
    echo "Using system protobuf"
fi

if [ -z "$OPTFLAGS" ]; then
    OPTFLAGS="-O3 -march=native"
fi


echo "Optimization flags: $OPTFLAGS"

# Build TensorFlow.
make -j"${JOB_COUNT}" -f tensorflow/contrib/makefile/Makefile \
  OPTFLAGS="$OPTFLAGS" \
  HOST_CXXFLAGS="--std=c++11 -march=native" \
  MAKEFILE_DIR=$SCRIPT_DIR \
  BUILD_SHARED=$BUILD_SHARED
