# The key of the repository of the project

`microos-progs.pub` is the Ed25519 public key of the repository
<https://turbososiska304.github.io/microos-progs/>, the one `pkg` of the
x86-64 image installs from by default. `tools/x86_image.py` writes it into
`/etc/pkg/keys/microos-progs.pub` of the image, which is the file
`docs/wiki/pkg.md` names as the key of a repository.

Without the file the key of the image answers for every repository, and the
key of an image built from this tree is a key generated in the build
directory of that build (the private key of a release is not in the tree,
ADR 0020): the documented `pkg -S hello` then failed with "the signature is
not the one of the repository" on every image a person builds. The key of the
project is public and belongs to the image; a repository of one's own is named
with its own `/etc/pkg/keys/<repo>.pub`, as the documentation says.

The file is sixty-four hexadecimal digits and a newline, the text
`microos_sign pubkey` writes:

```bash
build-x86-net/microos_sign pubkey --key signing.key   # or: cat microos-progs.pub
curl -o tools/pkg/microos-progs.pub \
     https://turbososiska304.github.io/microos-progs/microos-progs.pub
```
