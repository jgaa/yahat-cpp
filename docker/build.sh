#!/bin/bash
set -e  # Exit immediately if any command exits with a non-zero status

# Check for the '--push' flag among the command-line arguments.
PUSH_IMAGES=false
for arg in "$@"; do
  if [ "$arg" == "--push" ]; then
    PUSH_IMAGES=true
    break
  fi
done

image_repro="ghcr.io/jgaa"
target="yahatchat"
# Extract version from CMakeLists.txt (e.g., v1.2.3)
version="v$(grep ' set(YAHAT_VERSION' CMakeLists.txt | xargs | cut -f 2 -d ' ' | cut -f1 -d')')"

if [ -z "${REGISTRY+x}" ]; then
    target_image="${image_repro}/${target}"
else
    target_image="${REGISTRY}/${target}"
fi
version_tag="${target_image}:${version}"

# Build the builder image.
docker build -f docker/Build.dockerfile -t build docker

# Create a temporary artifacts directory.
artifacts_dir="/tmp/artifacts-$(uuid)"
mkdir -pv "${artifacts_dir}"
chmod 777 "${artifacts_dir}"

# Ensure cleanup always happens
cleanup() {
  echo "Cleaning up artifacts directory: ${artifacts_dir}"
  rm -rfv "${artifacts_dir}"
}
trap cleanup EXIT

# Determine if a TTY is available.
if [ -t 0 ]; then
  tty_flags="-it"
else
  tty_flags=""
fi

# Run the build inside the builder container and copy the binary to artifacts_dir.
docker run --rm ${tty_flags} \
  -v "$(pwd)":/src:ro \
  -v "${artifacts_dir}":/artifacts \
  build bash -c 'cd && mkdir build && cd build && \
    cmake -S /src -DCMAKE_BUILD_TYPE=Release -DYAHAT_WITH_EXAMPLES=ON -DYAHAT_WITH_TESTS=OFF -DYAHAT_STATIC_BOOST=ON -G Ninja && \
    cmake --build . && cp -v bin/yahatchat /artifacts'

# Build the final image from the artifacts directory and tag it.
docker build -f docker/App.dockerfile -t "${target_image}" "${artifacts_dir}"
docker tag "${target_image}" "${version_tag}"

# Only push if the '--push' flag was provided.
if [ "${PUSH_IMAGES}" = true ]; then
  echo "Pushing images..."
  docker push "${target_image}"
  docker push "${version_tag}"
else
  echo "Skipping push. Use '--push' to push images."
fi
