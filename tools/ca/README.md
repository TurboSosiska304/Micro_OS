# The roots of the machine

`mozilla.pem` is the store of certificate authorities that `tools/x86_image.py`
writes into `/etc/ssl/certs.pem` of the x86-64 image. Without it a fresh
machine trusts nobody and `pkg -Sy` over `https://` answers that there is no
store, so the documented install path did not pass on an image out of the
box.

The file is the bundle that curl distributes at
<https://curl.se/docs/caextract.html>: the certificates of the Mozilla root
program, extracted from `certdata.txt` and written as PEM (the header of the
file names the date of the extraction and the SHA-256 of the conversion). The
list itself is the data of the Mozilla project and is distributed under the
MPL 2.0; it is data and not code, and the tree links none of it.

To update it, download the new bundle and copy it over this file:

```bash
curl -o tools/ca/mozilla.pem https://curl.se/ca/cacert.pem
make x86-disk reproducible
```

The store is read by `components/net/src/tls.c` (mbedTLS). A certificate whose
signature algorithm the image does not implement - an Ed25519 authority, for
one - is skipped with a line in the log and the rest of the store is kept, so
a full distribution bundle is a store this client can load.

A person replaces the store of a running machine the way the documentation
says: `wget http://host/ca.pem /etc/ssl/certs.pem` (or over `https` once a
store is there). It is one file of the filesystem, and no reflash.
