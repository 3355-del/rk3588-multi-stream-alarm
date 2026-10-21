#!/bin/sh
# Build an isolated, pinned upstream MPP without installing system-wide.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
revision=14729dd578e570e5f00fd1dd2113f5429012d64b
mkdir -p "$project_dir/vendor"
source_dir="$project_dir/vendor/mpp"
if [ ! -d "$source_dir/.git" ]; then
    git clone --depth 1 https://github.com/rockchip-linux/mpp.git "$source_dir"
fi
if [ -n "$(git -C "$source_dir" status --porcelain)" ]; then
    echo 'MPP checkout has local changes; preserve them before rebuilding.' >&2
    exit 1
fi
if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$source_dir" fetch --depth 1 origin "$revision"
fi
git -C "$source_dir" checkout --detach "$revision"
cmake -S "$source_dir" -B "$project_dir/vendor/mpp-build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$project_dir/vendor/mpp-install"
cmake --build "$project_dir/vendor/mpp-build" -j 4
cmake --install "$project_dir/vendor/mpp-build"
cmake -S "$project_dir" -B "$project_dir/build" -DMSAD_ENABLE_RKNN=ON \
    -DMSAD_MPP_ROOT="$project_dir/vendor/mpp-install"
cmake --build "$project_dir/build" -j 4
