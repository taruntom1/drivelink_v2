# DriveLink apt repository

GitHub Pages serves the signed apt repository and the DriveLink user guide.
See [Install from apt](https://taruntom1.github.io/drivelink_v2/apt/) for the guide commands. For v0.1.0, see the
[tagged installation instructions](https://github.com/taruntom1/drivelink_v2/blob/v0.1.0/docs/pi-setup.md#4-install-the-packages).

| File | What |
|---|---|
| `drivelink-archive-keyring.asc` | The repository's public key, served next to the repository |
| `FINGERPRINT` | Its fingerprint; the publish refuses any other signing key |
| `index.html` | Landing page for a standalone apt build. The combined build uses the guide’s home page and links to `apt/` |
| `create_signing_key.sh` | Makes the key, the `apt-repo` environment and its secret (owner only) |

## Layout

One suite per Ubuntu release, named by codename: `noble` (24.04) and
`resolute` (26.04), component `main`, amd64 and arm64. A package's version
carries its release (`<VERSION>~ubuntu26.04`), so one pool holds both builds:

```
dists/<suite>/{Release,InRelease,Release.gpg}
dists/<suite>/main/binary-{amd64,arm64}/Packages{,.gz}
pool/main/d/<package>/<package>_<version>_<arch>.deb
```

`gh-pages` mirrors the verified release tree in one commit per tag.
Older packages stay on their GitHub release pages.

## ROS 2 packages

The `resolute` suite carries ROS 2 Lyrical and the `noble` suite carries Jazzy.
Both have amd64 and arm64 builds of these packages.

- `ros-<distro>-drivelink-protocol`
- `ros-<distro>-drivelink` (the C++ host library)
- `ros-<distro>-drivelink-hardware` (the ros2_control plugin)
- `ros-<distro>-drivelink-bringup` (launch files, config and diagnostics)

Install the bringup package to pull in the other three at the same DriveLink
version. Apt obtains upstream ROS dependencies from the
[ROS 2 apt repository](https://github.com/ros-infrastructure/ros-apt-source).
See the [ROS 2 installation guide](https://taruntom1.github.io/drivelink_v2/ros/install/).

## How it is built and published

- `tools/package_ros.py` runs Bloom and dpkg in the matching ROS build container.
  It stages each package with the shared version and CMake files and fetches the
  existing SHA256-pinned Asio and mDNS sources before the Debian build.
- `tools/apt_repo.py build` writes the tree from the `.deb` files and signs
  `Release` (InRelease and Release.gpg); `verify` checks it with apt itself,
  rootless: the signature, the indices and their hashes, resolution, and that
  a copy signed by another key, an unsigned copy and a changed index are
  refused.
- The `apt-repo` validate check and CI job do that with a throwaway key on
  every run. CI requires all 26 release packages. The `apt-install` job then
  installs from it in clean Lyrical and Jazzy containers on both architectures
  and runs the packaged bringup against the packaged agent and vdev.
- Only a `vX.Y.Z` tag publishes. After `release`, the `apt-publish` job
  signs with the real key, checks the fingerprint, and combines the apt
  repository with the guide. It verifies the combined tree using apt and
  the committed public key, uploads it for Pages, and mirrors the same tree
  to `gh-pages`. Only the signing step receives the private key.
- `guide-deploy` publishes that artifact with `pages: write` and
  `id-token: write`. Both the guide and apt use the same project URL.
  `dists/`, `pool/`, the public key URL and their contents are checked before
  deployment. Branch and PR previews use a throwaway key and are downloadable
  from Actions.

## The signing key

A sign-only ed25519 key, valid for 5 years, made offline by the repository
owner. Its secret half lives only in the Actions secret `APT_SIGNING_KEY` of
the environment `apt-repo`, whose deployment rule admits only `v*` tags (so no
branch or pull-request job can read it), and in the owner's encrypted offline
backup. No session or script prints it.

Create it (or rotate it) from the repository root, with `gh` logged in as the
owner:

```sh
sh packaging/apt/create_signing_key.sh <backup file outside the repository>
```

Then commit the new `drivelink-archive-keyring.asc` and `FINGERPRINT`.
After a rotation, existing users must [fetch the new key](https://taruntom1.github.io/drivelink_v2/apt/#add-the-repository)
before apt accepts the repository.
Before the key expires, extend it from the backup
(`gpg --quick-set-expire <fingerprint> 5y`), export the public key again and
replace the secret.

Before publishing the first tag with the guide, open **Settings → Pages**
and set **Source** to **GitHub Actions**. In **Settings → Environments →
github-pages**, allow release tags matching `v*` under deployment tags. This
lets the tag job deploy to the existing Pages URL.
