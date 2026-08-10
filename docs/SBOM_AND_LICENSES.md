# SBOM and licensing guide

This guide describes how to make a source distribution of NNAGA auditable. It is an inventory and packaging procedure, not a generated SPDX document or a legal opinion. The repository does not currently contain a generated SBOM or a machine-validated license report; do not describe a release as having either unless one is generated and shipped with that release.

## What a source release identifies

A source release has three related identities:

- **Release artifact:** the exact archive (or other file) delivered to users. Record its filename, byte size, and SHA-256 digest. The digest identifies the bytes, not merely the tag.
- **Source revision:** the NNAGA superproject commit (`git rev-parse HEAD`) used to make the archive. A tag or release name is only a human label; record the full commit ID.
- **Submodule revisions:** each gitlink recorded by that superproject commit. A submodule's full commit ID is part of the source identity. Record the recursive list, including `liblowlatencyaudio` and its `third_party/libusb` submodule.

An archive is reproducible only when the same superproject revision, recursive submodule commits, build inputs, and archive procedure produce the same bytes. A normal `git archive` of the superproject records gitlinks but does **not** embed submodule working trees. A source distribution that is intended to build offline must therefore include the initialized submodule contents (or provide a separately identified, matching source bundle) and retain the recorded gitlink SHAs.

## Inventory record for each release

Keep the following alongside the release artifact (plain text is sufficient):

1. artifact filename and SHA-256 digest;
2. superproject commit and release/tag name, if any;
3. recursive submodule path, URL from `.gitmodules`, and checked-out commit;
4. whether each submodule's source is embedded in the archive or supplied separately;
5. the exact notice/license files included in the source bundle.

The authoritative top-level project license text is [`LICENSE`](../LICENSE), which is GNU GPL version 3. The independently maintained low-latency driver repository has its own copy at [`liblowlatencyaudio/LICENSE`](../liblowlatencyaudio/LICENSE). Its bundled libusb source is a separate licensing boundary: [`liblowlatencyaudio/third_party/libusb/README`](../liblowlatencyaudio/third_party/libusb/README) identifies libusb as GNU LGPL 2.1-or-later and points to [`liblowlatencyaudio/third_party/libusb/COPYING`](../liblowlatencyaudio/third_party/libusb/COPYING). Do not replace the libusb notice with NNAGA's GPL notice or imply that libusb is first-party NNAGA code.

## License and notice boundaries

### First-party NNAGA code

The top-level `LICENSE` is the authoritative license text for first-party NNAGA material covered by it. It is GPL-3.0-or-later (the license text is the GPL-3.0 text and permits the “any later version” option). The low-latency library is maintained as a separate repository/submodule and its authoritative text is `liblowlatencyaudio/LICENSE`; follow that file for files belonging to that repository. Individual source or metadata files and bundled components can carry more specific notices, which take precedence for those components.

### `3rd_party/` and LV2 plugins

Treat every directory under `3rd_party/` as an independent upstream boundary. The superproject's `.gitmodules` records the source URL and gitlink, but it does not replace the upstream license. Preserve each component's own `LICENSE`, `COPYING`, `NOTICE`, README license section, and source-file notices in the source distribution. This includes the LV2 stack (`3rd_party/lv2`, `serd`, `zix`, `sord`, `sratom`, and `lilv`), X11 libraries, audio/ML libraries, and the LV2 plugin submodules.

For bundled LV2 plugin trees, keep the plugin's own metadata and notices together with the plugin. LV2 manifest/Turtle `doap:license` declarations are useful provenance, but they are not a substitute for shipping the referenced license text. Do not collapse all LV2 material into “GPL” merely because some plugins are GPL: inspect and preserve the license boundary for each plugin and nested component. `3rd_party/` may also contain copied third-party files inside a submodule; those files retain their upstream notices.

### Wine, FEX, VST3, and their dependencies

The VST host sources are separate submodules under `vsthost_lib/external/`. Preserve notices recursively rather than treating the whole directory as one license:

- Wine: authoritative top-level text is `vsthost_lib/external/wine-upstream/LICENSE`; Wine's `libs/` tree also contains component notices (for example `COPYING`, `LICENSE`, and `LICENSE.md` files) that must remain with those components.
- FEX: authoritative top-level text is `vsthost_lib/external/fex-upstream/LICENSE`; preserve notices in `External/` and other nested upstream directories as well.
- VST3 SDK: authoritative top-level text is `vsthost_lib/external/vst3sdk/LICENSE.txt`. Its `vstgui4`, `public.sdk`, `base`, `pluginterfaces`, `cmake`, samples, and nested `thirdparty` directories contain additional license files.
- Other external submodules (for example DXVK, Vulkan headers/loader, glslang, llvm-mingw, and libadrenotools) have their own upstream notice locations. Include them when their source is included; do not infer a license from the directory name or from NNAGA's top-level GPL.

A binary release may not contain every source component, so make the inventory match the actual artifact. If a component is dynamically supplied by the platform rather than distributed, record that fact and do not accidentally claim its license text is inside the NNAGA archive.

## Reproducible inventory commands

Run these commands from the NNAGA repository root after checking out the intended release revision and initializing submodules. Save their output as release provenance; commands report facts from the current checkout and do not make a legal determination.

```bash
# Artifact-independent source identity.
git rev-parse HEAD
git describe --always --dirty

# Recursive submodule paths, recorded SHAs, and status markers.
git submodule status --recursive

# Exact gitlink entries stored in the superproject commit.
git ls-files -s | awk '$1 == "160000" { print $4 "\t" $3 }'

# Verify that every initialized submodule reports the commit expected by its gitlink.
git submodule update --init --recursive
git submodule status --recursive
```

To inventory notice candidates without modifying files, run the following. The first command covers tracked files in the superproject; the `foreach` command repeats the same check in every initialized submodule, where nested upstream notices actually live.

```bash
# Top-level and non-submodule tracked notice candidates.
git ls-files | grep -Ei '(^|/)(LICENSE|COPYING|NOTICE)(\.|$)|(^|/)(LICENSE|COPYING|NOTICE)(\.md|\.txt)?$' || true

# Notice candidates in every initialized submodule, with its path and commit.
git submodule foreach --recursive '
  printf "[%s] %s\\n" "$displaypath" "$(git rev-parse HEAD)"
  git ls-files | grep -Ei "(^|/)(LICENSE|COPYING|NOTICE)(\\.|$)|(^|/)(LICENSE|COPYING|NOTICE)(\\.md|\\.txt)?$" || true
'
```

For a byte-level artifact record, hash the exact file that will be uploaded (and hash any separately supplied source bundle separately):

```bash
sha256sum path/to/NNAGA-source.tar.*
stat --printf='%n\t%s bytes\n' path/to/NNAGA-source.tar.*
```

Review the resulting paths manually. A filename matching `LICENSE` is a candidate, not proof of scope; retain notices mentioned by README files, source headers, build manifests, and nested third-party directories even when their names do not match the simple pattern above. In particular, compare the final archive against the recursive submodule-SHA record so a source archive cannot silently omit a gitlink's contents or its notices.

## Distribution checklist

Before publishing a source artifact:

- build it from a clean checkout at the recorded superproject commit;
- initialize the same recursive submodule commits and record them;
- include the applicable top-level, `liblowlatencyaudio`, libusb, LV2, Wine/FEX/VST, and nested third-party notices;
- verify that generated assets do not replace source notices or alter the recorded source revision;
- calculate SHA-256 only after the final archive is complete;
- publish the provenance record with the artifact, without calling it an SBOM or a formal compliance report unless such a report was actually generated.
