# SPDX-FileCopyrightText: 2024 Howetuft
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

wheel=$1
dest_dir=$2
VCToolsRedistDir=`cygpath -u "$3"`
workspace=`cygpath -u "$4"`


echo "Repairing:"
echo "- wheel=${wheel}"
echo "- dest_dir=${dest_dir}"
echo "- VCToolsRedistDir=${VCToolsRedistDir}"
echo "- workspace=${workspace}"

pip install delvewheel

# Find system folders
# (list folders, enclose in double quotes and concat)
redist_paths=`find "${VCToolsRedistDir}" -type d | paste -s -d ":"`

echo "Paths: ${redist_paths}"

# Compute dependency paths
base="$workspace/out/dependencies/full_deploy/host"
# workspace is already absolute. MSYS xargs can abort while computing its
# argument budget in cibuildwheel's large environment, losing every DLL path.
# Keep path collection independent of that budget and stop on repair errors.
paths=$(find "$base" -type d -wholename "*/bin" -print | paste -s -d ":")

# Repair wheel
delvewheel repair -v \
  --add-path="$GITHUB_WORKSPACE/libs" \
  --add-path="${redist_paths}" \
  --add-path="${paths}" \
  -w "${dest_dir}" \
  "${wheel}"

# Rename oidnDenoise
pip install wheel
dest_dir2=$(cygpath -u "${dest_dir}")
shopt -s nullglob
files=("${dest_dir2}"/*.whl)
if [ ${#files[@]} -eq 0 ]; then
  echo "ERROR: wheel repair produced no wheel in ${dest_dir2}" >&2
  exit 1
fi
# There should be only one wheel, but strictly speaking, we need a loop
for filename in "${files[@]}";
do
  make win-recompose "$filename"
done
