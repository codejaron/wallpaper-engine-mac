# CQuickJSNG

This directory vendors the embeddable QuickJS-NG engine core from
<https://github.com/quickjs-ng/quickjs> at commit
`72ba50f63ee31202f8c18b8d07ab1e1c3486ee6f`.

Included engine sources:

- `quickjs.c`
- `dtoa.c`
- `libregexp.c`
- `libunicode.c`
- the headers and generated tables required by those sources

The command-line programs and host standard-library integration are deliberately
not included. In particular, this target does not contain `qjs.c`, `qjsc.c`, or
`quickjs-libc.c`, and it performs no runtime download.

To update the vendored code, review the upstream diff, replace only the files in
the list above and their required headers, update `UPSTREAM_REVISION`, copy the
new upstream license verbatim to
`ThirdPartyLicenses/QuickJS-NG-LICENSE.txt`, then build `CQuickJSNG` on both
supported macOS architectures before committing.
