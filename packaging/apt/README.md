# DriveLink apt repository

GitHub Pages serves the signed apt repository alongside the user guide. [Client installation](https://taruntom1.github.io/drivelink_v2/apt/)

## Layout

The `noble` and `resolute` suites each publish `main` for amd64 and arm64. Ubuntu-specific version suffixes let both suites share the package pool.

```text
dists/<suite>/{Release,InRelease,Release.gpg}
dists/<suite>/main/binary-{amd64,arm64}/Packages{,.gz}
pool/main/d/<package>/<package>_<version>_<arch>.deb
```

| Input | Use |
|---|---|
| [Public key](drivelink-archive-keyring.asc) | Published alongside the repository for client signature verification. |
| [Expected fingerprint](FINGERPRINT) | Pins the release signing key. |
| [Standalone index](index.html) | Landing page for an apt-only build. The combined site uses the guide home page. |
| [Key setup script](create_signing_key.sh) | Owner-only creation or rotation of the release key and its Actions secret. |

The latest verified release tree is mirrored to `gh-pages`. Older packages remain attached to their GitHub releases.

## ROS 2 packages

`noble` carries Jazzy and `resolute` carries Lyrical. Each distribution ships four DriveLink packages with names beginning `ros-<distro>-drivelink`. The bringup package pins the matching DriveLink version of the hardware plugin, host library and protocol library. [ROS installation](https://taruntom1.github.io/drivelink_v2/ros/install/)

## How it is built and published

| Stage | Source |
|---|---|
| Build packages | [Host Debian builder](../../tools/package_linux.py) · [ROS Bloom builder](../../tools/package_ros.py) |
| Build and verify indexes | `apt_repo.py build` creates and signs the tree. `verify` uses apt to check trust, index integrity and resolution, including rejection of altered or incorrectly signed inputs. [Repository tool](../../tools/apt_repo.py) |
| Test signing and installation | The `apt-repo` job checks the release matrix with a test key. `apt-install` installs in clean ROS containers and runs packaged bringup against the agent and vdev. [CI jobs](../../.github/workflows/ci.yml) · [Validation matrix](../../docs/development.md#validation-toolsvalidatepy) |
| Publish a release tag | `apt-publish` signs with the release key, verifies the combined guide and apt tree, and mirrors it to `gh-pages`. `guide-deploy` deploys the same artifact to Pages. [Release workflow](../../.github/workflows/ci.yml) |

## The signing key

Routine checks use disposable keys. The repository owner manages the sign-only Ed25519 release key and keeps an encrypted offline backup. The `apt-repo` Actions environment holds `APT_SIGNING_KEY` and admits only `v*` tags. Only the signing step receives that secret.

To create or rotate the release key, run the following from the repository root with `gh` authenticated as the owner. Choose a backup path outside the repository.

```sh
sh packaging/apt/create_signing_key.sh /path/to/encrypted-backup
```

Commit the public key and fingerprint together. After rotation, existing clients must fetch the updated public key. Before the five-year expiration, extend the key from the offline backup with `gpg --quick-set-expire <fingerprint> 5y`, export the updated public key, and replace the signing secret. [Client key setup](https://taruntom1.github.io/drivelink_v2/apt/#add-the-repository)

For initial deployment, set the Pages source to GitHub Actions and allow `v*` tags in the `github-pages` environment's deployment rules.
