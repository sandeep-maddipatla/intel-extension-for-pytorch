#!/bin/bash
DRIVER_VERSION=${1:-}

if [[ ${IMAGE_NAME} != "" ]]; then
    docker build --build-arg http_proxy=$http_proxy \
                 --build-arg https_proxy=$https_proxy \
                 --build-arg no_proxy=repositories.gfxs.intel.com \
                 --build-arg NO_PROXY=$no_proxy \
                 --build-arg UNAME=ubuntu \
                 --build-arg GID=$(getent group render | sed -E 's,^render:[^:]*:([^:]*):.*$,\1,') \
                 --build-arg UBUNTU_VERSION=22.04 \
                 --build-arg PYTHON=python3.10 \
                 --build-arg ICD_VER=23.30.26918.50-736~22.04 \
                 --build-arg LEVEL_ZERO_GPU_VER=1.3.26918.50-736~22.04 \
                 --build-arg LEVEL_ZERO_VER=1.13.1-719~22.04 \
                 --build-arg LEVEL_ZERO_DEV_VER=1.13.1-719~22.04 \
                 --build-arg DPCPP_VER=2024.0.0-49819 \
                 --build-arg MKL_VER=2024.0.0-49656 \
                 --build-arg TORCH_VERSION=2.0.1a0+cxx11.abi \
                 --build-arg IPEX_VERSION=2.0.110+xpu \
                 --build-arg TORCHVISION_VERSION=0.15.2a0+cxx11.abi \
                 --build-arg TORCH_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg IPEX_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg TORCHVISION_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg CCL_VER=2021.11.0-49156 \
                 --build-arg ONECCL_BIND_PT_VERSION=2.1.100 \
                 --build-arg ONECCL_BIND_PT_WHL_URL=https://pytorch-extension.intel.com/release-whl/stable/xpu/us/ \
                 --build-arg IGFX_VERSION=${DRIVER_VERSION} \
                 -t ${IMAGE_NAME} \
                 -f docker/Dockerfile.compile .
fi
