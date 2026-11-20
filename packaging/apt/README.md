# apt repository (H3d)

DriveLink's signed apt repository, served by GitHub Pages from the `gh-pages`
branch at <https://taruntom1.github.io/drivelink_v2/>. Installing from it is
in [docs/pi-setup.md](../../docs/pi-setup.md), "Install the packages".

| File | What |
|---|---|
| `drivelink-archive-keyring.asc` | The repository's public key, served next to the repository |
| `FINGERPRINT` | Its fingerprint; the publish refuses any other signing key |
| `index.html` | The repository's page (`@URL@` and `@FINGERPRINT@` filled in by `tools/apt_repo.py`) |
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

`gh-pages` holds one commit: the latest release, force-pushed by each tag.
Older packages stay on their GitHub release pages.

## How it is built and published

- `tools/apt_repo.py build` writes the tree from the `.deb` files and signs
  `Release` (InRelease and Release.gpg); `verify` checks it with apt itself,
  rootless: the signature, the indices and their hashes, resolution, and that
  a copy signed by another key, an unsigned copy and a changed index are
  refused.
- The `apt-repo` validate check and CI job do that with a throwaway key on
  every run; CI's `apt-install` job then installs from it in clean 26.04 and
  24.04 containers (`host/test/apt_repo_smoke.py`).
- Only a `vX.Y.Z` tag publishes: CI's `apt-publish` job, after `release`,
  signs with the real key, checks it against `FINGERPRINT` and the committed
  public key, and replaces `gh-pages`. It is the only job that reads the key.

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
After a rotation, existing users must fetch the new key (the commands in
docs/pi-setup.md), because apt refuses the repository until they do.
Before the key expires, extend it from the backup
(`gpg --quick-set-expire <fingerprint> 5y`), export the public key again and
replace the secret.

Pages must be enabled once, after the first publish creates `gh-pages`:
Settings → Pages → Deploy from a branch, `gh-pages`, `/ (root)`.
