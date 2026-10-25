#!/usr/bin/env bash
set -euo pipefail
rtl=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(dirname -- "$rtl")
image=${CPLD_QUARTUS_IMAGE:-localhost/pistorm-quartus:20.1.1}
output=${CPLD_OUTPUT_DIR:-$repo/build/cpld}
case ${1:-EPM240} in
    EPM240|EPM570) targets=("${1:-EPM240}") ;;
    all) targets=(EPM240 EPM570) ;;
    *) echo "Usage: bash rtl/tools/build.sh [EPM240|EPM570|all]" >&2; exit 2 ;;
esac
mkdir -p "$output"
output=$(cd -- "$output" && pwd)
for target in "${targets[@]}"; do
    project=pistormsxb_dev$target
    version=version_reg.v
    [[ "$target" != EPM570 ]] || version=version_reg_570.v
    work=$output/$target
    mkdir -p "$work"
    for file in "$project.qpf" "$project.qsf" "pistormSXB_dev${target}_1.1b.v" pistormsxb_dev.sdc "$version"; do
        cp -- "$rtl/$file" "$work/$file"
    done
    # Older images can use the same verified libudev workaround without a rebuild.
    run=(podman run --rm --env LD_PRELOAD=/lib/x86_64-linux-gnu/libudev.so.1
         --volume "$work:/work:rw" --workdir /work "$image")
    tool_version=$("${run[@]}" quartus_sh --version)
    [[ "$tool_version" == *"Version 20.1.1 Build 720"* ]] || {
        echo "Expected Quartus 20.1.1 Build 720, got: $tool_version" >&2; exit 1;
    }
    image_id=$(podman image inspect "$image" --format '{{.Id}}')
    "${run[@]}" quartus_sh --flow compile "$project"
    "${run[@]}" quartus_cpf -c -q 100KHz -g 3.3 -n p \
        "output_files_$target/$project.pof" "$target-program.svf"
    "${run[@]}" quartus_cpf -c -q 100KHz -g 3.3 -n v \
        "output_files_$target/$project.pof" "$target-verify.svf"
    python3 - "$repo" "$work" "$target" "$image" "$image_id" "$tool_version" <<'PY'
from pathlib import Path
import hashlib
import json
import subprocess
import sys

repo, work = map(Path, sys.argv[1:3])
target, image, image_id, tool_version = sys.argv[3:7]
files = sorted(p for p in work.rglob('*') if p.is_file() and
               (p.suffix in ('.v', '.qpf', '.qsf', '.sdc', '.pof', '.svf', '.pin', '.summary', '.rpt')))
hashes = {str(p.relative_to(work)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
manifest = dict(target=target, image=image, image_id=image_id, quartus=tool_version,
                revision=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
                dirty=bool(subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True)),
                firmware_word='0x421' if target == 'EPM240' else '0x521', files=hashes)
(work/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
(work/'SHA256SUMS').write_text(''.join(f'{digest}  {name}\n' for name, digest in hashes.items()))
print(f'Build and program/verify SVFs: {work}')
PY
done
