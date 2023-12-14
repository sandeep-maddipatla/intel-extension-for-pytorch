#!/bin/bash

if [ $# -eq 0 ]; then
    echo "Usage: bash $0 <MODE> <optional-driver-version>"
    echo "MODE: \"driver\" for installing required driver packages."
    echo "      \"dev\" for installing required packages for compilation."
    echo "      \"runtime\" for installing required packages for runtime."
    echo "optional-driver-version: Only applicable to Ubuntu 22.04 (Jammy) env"
    echo "      default (blank input) to use latest internal stable"
    echo "      \"agama/<ver-number>\" to use a particular agama version."
    echo "          e.g. \"agama/799\" for agama-799"
    echo "      \"production\" to use latest internal production version"
    echo "      \"production/<ver-number>\" to use a particular version of internal production driver."
    echo "          e.g. \"production/2328.38\" for that specific production driver"
    echo "      \"testing/<ver-number>\" to use a particular version of internal testing driver."
    echo "          e.g. \"production/2344.12\" for that specific testing driver"
    echo "  All options support only \"unified\" component"
    echo "  All options referenced in https://repositories.gfxs.intel.com/"
    exit 1
fi
MODE=$1

if [ "${MODE}" != "driver" ] &&
   [ "${MODE}" != "dev" ] &&
   [ "${MODE}" != "runtime" ]; then
    echo "MODE \"${MODE}\" not supported."
    echo "MODE: \"driver\" for installing required driver packages."
    echo "      \"dev\" for installing required packages for compilation."
    echo "      \"runtime\" for installing required packages for runtime."
    exit 2
fi

DRIVER_VERSION=$2
if [ ! -z $DRIVER_VERSION ];then
    DRIVER_VERSION=jammy/${DRIVER_VERSION}
fi

SUDO="null"
if [ $UID -ne 0 ]; then
    SUDO="sudo"
fi

OS_ID=""
OS_VERSION=""
while read line
do
    KEY=$(echo ${line} | cut -d '=' -f 1)
    VAL=$(echo ${line} | cut -d '=' -f 2)
    if [ "${KEY}" = "ID" ]; then
        OS_ID=${VAL}
    fi
    if [ "${KEY}" = "VERSION_ID" ]; then
        OS_VERSION=${VAL}
    fi
done < <(cat /etc/os-release)

function add-repo-driver() {
    SUDO=$1
    OS_ID=$2
    OS_VERSION=$3
    if [ "${SUDO}" = "null" ]; then
        SUDO=""
    fi

    #need to carry proxy settings to sudo to allow internal driver install
    if [ "${SUDO}" = "sudo" ]; then
        SUDO="sudo -E"
    fi

    if [ "${OS_ID}" = "ubuntu" ]; then
        LEVEL_STRING=${4:-jammy}
        COMP_STRING=${5:-unified}
        wget -qO - https://repositories.gfxs.intel.com/repositories/intel-graphics.key | ${SUDO} gpg --dearmor --output /usr/share/keyrings/intel-graphics.gpg
        echo "deb [arch=amd64 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.gfxs.intel.com/repositories/ubuntu ${LEVEL_STRING} ${COMP_STRING}" | ${SUDO} tee /etc/apt/sources.list.d/intel-gpu-jammy.list
        ${SUDO} apt update
    else
        if [ ! -z $4 || ! -z $5 ]; then
            echo "Warning: ignoring args $4 $5.Supported only for OS_ID=ubuntu"
        fi
    fi
    if [ "${OS_ID}" = "\"rhel\"" ] || [ "${OS_ID}" = "\"centos\"" ]; then
        if [ "${OS_VERSION}" = "\"8\"" ] || [ "${OS_VERSION}" = "\"8.6\"" ]; then
            ${SUDO} dnf install -y 'dnf-command(config-manager)'
            ${SUDO} dnf config-manager --add-repo https://repositories.intel.com/gpu/rhel/8.6/unified/intel-gpu-8.6.repo
        fi
        if [ "${OS_VERSION}" = "\"8.8\"" ]; then
            ${SUDO} dnf install -y 'dnf-command(config-manager)'
            ${SUDO} dnf config-manager --add-repo https://repositories.intel.com/gpu/rhel/8.8/unified/intel-gpu-8.8.repo
        fi
        if [ "${OS_VERSION}" = "\"9\"" ] || [ "${OS_VERSION}" = "\"9.0\"" ]; then
            ${SUDO} dnf install -y 'dnf-command(config-manager)'
            ${SUDO} dnf config-manager --add-repo https://repositories.intel.com/gpu/rhel/9.0/unified/intel-gpu-9.0.repo
        fi
        if [ "${OS_VERSION}" = "\"9.2\"" ]; then
            ${SUDO} dnf install -y 'dnf-command(config-manager)'
            ${SUDO} dnf config-manager --add-repo https://repositories.intel.com/gpu/rhel/9.2/unified/intel-gpu-9.2.repo
        fi
    fi
}

function add-repo-basekit() {
    SUDO=$1
    OS_ID=$2
    OS_VERSION=$3
    if [ "${SUDO}" = "null" ]; then
        SUDO=""
    fi

    #need to carry proxy settings to sudo to allow internal driver install
    #this is meant for oneapi, but need driver settings to avoid connection errors
    if [ "${SUDO}" = "sudo" ]; then
        SUDO="sudo -E"
    fi

    if [ "${OS_ID}" = "ubuntu" ]; then
        wget -O- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB | gpg --dearmor | ${SUDO} tee /usr/share/keyrings/oneapi-archive-keyring.gpg > /dev/null
        echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" | ${SUDO} tee /etc/apt/sources.list.d/oneAPI.list
        ${SUDO} apt update
    fi
    if [ "${OS_ID}" = "\"rhel\"" ] || [ "${OS_ID}" = "\"centos\"" ]; then
        tee > /tmp/oneAPI.repo << EOF
[oneAPI]
name=Intel® oneAPI repository
baseurl=https://yum.repos.intel.com/oneapi
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://yum.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
EOF
        ${SUDO} mv /tmp/oneAPI.repo /etc/yum.repos.d
    fi
}

function install-driver() {
    SUDO=$1
    OS_ID=$2
    OS_VERSION=$3
    LEVEL_STRING=${4:-jammy}
    COMP_STRING=${5:-unified} #only unified is supported

    add-repo-driver ${SUDO} ${OS_ID} ${OS_VERSION} ${LEVEL_STRING} ${COMP_STRING}
    if [ "${SUDO}" = "null" ]; then
        SUDO=""
    fi

    if [ "${OS_ID}" = "ubuntu" ]; then
        ${SUDO} apt update
        ${SUDO} apt install -y \
        intel-opencl-icd \
        level-zero \
        level-zero-dev \
        intel-level-zero-gpu \
        xpu-smi

        rm -f /opt/DriverPackages.log && \
            echo "LEVEL_STRING=${LEVEL_STRING}" >> /opt/DriverPackages.log && \
            echo "COMP_STRING=${COMP_STRING}" >> /opt/DriverPackages.log && \
            apt list -a intel-opencl-icd | tee -a /opt/DriverPackages.log && \
            apt list -a intel-level-zero-gpu | tee -a /opt/DriverPackages.log && \
            apt list -a level-zero | tee -a /opt/DriverPackages.log && \
            apt list -a level-zero-dev | tee -a /opt/DriverPackages.log && \
            apt list -a xpu-smi | tee -a /opt/DriverPackages.log
    else
        if [ ! -z $4 || ! -z $5 ]; then
            echo "Warning: ignoring args $4 $5.Supported only for OS_ID=ubuntu"
        fi
    fi
    if [ "${OS_ID}" = "\"rhel\"" ] || [ "${OS_ID}" = "\"centos\"" ]; then
        ${SUDO} dnf install -y intel-opencl-23.30.26918.50 \
        level-zero-1.13.1 \
        level-zero-devel-1.13.1 \
        intel-level-zero-gpu-1.3.26918.50 \
        intel-ocloc-23.30.26918.50 \
        xpu-smi-1.2.22
    fi
}

function install-dev() {
    SUDO=$1
    OS_ID=$2
    OS_VERSION=$3
    add-repo-basekit ${SUDO} ${OS_ID} ${OS_VERSION}
    if [ "${SUDO}" = "null" ]; then
        SUDO=""
    fi

    if [ "${OS_ID}" = "ubuntu" ]; then
        ${SUDO} apt update
        ${SUDO} apt install -y intel-level-zero-gpu-dev \
        intel-oneapi-dpcpp-cpp-2024.0 \
        intel-oneapi-mkl-devel=2024.0.0-49656 \
        intel-oneapi-ccl-devel=2021.11.1-6

        # Log version for intel-level-zero-gpu-dev
        apt list -a intel-level-zero-gpu-dev | ${SUDO} tee -a /opt/DriverPackages.log
    fi
    if [ "${OS_ID}" = "\"rhel\"" ] || [ "${OS_ID}" = "\"centos\"" ]; then
        ${SUDO} dnf install -y intel-level-zero-gpu-devel-1.3.26918.50 \
        intel-oneapi-dpcpp-cpp-2024.0 \
        intel-oneapi-mkl-devel-2024.0.0-49656 \
        intel-oneapi-ccl-devel-2021.11.1-6
    fi
}

function install-runtime() {
    SUDO=$1
    OS_ID=$2
    OS_VERSION=$3
    add-repo-basekit ${SUDO} ${OS_ID} ${OS_VERSION}
    if [ "${SUDO}" = "null" ]; then
        SUDO=""
    fi

    if [ "${OS_ID}" = "ubuntu" ]; then
        ${SUDO} apt update
        ${SUDO} apt install -y intel-oneapi-runtime-dpcpp-cpp=2024.0.0-49819 \
        intel-oneapi-runtime-mkl=2024.0.0-49656 \
        intel-oneapi-runtime-ccl=2021.11.1-6
    fi
    if [ "${OS_ID}" = "\"rhel\"" ] || [ "${OS_ID}" = "\"centos\"" ]; then
        ${SUDO} dnf install -y intel-oneapi-runtime-dpcpp-cpp-2024.0.0-49819 \
        intel-oneapi-runtime-mkl-2024.0.0-49656 \
        intel-oneapi-runtime-ccl-2021.11.1-6
    fi
}
if [ "${MODE}" = "driver" ]; then
    install-driver ${SUDO} ${OS_ID} ${OS_VERSION} ${DRIVER_VERSION}
fi
if [ "${MODE}" = "dev" ]; then
    install-dev ${SUDO} ${OS_ID} ${OS_VERSION}
fi
if [ "${MODE}" = "runtime" ]; then
    install-runtime ${SUDO} ${OS_ID} ${OS_VERSION}
fi
