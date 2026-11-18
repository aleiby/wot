# dmgbuild 1.6.5 vendored wheels

These are the unmodified universal Python wheels published on PyPI for
`dmgbuild` 1.6.5 and its two runtime dependencies. They are kept in the
repository so the Mac release builder is deterministic and does not access the
network.

The source artifacts were obtained with:

```sh
python3 -m pip download --no-deps dmgbuild==1.6.5 ds_store==1.3.1 mac_alias==2.2.2
```

SHA-256 digests:

```text
e19ab8c5e8238e6455d9ccb9175817be7fd62b9cdd1eef20f63dd88e0ec469ab  dmgbuild-1.6.5-py3-none-any.whl
fbacbb0bd5193ab3e66e5a47fff63619f15e374ffbec8ae29744251a6c8f05b5  ds_store-1.3.1-py3-none-any.whl
504ab8ac546f35bbd75ad014d6ad977c426660aa721f2cd3acf3dc2f664141bd  mac_alias-2.2.2-py3-none-any.whl
```

The corresponding upstream license texts are preserved alongside the wheels:

- `LICENSE-dmgbuild` — MIT
- `LICENSE-ds_store` — MIT
- `LICENSE-mac_alias` — MIT
