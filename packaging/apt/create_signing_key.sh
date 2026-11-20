#!/bin/sh
# Create the apt repository's signing key (H3d). Run by the repository owner,
# once, from the repository root (Git Bash on Windows, or Linux/WSL), with
# gpg and an authenticated gh:
#
#     sh packaging/apt/create_signing_key.sh <backup file outside the repository>
#
# It makes a sign-only ed25519 key in a temporary GNUPGHOME and:
#   1. creates the GitHub Environment `apt-repo`, deployable only from `v*`
#      tags, and stores the armored secret key in it as APT_SIGNING_KEY;
#   2. writes an offline backup of the secret key, encrypted with a
#      passphrase gpg asks for;
#   3. writes the public key and its fingerprint into packaging/apt/;
#   4. deletes the temporary GNUPGHOME.
# The secret key goes only to the pipe into `gh secret set` and to the
# encrypted backup: it is never printed or written in the clear.
# Rotation: run it again (the secret is replaced), commit the new public key
# and FINGERPRINT, and tell users to fetch the new key (docs/pi-setup.md).
set -eu

backup=${1:?usage: sh packaging/apt/create_signing_key.sh <backup file outside the repository>}
repo=${DRIVELINK_REPO:-taruntom1/drivelink_v2}
here=packaging/apt
[ -f "$here/create_signing_key.sh" ] || { echo "run from the repository root" >&2; exit 2; }
[ ! -e "$backup" ] || { echo "$backup exists; choose a new file" >&2; exit 2; }
command -v gpg >/dev/null && command -v gh >/dev/null || { echo "needs gpg and gh" >&2; exit 2; }

GNUPGHOME=$(mktemp -d)
export GNUPGHOME
chmod 700 "$GNUPGHOME"
trap 'gpgconf --kill all >/dev/null 2>&1 || true; rm -rf "$GNUPGHOME"' EXIT

gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-gen-key 'DriveLink apt repository signing key' ed25519 sign 5y
fpr=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ { print $10; exit }')
[ ${#fpr} -eq 40 ] || { echo "no fingerprint found" >&2; exit 1; }
echo "fingerprint: $fpr"

echo "creating the environment apt-repo (v* tags only)"
printf '{"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}' |
    gh api -X PUT "repos/$repo/environments/apt-repo" --input - >/dev/null
gh api "repos/$repo/environments/apt-repo/deployment-branch-policies" --jq '.branch_policies[].name' |
    grep -qx 'v\*' ||
    gh api -X POST "repos/$repo/environments/apt-repo/deployment-branch-policies" \
        -f name='v*' -f type=tag >/dev/null
gpg --armor --export-secret-keys "$fpr" | gh secret set APT_SIGNING_KEY --env apt-repo --repo "$repo"

echo "writing the encrypted backup (gpg asks for its passphrase)"
gpg --armor --export-secret-keys "$fpr" |
    gpg --pinentry-mode loopback --symmetric --armor --output "$backup"

gpg --armor --export "$fpr" > "$here/drivelink-archive-keyring.asc"
echo "$fpr" > "$here/FINGERPRINT"
echo "done: $here/drivelink-archive-keyring.asc and $here/FINGERPRINT written;"
echo "the secret is in the apt-repo environment; keep $backup offline"
